#include "stdafx.h"
#include "ExternalAmp.hpp"
#include "../Mods/VolumeControl.hpp"
#include "../Mods/DropPedal/DropPedal.hpp"
#include "../GameState.hpp"
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace ExternalAmp
{
	namespace
	{
		constexpr wchar_t kKey[] = L"Software\\RSMods\\AsioProxy";
		constexpr unsigned kPlayer1Channel = 1;   // VolumeControl channel index of Mixer_Player1
		constexpr int kMinReturnTenths = -300, kMaxReturnTenths = 60;

		std::atomic<bool> g_enabled{ true };
		std::atomic<bool> g_fallback{ true };
		std::atomic<int> g_returnTenths{ 0 };
		std::atomic<bool> g_safety{ false };

		std::mutex g_snapshotMutex;
		Snapshot g_snapshot;

		DWORD ReadDword(const wchar_t* name, DWORD fallback)
		{
			HKEY key = nullptr;
			DWORD value = fallback, size = sizeof(value), type = 0;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return fallback;
			if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS || type != REG_DWORD) value = fallback;
			RegCloseKey(key);
			return value;
		}

		void WriteDword(const wchar_t* name, DWORD value)
		{
			HKEY key = nullptr;
			if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
			RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
			RegCloseKey(key);
		}

		// 64-bit amp sims list ASIO drivers from the 64-bit view, which this 32-bit process must ask for.
		bool Guest64Registered()
		{
			HKEY key = nullptr;
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO\\Rocksmith Audio Bridge ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return false;
			RegCloseKey(key);
			return true;
		}

		HMODULE Proxy() { return GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll"); }

		bool ReadLinkStatus(AmpLink::Status& status)
		{
			HMODULE proxy = Proxy();
			if (!proxy) return false;
			using StatusFn = int(__cdecl*)(AmpLink::Status*, int);
			auto fn = reinterpret_cast<StatusFn>(GetProcAddress(proxy, "RSModsAsio_GetAmpLinkStatus"));
			return fn && fn(&status, static_cast<int>(sizeof(status))) == 1;
		}

		void ConfigureProxy(bool mixOn, float gain, bool safety)
		{
			HMODULE proxy = Proxy();
			if (!proxy) return;
			using ConfigureFn = void(__cdecl*)(int, float);
			using LatencyFn = void(__cdecl*)(int);
			if (auto fn = reinterpret_cast<ConfigureFn>(GetProcAddress(proxy, "RSModsAsio_ConfigureAmpLink"))) fn(mixOn ? 1 : 0, gain);
			if (auto fn = reinterpret_cast<LatencyFn>(GetProcAddress(proxy, "RSModsAsio_SetAmpLinkLatency"))) fn(safety ? 2 : 1);
		}

		// The semitones the Drop Pedal applies to the game's guitar input right now (0 when off, or when it shifts
		// the song instead). The guest applies the same shift to the amp sim's input.
		void SendPedalShift(int semitones)
		{
			HMODULE proxy = Proxy();
			if (!proxy) return;
			using PitchFn = void(__cdecl*)(int);
			if (auto fn = reinterpret_cast<PitchFn>(GetProcAddress(proxy, "RSModsAsio_SetAmpLinkPitch"))) fn(semitones);
		}

		// Player 1's level as the game would play it: the louder of our game object's value and the global value.
		// We write both when we mute, but the game can raise only the global one, which our game-object 0 then hides
		// from VolumeControl::GetPlaybackVolume.
		bool ReadPlayer1(float& level, RTPCValue_type& source)
		{
			if (!VolumeControl::GetPlaybackVolume(kPlayer1Channel, level)) return false;
			source = RTPCValue_GameObject;
			float global = 0.0f;
			RTPCValue_type type = RTPCValue_Global;
			if (Wwise::SoundEngine::Query::GetRTPCValue("Mixer_Player1", AK_INVALID_GAME_OBJECT, &global, &type) == AK_Success
				&& std::isfinite(global) && global > level && global <= 100.0f)
			{
				level = global;
				source = RTPCValue_Global;
			}
			return true;
		}

		// Holds Mixer_Player1 at 0 while asked, remembering the player's level to put back.
		class GameAmpMute
		{
		public:
			bool Muted() const { return m_muted; }

			// With the amp sim connected at launch, the mute can run before the game has given Player 1 its level
			// and read 0. A 0 read at mute time is therefore only provisional: any later raise (global or game
			// object) replaces it, and if none arrives the release puts back kUnknownRestore.
			static constexpr float kUnknownRestore = 100.0f;   // Rocksmith's default Player 1 level

			// Our own 150 ms fades must never be read back as the player's level (a mid-fade read would shrink the
			// restore level on every off/on). Readings within kSettleMs of our last change are ignored, and a mute
			// that starts while our restore fade is still running keeps the level we were restoring.
			static constexpr ULONGLONG kSettleMs = 500;

			void Hold()
			{
				const ULONGLONG now = GetTickCount64();
				const bool settling = now - m_changedAt < kSettleMs;
				float current = 0.0f;
				RTPCValue_type source = RTPCValue_GameObject;
				if (!ReadPlayer1(current, source)) return;
				if (!m_muted)
				{
					if (!settling)
					{
						m_restore = current;
						m_restoreKnown = current > 0.5f;
					}
					if (VolumeControl::SetPlaybackVolumeWithTransition(kPlayer1Channel, 0.0f, 150))
					{
						m_muted = true;
						m_changedAt = now;
						LOG_INFO("(EXTERNAL AMP) amp sim connected: game guitar muted (level " << m_restore
							<< (m_restoreKnown ? "" : ", not trusted yet") << " kept to restore)" << std::endl);
					}
					return;
				}
				// Raised again by the game, the Mixer page or a volume key: that is the player's new level.
				if (!settling && current > 0.5f)
				{
					m_restore = current;
					m_restoreKnown = true;
					VolumeControl::SetPlaybackVolume(kPlayer1Channel, 0.0f);
					LOG_INFO("(EXTERNAL AMP) Player 1 raised to " << current << (source == RTPCValue_Global ? " (global)" : "")
						<< " while the amp sim plays; held at 0, will restore " << current << std::endl);
				}
			}

			void Release(const char* why)
			{
				if (!m_muted) return;
				if (!m_restoreKnown)
				{
					LOG_INFO("(EXTERNAL AMP) Player 1 read " << m_restore << " when muted and the game never set it since; restoring "
						<< kUnknownRestore << " instead" << std::endl);
					m_restore = kUnknownRestore;
					m_restoreKnown = true;
				}
				// A keybind mute taken meanwhile stays muted; it restores our remembered level when undone.
				if (VolumeControl::player1Muted) player1VolumeBeforeMute = m_restore;
				else if (!VolumeControl::SetPlaybackVolumeWithTransition(kPlayer1Channel, m_restore, 150)) return;
				m_muted = false;
				m_changedAt = GetTickCount64();
				LOG_INFO("(EXTERNAL AMP) " << why << ": game guitar back at " << m_restore << std::endl);
			}

		private:
			bool m_muted = false;
			float m_restore = 100.0f;
			bool m_restoreKnown = true;   // false while m_restore is a 0 read at mute time that nothing has confirmed
			ULONGLONG m_changedAt = 0;   // when we last moved Player 1 ourselves
		};

		void Run()
		{
			while (!GameState::GameLoaded) Sleep(500);
			GameAmpMute mute;
			// Flip only after the state has held for a moment, so a short hiccup (the amp sim reloading a preset,
			// a late block) does not toggle the game's amp on and off.
			int connectedTicks = 0, disconnectedTicks = 0;
			bool useAmpSim = false;
			bool lastProxyLoaded = false;
			ULONGLONG nextRegistryCheck = 0;
			bool guest64 = false;

			while (!GameState::GameClosing)
			{
				const bool enabled = g_enabled.load();
				const bool fallback = g_fallback.load();
				const int tenths = g_returnTenths.load();
				const float gain = std::pow(10.0f, tenths / 200.0f);

				AmpLink::Status link{};
				const bool proxyLoaded = ReadLinkStatus(link);
				if (proxyLoaded != lastProxyLoaded)
				{
					lastProxyLoaded = proxyLoaded;
					LOG_INFO("(EXTERNAL AMP) Audio Bridge driver " << (proxyLoaded ? "present: external amp available" : "not loaded: external amp unavailable") << std::endl);
				}
				ConfigureProxy(enabled, gain, g_safety.load());
				const int pedalSemitones = DropPedal::GetAppliedInputShiftSemitones();
				SendPedalShift(pedalSemitones);

				const bool connected = proxyLoaded && link.linkOpen && link.connected;
				connectedTicks = connected ? connectedTicks + 1 : 0;
				disconnectedTicks = connected ? 0 : disconnectedTicks + 1;
				if (connected && connectedTicks >= 2) useAmpSim = true;     // ~0.2 s
				if (!connected && disconnectedTicks >= 5) useAmpSim = false; // ~0.5 s

				// Fallback off: the game's amp stays muted while the switch is on and the link exists, even with
				// nothing connected. Never muted without the proxy: there would be no way to hear the guitar.
				const bool wantMute = enabled && proxyLoaded && link.linkOpen && (fallback ? useAmpSim : true);
				if (Wwise::SoundEngine::IsInitialized())
				{
					if (wantMute) mute.Hold();
					else mute.Release(!enabled ? "external amp switched off" : "amp sim disconnected");
				}

				const ULONGLONG now = GetTickCount64();
				if (now >= nextRegistryCheck) { guest64 = Guest64Registered(); nextRegistryCheck = now + 5000; }

				{
					std::lock_guard<std::mutex> lock(g_snapshotMutex);
					g_snapshot.enabled = enabled;
					g_snapshot.fallback = fallback;
					g_snapshot.returnTenths = tenths;
					g_snapshot.proxyLoaded = proxyLoaded;
					g_snapshot.guest64Registered = guest64;
					g_snapshot.gameAmpMuted = mute.Muted();
					g_snapshot.pedalSemitones = pedalSemitones;
					g_snapshot.link = link;
				}
				Sleep(100);
			}
		}
	}

	void Start()
	{
		g_enabled.store(ReadDword(L"ExternalAmp", 1) != 0);
		g_fallback.store(ReadDword(L"ExternalAmpFallback", 1) != 0);
		const int tenths = static_cast<int>(static_cast<int32_t>(ReadDword(L"ExternalAmpReturn", 0)));
		g_returnTenths.store(std::clamp(tenths, kMinReturnTenths, kMaxReturnTenths));
		g_safety.store(ReadDword(L"ExternalAmpSafety", 0) != 0);
		std::thread(Run).detach();
	}

	Snapshot GetSnapshot()
	{
		std::lock_guard<std::mutex> lock(g_snapshotMutex);
		Snapshot snapshot = g_snapshot;
		// The switches answer immediately, without waiting for the next tick.
		snapshot.enabled = g_enabled.load();
		snapshot.fallback = g_fallback.load();
		snapshot.returnTenths = g_returnTenths.load();
		snapshot.safetyBuffer = g_safety.load();
		return snapshot;
	}

	void SetEnabled(bool enabled) { g_enabled.store(enabled); WriteDword(L"ExternalAmp", enabled ? 1 : 0); }
	void SetSafetyBuffer(bool on) { g_safety.store(on); WriteDword(L"ExternalAmpSafety", on ? 1 : 0); }
	void SetFallback(bool fallback) { g_fallback.store(fallback); WriteDword(L"ExternalAmpFallback", fallback ? 1 : 0); }
	void SetReturnTenths(int tenths, bool persist)
	{
		tenths = std::clamp(tenths, kMinReturnTenths, kMaxReturnTenths);
		g_returnTenths.store(tenths);
		if (persist) WriteDword(L"ExternalAmpReturn", static_cast<DWORD>(static_cast<int32_t>(tenths)));
	}
}
