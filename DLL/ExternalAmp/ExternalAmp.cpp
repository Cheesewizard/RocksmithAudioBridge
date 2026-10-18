#include "stdafx.h"
#include "ExternalAmp.hpp"
#include "CableAmpLink.hpp"
#include "ToneDesigner/VstHostLink.hpp"
#include "Mods/VolumeControl.hpp"
#include "DropPedal/DropPedal.hpp"
#include "GameState.hpp"
#include "AudioBridge/Engine/OutputReserve.hpp"
#include "AudioBridge/Input/InputCaptureHook.hpp"
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
		constexpr char kToneDesignerGuest[] = "RocksmithAudioBridgeVstHost.exe";

		std::atomic<bool> g_enabled{ true };
		std::atomic<bool> g_compare{ false };   // A/B: the game's amp for now, not saved
		std::atomic<bool> g_fallback{ true };
		std::atomic<int> g_returnTenths{ 0 };
		std::atomic<bool> g_safety{ false };
		std::atomic<int> g_cableBlock{ 0 };
		std::atomic<bool> g_cableBlockApplying{ false };   // a SetCableBlock thread is running
		std::atomic<bool> g_silenceTuning{ false };

		std::mutex g_snapshotMutex;
		Snapshot g_snapshot;

		DWORD ReadDword(const wchar_t* name, DWORD fallback, const wchar_t* keyPath = kKey)
		{
			HKEY key = nullptr;
			DWORD value = fallback, size = sizeof(value), type = 0;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return fallback;
			if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS || type != REG_DWORD) value = fallback;
			RegCloseKey(key);
			return value;
		}

		void WriteDword(const wchar_t* name, DWORD value, const wchar_t* keyPath = kKey)
		{
			HKEY key = nullptr;
			if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
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

		// The link's host: the ASIO driver when it is the game's driver, else (Real Tone Cable) the game DLL's own
		// CableAmpLink. The rest of this file does not care which.
		bool ReadLinkStatus(AmpLink::Status& status)
		{
			HMODULE proxy = Proxy();
			if (!proxy)
			{
				if (!CableAmpLink::IsOpen()) return false;
				status = CableAmpLink::GetStatus();
				return true;
			}
			using StatusFn = int(__cdecl*)(AmpLink::Status*, int);
			auto fn = reinterpret_cast<StatusFn>(GetProcAddress(proxy, "RSModsAsio_GetAmpLinkStatus"));
			return fn && fn(&status, static_cast<int>(sizeof(status))) == 1;
		}

		void ConfigureProxy(bool mixOn, float gain, bool safety)
		{
			HMODULE proxy = Proxy();
			if (!proxy)
			{
				CableAmpLink::Configure(mixOn, gain);
				CableAmpLink::SetLatencyBlocks(safety ? 2 : 1);
				return;
			}
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
			if (!proxy) { CableAmpLink::SetPitchSemitones(semitones); return; }
			using PitchFn = void(__cdecl*)(int);
			if (auto fn = reinterpret_cast<PitchFn>(GetProcAddress(proxy, "RSModsAsio_SetAmpLinkPitch"))) fn(semitones);
		}

		// The Input page's hum filter and noise suppressor, for the amp sim's input too (AmpFeedConditioner.hpp): the
		// link feeds the amp sim the raw DI, so without this a noisy interface reached a high-gain amp sim unfiltered.
		// Read from the game's input chain, so the switches there mean the same thing for both.
		void SendConditioner()
		{
			const float humBaseHz = Audio::AsioHook::GetHumFilterBaseHz();
			const float gateDb = Audio::AsioHook::GetNoiseGateThresholdDb();   // 0 = off
			const float gate = gateDb < 0.0f ? std::pow(10.0f, gateDb / 20.0f) : 0.0f;
			HMODULE proxy = Proxy();
			if (!proxy) { CableAmpLink::ConfigureConditioner(humBaseHz, gate); return; }
			using ConditionerFn = void(__cdecl*)(float, float);
			if (auto fn = reinterpret_cast<ConditionerFn>(GetProcAddress(proxy, "RSModsAsio_ConfigureAmpFeed"))) fn(humBaseHz, gate);
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

		// Silence the guitar in the tuner screens: one General setting for every way the guitar is heard. It used to
		// be Tone designer's "Mute while tuning" (VstHost\MuteWhileTuning), which is kept in step so the VST host still
		// mutes its chain there, and is this setting's starting value.
		constexpr wchar_t kVstHostKey[] = L"Software\\RSMods\\VstHost";

		// The tuner screens, read here rather than from GameState::currentMenu, which other threads write.
		bool InTuner()
		{
			return GameState::GameLoaded && Contains(GameState::GetCurrentMenu(), GameState::tuningMenus);
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

			void Hold(const char* why)
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
						LOG_INFO("(EXTERNAL AMP) " << why << ": game guitar muted (level " << m_restore
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
						<< " while muted (" << why << "); held at 0, will restore " << current << std::endl);
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
			bool wasTuning = false;

			while (!GameState::GameClosing)
			{
				AmpLink::Status link{};
				const bool proxyLoaded = ReadLinkStatus(link);

				// Two features share the link: an external app (this page's switch) and Tone designer (the VST host,
				// its own page). Tone designer plays whenever it is the one connected; the switch is for other apps.
				const bool toneDesigner = link.guestPresent && _stricmp(link.guestName, kToneDesignerGuest) == 0;
				// The External amp page's settings (switch, return level, fallback) are for external apps only: Tone
				// designer has its own output trim, and the game's amp always comes back if it stops.
				const bool enabled = (g_enabled.load() || toneDesigner) && !g_compare.load();
				const bool fallback = toneDesigner || g_fallback.load();
				const int tenths = toneDesigner ? 0 : g_returnTenths.load();
				const float gain = std::pow(10.0f, tenths / 200.0f);
				if (proxyLoaded != lastProxyLoaded)
				{
					lastProxyLoaded = proxyLoaded;
					LOG_INFO("(EXTERNAL AMP) " << (!proxyLoaded ? "No amp link host (no Audio Bridge driver, no cable link): external amp unavailable"
						: Proxy() ? "Audio Bridge driver present: external amp available" : "Real Tone Cable hosts the amp link: external amp available") << std::endl);
				}
				// In the tuner, with Silence guitar while tuning on: the amp sim's return is not mixed in and the game's
				// amp is held at 0 below, so nothing plays the guitar whichever way it is heard.
				const bool tuning = g_silenceTuning.load() && InTuner();
				ConfigureProxy(enabled && !tuning, gain, g_safety.load());
				const int pedalSemitones = DropPedal::GetAppliedInputShiftSemitones();
				SendPedalShift(pedalSemitones);
				SendConditioner();

				const bool connected = proxyLoaded && link.linkOpen && link.connected;
				connectedTicks = connected ? connectedTicks + 1 : 0;
				disconnectedTicks = connected ? 0 : disconnectedTicks + 1;
				if (connected && connectedTicks >= 2) useAmpSim = true;     // ~0.2 s
				if (!connected && disconnectedTicks >= 5) useAmpSim = false; // ~0.5 s

				// Fallback off: the game's amp stays muted while the switch is on and the link exists, even with
				// nothing connected. Never muted without the proxy: there would be no way to hear the guitar.
				const bool ampSimMute = enabled && proxyLoaded && link.linkOpen && (fallback ? useAmpSim : true);
				if (Wwise::SoundEngine::IsInitialized())
				{
					if (ampSimMute) mute.Hold("amp sim connected");
					else if (tuning) mute.Hold("tuner, silence guitar while tuning");
					else mute.Release(wasTuning ? "left the tuner" : !enabled ? "external amp switched off" : "amp sim disconnected");   // the switch is ignored for Tone designer
				}

				wasTuning = tuning;

				// Cable mode's exclusive / ASIO output keeps a ~50 ms reserve while the game loads (OutputReserve), and
				// goes lean where the game's sound has to be fast: in songs, tuners and calibration, and in menus while
				// the player plays through the game's own amp (not while Tone designer plays it: that skips the queue).
				{
					const std::string menu = GameState::GetCurrentMenu();
					// SelectionListDialog is left out: the game uses it for other lists too, and at boot.
					const bool playScreen = menu != "SelectionListDialog" && (Contains(menu, GameState::songModes) || Contains(menu, GameState::songMenus)
						|| Contains(menu, GameState::tuningMenus) || Contains(menu, GameState::calibrationMenus)
						|| Contains(menu, GameState::lessonModes) || Contains(menu, GameState::riffRepeaterMenus));
					const bool noodling = !mute.Muted() && Audio::OutputReserve::Playing();
					const bool reserve = !playScreen && !noodling;
					if (reserve != Audio::OutputReserve::Wanted())
					{
						Audio::OutputReserve::SetWanted(reserve);
						LOG_INFO("(CABLE LATENCY) output reserve " << (reserve ? "on (menus / loading)" : playScreen ? "off (playing screen " + menu + ")" : std::string("off (playing through the game's amp)")) << std::endl);
					}
				}

				const ULONGLONG now = GetTickCount64();
				if (now >= nextRegistryCheck) { guest64 = Guest64Registered(); nextRegistryCheck = now + 5000; }

				{
					std::lock_guard<std::mutex> lock(g_snapshotMutex);
					g_snapshot.enabled = enabled;
					g_snapshot.toneDesigner = toneDesigner;
					g_snapshot.fallback = fallback;
					g_snapshot.returnTenths = tenths;
					g_snapshot.proxyLoaded = proxyLoaded;
					g_snapshot.guest64Registered = guest64;
					g_snapshot.gameAmpMuted = mute.Muted();
					g_snapshot.pedalSemitones = pedalSemitones;
					g_snapshot.link = link;
					// Blocks the amp missed while Tone designer was loading are not a performance problem: not shown as late.
					if (!Proxy()) g_snapshot.link.lateBlocks -= (std::min)(g_snapshot.link.lateBlocks, CableAmpLink::LateWhileLoading());
					g_snapshot.cableHost = proxyLoaded && !Proxy();
					g_snapshot.cableDirectUsb = CableAmpLink::IsDirectUsb();
					g_snapshot.cableLatencyMs = CableAmpLink::RecentLatencyMs();
					g_snapshot.cableReadMs = CableAmpLink::RecentReadMs();
				}
				CableAmpLink::LogLatency();   // every 30 s while the cable hosts the link
				CableAmpLink::SetGuestLoading(VstHostLink::IsLoading());
				CableAmpLink::Maintain();
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
		g_cableBlock.store(static_cast<int>(ReadDword(L"CableBlockFrames", 0)));
		CableAmpLink::SetBlockSetting(g_cableBlock.load());   // before the cable opens the link, so it opens at that size
		g_silenceTuning.store(ReadDword(L"SilenceGuitarWhileTuning", ReadDword(L"MuteWhileTuning", 0, kVstHostKey)) != 0);
		std::thread(Run).detach();
	}

	Snapshot GetSnapshot()
	{
		std::lock_guard<std::mutex> lock(g_snapshotMutex);
		Snapshot snapshot = g_snapshot;
		// The switches answer immediately, without waiting for the next tick.
		snapshot.enabled = g_enabled.load();
		snapshot.comparing = g_compare.load();
		snapshot.fallback = g_fallback.load();
		snapshot.returnTenths = g_returnTenths.load();
		snapshot.safetyBuffer = g_safety.load();
		snapshot.cableBlockSetting = g_cableBlock.load();
		snapshot.silenceWhileTuning = g_silenceTuning.load();
		return snapshot;
	}

	void SetEnabled(bool enabled) { g_enabled.store(enabled); WriteDword(L"ExternalAmp", enabled ? 1 : 0); }
	void SetCompare(bool gameAmp) { g_compare.store(gameAmp); }
	void SetSafetyBuffer(bool on) { g_safety.store(on); WriteDword(L"ExternalAmpSafety", on ? 1 : 0); }
	void SetCableBlock(int frames)
	{
		frames = frames == 64 || frames == 128 || frames == 256 ? frames : 0;
		g_cableBlock.store(frames);
		WriteDword(L"CableBlockFrames", static_cast<DWORD>(frames));
		// Reopening the link waits for the audio threads and writes the registry: not on the render thread. One thread
		// at a time, applying the newest setting until nothing changed meanwhile, so quick clicks cannot end on an older size.
		if (g_cableBlockApplying.exchange(true)) return;
		std::thread([]
		{
			for (;;)
			{
				const int applied = g_cableBlock.load();
				CableAmpLink::SetBlockSetting(applied);
				if (g_cableBlock.load() != applied) continue;
				g_cableBlockApplying.store(false);
				// A click just before the store saw this thread still running and left its value to it.
				if (g_cableBlock.load() == applied || g_cableBlockApplying.exchange(true)) return;
			}
		}).detach();
	}
	void SetSilenceWhileTuning(bool on)
	{
		g_silenceTuning.store(on);
		WriteDword(L"SilenceGuitarWhileTuning", on ? 1 : 0);
		WriteDword(L"MuteWhileTuning", on ? 1 : 0, kVstHostKey);   // Tone designer reads it for its own chain
	}
	void SetFallback(bool fallback) { g_fallback.store(fallback); WriteDword(L"ExternalAmpFallback", fallback ? 1 : 0); }
	void SetReturnTenths(int tenths, bool persist)
	{
		tenths = std::clamp(tenths, kMinReturnTenths, kMaxReturnTenths);
		g_returnTenths.store(tenths);
		if (persist) WriteDword(L"ExternalAmpReturn", static_cast<DWORD>(static_cast<int32_t>(tenths)));
	}
}
