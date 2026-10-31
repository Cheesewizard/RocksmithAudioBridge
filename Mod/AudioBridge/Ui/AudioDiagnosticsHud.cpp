#include "AudioDiagnosticsHud.hpp"

#include "../Cable/CableInput.hpp"
#include "../Engine/AudioBridgeEngine.hpp"
#include "../Engine/GameToneLatency.hpp"
#include "../Engine/LatencyCalibration.hpp"
#include "../../ExternalAmp/ExternalAmp.hpp"
#include "../../Shared/AmpLink.hpp"
#include "../../Shell/HudDraw.hpp"

#include "GameState.hpp"
#include "Log.hpp"
#include "MemUtil.hpp"
#include "Offsets.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace AudioBridge::DiagnosticsHud
{
	namespace
	{
		std::atomic<bool> multiplayer{ false };   // PublishGameState, MainThread

		std::atomic_bool& LatencyReadoutSetting()
		{
			static std::atomic_bool enabled{ []()
			{
				DWORD value = 0;
				DWORD size = sizeof(value);
				const LSTATUS result = RegGetValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\Overlay", L"LatencyReadout",
					RRF_RT_REG_DWORD, nullptr, &value, &size);
				if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND && result != ERROR_PATH_NOT_FOUND)
				{
					LOG_WARNING("(OVERLAY) Could not read latency readout setting, error " << result << std::endl);
				}
				return result == ERROR_SUCCESS && value != 0;
			}() };
			return enabled;
		}

		// One row: text runs and, in a signal row, the 12-cell bar between them. 4.x drew the bar with U+2588 / U+2591
		// block characters, which the baked HUD font doesn't carry, so the cells are drawn as rectangles.
		struct Row
		{
			std::string before;    // "SIGNAL  ", or the whole row when there is no bar
			int bars = -1;         // lit cells (0..12); -1 = no bar
			std::string after;     // "  -12 dB   1000 pkt/s"
		};

		// One formatter for both players: the bar, the dB figure and the packet rate read the same
		// way on each row, so the eye compares them directly.
		Row FormatLive(const char* label, float meterPeak, double packetsPerSecond, uint64_t dropouts)
		{
			const double db = meterPeak > 0.0f ? 20.0 * std::log10(meterPeak) : -90.0;
			Row row;
			row.before = std::string(label) + "  ";
			row.bars = std::clamp(static_cast<int>((db + 60.0) / 60.0 * 12.0 + 0.5), 0, 12);
			std::ostringstream after;
			after << "  " << std::fixed << std::setprecision(0) << (std::max)(db, -90.0) << " dB   "
				<< packetsPerSecond << " pkt/s";
			if (dropouts > 0) after << "   dropouts " << dropouts;
			row.after = after.str();
			return row;
		}

		Row Plain(std::string text)
		{
			Row row;
			row.before = std::move(text);
			return row;
		}

		// Right-aligned at `right`, a 2 px black shadow then white, bold (4.x's FW_BOLD).
		void DrawRow(ImDrawList* list, const Row& row, float right, float top, float fontSize)
		{
			const float cellWidth = HudDraw::Measure(fontSize, "0").x;
			const float barWidth = row.bars >= 0 ? cellWidth * 12.0f : 0.0f;
			const float beforeWidth = HudDraw::Measure(fontSize, row.before.c_str()).x;
			const float afterWidth = row.after.empty() ? 0.0f : HudDraw::Measure(fontSize, row.after.c_str()).x;
			const float left = right - beforeWidth - barWidth - afterWidth;
			for (const int pass : { 0, 1 })
			{
				const float offset = pass == 0 ? 2.0f : 0.0f;
				const ImU32 color = pass == 0 ? HudDraw::Argb(0xFF000000) : HudDraw::Argb(0xFFFFFFFF);
				float x = left + offset;
				const float y = top + offset;
				HudDraw::Text(list, fontSize, ImVec2(x, y), color, row.before.c_str(), true);
				x += beforeWidth;
				if (row.bars >= 0)
				{
					// A full block for a lit cell, a light shade (a faint fill) for an unlit one.
					const float cellTop = y + fontSize * 0.12f;
					const float cellBottom = y + fontSize * 0.92f;
					for (int cell = 0; cell < 12; ++cell)
					{
						const ImVec2 a(x + cell * cellWidth, cellTop);
						const ImVec2 b(x + (cell + 1) * cellWidth - 1.0f, cellBottom);
						if (cell < row.bars) list->AddRectFilled(a, b, color);
						else if (pass == 1) list->AddRectFilled(a, b, IM_COL32(255, 255, 255, 70));
					}
					x += barWidth;
				}
				if (!row.after.empty()) HudDraw::Text(list, fontSize, ImVec2(x, y), color, row.after.c_str(), true);
			}
		}
	}

	// Single source of truth for "the audio diagnostics rows are on screen"; keep every visibility condition here.
	// 4.x also gated it on its OverlayToggles "audio_diag", a research switch that shipped on and that only its research
	// bridge flipped; it is always on here.
	bool IsVisible()
	{
		if (!Audio::CableInput::IsOverlayEnabled()) return false;
		const Audio::CableInput::Diagnostics d = Audio::CableInput::GetDiagnostics();
		return d.installed || d.rsAsio;
	}

	void PublishGameState()
	{
		// Upstream's GameState::IsMultiplayer dereferences the end of its pointer chain unchecked, which faults while the
		// chain isn't there yet. Here the walk is the guarded one and a missing chain means single player.
		const uintptr_t address = MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_multiplayer, Offsets::ptr_multiplayerOffsets, true);
		multiplayer.store(address != 0 && *reinterpret_cast<const int*>(address) != 0, std::memory_order_release);
	}

	void Draw()
	{
		// 4.x drew the HUD only once the game had loaded.
		if (!GameState::GameLoaded || !IsVisible()) return;
		const Audio::CableInput::Diagnostics d = Audio::CableInput::GetDiagnostics();

		Row signal;
		if (d.proxyInputMode == 1)
		{
			signal = Plain("NO INPUT  connect a guitar input");
		}
		else if (!d.streamActive)
		{
			signal = Plain("SIGNAL  waiting for input");
		}
		else if (d.stalled)
		{
			signal = Plain("NO SIGNAL  the input stream has stalled");
		}
		else
		{
			signal = FormatLive("SIGNAL", d.meterPeak, d.packetsPerSecond, d.dropouts);
		}

		// Player 2's row. Drawn whenever the game is in 2-player, directly under Player 1's
		// row, mirroring the Drop Pedal overlay's second Pitch row on the left. Its states name what
		// Player 2 actually has, so a silent second guitar is diagnosable from the screen alone.
		Row playerTwo;
		const bool showPlayerTwo = multiplayer.load(std::memory_order_acquire);
		if (showPlayerTwo)
		{
			if (!d.playerTwoInput)
			{
				playerTwo = Plain("P2 NO INPUT  no second input is configured");
			}
			else if (d.playerTwoCableFeedOff)
			{
				playerTwo = Plain("P2 CABLE OFF  enable the Real Tone Cable for Player 2");
			}
			else if (!d.playerTwoStreamActive)
			{
				playerTwo = Plain("P2 SIGNAL  waiting for input");
			}
			else if (d.playerTwoStalled)
			{
				playerTwo = Plain("P2 NO SIGNAL  the input stream has stalled");
			}
			else
			{
				playerTwo = FormatLive("P2 SIGNAL", d.playerTwoMeterPeak, d.playerTwoPacketsPerSecond, 0);
			}
		}

		std::ostringstream latencyEstimate;
		bool showLatencyReadout = false;
		if (IsLatencyReadoutEnabled())
		{
			const ExternalAmp::Snapshot amp = ExternalAmp::GetSnapshot();
			const bool ampPlaying = amp.cableHost && amp.enabled && !amp.gameAmp && amp.link.linkOpen && amp.link.connected;
			if (ampPlaying && amp.cableLatencyMs > 0.0f)
			{
				// The DAW convention: input (the cable read), output (Tone designer to speaker), total. Rounded once and
				// subtracted so the parts add up to the total.
				const long total = std::lround(amp.cableLatencyMs);
				const long input = (std::min)(total, std::lround(amp.cableReadMs));
				latencyEstimate << "LATENCY  IN " << input << "  OUT " << total - input << "  TOTAL ~" << total << " ms";
				showLatencyReadout = true;
			}
			else if (amp.cableHost && !ampPlaying && Audio::GameToneLatency::Available())
			{
				latencyEstimate << "GAME TONES  ~" << std::fixed << std::setprecision(0) << Audio::GameToneLatency::CurrentMs() << (Audio::GameToneLatency::Guessed() ? " ms GUESS" : " ms EST");
				showLatencyReadout = true;
			}
			else if (!amp.cableHost && amp.asioInMs + amp.asioOutMs > 0.0f)
			{
				// ASIO mode (LatencyCalibration::SplitAsio, the same figures as the overlay's header): the interface's round
				// trip, measured with a patch cable or else the driver's figures, plus the path being heard: the amp link
				// while an amp sim or Tone designer plays, else the game's amp; each measured on this setup, else estimated.
				const bool linkPlaying = amp.link.connected && !amp.gameAmp;
				const int rate = amp.link.sampleRate > 0 ? amp.link.sampleRate : AmpLink::kSampleRate;
				const float linkMs = linkPlaying && amp.link.frames > 0 ? static_cast<float>(amp.link.frames * (amp.link.latencyBlocks > 1 ? 2 : 1) * 1000.0 / rate) : 0.0f;
				const float blockMs = (amp.link.frames > 0 ? amp.link.frames : 128) * 1000.0f / rate;
				const bool picks = Audio::GameToneLatency::Valid();   // timed from picks in the last minute (ASIO feed)
				const float gameOwn = picks ? Audio::GameToneLatency::MedianMs() : Audio::GameToneLatency::GameOwnMs(blockMs);
				const auto split = Audio::LatencyCalibration::SplitAsio(amp.asioInMs, amp.asioOutMs, linkMs, gameOwn, !picks && Audio::GameToneLatency::GameOwnGuessed());
				const auto upper = [](Audio::LatencyCalibration::Quality q) { return q == Audio::LatencyCalibration::Quality::Measured ? " ms MEASURED" : q == Audio::LatencyCalibration::Quality::Estimate ? " ms EST" : " ms GUESS"; };
				const long input = std::lround(split.inMs);
				if (linkPlaying) {
					const long total = std::lround(split.ampTotalMs);
					latencyEstimate << "ASIO LATENCY  IN " << input << "  OUT " << total - input << "  TOTAL ~" << total << upper(split.amp);
				}
				else latencyEstimate << "GAME TONES  ~" << std::lround(split.gameTotalMs) << upper(split.game);
				showLatencyReadout = true;
			}
		}

		// Top row (the Drop Pedal "Pitch" row; that overlay draws one Pitch row per player, so the signal
		// readout must not take a row of its own), anchored to the right edge of the screen. While a take is recording
		// the REC badge owns the corner, so the readout steps left of it. The Player 2 row uses the same row pitch as the
		// Drop Pedal overlay (height / 36) so the two players' rows line up across the screen. 4.x widened the game's
		// viewport for this draw (it clipped D3DX text); ImGui's renderer sets its own full-screen viewport.
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const int surfaceW = static_cast<int>(display.x);
		const int surfaceH = static_cast<int>(display.y);
		if (surfaceW <= 0 || surfaceH <= 0) return;
		const int margin = (std::max)(8, surfaceW / 96);
		const int fontSize = (std::max)(14, surfaceH / 62);
		const int rowPitch = surfaceH / 36;
		const int top = (std::max)(8, surfaceH / 54);
		const int recBadgeWidth = Audio::SharedOutput::IsRecording() ? (std::max)(18, surfaceH / 44) * 5 : 0;
		const int right = surfaceW - margin - recBadgeWidth;
		ImDrawList* list = ImGui::GetBackgroundDrawList();
		auto drawRow = [&](const Row& row, int index)
		{
			DrawRow(list, row, static_cast<float>(right), static_cast<float>(top + index * rowPitch), static_cast<float>(fontSize));
		};
		drawRow(signal, 0);
		if (showPlayerTwo) drawRow(playerTwo, 1);
		if (showLatencyReadout)
		{
			const int firstLatencyRow = showPlayerTwo ? 2 : 1;
			drawRow(Plain(latencyEstimate.str()), firstLatencyRow);
		}
	}

	bool IsLatencyReadoutEnabled()
	{
		return LatencyReadoutSetting().load(std::memory_order_relaxed);
	}

	bool SetLatencyReadoutEnabled(bool enabled)
	{
		HKEY key = nullptr;
		LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\Overlay", 0, nullptr, 0,
			KEY_SET_VALUE, nullptr, &key, nullptr);
		if (result == ERROR_SUCCESS)
		{
			const DWORD value = enabled ? 1 : 0;
			result = RegSetValueExW(key, L"LatencyReadout", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
			RegCloseKey(key);
		}
		if (result != ERROR_SUCCESS)
		{
			LOG_WARNING("(OVERLAY) Could not save latency readout setting, error " << result << std::endl);
			return false;
		}
		LatencyReadoutSetting().store(enabled, std::memory_order_relaxed);
		return true;
	}
}
