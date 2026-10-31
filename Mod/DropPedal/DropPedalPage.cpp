#include "DropPedalPage.hpp"

#include "DropPedalHud.hpp"
#include "DropPedalSettings.hpp"
#include "DropPedalState.hpp"
#include "../Core/Text.hpp"
#include "../Input/CaptureTap.hpp"
#include "../Shell/OverlayUi.hpp"
#include "../Shell/ShellPages.hpp"

#include "Settings.hpp"
#include "SettingEdits.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace Overlay;
using DropPedal::PitchMode;
using DropPedal::Player;

namespace AudioBridge::DropPedal
{
	namespace
	{
		// DropPedalState's target range, either way.
		constexpr int SHIFT_LIMIT = 24;
		// The tap's input index for Player 2.
		constexpr std::uint32_t PLAYER_TWO_INPUT = 1;

		PedalControls* pedal = nullptr;

		struct ColourSetting
		{
			const char* key;
			const char* iniName;
			std::uint32_t fallback;
			const char* label;
			const char* hint;
		};

		const ColourSetting COLOURS[] = {
			{ DropPedalHud::DOWN_COLOR_SETTING, "OverlayDownColor", 0x6BE06B, "Pitch down", "Colour of the readout while the pedal shifts down." },
			{ DropPedalHud::UP_COLOR_SETTING, "OverlayUpColor", 0xFFC24D, "Pitch up", "Colour of the readout while the pedal shifts up." },
			{ DropPedalHud::STATUS_COLOR_SETTING, "OverlayStatusColor", 0xFFFFFF, "Status", "Colour of the pedal's status text." },
		};

		// Live and saved through upstream's setting edits: applied on MainThread (DropPedalMod reloads the readout from
		// OnSettingsChanged) and written to RSMods.ini, whichever thread asks.
		void SaveSetting(const char* key, const char* iniName, const std::string& value, const char* label)
		{
			SettingEdits::Apply({ key, "Drop Pedal", iniName, value, label, false });
		}

		std::uint32_t ParseHex(const std::string& text, std::uint32_t fallback)
		{
			if (text.size() != 6) return fallback;
			char* end = nullptr;
			const unsigned long value = std::strtoul(text.c_str(), &end, 16);
			return end == text.c_str() + 6 ? static_cast<std::uint32_t>(value) : fallback;
		}

		std::string Hex(std::uint32_t rgb)
		{
			char text[8];
			std::snprintf(text, sizeof(text), "%06X", static_cast<unsigned int>(rgb & 0xFFFFFF));
			return text;
		}

		ImU32 ToImColor(std::uint32_t rgb)
		{
			return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
		}

		std::uint32_t PackRgb(const float color[3])
		{
			const auto channel = [](float value) { return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
			return (channel(color[0]) << 16) | (channel(color[1]) << 8) | channel(color[2]);
		}

		// A key binding's name as the keyboard prints it ("VK_OEM_COMMA" -> ","), or `fallback`.
		std::string KeyName(const char* key, const char* fallback)
		{
			const auto found = Settings::keyMap.find(Settings::ReturnSettingValue(key));
			if (found != Settings::keyMap.end())
			{
				const UINT scan = MapVirtualKeyW(found->second, MAPVK_VK_TO_VSC);
				wchar_t name[64]{};
				if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) return Text::Utf8(name);
			}
			return fallback;
		}

		// "My guitar is in": the tuning a player's strings are really in, which the pedal's tuning names count from.
		// Saved through the setting, so DropPedalMod applies it from OnSettingsChanged like an edit in RSMods.
		void BaseTuningCombo(const char* id, Player player, float width)
		{
			const int base = DropPedalState::GetBaseTuningSemitones(player);
			const bool canChange = DropPedalState::CanChangeBaseTuning();
			ImGui::BeginDisabled(!canChange);
			ImGui::SetNextItemWidth(width);
			if (ImGui::BeginCombo(id, DropPedalState::GetBaseTuningName(player).c_str()))
			{
				for (int i = 0; i < 12; ++i)
				{
					const std::string tuning = DropPedalState::GetAbsoluteTuningName(-i);
					const bool selected = base == -i;
					if (ImGui::Selectable((tuning + " standard").c_str(), selected) && !selected)
					{
						if (player == Player::One)
							SettingEdits::Apply({ SettingKeys::BASE_TUNING, "Drop Pedal", "BaseTuning", tuning, "My guitar is in", false });
						else
							SettingEdits::Apply({ SettingKeys::BASE_TUNING_PLAYER2, "Drop Pedal", "BaseTuningPlayer2", tuning, "Player 2's guitar is in", false });
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			Hint(canChange
				? "The tuning your strings are really in. The tuning names here and in the readout count from it. Saved for the next launch."
				: "Fixed while Speaker Mode plays a song.");
			ImGui::EndDisabled();
		}

		// The NOW card (mockup B, 2026-10-23), on one grid so both columns line up:
		//   [ - | E -> F#  +2 | + ]           My guitar is in
		//                                      [ E standard          v ]
		//   [ Match song (F#)       ]          [ Off | Drop Pedal | Speaker Mode ]
		// The stepper and Match song have a fixed width, so nothing moves when the tuning text changes. Below 560 px
		// the right column goes under the left. Player 1 only, like the readout; Player 2 has its own card. A control
		// the pedal would refuse right now is greyed out with the reason as its hint.
		void DrawNow(PedalControls& controls, ImU32 downColor, ImU32 upColor)
		{
			const PitchMode mode = DropPedalState::GetPitchMode();
			const int shift = DropPedalState::GetTargetSemitones(Player::One);
			const int base = DropPedalState::GetBaseTuningSemitones(Player::One);
			const bool speaker = mode == PitchMode::SpeakerMode;
			const bool speakerLocked = speaker && !controls.CanSelectMode(PitchMode::Off);
			int chartShift = 0;
			const bool chartKnown = controls.TryGetChartMatch(chartShift);

			const float full = RowWidth();
			const bool stacked = full < 560.0f;
			const float rightW = stacked ? full : std::clamp(full * 0.42f, 260.0f, 420.0f);
			const float leftW = stacked ? full : std::min(320.0f, full - rightW - 32.0f);
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const float frameH = ImGui::GetFrameHeight();
			const float rowGap = 10.0f;
			ImGui::PushFont(GetFonts().caption);
			const float labelH = ImGui::GetTextLineHeight() + 4.0f;
			ImGui::PopFont();
			const auto at = [](float x, float y) { ImGui::SetCursorScreenPos(ImVec2(x, y)); };

			const float leftX = origin.x;
			const float rightX = stacked ? origin.x : origin.x + full - rightW;
			const float row1 = origin.y + labelH;
			const float row2 = row1 + frameH + rowGap;
			const float rightTop = stacked ? row2 + frameH + rowGap : origin.y;
			const float rightRow1 = rightTop + labelH;
			const float rightRow2 = rightRow1 + frameH + rowGap;

			// ---- Left: stepper, then Match song.
			const bool canStep = mode == PitchMode::Off ? controls.CanSelectMode(PitchMode::DropPedal) : controls.CanAdjustShift();
			const char* const lockedWhy = "Speaker Mode follows the song's tuning.";
			const std::string baseName = DropPedalState::GetAbsoluteTuningName(base);
			const std::string tuning = shift == 0 ? baseName : baseName + " -> " + DropPedalState::GetAbsoluteTuningName(base + shift);
			char badge[8] = "";
			if (shift != 0) std::snprintf(badge, sizeof(badge), "%+d", shift);
			at(leftX, row1);
			const int step = JoinedStepper("##shift", leftW, tuning.c_str(), badge, shift < 0 ? downColor : upColor,
				canStep && shift > -SHIFT_LIMIT, canStep && shift < SHIFT_LIMIT, mode == PitchMode::Off,
				canStep ? "One semitone down. Turns Drop Pedal on if it is off." : lockedWhy,
				canStep ? "One semitone up. Turns Drop Pedal on if it is off." : lockedWhy);
			if (step != 0 && (mode != PitchMode::Off || controls.SelectMode(PitchMode::DropPedal)))
				controls.AdjustShift(Player::One, step);

			const bool canMatch = !speaker && chartKnown
				&& (mode == PitchMode::DropPedal || controls.CanSelectMode(PitchMode::DropPedal));
			const bool matched = canMatch && mode == PitchMode::DropPedal && chartShift == shift;
			std::string matchLabel = matched ? "Matches the song" : "Match song";
			if (chartKnown && !speaker) matchLabel += " (" + DropPedalState::GetAbsoluteTuningName(base + chartShift) + ")";
			const char* matchHint = speaker ? "Speaker Mode already plays the song in your tuning."
				: !chartKnown ? "Sets the shift so your guitar plays in the song's tuning. Works in the tuner (named tunings) and during a song."
				: matched ? "The shift already matches the song."
				: "Sets the shift so your guitar plays in the song's tuning. A drop tuning still needs its low string dropped.";
			at(leftX, row2);
			ImGui::BeginDisabled(!canMatch || matched);
			if (WideButton((matchLabel + "##match").c_str(), leftW, canMatch && !matched, matchHint)
				&& (mode == PitchMode::DropPedal || controls.SelectMode(PitchMode::DropPedal)))
				controls.SetShift(Player::One, chartShift);
			ImGui::EndDisabled();

			// ---- Right: the guitar's own tuning, then the mode.
			at(rightX, rightTop);
			ImGui::PushFont(GetFonts().caption);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Muted));
			ImGui::TextUnformatted("My guitar is in");
			ImGui::PopStyleColor();
			ImGui::PopFont();
			at(rightX, rightRow1);
			BaseTuningCombo("##baseTuning", Player::One, rightW);
			at(rightX, rightRow2);
			static const char* const modes[] = { "Off", "Drop Pedal", "Speaker Mode" };
			int modeIndex = static_cast<int>(mode);
			const bool selectable[] = {
				controls.CanSelectMode(PitchMode::Off),
				controls.CanSelectMode(PitchMode::DropPedal),
				controls.CanSelectMode(PitchMode::SpeakerMode),
			};
			if (Segmented("##mode", &modeIndex, modes, 3, rightW, selectable,
				"Drop Pedal shifts your guitar. Speaker Mode shifts the song instead, to match your guitar: pick it before a song, and it stays on until the song ends."))
				controls.SelectMode(static_cast<PitchMode>(modeIndex));
			float bottom = rightRow2 + frameH;
			if (speakerLocked)
			{
				at(rightX, bottom + 4.0f);
				ImGui::PushFont(GetFonts().caption);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
				ImGui::TextUnformatted("Speaker Mode stays on until the song ends.");
				ImGui::PopStyleColor();
				ImGui::PopFont();
				bottom = ImGui::GetItemRectMax().y;
			}

			at(origin.x, std::max(bottom, row2 + frameH) + 10.0f);
			ImGui::Dummy(ImVec2(0, 0));
		}

		void Draw()
		{
			PageHeader("Drop Pedal", "Shift your guitar's tuning without retuning the strings.");

			const bool custom = Settings::ReturnSettingValue(DropPedalHud::CUSTOM_COLORS_SETTING) == "on";
			std::uint32_t rgb[3];
			for (int i = 0; i < 3; ++i)
				rgb[i] = custom ? ParseHex(Settings::ReturnSettingValue(COLOURS[i].key), COLOURS[i].fallback) : COLOURS[i].fallback;

			const std::string downKey = KeyName(SettingKeys::PITCH_DOWN_KEY, ",");
			const std::string upKey = KeyName(SettingKeys::PITCH_UP_KEY, ".");
			BeginCard("Now");
			DrawNow(*pedal, ToImColor(rgb[0]), ToImColor(rgb[1]));
			Caption(("Shortcuts: " + KeyName(SettingKeys::TOGGLE_KEY, "F7") + " mode, " + downKey + " and " + upKey
				+ " shift down and up. Change them in RSMods, Keybindings.").c_str());
			EndCard();

			// Speaker Mode is one shift for the song, so Player 2's card shows only while their own shift can apply.
			if (Input::CaptureTap::IsPlayerLive(PLAYER_TWO_INPUT) && DropPedalState::GetPitchMode() != PitchMode::SpeakerMode)
			{
				BeginCard("Player 2");
				ImGui::TextUnformatted(DropPedalHud::Line(Player::Two).c_str());
				Caption("My guitar is in");
				BaseTuningCombo("##baseTuningPlayer2", Player::Two, std::min(RowWidth(), 420.0f));
				Caption(("Player 2 shifts with Ctrl+" + downKey + " and Ctrl+" + upKey + ".").c_str());
				EndCard();
			}

			// The pedal itself always works; this only hides its readout, for a clean screen.
			BeginCard("Display");
			bool shown = Settings::ReturnSettingValue(DropPedalHud::SHOW_SETTING) != "off";
			if (ToggleRow("Show pedal readout", "The Pitch line in the top left corner. The pedal keeps working while it is hidden.", &shown))
				SaveSetting(DropPedalHud::SHOW_SETTING, "ShowOverlay", shown ? "on" : "off", "Show pedal readout");
			EndCard();

			BeginCard("Colours");
			bool useCustom = custom;
			if (ToggleRow("Custom colours", custom ? "Your colours are active." : "Using the default colours.", &useCustom))
				SaveSetting(DropPedalHud::CUSTOM_COLORS_SETTING, "CustomOverlayColors", useCustom ? "on" : "off", "Custom colours");
			ImGui::BeginDisabled(!custom);
			for (int i = 0; i < 3; ++i)
			{
				float color[3] = { ((rgb[i] >> 16) & 0xFF) / 255.0f, ((rgb[i] >> 8) & 0xFF) / 255.0f, (rgb[i] & 0xFF) / 255.0f };
				bool released = false;
				ColorSwatch(COLOURS[i].label, color, &released, COLOURS[i].hint);
				// Saved once, when the picker closes, not every frame of a drag.
				if (released) SaveSetting(COLOURS[i].key, COLOURS[i].iniName, Hex(PackRgb(color)), COLOURS[i].label);
				if (i < 2) ImGui::SameLine(0.0f, 18.0f);
			}

			// Preview on game black.
			const ImVec2 start = ImGui::GetCursorScreenPos();
			const float width = RowWidth();
			const float height = 38.0f;
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->AddRectFilled(start, ImVec2(start.x + width, start.y + height), IM_COL32(0, 0, 0, 255), 4.0f);
			const char* const samples[3] = { "Drop D  -2", "Eb Standard  +1", "Drop Pedal" };
			float x = start.x + 14.0f;
			for (int i = 0; i < 3; ++i)
			{
				const ImVec2 size = ImGui::CalcTextSize(samples[i]);
				list->AddText(ImVec2(x, start.y + (height - size.y) * 0.5f), ToImColor(rgb[i]), samples[i]);
				x += size.x + 30.0f;
			}
			ImGui::Dummy(ImVec2(width, height));
			if (Button("Reset colours", false, "Puts the three colours back to the defaults."))
				for (const ColourSetting& colour : COLOURS) SaveSetting(colour.key, colour.iniName, Hex(colour.fallback), colour.label);
			ImGui::EndDisabled();
			EndCard();
		}
	}

	void RegisterPage(PedalControls& controls)
	{
		pedal = &controls;
		Shell::RegisterPage({ "drop-pedal", "Practice", Glyph::Pedal, "Drop Pedal", 20, Draw,
			[] { return DropPedalState::IsConfiguredEnabled(); } });
	}
}
