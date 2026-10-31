#include "stdafx.h"
#include "LatencyCalibration.hpp"
#include "../../Shared/LatencyMeasure.hpp"
#include "GameState.hpp"
#include "../../Core/GameMenus.hpp"
#include "../../Core/GameView.hpp"
#include "../../ExternalAmp/ExternalAmp.hpp"
#include "../ToneDesignerLink.hpp"

#include <cmath>
#include <cstdio>
#include <mutex>

namespace Audio::LatencyCalibration
{
	namespace
	{
		using Result = AsioProxy::LatencyMeasure::Result;
		using State = AsioProxy::LatencyMeasure::State;

		constexpr const wchar_t* kStoreKey = L"Software\\RSMods\\AsioProxy\\MeasuredLatency";
		constexpr ULONGLONG kSettleMs = 8000;      // after a stream starts: the game's amp and tone are up
		constexpr ULONGLONG kRetryMs = 60000;      // an automatic try that found nobody quiet, or no answer
		constexpr int kAutoTries = 3;              // per reason per launch (see g_wantKey)
		constexpr ULONGLONG kRecheckMs = 300000;   // after a song, the game path is checked again at most this often

		std::mutex g_mutex;
		bool g_haveResult = false;
		Result g_last{};                 // the driver's latest state
		std::wstring g_setup;            // the store's subkey for the stream now, empty = none
		Figures g_figures;               // stored for g_setup
		ULONGLONG g_setupSince = 0;
		ULONGLONG g_nextAuto = 0;
		int g_autoTries = 0;
		int32_t g_seenSequence = -1;
		bool g_autoRun = false;          // the running measurement was started automatically
		std::string g_status;
		std::string g_wantKey;           // why the automatic measurement is wanted now; a new reason gets new tries
		bool g_wasInSong = false;
		bool g_recheckDue = false;       // a song ended: check the game path again (a dropout might have moved it)
		ULONGLONG g_lastRecheck = 0;
		float g_gameBefore = -1.0f;      // the game figure before an after-song recheck, to log whether it moved

		// The amp link now, read every Tick: the live parts the amp figure is rebuilt from.
		struct Link
		{
			bool playing = false;        // a guest is writing blocks back
			std::string guest;           // its program, e.g. RocksmithAudioBridgeVstHost.exe
			float blocksMs = 0.0f;       // the link's own blocks (1, or 2 with the safety buffer)
			float pluginMs = 0.0f;       // Tone designer: the delay the chain's plugins report; 0 for other guests
		};
		Link g_link;

		// ExternalAmp's worker, outside its snapshot lock (Tick is called after it).
		Link ReadLink()
		{
			const ExternalAmp::Snapshot amp = ExternalAmp::GetSnapshot();
			Link link;
			link.playing = amp.link.connected != 0;
			link.guest = amp.link.guestName;
			const double rate = amp.link.sampleRate > 0 ? amp.link.sampleRate : 48000.0;
			if (amp.link.frames > 0) link.blocksMs = static_cast<float>(amp.link.frames * (amp.link.latencyBlocks > 1 ? 2 : 1) * 1000.0 / rate);
			const AudioBridge::IToneDesignerHost* toneDesigner = AudioBridge::ToneDesignerHost();
			const int plugin = toneDesigner && toneDesigner->IsHostGuest(link.guest) ? toneDesigner->ChainLatencySamples() : 0;
			if (plugin > 0) link.pluginMs = static_cast<float>(plugin * 1000.0 / rate);
			return link;
		}

		// The amp path now: the measured rest plus the live blocks and plugin delay. Measured with another guest (an
		// amp sim app, then the Tone designer) it does not count. With g_mutex held.
		float AmpNowMs()
		{
			const Figures& f = g_figures;
			if (f.ampMs < 0.0f) return -1.0f;
			if (!f.hasAmpBase) return f.ampMs;   // stored before the split was kept: as measured
			if (!g_link.guest.empty() && _stricmp(f.ampGuest.c_str(), g_link.guest.c_str()) != 0) return -1.0f;
			return (std::max)(0.0f, f.ampBaseMs + g_link.blocksMs + g_link.pluginMs);
		}

		HMODULE Proxy() { return GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll"); }

		template <typename Fn> Fn Export(const char* name)
		{
			HMODULE proxy = Proxy();
			return proxy ? reinterpret_cast<Fn>(GetProcAddress(proxy, name)) : nullptr;
		}

		bool ReadResult(Result& result)
		{
			auto get = Export<int(__cdecl*)(Result*, int)>("RSModsAsio_GetLatencyMeasure");
			return get && get(&result, static_cast<int>(sizeof(result))) == 1;
		}

		bool StartKind(int kind)
		{
			auto start = Export<int(__cdecl*)(int)>("RSModsAsio_StartLatencyMeasure");
			return start && start(kind) == 1;
		}

		std::wstring SetupOf(const Result& r)
		{
			if (r.bufferSize <= 0 || r.sampleRate <= 0 || !r.driver[0]) return {};
			std::wstring name = r.driver;
			for (wchar_t& c : name) if (c == L'\\' || c == L'/') c = L'_';
			wchar_t tail[48];
			swprintf_s(tail, L"@%d@%d%s", r.bufferSize, r.sampleRate, r.host64 ? L"@64" : L"");
			return name + tail;
		}

		float ReadUs(const std::wstring& setup, const wchar_t* name)
		{
			DWORD value = 0, size = sizeof(value);
			const std::wstring key = std::wstring(kStoreKey) + L"\\" + setup;
			if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return -1.0f;
			return value / 1000.0f;
		}

		void WriteUs(const std::wstring& setup, const wchar_t* name, float ms)
		{
			const std::wstring key = std::wstring(kStoreKey) + L"\\" + setup;
			if (ms < 0.0f) { RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), name); return; }
			const DWORD value = static_cast<DWORD>(std::lround(ms * 1000.0f));
			RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_DWORD, &value, sizeof(value));
		}

		Figures Load(const std::wstring& setup)
		{
			Figures f;
			if (setup.empty()) return f;
			f.gameMs = ReadUs(setup, L"GameUs");
			f.ampMs = ReadUs(setup, L"AmpUs");
			f.roundTripMs = ReadUs(setup, L"RoundTripUs");
			// The part of the amp figure that is neither the link's blocks nor the plugins' own delay (signed, an int32
			// in a DWORD), and the guest it was measured with. Older stores have neither: the amp figure then shows as
			// measured, unadjusted.
			const std::wstring key = std::wstring(kStoreKey) + L"\\" + setup;
			DWORD value = 0, size = sizeof(value);
			if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"AmpBaseUs", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
			{
				f.ampBaseMs = static_cast<int32_t>(value) / 1000.0f;
				f.hasAmpBase = true;
			}
			char guest[64]{};
			DWORD bytes = sizeof(guest);
			// The setup key is wide (driver names); the guest name is stored narrow, as the amp link reports it.
			HKEY store = nullptr;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &store) == ERROR_SUCCESS)
			{
				if (RegGetValueA(store, nullptr, "AmpGuest", RRF_RT_REG_SZ, nullptr, guest, &bytes) == ERROR_SUCCESS) f.ampGuest = guest;
				RegCloseKey(store);
			}
			return f;
		}

		// No song, no calibration meter or tuner listening to the input: a second of chirps in place of the guitar
		// goes unnoticed (and could not upset the game's own input calibration).
		bool QuietMoment()
		{
			// Published on MainThread (Core/GameView): this runs on the External amp worker.
			if (!GameState::GameLoaded || AudioBridge::GameView::IsInSong()) return false;
			const std::string menu = AudioBridge::GameView::CurrentMenu();
			if (menu.empty() || menu == "SelectionListDialog") return false;   // also shown at boot
			return !Contains(menu, GameState::songMenus) && !Contains(menu, GameState::tuningMenus) && !Contains(menu, GameState::calibrationMenus)
				&& !Contains(menu, GameState::lessonModes) && !Contains(menu, AudioBridge::GameMenus::riffRepeaterMenus)
				&& menu.find("Tuner") == std::string::npos && menu.find("Calibration") == std::string::npos;
		}

		std::string Ms(float ms) { char text[16]; sprintf_s(text, "%.1f ms", ms); return text; }

		// Called with g_mutex held: a measurement ended.
		void TakeOutcome(const Result& r)
		{
			using namespace AsioProxy::LatencyMeasure;
			const bool hardware = r.kind == static_cast<int32_t>(Kind::Hardware);
			if (r.outcome == Measured && !g_setup.empty())
			{
				if (hardware)
				{
					g_figures.roundTripMs = r.roundTripMs;
					WriteUs(g_setup, L"RoundTripUs", r.roundTripMs);
					g_status = "Interface measured: " + Ms(r.roundTripMs) + " out and back in.";
				}
				else
				{
					// A path that did not answer keeps what was stored (the game's amp is silent while the Tone designer plays).
					if (r.gameMs >= 0.0f) { g_figures.gameMs = r.gameMs; WriteUs(g_setup, L"GameUs", r.gameMs); }
					if (r.ampMs >= 0.0f)
					{
						g_figures.ampMs = r.ampMs;
						WriteUs(g_setup, L"AmpUs", r.ampMs);
						// What stays when the link's blocks and the plugins' reported delay come off. Those two are added
						// back live (AmpNowMs), so the safety buffer, a new chain or a plugin changing its own delay
						// (oversampling) shows at once, without measuring again.
						g_figures.ampBaseMs = r.ampMs - g_link.blocksMs - g_link.pluginMs;
						g_figures.hasAmpBase = true;
						g_figures.ampGuest = g_link.guest;
						const std::wstring key = std::wstring(kStoreKey) + L"\\" + g_setup;
						const DWORD base = static_cast<DWORD>(static_cast<int32_t>(std::lround(g_figures.ampBaseMs * 1000.0f)));
						RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"AmpBaseUs", REG_DWORD, &base, sizeof(base));
						HKEY store = nullptr;
						if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &store, nullptr) == ERROR_SUCCESS)
						{
							RegSetValueExA(store, "AmpGuest", 0, REG_SZ, reinterpret_cast<const BYTE*>(g_link.guest.c_str()),
								static_cast<DWORD>(g_link.guest.size() + 1));
							RegCloseKey(store);
						}
						LOG_INFO("(LATENCY CALIBRATION) amp path " << r.ampMs << " ms = link blocks " << g_link.blocksMs << " + plugins' own delay "
							<< g_link.pluginMs << " + the rest " << g_figures.ampBaseMs << " (" << g_link.guest << ")" << std::endl);
					}
					if (g_gameBefore >= 0.0f && r.gameMs >= 0.0f)
						LOG_INFO("(LATENCY CALIBRATION) after-song recheck: game path " << g_gameBefore << " -> " << r.gameMs << " ms"
							<< (std::fabs(r.gameMs - g_gameBefore) > 1.0f ? " (MOVED)" : " (same)") << std::endl);
					g_gameBefore = -1.0f;
					g_status = "Measured";
					if (r.gameMs >= 0.0f) g_status += ": game tones " + Ms(r.gameMs) + " in Rocksmith";
					if (r.ampMs >= 0.0f) g_status += std::string(r.gameMs >= 0.0f ? ", " : ": ") + "Tone designer " + Ms(r.ampMs) + " in the PC";
					g_status += ".";
				}
				LOG_INFO("(LATENCY CALIBRATION) " << g_status << std::endl);
				return;
			}
			switch (r.outcome)
			{
			case NotQuiet: g_status = hardware ? "Not measured: the input was not quiet." : "Not measured: the guitar was playing. It tries again in the menus."; break;
			case NoSignal: g_status = "Not measured: nothing reached the input, turn the output up. Headphone volume about halfway, the input's gain as for the guitar, and the loop cable from the headphone jack into the guitar input."; break;
			case NoAnswer: g_status = hardware ? "Not measured: something came back, but not the test tones. Is the direct monitor fully on playback (USB), and the loop cable from the headphone jack into the guitar input?" : "Not measured: no tone answered. Is the guitar's volume up in Rocksmith's mixer?"; break;
			case Disagreed: g_status = "Not measured: the answers disagreed. Try again with the music paused."; break;
			case Stopped: g_status = "Not measured: the audio stream stopped."; break;
			default: g_status = "Not measured: the analysis failed."; break;
			}
			LOG_WARNING("(LATENCY CALIBRATION) " << g_status << std::endl);
		}
	}

	void Tick()
	{
		Result r{};
		const bool have = ReadResult(r);
		const Link link = ReadLink();
		const bool inSong = GameState::GameLoaded && AudioBridge::GameView::IsInSong();   // External amp worker: MainThread's copy
		std::lock_guard<std::mutex> lock(g_mutex);
		g_link = link;
		// A song ended: the game path is checked once more in the menus. Nothing is timed during play, so this is how
		// a shift after a dropout (should the game's buffering ever resync at another offset) gets caught; the log
		// line from TakeOutcome says whether it moved.
		if (g_wasInSong && !inSong && GetTickCount64() - g_lastRecheck >= kRecheckMs) g_recheckDue = true;
		g_wasInSong = inSong;
		g_haveResult = have;
		if (!have) { g_setup.clear(); g_figures = {}; return; }
		g_last = r;
		const std::wstring setup = SetupOf(r);
		const ULONGLONG now = GetTickCount64();
		if (setup != g_setup)
		{
			g_setup = setup;
			g_figures = Load(setup);
			g_setupSince = now;
			g_nextAuto = 0;
			g_autoTries = 0;
		}
		if (g_seenSequence < 0) g_seenSequence = r.sequence;   // a result from before this launch's first look is old
		if (r.sequence != g_seenSequence)
		{
			g_seenSequence = r.sequence;
			TakeOutcome(r);
			if (g_autoRun && r.outcome == AsioProxy::LatencyMeasure::Measured) { g_autoTries = kAutoTries; g_recheckDue = false; }   // done for this reason
			else if (g_autoRun) g_nextAuto = now + kRetryMs;
			g_autoRun = false;
		}
		// Automatically, in the menus, after the stream has settled: the game measurement (it times the amp link too)
		// when this setup has no game figure, when an amp sim or the Tone designer plays with no figure of its own
		// (a new guest), or once after a song.
		if (setup.empty() || r.state != static_cast<int32_t>(State::Idle)) return;
		const bool ampWanted = g_link.playing && !g_link.guest.empty() && AmpNowMs() < 0.0f;
		const char* reason = g_figures.gameMs < 0.0f ? "new setup" : ampWanted ? "new amp" : g_recheckDue ? "after a song" : nullptr;
		if (!reason) return;
		const std::string want = std::string(reason) + "|" + g_link.guest;
		if (want != g_wantKey) { g_wantKey = want; g_autoTries = 0; g_nextAuto = 0; }
		if (now - g_setupSince < kSettleMs || now < g_nextAuto || g_autoTries >= kAutoTries || !QuietMoment()) return;
		++g_autoTries;
		g_nextAuto = now + kRetryMs;
		if (StartKind(static_cast<int>(AsioProxy::LatencyMeasure::Kind::Game)))
		{
			g_autoRun = true;
			if (g_recheckDue) { g_lastRecheck = now; g_gameBefore = g_figures.gameMs; }
			g_status = "Measuring the game's latency (a second of silence)...";
			LOG_INFO("(LATENCY CALIBRATION) measuring automatically: " << reason << " (try " << g_autoTries << ")" << std::endl);
		}
	}

	Figures Current()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_figures;
	}

	AsioSplit SplitAsio(float driverInMs, float driverOutMs, float linkMs, float gameEstimateMs, bool gameEstimateGuessed)
	{
		Figures f;
		float ampNow = -1.0f, pluginMs = 0.0f;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			f = g_figures;
			ampNow = AmpNowMs();
			pluginMs = g_link.pluginMs;
		}
		AsioSplit split;
		const bool hardwareMeasured = f.roundTripMs >= 0.0f;
		if (hardwareMeasured)
		{
			const float driver = driverInMs + driverOutMs;
			split.inMs = driver > 0.0f ? f.roundTripMs * driverInMs / driver : f.roundTripMs * 0.5f;
			split.outMs = f.roundTripMs - split.inMs;
		}
		else { split.inMs = driverInMs; split.outMs = driverOutMs; }
		const Quality hardware = hardwareMeasured ? Quality::Measured : Quality::Estimate;
		const auto worse = [](Quality a, Quality b) { return static_cast<int>(a) > static_cast<int>(b) ? a : b; };
		// Not measured with this guest: the link's blocks plus whatever its plugins report.
		split.ampTotalMs = split.inMs + split.outMs + (ampNow >= 0.0f ? ampNow : linkMs + pluginMs);
		split.amp = worse(hardware, ampNow >= 0.0f ? Quality::Measured : Quality::Estimate);
		split.gameTotalMs = split.inMs + split.outMs + (f.gameMs >= 0.0f ? f.gameMs : gameEstimateMs);
		split.game = worse(hardware, f.gameMs >= 0.0f ? Quality::Measured : gameEstimateGuessed ? Quality::Guess : Quality::Estimate);
		return split;
	}

	const char* Label(Quality quality) { return quality == Quality::Measured ? "measured" : quality == Quality::Estimate ? "est." : "guess"; }

	bool Available()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_haveResult && !g_setup.empty();
	}

	bool Running()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const State state = static_cast<State>(g_last.state);
		return g_haveResult && state != State::Idle && state != State::HoldMuted;
	}

	bool HoldingMuted()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_haveResult && static_cast<State>(g_last.state) == State::HoldMuted;
	}

	bool Recalibrate(std::string& why)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_haveResult || g_setup.empty()) { why = "Measuring needs an ASIO interface playing."; return false; }
		if (static_cast<State>(g_last.state) != State::Idle) { why = "A measurement is already running."; return false; }
		if (!StartKind(static_cast<int>(AsioProxy::LatencyMeasure::Kind::Game))) { why = "Could not start: is the audio stream running?"; return false; }
		g_last.state = static_cast<int32_t>(State::WaitingQuiet);
		g_autoRun = false;
		g_status = "Measuring... stop playing for a moment (a second of silence).";
		return true;
	}

	bool MeasureHardware(std::string& why)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_haveResult || g_setup.empty()) { why = "Measuring needs an ASIO interface playing."; return false; }
		if (static_cast<State>(g_last.state) != State::Idle) { why = "A measurement is already running."; return false; }
		if (!StartKind(static_cast<int>(AsioProxy::LatencyMeasure::Kind::Hardware))) { why = "Could not start: is the audio stream running?"; return false; }
		g_last.state = static_cast<int32_t>(State::WaitingQuiet);
		g_autoRun = false;
		g_status = "Measuring the interface through the loop cable...";
		return true;
	}

	void CableOut()
	{
		if (auto release = Export<void(__cdecl*)()>("RSModsAsio_ReleaseLatencyMeasure")) release();
		std::lock_guard<std::mutex> lock(g_mutex);
		if (static_cast<State>(g_last.state) == State::HoldMuted) g_last.state = static_cast<int32_t>(State::Idle);
		if (g_status.find("Unplug") != std::string::npos) g_status = g_status.substr(0, g_status.find(" Unplug"));
	}

	void ForgetHardware()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_setup.empty()) return;
		g_figures.roundTripMs = -1.0f;
		WriteUs(g_setup, L"RoundTripUs", -1.0f);
		g_status = "Back on the driver's own figures for the interface.";
	}

	std::string Status()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_status.empty()) return g_status;
		if (g_setup.empty()) return "Measuring works with an ASIO interface.";
		std::string text = g_figures.gameMs >= 0.0f ? "Game tones measured: " + Ms(g_figures.gameMs) + " in Rocksmith." : "Game tones not measured yet on this setup.";
		text += g_figures.roundTripMs >= 0.0f ? " Interface measured: " + Ms(g_figures.roundTripMs) + "." : " Interface: the driver's own figures.";
		return text;
	}
}
