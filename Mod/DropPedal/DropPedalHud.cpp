#include "DropPedalHud.hpp"

#include "DropPedalHooks.hpp"
#include "DropPedalState.hpp"
#include "TuningNames.hpp"
#include "SpeakerMode/SpeakerModeSession.hpp"

#include "Log.hpp"
#include "MemUtil.hpp"
#include "Offsets.hpp"

using DropPedal::PitchMode;
using DropPedal::Player;
using Framework::HudAnchor;
using Framework::HudText;
using Framework::ModContext;
using Framework::SettingDef;
using Framework::SettingDefs;

namespace AudioBridge::DropPedal
{
	namespace
	{
		constexpr int DOWN_COLOR = static_cast<int>(0xFF6BE06B);
		constexpr int UP_COLOR = static_cast<int>(0xFFFFC24D);
		constexpr int WHITE = static_cast<int>(0xFFFFFFFF);
		constexpr int OFF_COLOR = static_cast<int>(0xFFC8C8C8);
		// A refused pedal key while Speaker Mode is locked: the row alternates orange for this long.
		constexpr unsigned long long LOCKED_FLASH_MS = 2500;
		constexpr unsigned long long LOCKED_FLASH_STEP_MS = 250;
		// Deep orange: distinct from the light amber of an upward shift.
		constexpr int LOCKED_FLASH_COLOR = static_cast<int>(0xFFFF6A00);

		const char* const ROW_IDS[] = { "drop-pedal-player1", "drop-pedal-player2" };

		// "RRGGBB" -> opaque ARGB; the fallback (and a logged error) for anything else.
		int ParseColor(const std::string& text, const char* setting, int fallback)
		{
			if (text.size() == 6)
			{
				unsigned int color = 0;
				bool valid = true;
				for (const char digit : text)
				{
					int value = -1;
					if (digit >= '0' && digit <= '9') value = digit - '0';
					else if (digit >= 'a' && digit <= 'f') value = digit - 'a' + 10;
					else if (digit >= 'A' && digit <= 'F') value = digit - 'A' + 10;
					if (value < 0) { valid = false; break; }
					color = (color << 4) | static_cast<unsigned int>(value);
				}
				if (valid) return static_cast<int>(0xFF000000u | color);
			}
			LOG_ERROR("[DropPedal] " << setting << " is '" << text << "'; expected six hexadecimal digits." << std::endl);
			return fallback;
		}

		std::string SignedSemitones(int semitones)
		{
			return std::string(semitones > 0 ? "+" : "") + std::to_string(semitones);
		}

		TuningNames::Shape ToShape(SpeakerMode::ChartTuningShape shape)
		{
			switch (shape)
			{
			case SpeakerMode::ChartTuningShape::Drop: return TuningNames::Shape::Drop;
			case SpeakerMode::ChartTuningShape::Custom: return TuningNames::Shape::Custom;
			default: return TuningNames::Shape::Uniform;
			}
		}

		// 4.x's GetTargetTuningName / GetPhysicalTuningName: with the song's chart synced, both ends are named by the
		// chart's shape (the guitar's is the chart moved onto the player's base), so a drop chart reads "Drop D".
		bool TryNameSpeakerRoute(int base, std::string& target, std::string& physical)
		{
			SpeakerMode::SpeakerModeSession::Chart chart;
			if (!DropPedalState::IsSpeakerTargetSynchronized() || !SpeakerMode::SpeakerModeSession::TryGetSyncedChart(chart)) return false;
			const TuningNames::Shape shape = ToShape(chart.shape);
			int physicalOffsets[6];
			const int shapeShift = base - chart.reference;
			for (int string = 0; string < 6; string++) physicalOffsets[string] = chart.offsets[string] + shapeShift;
			target = TuningNames::NameShape(shape, chart.offsets, chart.reference);
			physical = TuningNames::NameShape(shape, physicalOffsets, base);
			return true;
		}

		// Upstream's GameState::IsMultiplayer dereferences the end of its pointer chain unchecked, which faults while the
		// chain isn't there yet. Here the walk is the guarded one and a missing chain means single player.
		bool IsMultiplayer()
		{
			const uintptr_t address = MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_multiplayer, Offsets::ptr_multiplayerOffsets, true);
			return address != 0 && *reinterpret_cast<const int*>(address) != 0;
		}
	}

	SettingDefs DropPedalHud::Settings(const char* parentSetting)
	{
		return {
			SettingDef::Toggle(SHOW_SETTING, "ShowOverlay", "Show the pedal's readout", "Drop Pedal", "Drop Pedal", "on",
				"The pedal's tuning in the top left corner. The keys keep working with it hidden.")
				.WithVisibleWhen(parentSetting),
			SettingDef::Toggle(CUSTOM_COLORS_SETTING, "CustomOverlayColors", "Custom readout colours", "Drop Pedal", "Drop Pedal",
				"off")
				.WithVisibleWhen(parentSetting),
			SettingDef::String(DOWN_COLOR_SETTING, "Shifted down (RRGGBB)")
				.Ini("Drop Pedal", "OverlayDownColor")
				.Default("6BE06B")
				.WithVisibleWhen(CUSTOM_COLORS_SETTING),
			SettingDef::String(UP_COLOR_SETTING, "Shifted up (RRGGBB)")
				.Ini("Drop Pedal", "OverlayUpColor")
				.Default("FFC24D")
				.WithVisibleWhen(CUSTOM_COLORS_SETTING),
			SettingDef::String(STATUS_COLOR_SETTING, "Not shifted (RRGGBB)")
				.Ini("Drop Pedal", "OverlayStatusColor")
				.Default("FFFFFF")
				.WithVisibleWhen(CUSTOM_COLORS_SETTING),
		};
	}

	void DropPedalHud::LoadSettings(const ModContext& c)
	{
		customColors = c.IsOn(CUSTOM_COLORS_SETTING);
		downColor = customColors ? ParseColor(c.Value(DOWN_COLOR_SETTING), "OverlayDownColor", DOWN_COLOR) : DOWN_COLOR;
		upColor = customColors ? ParseColor(c.Value(UP_COLOR_SETTING), "OverlayUpColor", UP_COLOR) : UP_COLOR;
		statusColor = customColors ? ParseColor(c.Value(STATUS_COLOR_SETTING), "OverlayStatusColor", WHITE) : WHITE;
	}

	void DropPedalHud::Publish(ModContext& c)
	{
		// Nothing to show while the game boots, and its memory isn't safe to read yet.
		if (c.phase == Framework::GamePhase::Loading) return;

		const unsigned long long lockedTick = DropPedalState::GetModeLockedNoticeTick();
		const unsigned long long now = GetTickCount64();
		const bool lockedFlash = lockedTick != 0 && now - lockedTick < LOCKED_FLASH_MS;
		if (lockedFlash) c.RequestFastTick();

		// Hidden, the readout still flashes for a refused key, so the press is visibly acknowledged.
		const bool shown = c.IsOn(SHOW_SETTING) || lockedFlash;
		// Speaker Mode shifts the one song mix: Player 2 has nothing of their own to show.
		const bool playerTwoRow = DropPedalState::GetPitchMode() != PitchMode::SpeakerMode && IsMultiplayer();

		for (const Player player : { Player::One, Player::Two })
		{
			HudText text;
			text.visible = shown && (player == Player::One || playerTwoRow);
			if (text.visible)
			{
				text.text = Line(player);
				text.colorHex = Color(player);
				if (player == Player::One && lockedFlash && ((now - lockedTick) / LOCKED_FLASH_STEP_MS) % 2 == 0)
					text.colorHex = LOCKED_FLASH_COLOR;
			}
			const int row = static_cast<int>(::DropPedal::GetPlayerIndex(player));
			c.Hud().Set(ROW_IDS[row], { HudAnchor::TopLeft, row }, std::move(text));
		}
	}

	std::string DropPedalHud::Line(Player player)
	{
		const int shift = DropPedalState::GetTargetSemitones(player);
		switch (DropPedalState::GetPitchMode())
		{
		case PitchMode::DropPedal:
			// 4.x: the shifter isn't in the input path (no capture, or the tap is off), so nothing shifts.
			if (!DropPedalHooks::IsInputShifterActive()) return "Drop: Input unavailable";
			return "Drop: " + DropPedalState::GetTuningName(player);
		case PitchMode::SpeakerMode:
		{
			// The song moves from the chart's tuning onto the guitar's, the opposite way to the pedal.
			const int base = DropPedalState::GetBaseTuningSemitones(Player::One);
			std::string target = DropPedalState::GetAbsoluteTuningName(base + shift);
			std::string guitar = DropPedalState::GetAbsoluteTuningName(base);
			TryNameSpeakerRoute(base, target, guitar);
			if (shift == 0) return "Speaker: " + guitar;
			return "Speaker: " + target + " -> " + guitar + " (" + SignedSemitones(-shift) + ")";
		}
		case PitchMode::Off:
		default:
			return "Pitch: Off";
		}
	}

	int DropPedalHud::Color(Player player) const
	{
		const PitchMode mode = DropPedalState::GetPitchMode();
		if (mode == PitchMode::Off || (mode == PitchMode::DropPedal && !DropPedalHooks::IsInputShifterActive()))
			return customColors ? statusColor : OFF_COLOR;

		int direction = DropPedalState::GetShiftDirection(player);
		if (mode == PitchMode::SpeakerMode) direction = -direction;
		return direction < 0 ? downColor : (direction > 0 ? upColor : statusColor);
	}
}
