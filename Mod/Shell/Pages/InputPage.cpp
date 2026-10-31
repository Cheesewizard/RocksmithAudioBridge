#include "InputPage.hpp"

#include "PageCommon.hpp"
#include "Routing.hpp"
#include "../ShellPages.hpp"
#include "../../AudioBridge/Cable/CableCaptureSession.hpp"
#include "../../AudioBridge/Engine/OutputTap.hpp"
#include "../../ExternalAmp/CableAmpLink.hpp"
#include "../../ExternalAmp/ExternalAmp.hpp"
#include "../../Input/CaptureTap.hpp"
#include "../../Input/InputConditioner.hpp"
#include "../../Input/ToneGates.hpp"

#include "Settings.hpp"

#include <algorithm>
#include <cmath>

using namespace Overlay;
using namespace AudioBridge::Pages::Routing;

namespace AudioBridge::Pages
{
	namespace
	{
		// The settings these controls save, as their owners declare them (InputConditionerMod, AudioBridgeRoutingMod).
		namespace Keys
		{
			constexpr char INPUT_GAIN[] = "AudioBridgeInputGain";
			constexpr char COMPRESSOR_STRENGTH[] = "AudioBridgeCompressorStrength";
			constexpr char GATE_THRESHOLD[] = "AudioBridgeNoiseGateThreshold";
			constexpr char GATE_LAST_THRESHOLD[] = "AudioBridgeNoiseGateLastThreshold";
			constexpr char GATE_DECAY[] = "AudioBridgeNoiseGateDecay";
			constexpr char GATE_REDUCTION[] = "AudioBridgeNoiseGateReduction";
			constexpr char GATE_ATTACK[] = "AudioBridgeNoiseGateAttack";
			constexpr char ROCKSMITH_GATE_OVERRIDE[] = "AudioBridgeRocksmithGateOverride";
			constexpr char ROCKSMITH_GATE_THRESHOLD[] = "AudioBridgeRocksmithGateThreshold";
		}

		// The tap's input index for Player 1.
		constexpr std::uint32_t PLAYER_ONE_INPUT = 0;

		// Player 1's gate: the detector level (dBFS) and whether it is open. False when it has not run in the last 500 ms.
		bool GateMeter(float& detectorDb, bool& open)
		{
			Input::NoiseGateMeter meter;
			if (!Input::SharedConditioner().TryGetNoiseGateMeter(meter)) return false;
			detectorDb = meter.detectorDb;
			open = meter.open;
			return true;
		}

		// ---- Noise gate pedal -------------------------------------------------------------------------
		// Player 1's noise gate (NoiseSuppressor.hpp): a row on the Input page, and its dials on a pop-out stomp box that
		// stays over the game while the overlay is open. Threshold goes over op 19 (signed tenths of a dB, 0 = off);
		// decay, reduction and attack over op 31 ("<ms>,<dB>,<tenths of a ms>"). Drags apply live (Preview*) and save
		// once on release. The settings app has the same pedal (GUI/Audio/NoiseGatePedalForm.cs).
		// A Tone designer tone's own gate is saved by the op itself (ToneGates.ini); the global gate is
		// InputConditionerMod's setting, so it goes through upstream's setting edits.
		struct NoiseGatePedalState
		{
			bool seeded = false;
			bool on = false;
			bool windowOpen = false;
			int thresholdTenths = -620;   // kept while off (NoiseGateLastThreshold) so the footswitch returns to it
			int decayMs = 200;
			int reductionDb = -60;
			int attackTenths = 5;

			std::string tone;             // the Tone designer tone being edited; empty = all tones (the global gate)
			bool own = false;             // that tone has its own gate
			std::uint32_t revision = 0;

			// Follows ToneGates every frame: the playing tone's own gate, else the global one. Edits from here or the
			// settings app bump the revision, so both pedals stay on the same values.
			void Seed()
			{
				const ToneGates::View view = ToneGates::Current();
				if (seeded && view.revision == revision) return;
				seeded = true;
				revision = view.revision;
				tone = view.tone;
				own = view.own;
				on = view.gate.on;
				thresholdTenths = view.gate.thresholdTenths;
				decayMs = view.gate.decayMs;
				reductionDb = view.gate.reductionDb;
				attackTenths = view.gate.attackTenths;
			}

			// "Noise gate · Clean Strat" or "Noise gate · All tones", cut to fit the pedal's title bar.
			std::string Title() const
			{
				std::string name = tone.empty() ? std::string("All tones") : tone;
				if (name.size() > 30) name = name.substr(0, 28) + "...";
				return std::string("Noise gate \xC2\xB7 ") + name;
			}

			void SendThreshold(bool save)
			{
				const int value = on ? thresholdTenths : 0;
				if (!save)
				{
					if (on) PreviewInt(19, thresholdTenths);
					return;
				}
				if (!tone.empty())
				{
					SendInt(19, value);
					return;
				}
				PreviewInt(19, value);
				SaveModInt(Keys::GATE_THRESHOLD, "NoiseGateThreshold", value, "Noise gate threshold");
				// Remembered while the pedal is off, so switching it back on returns to the same threshold.
				if (on) SaveModInt(Keys::GATE_LAST_THRESHOLD, "NoiseGateLastThreshold", thresholdTenths, "Noise gate threshold to switch back on at");
			}

			void SendShape(bool save)
			{
				wchar_t value[48];
				swprintf_s(value, L"%d,%d,%d", decayMs, reductionDb, attackTenths);
				if (save && !tone.empty())
				{
					BridgeControl(31, value);
					return;
				}
				PreviewControl(31, value);
				if (!save) return;
				SaveModInt(Keys::GATE_DECAY, "NoiseGateDecay", decayMs, "Noise gate decay");
				SaveModInt(Keys::GATE_REDUCTION, "NoiseGateReduction", reductionDb, "Noise gate reduction");
				SaveModInt(Keys::GATE_ATTACK, "NoiseGateAttack", attackTenths, "Noise gate attack");
			}

			void FormatDecay(char* out, size_t size) const
			{
				if (decayMs >= 1000) snprintf(out, size, "%.1f s", decayMs / 1000.0f);
				else snprintf(out, size, "%d ms", decayMs);
			}
		};
		NoiseGatePedalState gatePedal;

		namespace PedalInk
		{
			constexpr ImU32 Body = IM_COL32(31, 91, 84, 255);
			constexpr ImU32 Edge = IM_COL32(45, 122, 112, 255);
			constexpr ImU32 Lettering = IM_COL32(234, 247, 243, 255);
			constexpr ImU32 Label = IM_COL32(215, 239, 233, 255);
			constexpr ImU32 Value = IM_COL32(168, 216, 206, 255);
			constexpr ImU32 KnobSkirt = IM_COL32(20, 64, 58, 255);
			constexpr ImU32 KnobSkirtEdge = IM_COL32(13, 42, 38, 255);
			constexpr ImU32 KnobCap = IM_COL32(27, 31, 30, 255);
			constexpr ImU32 KnobCapEdge = IM_COL32(58, 68, 66, 255);
			constexpr ImU32 Pointer = IM_COL32(242, 245, 244, 255);
			constexpr ImU32 MeterTrack = IM_COL32(18, 58, 53, 255);
			constexpr ImU32 MeterFill = IM_COL32(93, 202, 165, 255);
			constexpr ImU32 ThresholdLine = IM_COL32(250, 199, 117, 255);
			constexpr ImU32 LedOn = IM_COL32(151, 196, 89, 255);
			constexpr ImU32 LedOff = IM_COL32(51, 71, 63, 255);
			constexpr ImU32 Switch = IM_COL32(201, 211, 209, 255);
			constexpr ImU32 SwitchOff = IM_COL32(140, 153, 150, 255);
			constexpr ImU32 SwitchText = IM_COL32(61, 74, 71, 255);
		}

		// One dial: drag up or down (150 px = the full sweep), mouse wheel steps, double-click resets. `t` is the 0..1
		// position. Returns true on any frame it moves; `commit` is set on the frame to save (drag released, a wheel
		// step or a reset).
		bool PedalKnob(const char* id, const char* label, const char* valueText, float& t, float defaultT, float step,
			float cellWidth, bool& commit, const char* hint)
		{
			ImGui::PushID(id);
			ImGuiIO& io = ImGui::GetIO();
			const float radius = 27.0f;
			const ImVec2 start = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("##dial", ImVec2(cellWidth, radius * 2.0f + 40.0f));
			const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
			Hint(hint);
			bool changed = false;
			commit = false;
			if (active && io.MouseDelta.y != 0.0f) { t -= io.MouseDelta.y / 150.0f; changed = true; }
			if (ImGui::IsItemDeactivated()) commit = true;
			if (hovered && io.MouseWheel != 0.0f) { t += io.MouseWheel * step; changed = commit = true; }
			if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { t = defaultT; changed = commit = true; }
			t = std::clamp(t, 0.0f, 1.0f);

			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 center(start.x + cellWidth * 0.5f, start.y + radius + 2.0f);
			list->AddCircleFilled(center, radius, PedalInk::KnobSkirt, 32);
			list->AddCircle(center, radius, PedalInk::KnobSkirtEdge, 32, 2.0f);
			// Sweep marks at the two ends of the 270 degree travel.
			for (const float end : { 0.0f, 1.0f })
			{
				const float a = (-135.0f + 270.0f * end) * 3.14159265f / 180.0f;
				list->AddLine(ImVec2(center.x + std::sin(a) * (radius - 4.0f), center.y - std::cos(a) * (radius - 4.0f)),
					ImVec2(center.x + std::sin(a) * (radius - 1.0f), center.y - std::cos(a) * (radius - 1.0f)), PedalInk::Value, 1.5f);
			}
			const float capRadius = radius - 6.0f;
			list->AddCircleFilled(center, capRadius, hovered || active ? Mix(PedalInk::KnobCap, IM_COL32(255, 255, 255, 255), 0.06f) : PedalInk::KnobCap, 32);
			list->AddCircle(center, capRadius, PedalInk::KnobCapEdge, 32, 2.0f);
			const float angle = (-135.0f + 270.0f * t) * 3.14159265f / 180.0f;
			list->AddLine(center, ImVec2(center.x + std::sin(angle) * (capRadius - 3.0f), center.y - std::cos(angle) * (capRadius - 3.0f)),
				PedalInk::Pointer, 3.0f);

			ImFont* caption = GetFonts().caption ? GetFonts().caption : ImGui::GetFont();
			const ImVec2 labelSize = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, label);
			list->AddText(caption, caption->FontSize, ImVec2(center.x - labelSize.x * 0.5f, center.y + radius + 5.0f), PedalInk::Label, label);
			const ImVec2 valueSize = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, valueText);
			list->AddText(caption, caption->FontSize, ImVec2(center.x - valueSize.x * 0.5f, center.y + radius + 5.0f + labelSize.y),
				PedalInk::Value, valueText);
			ImGui::PopID();
			return changed;
		}

		// The pop-out stomp box. The shell draws it after its own window (Page::floating), so it floats over it and the
		// game.
		void NoiseGatePedalWindow()
		{
			NoiseGatePedalState& gate = gatePedal;
			if (!gate.windowOpen) return;
			gate.Seed();
			const float width = 340.0f, height = 494.0f;
			ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
			ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - width - 40.0f, 80.0f), ImGuiCond_FirstUseEver);
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(Color::Window));
			ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(Color::CardEdge));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			const bool shown = ImGui::Begin("Noise gate##pedal", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::PopStyleVar(2);
			ImGui::PopStyleColor(2);
			if (!shown)
			{
				ImGui::End();
				return;
			}

			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 origin = ImGui::GetWindowPos();
			ImFont* caption = GetFonts().caption ? GetFonts().caption : ImGui::GetFont();
			ImFont* title = GetFonts().title ? GetFonts().title : ImGui::GetFont();

			// Title bar: drag anywhere on the window that is not a control (ImGui moves windows by their background).
			const std::string windowTitle = gate.Title();
			list->AddText(caption, caption->FontSize, ImVec2(origin.x + 14.0f, origin.y + 11.0f), Color::Muted, windowTitle.c_str());
			ImGui::SetCursorScreenPos(ImVec2(origin.x + width - 34.0f, origin.y + 6.0f));
			if (ImGui::InvisibleButton("##close", ImVec2(26.0f, 24.0f))) gate.windowOpen = false;
			const bool closeHovered = ImGui::IsItemHovered();
			Hint("Close the pedal. Its settings stay as they are.");
			DrawIcon(list, ImVec2(origin.x + width - 21.0f, origin.y + 18.0f), Glyph::Close, closeHovered ? Color::Text : Color::Muted);
			list->AddLine(ImVec2(origin.x, origin.y + 36.0f), ImVec2(origin.x + width, origin.y + 36.0f), Color::CardEdge);

			// Whose gate this is: the playing tone's own (with a way back to the global one), or the one all tones share.
			const char* scope = gate.tone.empty() ? "Shared by every tone without its own gate."
				: gate.own ? "Saved with this tone." : "Uses the global gate. Turn a dial to give this tone its own.";
			list->AddText(caption, caption->FontSize, ImVec2(origin.x + 14.0f, origin.y + 46.0f), Color::Muted, scope);
			if (gate.own)
			{
				const char* useGlobal = "Use the global gate";
				const float buttonW = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, useGlobal).x;
				const float scopeW = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, scope).x;
				const ImVec2 at(origin.x + 14.0f + scopeW + 10.0f, origin.y + 46.0f);
				ImGui::SetCursorScreenPos(at);
				if (ImGui::InvisibleButton("##useGlobal", ImVec2(buttonW, caption->FontSize + 2.0f))) BridgeControl(33);
				const bool linkHovered = ImGui::IsItemHovered();
				Hint("This tone stops using its own gate settings and follows the global gate again.");
				list->AddText(caption, caption->FontSize, at, linkHovered ? Color::AccentHi : Color::Accent, useGlobal);
			}

			// Enclosure
			const ImVec2 box(origin.x + 20.0f, origin.y + 74.0f), boxEnd(origin.x + width - 20.0f, origin.y + height - 16.0f);
			list->AddRectFilled(box, boxEnd, PedalInk::Body, 14.0f);
			list->AddRect(box, boxEnd, PedalInk::Edge, 14.0f, 0, 2.0f);
			list->AddText(title, title->FontSize, ImVec2(box.x + 16.0f, box.y + 12.0f), PedalInk::Lettering, "NOISE GATE");

			float meterDb = -120.0f;
			bool gateOpen = false;
			const bool running = GateMeter(meterDb, gateOpen);
			const ImVec2 led(boxEnd.x - 22.0f, box.y + 22.0f);
			list->AddCircleFilled(led, 7.0f, running && gateOpen && gate.on ? PedalInk::LedOn : PedalInk::LedOff, 20);
			list->AddCircle(led, 7.0f, PedalInk::KnobSkirtEdge, 20, 1.0f);
			ImGui::SetCursorScreenPos(ImVec2(led.x - 9.0f, led.y - 9.0f));
			ImGui::InvisibleButton("##led", ImVec2(18.0f, 18.0f));
			Hint("Lit while the gate is open and your guitar comes through.");

			// Dials, two by two
			const float cell = (boxEnd.x - box.x - 24.0f) / 2.0f;
			char text[32];
			bool commit = false;
			const float rowTop = box.y + 52.0f, rowStep = 102.0f;

			ImGui::SetCursorScreenPos(ImVec2(box.x + 12.0f, rowTop));
			float t = (gate.thresholdTenths / 10.0f + 80.0f) / 60.0f;
			snprintf(text, sizeof(text), "%.1f dB", gate.thresholdTenths / 10.0f);
			if (PedalKnob("threshold", "THRESHOLD", text, t, (-62.0f + 80.0f) / 60.0f, 0.5f / 60.0f, cell, commit,
				"Where it opens. It listens to the guitar's own range, not the hiss or hum. Lower keeps quieter note tails; if noise gets through between notes, raise it a few dB."))
			{
				gate.thresholdTenths = std::clamp(static_cast<int>(std::lround((-80.0f + 60.0f * t) * 2.0f)) * 5, -800, -200);
				gate.SendThreshold(false);
			}
			if (commit) gate.SendThreshold(true);

			ImGui::SetCursorScreenPos(ImVec2(box.x + 12.0f + cell, rowTop));
			t = std::log(gate.decayMs / 50.0f) / std::log(40.0f);
			gate.FormatDecay(text, sizeof(text));
			if (PedalKnob("decay", "DECAY", text, t, std::log(200.0f / 50.0f) / std::log(40.0f), 0.02f, cell, commit,
				"How long a note takes to fade once the gate closes."))
			{
				gate.decayMs = std::clamp(static_cast<int>(std::lround(50.0f * std::pow(40.0f, t))), 50, 2000);
				gate.SendShape(false);
			}
			if (commit) gate.SendShape(true);

			ImGui::SetCursorScreenPos(ImVec2(box.x + 12.0f, rowTop + rowStep));
			t = (gate.reductionDb + 80.0f) / 70.0f;
			snprintf(text, sizeof(text), "%d dB", gate.reductionDb);
			if (PedalKnob("reduction", "REDUCTION", text, t, 20.0f / 70.0f, 1.0f / 70.0f, cell, commit,
				"How far it turns the noise down. -60 dB is near silence; around -20 dB leaves a faint natural floor."))
			{
				gate.reductionDb = std::clamp(static_cast<int>(std::lround(-80.0f + 70.0f * t)), -80, -10);
				gate.SendShape(false);
			}
			if (commit) gate.SendShape(true);

			ImGui::SetCursorScreenPos(ImVec2(box.x + 12.0f + cell, rowTop + rowStep));
			t = std::log(gate.attackTenths / 1.0f) / std::log(200.0f);
			if (gate.attackTenths < 10) snprintf(text, sizeof(text), "%.1f ms", gate.attackTenths / 10.0f);
			else snprintf(text, sizeof(text), "%d ms", static_cast<int>(std::lround(gate.attackTenths / 10.0f)));
			if (PedalKnob("attack", "ATTACK", text, t, std::log(5.0f) / std::log(200.0f), 0.02f, cell, commit,
				"How fast it opens. Keep it short so your pick attack stays sharp."))
			{
				gate.attackTenths = std::clamp(static_cast<int>(std::lround(std::pow(200.0f, t))), 1, 200);
				gate.SendShape(false);
			}
			if (commit) gate.SendShape(true);

			// Input meter with the threshold line (-90 dB empty, 0 dB full), on the gate's own detector.
			const float meterTop = rowTop + rowStep * 2.0f + 6.0f;
			list->AddText(caption, caption->FontSize, ImVec2(box.x + 16.0f, meterTop), PedalInk::Label, "Input");
			if (running) snprintf(text, sizeof(text), "%.0f dB", meterDb);
			else strcpy_s(text, gate.on ? "No signal" : "Off");
			const float textW = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, text).x;
			list->AddText(caption, caption->FontSize, ImVec2(boxEnd.x - 16.0f - textW, meterTop), PedalInk::Label, text);
			const ImVec2 track(box.x + 16.0f, meterTop + caption->FontSize + 5.0f), trackEnd(boxEnd.x - 16.0f, track.y + 10.0f);
			list->AddRectFilled(track, trackEnd, PedalInk::MeterTrack, 5.0f);
			const float fill = running ? std::clamp((meterDb + 90.0f) / 90.0f, 0.0f, 1.0f) : 0.0f;
			if (fill > 0.0f) list->AddRectFilled(track, ImVec2(track.x + (trackEnd.x - track.x) * fill, trackEnd.y), PedalInk::MeterFill, 5.0f);
			const float lineX = track.x + (trackEnd.x - track.x) * std::clamp((gate.thresholdTenths / 10.0f + 90.0f) / 90.0f, 0.0f, 1.0f);
			list->AddRectFilled(ImVec2(lineX - 1.0f, track.y - 2.0f), ImVec2(lineX + 1.0f, trackEnd.y + 2.0f), PedalInk::ThresholdLine);
			ImGui::SetCursorScreenPos(ImVec2(track.x, trackEnd.y + 5.0f));
			ImGui::PushFont(caption);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(PedalInk::Value));
			ImGui::PushTextWrapPos(trackEnd.x);
			ImGui::TextUnformatted("Yellow line = threshold. Set it just above where the meter rests when you're not playing.");
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::PopFont();

			// Footswitch
			const ImVec2 foot(box.x + (boxEnd.x - box.x) * 0.5f, boxEnd.y - 40.0f);
			ImGui::SetCursorScreenPos(ImVec2(foot.x - 28.0f, foot.y - 28.0f));
			if (ImGui::InvisibleButton("##footswitch", ImVec2(56.0f, 56.0f)))
			{
				gate.on = !gate.on;
				gate.SendThreshold(true);
			}
			const bool footHovered = ImGui::IsItemHovered();
			Hint("Switches the gate on or off.");
			list->AddCircleFilled(foot, 27.0f, gate.on ? PedalInk::Switch : PedalInk::SwitchOff, 32);
			list->AddCircle(foot, 27.0f, footHovered ? PedalInk::Lettering : PedalInk::SwitchOff, 32, 4.0f);
			const char* footText = gate.on ? "ON" : "OFF";
			const ImVec2 footSize = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, footText);
			list->AddText(caption, caption->FontSize, ImVec2(foot.x - footSize.x * 0.5f, foot.y - footSize.y * 0.5f), PedalInk::SwitchText, footText);
			ImGui::End();
		}

		// Player 1's processing: input gain, noise gate, compressor, then the game's own amp gate. Drags apply live
		// (Preview*); a release or a switch saves the owner's setting, which applies it again from OnSettingsChanged.
		void PlayerOneProcessing()
		{
			// Seeded once from the settings the mods apply at launch, then user-driven.
			static bool seeded = false;
			static bool gainOn, compOn, rgOn;
			static int gainTenths, compPct, rgTenths;
			NoiseGatePedalState& gate = gatePedal;
			gate.Seed();
			if (!seeded)
			{
				gainTenths = Settings::GetModSetting(Keys::INPUT_GAIN);
				gainOn = gainTenths > 0;
				compPct = Settings::GetModSetting(Keys::COMPRESSOR_STRENGTH);
				compOn = compPct > 0;
				rgOn = Settings::GetModSetting(Keys::ROCKSMITH_GATE_OVERRIDE) != 0;
				rgTenths = Settings::GetModSetting(Keys::ROCKSMITH_GATE_THRESHOLD);
				if (rgTenths == 0) rgTenths = -593;
				seeded = true;
			}
			const auto saveGain = [](int tenths)
			{
				PreviewInt(15, tenths);
				SaveModInt(Keys::INPUT_GAIN, "AsioInputGain", tenths, "Guitar input gain");
			};
			const auto saveCompressor = [](int percent)
			{
				PreviewInt(20, percent);
				SaveModInt(Keys::COMPRESSOR_STRENGTH, "CompressorStrength", percent, "Guitar input compressor");
			};

			BeginCard("Processing");
			// 4.x warned here when the capture hook was not installed this launch; the framework's input chain always
			// carries the conditioner, so it always runs.
			// Input make-up gain (op 15, tenths of dB; off = 0).
			if (ToggleRow("Input gain", "Raises a quiet guitar.", &gainOn)) saveGain(gainOn ? gainTenths : 0);
			ImGui::BeginDisabled(!gainOn);
			float gainDb = gainTenths / 10.0f;
			bool gainSaved = false;
			if (SliderRow("Gain", &gainDb, 0.0f, 20.0f, "+%.1f dB", &gainSaved, "Input gain from 0 to +20 dB."))
			{
				gainTenths = static_cast<int>(gainDb * 10 + 0.5f);
				PreviewInt(15, gainTenths);
			}
			if (gainSaved) saveGain(gainTenths);
			ImGui::EndDisabled();

			// Noise gate: one row here, its dials on the pedal pop-out (NoiseGatePedalWindow). Also applied to the amp
			// sim's feed (AmpFeedConditioner.hpp).
			{
				char decay[16], summary[160];
				gate.FormatDecay(decay, sizeof(decay));
				std::string target = gate.tone.empty() ? std::string("All tones") : gate.tone;
				if (target.size() > 24) target = target.substr(0, 22) + "...";
				snprintf(summary, sizeof(summary), "%s: %.1f dB \xC2\xB7 decay %s \xC2\xB7 %d dB", target.c_str(), gate.thresholdTenths / 10.0f, decay, gate.reductionDb);
				if (ToggleRow("Noise gate", gate.on ? summary : "Silent between notes, without cutting your sustain.", &gate.on, "Switches the gate on or off. Open the pedal for its dials."))
					gate.SendThreshold(true);
				if (Button(gate.windowOpen ? "Close pedal" : "Open pedal", false, "The gate's dials: threshold, decay, reduction and attack, with a level meter."))
					gate.windowOpen = !gate.windowOpen;
				float meterDb = -120.0f;
				bool gateOpen = false;
				const bool lit = gate.on && GateMeter(meterDb, gateOpen) && gateOpen;
				ImGui::SameLine(0.0f, 14.0f);
				const ImVec2 dotAt = ImGui::GetCursorScreenPos();
				const float rowH = ImGui::GetItemRectSize().y;
				ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(dotAt.x + 5.0f, dotAt.y + rowH * 0.5f), 5.0f, lit ? PedalInk::LedOn : PedalInk::LedOff, 16);
				ImGui::Dummy(ImVec2(12.0f, rowH));
				ImGui::SameLine(0.0f, 6.0f);
				ImGui::AlignTextToFramePadding();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Muted));
				ImGui::TextUnformatted(!gate.on ? "Off" : lit ? "Open" : "Closed");
				ImGui::PopStyleColor();
			}

			// Compressor (op 20, 0..100; off = 0).
			if (ToggleRow("Compressor", "Evens out level swings during sustained notes.", &compOn)) saveCompressor(compOn ? compPct : 0);
			ImGui::BeginDisabled(!compOn);
			float comp = static_cast<float>(compPct);
			bool compSaved = false;
			if (SliderRow("Strength", &comp, 0.0f, 100.0f, "%.0f%%", &compSaved))
			{
				compPct = static_cast<int>(comp + 0.5f);
				PreviewInt(20, compPct);
			}
			if (compSaved) saveCompressor(compPct);
			ImGui::EndDisabled();
			EndCard();

			// The game's own amp gate (op 23, "<on>,<tenths>"): Player 1's noise floor. While an amp sim plays, the game's
			// amp is held at 0 (ExternalAmp gameAmpMuted), so the gate has nothing to act on.
			const bool ampSimPlays = ExternalAmp::GetSnapshot().gameAmpMuted;
			BeginCard("Rocksmith");
			if (ampSimPlays) Caption("The game's amp is muted while the amp sim plays, so its gate has nothing to do.");
			ImGui::BeginDisabled(ampSimPlays);
			const bool rgChanged = ToggleRow("Override Rocksmith's gate", "Stops the game's own amp gate cutting notes as they decay.", &rgOn);
			ImGui::BeginDisabled(!rgOn);
			float rgDb = rgTenths / 10.0f;
			bool rgSaved = false;
			// -90..0 dB: AudioBridgeRoutingMod's range for the setting (4.x's slider went -100..+10).
			const bool rgSlid = SliderRow("Gate threshold", &rgDb, -100.0f, 10.0f, "%.1f dB", &rgSaved, "Lower values keep the gate open for longer sustain.");
			if (rgSlid) rgTenths = static_cast<int>(rgDb * 10 + (rgDb < 0 ? -0.5f : 0.5f));
			ImGui::EndDisabled();
			if (rgChanged || rgSlid || rgSaved)
			{
				wchar_t value[24];
				swprintf_s(value, L"%d,%d", rgOn ? 1 : 0, rgTenths);
				PreviewControl(23, value);
				if (rgChanged || rgSaved)
				{
					SaveModInt(Keys::ROCKSMITH_GATE_OVERRIDE, "RocksmithGateOverride", rgOn ? 1 : 0, "Override Rocksmith's noise gate");
					SaveModInt(Keys::ROCKSMITH_GATE_THRESHOLD, "RocksmithGateThreshold", rgTenths, "Rocksmith noise gate");
				}
			}
			ImGui::EndDisabled();
			EndCard();
		}

		// One input role's ASIO source: its interface (the output's or any other) and channel, and the clock pill when
		// it runs on another interface's clock.
		void InputSourceRows(int role, const RouteView& view)
		{
			const std::vector<std::wstring> drivers = InstalledAsioDrivers();
			// RS_ASIO.ini gives this role the same Channel as an earlier one, so there is one input for both and it
			// follows that role. Picking it separately needs its own Channel= in RS_ASIO.ini.
			int sharesWith = 0;
			for (int earlier = 1; earlier < role; ++earlier)
				if (view.viaBridge[earlier] && view.host[earlier] == view.host[role])
				{
					sharesWith = earlier;
					break;
				}
			std::vector<std::string> interfaceLabels{ "Same as output" };
			int interfaceIndex = 0;
			for (size_t i = 0; i < drivers.size(); ++i)
			{
				interfaceLabels.push_back(Utf8(drivers[i]));
				if (!view.interfaceOf[role].empty() && _wcsicmp(drivers[i].c_str(), view.interfaceOf[role].c_str()) == 0) interfaceIndex = static_cast<int>(i) + 1;
			}
			const std::vector<std::string>& inputs = view.names[role];
			int index = view.device[role] >= 0 && view.device[role] < static_cast<long>(inputs.size()) ? static_cast<int>(view.device[role]) : -1;
			ImGui::BeginDisabled(sharesWith != 0 || RouteBusy());
			if (DropdownRow("Interface", &interfaceIndex, interfaceLabels, "Where this input comes from. Same as output: the interface the sound plays on.") && sharesWith == 0)
				SwitchInputInterface(role, interfaceIndex == 0 ? std::wstring() : drivers[static_cast<size_t>(interfaceIndex - 1)]);
			if (DropdownRow("Input channel", &index, inputs, "The input on that interface this player listens to.") && sharesWith == 0)
				ApplyRoute(role, index);
			ImGui::EndDisabled();
			if (sharesWith != 0)
				Caption((std::string(ROLE_NAMES[role]) + " follows " + ROLE_NAMES[sharesWith] + ": it has the same Channel in RS_ASIO.ini. Give it its own Channel there to pick it here.").c_str());
			InputClockPill(role, "Clock");
			RouteStatusMessage();
		}

		// Player 1's input device in Cable mode: automatic (a Real Tone Cable, else the Windows default recording
		// device), or any Windows recording device, for cables that do not report the Real Tone Cable's USB ID. The list
		// is re-read every 3 s while this card is shown, as enumerating is a few COM calls.
		void PlayerOneCableInputRows()
		{
			using namespace Audio::PersistentInput;
			static std::vector<CaptureDeviceInfo> devices;
			static ULONGLONG listedAt = 0;
			const ULONGLONG now = GetTickCount64();
			if (listedAt == 0 || now - listedAt > 3000)
			{
				devices = ListCaptureDevices();
				listedAt = now;
			}
			const std::wstring choice = GetPlayerOneInputChoice();
			std::vector<std::string> labels{ "Automatic" };
			int index = 0;
			for (size_t i = 0; i < devices.size(); ++i)
			{
				labels.push_back(Utf8(devices[i].name.empty() ? devices[i].id : devices[i].name) + (devices[i].realToneCable ? " (Real Tone Cable)" : ""));
				if (!choice.empty() && _wcsicmp(devices[i].id.c_str(), choice.c_str()) == 0) index = static_cast<int>(i) + 1;
			}
			if (!choice.empty() && index == 0)
			{
				labels.push_back("Chosen device (not plugged in)");
				index = static_cast<int>(labels.size()) - 1;
			}
			if (DropdownRow("Input device", &index, labels, "Automatic plays a Real Tone Cable, or the Windows default recording device when none is plugged in. Pick your cable here if it is not found."))
			{
				if (index == 0) SetPlayerOneInputChoice(L"");
				else if (static_cast<size_t>(index - 1) < devices.size()) SetPlayerOneInputChoice(devices[static_cast<size_t>(index - 1)].id);
			}
		}

		// ---- Input --------------------------------------------------------------------------------
		// One tab per player (and the mic while RS_ASIO.ini sends it through the bridge), each the same component: what
		// the input is (ASIO interface or Real Tone Cable), its level, that source's own settings, then the processing.
		// The processing controls are the same whatever the source: they work alike on ASIO and on the cable. The input
		// chain only conditions Player 1 so far (InputConditioner, input 0); Player 2's is planned.
		enum class InputSource { None, Asio, Cable, CableStandIn, GameCable };

		void Draw()
		{
			const LiveState& live = Poll();
			PageHeader("Input", "Each player's guitar before Rocksmith hears it. Every change applies live and is saved.");
			const bool asioMode = Audio::CableInput::IsAsioPath();   // RS_ASIO owns the input (else Cable mode)
			const RouteView& view = PolledRouteView();
			static bool p2CableSeeded = false, p2Cable = false;
			if (!p2CableSeeded)
			{
				p2Cable = Audio::PersistentInput::IsCableForPlayerTwoEnabled();
				p2CableSeeded = true;
			}

			// Tabs: Player 1, Player 2, and the mic while RS_ASIO.ini sends it through the bridge. A tab shows a coloured
			// dot only when its input runs on another clock (InputClockState), the Output page's convention.
			static const char* const badges[4] = { "", "P1", "P2", "MIC" };
			static const ImU32 tints[4] = { Color::AccentHi, Color::TintBlue, Color::TintCyan, Color::TintViolet };
			std::vector<int> roles{ 1, 2 };
			if (asioMode && view.viaBridge[3]) roles.push_back(3);
			static int selectedRole = 1;
			std::vector<RoleTab> tabs;
			int tabIndex = 0;
			for (size_t i = 0; i < roles.size(); ++i)
			{
				const int role = roles[i];
				if (role == selectedRole) tabIndex = static_cast<int>(i);
				const ClockView clock = InputClockState(role);
				RoleTab tab;
				tab.label = ROLE_NAMES[role];
				tab.badge = badges[role];
				tab.tint = tints[role];
				tab.icon = role == 3 ? RoleIcon::Mic : RoleIcon::Guitar;
				tab.hasStatus = clock.shown;
				tab.status = clock.settled ? Color::Good : Color::Warn;
				tabs.push_back(tab);
			}
			if (std::find(roles.begin(), roles.end(), selectedRole) == roles.end()) tabIndex = 0;
			RoleTabs("##input-roles", tabs.data(), static_cast<int>(tabs.size()), &tabIndex);
			selectedRole = roles[static_cast<size_t>(tabIndex)];
			const int role = selectedRole;
			ImGui::PushID(role);

			// What this input is.
			InputSource source = InputSource::None;
			if (role == 1)
			{
				if (!asioMode) source = InputSource::Cable;
				// The bridge on its virtual clock (no interface open) plays the Real Tone Cable as Player 1. Not
				// ProxyInputMode: that is 2 for a real ASIO stream as well.
				else if (Audio::OutputTap::ProxyOutputMode() == 1) source = InputSource::CableStandIn;
				else source = InputSource::Asio;
			}
			else if (role == 2)
			{
				if (!asioMode) source = InputSource::GameCable;
				else if (p2Cable) source = InputSource::Cable;
				else if (view.viaBridge[2]) source = InputSource::Asio;
			}
			else source = InputSource::Asio;

			// Player 1's cable stream through Windows: the tap has it bound and running (4.x asked its own capture hook).
			const bool cableReady = Input::CaptureTap::IsPlayerLive(PLAYER_ONE_INPUT);
			std::string line;
			if (source == InputSource::Asio && !view.viaBridge[role]) line = "ASIO input set in RS_ASIO.ini";
			else if (source == InputSource::Asio)
			{
				const std::wstring iface = view.interfaceOf[role].empty() ? ReadPassthroughOutputName() : view.interfaceOf[role];
				const std::vector<std::string>& names = view.names[role];
				line = Utf8(iface);
				if (view.device[role] >= 0 && view.device[role] < static_cast<long>(names.size())) line += ", " + names[static_cast<size_t>(view.device[role])];
			}
			// Cable mode Player 1 may be another cable or recording device (CableCaptureSession.hpp, GetPlayerOneInput).
			const Audio::PersistentInput::PlayerOneInput playerOneInput = Audio::PersistentInput::GetPlayerOneInput();
			const bool playerOneOtherDevice = source == InputSource::Cable && role == 1
				&& (playerOneInput.kind == Audio::PersistentInput::PlayerOneInputKind::ChosenDevice
					|| playerOneInput.kind == Audio::PersistentInput::PlayerOneInputKind::WindowsDefault);
			if (playerOneOtherDevice)
				line = Utf8(playerOneInput.name)
					+ (playerOneInput.kind == Audio::PersistentInput::PlayerOneInputKind::WindowsDefault ? " (Windows default), through Windows" : ", through Windows");
			else if (source == InputSource::Cable)
				line = role == 1 ? (CableAmpLink::IsDirectUsb() ? "Real Tone Cable, read directly over USB"
					: cableReady ? "Real Tone Cable, through Windows" : "Real Tone Cable, waiting for input") : "Real Tone Cable, added for Player 2";
			else if (source == InputSource::CableStandIn) line = "Real Tone Cable, while no ASIO input is open";
			else if (source == InputSource::GameCable) line = "A second Real Tone Cable, opened by Rocksmith";
			if (source != InputSource::None)
			{
				BeginCard();
				StatusPill(source == InputSource::Asio ? "ASIO" : playerOneOtherDevice ? "WINDOWS INPUT" : "REAL TONE CABLE", source == InputSource::Asio ? Color::Good : Color::AccentHi);
				ImGui::SameLine(0.0f, 12.0f);
				ImGui::TextUnformatted(line.c_str());
				EndCard();
			}

			// Level.
			if (role != 3)
			{
				BeginCard("Level");
				Meter(ROLE_NAMES[role], MeterFraction(role == 1 ? live.input.meterPeak : live.input.playerTwoMeterPeak),
					role == 1 ? "Player 1's level after the input gain." : "Player 2's level.");
				EndCard();
			}

			// Source settings.
			if (role == 2 && asioMode)
			{
				BeginCard("Source");
				// Op 26: the cable after the ASIO input, as Player 2 (the hot-swap device, always listed; the switch gates it).
				if (ToggleRow("Real Tone Cable for Player 2", "Player 2 plays on a Real Tone Cable instead of an ASIO input. RS_ASIO.ini is not changed.", &p2Cable))
					BridgeControl(26, p2Cable ? L"1" : L"0");
				if (!p2Cable && view.viaBridge[2]) InputSourceRows(2, view);
				else if (!p2Cable) Caption("Player 2 has no ASIO input: RS_ASIO.ini does not send Player 2 to Rocksmith Audio Bridge ASIO. Switch on the cable above, or add Player 2 there.");
				EndCard();
			}
			else if (source == InputSource::Asio)
			{
				BeginCard("Source");
				if (view.viaBridge[role]) InputSourceRows(role, view);
				else Caption((std::string(ROLE_NAMES[role]) + "'s input is set in RS_ASIO.ini, not through Rocksmith Audio Bridge ASIO. Send it to Rocksmith Audio Bridge ASIO there to pick its interface and channel here.").c_str());
				EndCard();
			}
			else if (source == InputSource::Cable && role == 1)
			{
				BeginCard("Source");
				PlayerOneCableInputRows();
				InfoRow("Read", CableAmpLink::IsDirectUsb() ? "Directly over USB, about 1 ms packets"
					: cableReady ? "Through Windows, 10 ms packets" : "No active cable capture");
				EndCard();
			}
			else if (source == InputSource::CableStandIn)
			{
				BeginCard("Source");
				Caption("No ASIO input is open, so the bridge plays the Real Tone Cable as Player 1. Pick an interface in Switch output on the Output page to use it instead.");
				EndCard();
			}
			else if (source == InputSource::GameCable)
			{
				BeginCard("Source");
				Caption("In Cable mode Rocksmith opens a second Real Tone Cable for Player 2 itself.");
				EndCard();
			}

			// Processing.
			if (role == 1) PlayerOneProcessing();
			else if (role == 2)
			{
				BeginCard("Processing");
				Caption("Player 2's own input gain, noise suppressor and compressor are coming in the next build.");
				EndCard();
			}
			ImGui::PopID();
		}
	}

	void RegisterInputPage()
	{
		Shell::Page page{ "input", "Audio", Glyph::Input, "Input", 30, Draw, {} };
		// The Noise gate pedal floats over the game while the overlay is open, whatever page is showing.
		page.floating = NoiseGatePedalWindow;
		Shell::RegisterPage(std::move(page));
	}
}
