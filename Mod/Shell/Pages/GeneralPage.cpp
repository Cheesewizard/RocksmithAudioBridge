#include "GeneralPage.hpp"

#include "PageCommon.hpp"
#include "../ShellPages.hpp"
#include "../../AudioBridge/Engine/LatencyCalibration.hpp"
#include "../../AudioBridge/Ui/AudioDiagnosticsHud.hpp"
#include "../../ExternalAmp/ExternalAmp.hpp"

using namespace Overlay;

namespace AudioBridge::Pages
{
	namespace
	{
		// The overlay key is pressed in menus with the song-list search open, so a letter or digit would type into it
		// (and could not be typed into the overlay's own text fields either).
		const char* OverlayKeyReason(unsigned int vk)
		{
			if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
				return "Letters and digits type into the song search, so the overlay key can't be one. Pick a symbol or function key.";
			return nullptr;
		}

		void Draw()
		{
			PageHeader("General", "Overlays and library tools.");

			// Here rather than under Amp sim: it silences the game's amp, Tone designer and an external app alike.
			BeginCard("Tuner");
			bool silenceTuning = ExternalAmp::GetSnapshot().silenceWhileTuning;
			if (ToggleRow("Silence guitar while tuning", "No guitar sound in the tuner screens, whichever amp plays it. The tuner still hears you.", &silenceTuning))
				ExternalAmp::SetSilenceWhileTuning(silenceTuning);
			EndCard();

			BeginCard("Overlay key");
			KeyBindPicker("OverlayKey", "Opens and closes this overlay", OverlayKeyReason);
			Caption("Esc always cancels a key change, so the overlay can't be left without a key.");
			EndCard();

			// The HUD AudioBridgeRoutingMod draws (Ui/AudioDiagnosticsHud). The diagnostics switch is op 27, which saves
			// RSMods.ini [Mod Settings] AudioDiagnosticsOverlay; the latency readout's is HKCU, as in 4.x.
			BeginCard("Display");
			bool diagnostics = Audio::CableInput::IsOverlayEnabled();
			if (ToggleRow("Audio diagnostics HUD", "Signal level, packet rate and route state in the top right corner.", &diagnostics))
				BridgeControl(27, diagnostics ? L"1" : L"0");
			bool latencyReadout = DiagnosticsHud::IsLatencyReadoutEnabled();
			if (ToggleRow("Latency readout", "Shows a compact estimate below SIGNAL.", &latencyReadout,
				"Requires Audio diagnostics HUD. Software timing plus reported output delay; cable hardware and USB delivery are excluded. Hidden when no estimate is available."))
				DiagnosticsHud::SetLatencyReadoutEnabled(latencyReadout);
			EndCard();

			// ASIO mode's measured latency (LatencyCalibration.hpp). The game measurement runs by itself once per setup;
			// Recalibrate runs it again. The interface's own delay needs a cable looped from its headphone jack into its
			// guitar input, and the output stays muted afterwards until the player says the cable is out (a guitar input
			// patched to the output would feed back through the game's amp).
			BeginCard("Latency");
			{
				static std::string refused;
				static bool cableSteps = false;
				const bool available = Audio::LatencyCalibration::Available();
				const bool running = Audio::LatencyCalibration::Running();
				const bool holding = Audio::LatencyCalibration::HoldingMuted();
				if (!available && !holding) Caption("Measuring works with an ASIO interface. In Cable mode the game tones figure is estimated from your playing.");
				else
				{
					Caption(Audio::LatencyCalibration::Status().c_str());
					if (holding)
					{
						Caption("Rocksmith is muted. Take the cable out of the headphone jack, plug your guitar back in, then press Cable out.");
						if (Button("Cable out", true))
						{
							Audio::LatencyCalibration::CableOut();
							cableSteps = false;
						}
					}
					else
					{
						ImGui::BeginDisabled(running);
						if (Button(running ? "Measuring..." : "Recalibrate", false,
							"Measures again what the game's amp adds (and the Tone designer, if it is playing) by sending three short chirps through it in place of your guitar. About a second of silence; it waits until you stop playing. It also runs by itself once for each interface, buffer size and sample rate, in the menus."))
						{
							refused.clear();
							Audio::LatencyCalibration::Recalibrate(refused);
						}
						ImGui::SameLine();
						if (Button("Measure the interface...", false, "Times your interface's own input and output delay by sending a sound out of the headphone jack and back into the guitar input through a cable, instead of trusting what its driver reports. Once per interface and buffer size."))
							cableSteps = !cableSteps;
						ImGui::EndDisabled();
						if (cableSteps && !running)
						{
							Caption("This needs no guitar: the interface plays a test sound into a cable that loops straight back into it.");
							Caption("1. Unplug the guitar from its cable. Plug that free end into the interface's headphone jack (or line out). The cable now runs from the headphone jack into the guitar input: a closed loop.");
							Caption("2. Headphone volume about halfway. Direct monitor fully to playback (USB), or the loop will feed back.");
							Caption("3. Press Start. Rocksmith stays muted until you press Cable out.");
							if (Button("Start", true))
							{
								refused.clear();
								Audio::LatencyCalibration::MeasureHardware(refused);
							}
							if (Audio::LatencyCalibration::Current().roundTripMs >= 0.0f)
							{
								ImGui::SameLine();
								if (Button("Use the driver's figures", false, "Forget the cable measurement for this interface and show the delays its driver reports."))
									Audio::LatencyCalibration::ForgetHardware();
							}
						}
					}
				}
				if (!refused.empty()) Caption(refused.c_str());
			}
			EndCard();

			BeginCard("Library");
			Caption("Picks up newly added CDLC without restarting. Open the song list once first if nothing changes.");
			if (Button("Update song list")) BridgeControl(28);
			EndCard();
		}
	}

	void RegisterGeneralPage()
	{
		SnapshotKeyBinds();
		Shell::RegisterPage({ "general", "System", Glyph::Settings, "General", 90, Draw, {} });
	}
}
