#include "NoteByNoteSettings.hpp"

#include "Log.hpp"
#include "SettingEdits.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>

using Framework::ModContext;
using Framework::SettingDef;
using Framework::SettingDefs;

namespace NoteByNote::ModSettings
{
	namespace
	{
		std::mutex snapshotMutex;
		Snapshot snapshot;   // under snapshotMutex

		const char* const COLOR_INI[4] = { "NoteByNoteNeutralColor", "NoteByNoteConfirmedColor", "NoteByNotePartialColor",
			"NoteByNoteRejectedColor" };
		const char* const COLOR_DEFAULTS[4] = { "FFFFFF", "55DD77", "FFAA44", "FF5555" };
		const char* const COLOR_KEYS[4] = { Keys::NEUTRAL_COLOR, Keys::CONFIRMED_COLOR, Keys::PARTIAL_COLOR, Keys::REJECTED_COLOR };

		bool TryParseHex(const std::string& text, std::uint32_t& rgb)
		{
			if (text.size() != 6 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
			rgb = static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
			return true;
		}

		// "Default" (or anything unreadable, logged) = the built-in layout; otherwise "x,y" fractions of the window.
		Placement ParsePlacement(const std::string& text, const char* iniName)
		{
			Placement placement;
			if (text.empty() || text == "Default") return placement;
			if (sscanf_s(text.c_str(), "%f,%f", &placement.x, &placement.y) != 2
				|| placement.x < 0.0f || placement.y < 0.0f || placement.x > 1.0f || placement.y > 1.0f)
			{
				LOG_ERROR("(NBN SETTINGS) Invalid " << iniName << ": expected Default or x,y fractions; using the default layout." << std::endl);
				return {};
			}
			return placement;
		}

		const char* const STYLE_NAMES[] = { "Detailed", "Simple", "Tab", "TabTimeline" };

		// Same keys, section and formats Load reads, so an overlay edit survives the next launch and the desktop GUI
		// shows it.
		void Save(const char* key, const char* iniName, const std::string& value, const char* label, bool isInt = false)
		{
			SettingEdits::Apply({ key, INI_SECTION, iniName, value, label, isInt });
		}

		std::string HexColor(std::uint32_t rgb)
		{
			char text[8]{};
			sprintf_s(text, "%06X", rgb & 0xFFFFFFu);
			return text;
		}

		// 4.x kept placements in 1/10000ths of the window, so a saved spot reads back exactly as it was dragged.
		float Unit(float value)
		{
			return static_cast<float>(static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 10000.0f + 0.5f)) / 10000.0f;
		}

		template <typename Change>
		void Update(Change change)
		{
			std::lock_guard<std::mutex> lock(snapshotMutex);
			change(snapshot);
		}

		int Percent(const ModContext& c, const char* key, int fallback)
		{
			const int value = c.Int(key);
			return value >= 50 && value <= 300 ? value : fallback;
		}
	}

	SettingDefs Declare()
	{
		SettingDefs settings = {
			SettingDef::Toggle(Keys::DETECTION_OVERLAY, DETECTION_OVERLAY_INI, "Show the detection readout", INI_SECTION,
				INI_SECTION, "on",
				"Note by Note's Native / Enhanced / ML readout and the target note while it holds a note. Note by Note\n"
				"itself is switched on in the game's Riff Repeater, Advanced Settings.")
				.Heading(Framework::SettingHeading::Practice)
				.EntryTitle("Note by Note"),
			SettingDef::Enum(Keys::TARGET_STYLE, "Target")
				.Ini(INI_SECTION, "NoteByNoteTargetStyle")
				.Choices({ "Detailed", "Simple", "Tab", "TabTimeline" }, "Detailed")
				.ListedUnder(Keys::DETECTION_OVERLAY),
			SettingDef::Enum(Keys::TARGET_POSITION, "Target position")
				.Ini(INI_SECTION, "NoteByNoteTargetPosition")
				.Choices({ "Left", "Center", "Custom" }, "Left")
				.ListedUnder(Keys::DETECTION_OVERLAY),
			SettingDef::Numeric(Keys::UI_SIZE, INI_SECTION, "NoteByNoteUiSize", "Readout size (%)", "100", 50, 300,
				std::nullopt, INI_SECTION)
				.ListedUnder(Keys::DETECTION_OVERLAY),
			SettingDef::Numeric(Keys::TARGET_SIZE, INI_SECTION, "NoteByNoteTargetSize", "Target size (%)", "150", 50, 300,
				std::nullopt, INI_SECTION)
				.ListedUnder(Keys::DETECTION_OVERLAY),
			SettingDef::Numeric(Keys::LINE_SPACING, INI_SECTION, "NoteByNoteLineSpacing", "Line spacing (%)", "100", 50, 300,
				std::nullopt, INI_SECTION)
				.ListedUnder(Keys::DETECTION_OVERLAY),
			// Dragged in game; "Default" or "x,y" fractions of the window.
			SettingDef::String(Keys::READOUT_PLACEMENT, "Readout placement")
				.Ini(INI_SECTION, "NoteByNoteReadoutPlacement")
				.Default("Default")
				.EditedBy("NoteByNoteHud"),
			SettingDef::String(Keys::TARGET_PLACEMENT, "Target placement")
				.Ini(INI_SECTION, "NoteByNoteTargetPlacement")
				.Default("Default")
				.EditedBy("NoteByNoteHud"),
			SettingDef::Toggle(Keys::CUSTOM_COLOURS, "NoteByNoteCustomColours", "Custom readout colours", INI_SECTION,
				INI_SECTION, "off")
				.ListedUnder(Keys::DETECTION_OVERLAY),
			SettingDef::String(Keys::NEUTRAL_COLOR, "Neutral (RRGGBB)")
				.Ini(INI_SECTION, COLOR_INI[0]).Default(COLOR_DEFAULTS[0]).WithVisibleWhen(Keys::CUSTOM_COLOURS),
			SettingDef::String(Keys::CONFIRMED_COLOR, "Confirmed (RRGGBB)")
				.Ini(INI_SECTION, COLOR_INI[1]).Default(COLOR_DEFAULTS[1]).WithVisibleWhen(Keys::CUSTOM_COLOURS),
			SettingDef::String(Keys::PARTIAL_COLOR, "Partial (RRGGBB)")
				.Ini(INI_SECTION, COLOR_INI[2]).Default(COLOR_DEFAULTS[2]).WithVisibleWhen(Keys::CUSTOM_COLOURS),
			SettingDef::String(Keys::REJECTED_COLOR, "Rejected (RRGGBB)")
				.Ini(INI_SECTION, COLOR_INI[3]).Default(COLOR_DEFAULTS[3]).WithVisibleWhen(Keys::CUSTOM_COLOURS),
			SettingDef::Toggle(Keys::TAB_DOCKED, "NoteByNoteTabDocked", "Dock the look-ahead tab", INI_SECTION, INI_SECTION,
				"off")
				.ListedUnder(Keys::DETECTION_OVERLAY),
			SettingDef::String(Keys::TAB_HIGHLIGHT, "Look-ahead tab highlight (RRGGBB or Default)")
				.Ini(INI_SECTION, "NoteByNoteTabHighlight")
				.Default("Default")
				.ListedUnder(Keys::DETECTION_OVERLAY),
		};
		return settings;
	}

	void Load(const ModContext& c)
	{
		Snapshot next;
		next.detectionVisible = !c.IsOff(Keys::DETECTION_OVERLAY);

		const std::string position = c.Value(Keys::TARGET_POSITION);
		next.targetPosition = position == "Center" ? TargetPosition::Center
			: position == "Custom" ? TargetPosition::Custom : TargetPosition::Left;

		const std::string style = c.Value(Keys::TARGET_STYLE);
		next.targetStyle = style == "Simple" ? TargetStyle::Simple
			: style == "Tab" ? TargetStyle::Tab
			: style == "TabTimeline" ? TargetStyle::TabTimeline : TargetStyle::Detailed;

		next.uiSize = Percent(c, Keys::UI_SIZE, next.uiSize);
		next.targetSize = Percent(c, Keys::TARGET_SIZE, next.targetSize);
		next.lineSpacing = Percent(c, Keys::LINE_SPACING, next.lineSpacing);
		next.readoutPlacement = ParsePlacement(c.Value(Keys::READOUT_PLACEMENT), "NoteByNoteReadoutPlacement");
		next.targetPlacement = ParsePlacement(c.Value(Keys::TARGET_PLACEMENT), "NoteByNoteTargetPlacement");

		next.customColours = c.IsOn(Keys::CUSTOM_COLOURS);
		for (int index = 0; index < 4; ++index)
		{
			std::uint32_t rgb = 0;
			if (TryParseHex(c.Value(COLOR_KEYS[index]), rgb)) next.colors[index] = 0xFF000000u | rgb;
			else LOG_ERROR("(NBN SETTINGS) Invalid Note by Note colour for " << COLOR_INI[index]
				<< ": expected six hexadecimal digits; using the default." << std::endl);
		}

		next.tabDocked = c.IsOn(Keys::TAB_DOCKED);
		const std::string highlight = c.Value(Keys::TAB_HIGHLIGHT);
		std::uint32_t rgb = 0;
		if (highlight.empty() || highlight == "Default") next.tabHighlight = 0;
		else if (TryParseHex(highlight, rgb)) next.tabHighlight = rgb == 0 ? 1u : rgb;   // pure black is 000001: 0 is the accent
		else LOG_ERROR("(NBN SETTINGS) Invalid NoteByNoteTabHighlight: expected Default or a hex colour like 3B8BEB;"
			<< " using the default." << std::endl);

		std::lock_guard<std::mutex> lock(snapshotMutex);
		snapshot = next;
	}

	Snapshot Get()
	{
		std::lock_guard<std::mutex> lock(snapshotMutex);
		return snapshot;
	}
}

namespace NoteByNote::ModSettings
{
	DetectionPalette Palette(const Snapshot& current)
	{
		if (!current.customColours) return {};
		return { current.colors[0], current.colors[1], current.colors[2], current.colors[3] };
	}

	Placement GetPlacement(const Snapshot& current, HudBlock block)
	{
		return block == HudBlock::Readout ? current.readoutPlacement : current.targetPlacement;
	}

	void SetDetectionVisible(bool visible)
	{
		Update([&](Snapshot& next) { next.detectionVisible = visible; });
		Save(Keys::DETECTION_OVERLAY, DETECTION_OVERLAY_INI, visible ? "on" : "off", "Show the detection readout");
	}

	void SetUiSize(int percent, bool persist)
	{
		percent = std::clamp(percent, 50, 300);
		Update([&](Snapshot& next) { next.uiSize = percent; });
		if (persist) Save(Keys::UI_SIZE, "NoteByNoteUiSize", std::to_string(percent), "Readout size (%)", true);
	}

	void SetTargetSize(int percent, bool persist)
	{
		percent = std::clamp(percent, 50, 300);
		Update([&](Snapshot& next) { next.targetSize = percent; });
		if (persist) Save(Keys::TARGET_SIZE, "NoteByNoteTargetSize", std::to_string(percent), "Target size (%)", true);
	}

	void SetLineSpacing(int percent, bool persist)
	{
		percent = std::clamp(percent, 50, 300);
		Update([&](Snapshot& next) { next.lineSpacing = percent; });
		if (persist) Save(Keys::LINE_SPACING, "NoteByNoteLineSpacing", std::to_string(percent), "Line spacing (%)", true);
	}

	void SetPlacement(HudBlock block, Placement placement, bool persist)
	{
		if (placement.IsSet()) placement = { Unit(placement.x), Unit(placement.y) };
		else placement = {};
		Update([&](Snapshot& next) {
			(block == HudBlock::Readout ? next.readoutPlacement : next.targetPlacement) = placement;
		});
		if (!persist) return;
		std::string value = "Default";
		if (placement.IsSet())
		{
			char text[32]{};
			sprintf_s(text, "%.4f,%.4f", placement.x, placement.y);
			value = text;
		}
		if (block == HudBlock::Readout) Save(Keys::READOUT_PLACEMENT, "NoteByNoteReadoutPlacement", value, "Readout placement");
		else Save(Keys::TARGET_PLACEMENT, "NoteByNoteTargetPlacement", value, "Target placement");
	}

	void SetTargetPosition(TargetPosition position)
	{
		Update([&](Snapshot& next) { next.targetPosition = position; });
		const char* value = position == TargetPosition::Center ? "Center"
			: position == TargetPosition::Custom ? "Custom" : "Left";
		Save(Keys::TARGET_POSITION, "NoteByNoteTargetPosition", value, "Target position");
	}

	void SetTargetStyle(TargetStyle style)
	{
		if (style < TargetStyle::Detailed || style > TargetStyle::TabTimeline)
		{
			LOG_ERROR("(NBN SETTINGS) Invalid NoteByNoteTargetStyle value." << std::endl);
			return;
		}
		Update([&](Snapshot& next) { next.targetStyle = style; });
		Save(Keys::TARGET_STYLE, "NoteByNoteTargetStyle", STYLE_NAMES[static_cast<int>(style)], "Target");
	}

	void SetCustomColours(bool enabled)
	{
		Update([&](Snapshot& next) { next.customColours = enabled; });
		Save(Keys::CUSTOM_COLOURS, "NoteByNoteCustomColours", enabled ? "on" : "off", "Custom readout colours");
	}

	void SetColor(int index, std::uint32_t rgb, bool persist)
	{
		if (index < 0 || index > 3) return;
		Update([&](Snapshot& next) { next.colors[index] = 0xFF000000u | (rgb & 0xFFFFFFu); });
		static const char* const labels[4] = { "Neutral (RRGGBB)", "Confirmed (RRGGBB)", "Partial (RRGGBB)", "Rejected (RRGGBB)" };
		if (persist) Save(COLOR_KEYS[index], COLOR_INI[index], HexColor(rgb), labels[index]);
	}

	void ResetColors()
	{
		for (int index = 0; index < 4; ++index)
			SetColor(index, static_cast<std::uint32_t>(std::stoul(COLOR_DEFAULTS[index], nullptr, 16)), true);
	}

	void SetTabDocked(bool docked)
	{
		Update([&](Snapshot& next) { next.tabDocked = docked; });
		Save(Keys::TAB_DOCKED, "NoteByNoteTabDocked", docked ? "on" : "off", "Dock the look-ahead tab");
	}

	void SetTabHighlight(std::uint32_t rgb, bool persist)
	{
		// Pure black is stored as 000001 so 0 can keep meaning "the overlay accent".
		const std::uint32_t value = rgb == 0 ? 0 : (rgb & 0xFFFFFFu) == 0 ? 1u : (rgb & 0xFFFFFFu);
		Update([&](Snapshot& next) { next.tabHighlight = value; });
		if (persist) Save(Keys::TAB_HIGHLIGHT, "NoteByNoteTabHighlight", value == 0 ? "Default" : HexColor(value),
			"Look-ahead tab highlight (RRGGBB or Default)");
	}
}
