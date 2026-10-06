#include "../../stdafx.h"
#include "DropPedalOverlay.hpp"

#include "../../GameState.hpp"
#include "../../Resolution.h"
#include "../../Settings.hpp"
#include "DropPedal.hpp"
#include "DropPedalState.hpp"

namespace
{
	// F7 refused while Speaker Mode is locked for the song: how long the mode line flashes, and in what.
	constexpr unsigned long long LOCKED_FLASH_MS = 2500;
	// Deep orange: distinct from the light amber DROP_PEDAL_UP_TEXT a Speaker "(+1)" line already uses.
	constexpr unsigned int LOCKED_FLASH_COLOR = 0xFFFF6A00;
}

namespace
{
	constexpr unsigned int WHITE_TEXT = 0xFFFFFFFF;
	constexpr unsigned int DROP_PEDAL_DOWN_TEXT = 0xFF6BE06B;
	constexpr unsigned int DROP_PEDAL_UP_TEXT = 0xFFFFC24D;
	constexpr unsigned int DROP_PEDAL_DISABLED_TEXT = 0xFFC8C8C8;

	struct OverlayTextColors
	{
		unsigned int down = DROP_PEDAL_DOWN_TEXT;
		unsigned int up = DROP_PEDAL_UP_TEXT;
		unsigned int status = WHITE_TEXT;
		bool usesCustomColors = false;
	};

	OverlayTextColors overlayTextColors;
	unsigned long long overlayTextColorRevision = 0;

	int GetHexDigitValue(char digit)
	{
		if (digit >= '0' && digit <= '9') return digit - '0';
		if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
		if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
		return -1;
	}

	unsigned int ReadConfiguredTextColor(
		const std::string& settingName,
		unsigned int defaultColor)
	{
		const std::string configuredColor = Settings::ReturnSettingValue(settingName);
		if (configuredColor.length() != 6)
		{
			LOG_ERROR("Invalid " << settingName << " value '" << configuredColor
				<< "'. Expected six hexadecimal digits." << std::endl);
			return defaultColor;
		}

		unsigned int color = 0;
		for (const char digit : configuredColor)
		{
			const int value = GetHexDigitValue(digit);
			if (value < 0)
			{
				LOG_ERROR("Invalid " << settingName << " value '" << configuredColor
					<< "'. Expected six hexadecimal digits." << std::endl);
				return defaultColor;
			}

			color = (color << 4) | static_cast<unsigned int>(value);
		}

		return 0xFF000000 | color;
	}

}

void DropPedal::Overlay::LoadSettings()
{
	overlayTextColors = {};
	const std::string customColorsSetting = Settings::ReturnSettingValue("DropPedalCustomOverlayColors");
	if (customColorsSetting == "on")
	{
		overlayTextColors.down = ReadConfiguredTextColor("DropPedalOverlayDownColor", DROP_PEDAL_DOWN_TEXT);
		overlayTextColors.up = ReadConfiguredTextColor("DropPedalOverlayUpColor", DROP_PEDAL_UP_TEXT);
		overlayTextColors.status = ReadConfiguredTextColor("DropPedalOverlayStatusColor", WHITE_TEXT);
		overlayTextColors.usesCustomColors = true;
	}
	else if (customColorsSetting != "off")
	{
		LOG_ERROR("Invalid DropPedalCustomOverlayColors value '" << customColorsSetting
			<< "'. Expected 'on' or 'off'." << std::endl);
	}

	++overlayTextColorRevision;
}

void DropPedal::Overlay::Render(ID3DXFont* font, const Resolution& windowSize)
{
	if (!DropPedal::IsConfiguredEnabled() || !font) return;
	// F7 refused mid-song because Speaker Mode is locked for the song: the mode line flashes orange
	// for a moment, even when the readout is hidden, so the press is visibly acknowledged.
	const unsigned long long lockedTick = DropPedalState::GetModeLockedNoticeTick();
	const unsigned long long now = GetTickCount64();
	const bool isLockedFlash = lockedTick != 0 && now - lockedTick < LOCKED_FLASH_MS;
	// Visibility only: the pedal keeps working (F7 and the shift keys) with its readout hidden.
	if (Settings::ReturnSettingValue("DropPedalShowOverlay") == "off" && !isLockedFlash) return;

	// Speaker Mode shifts the one shared song mix, so Player 2 has nothing to
	// control and only Player One's row renders.
	const bool showsPlayerTwoRow = GameState::IsMultiplayer()
		&& DropPedal::GetPitchMode() != PitchMode::SpeakerMode;
	RenderTuning(font, windowSize, Player::One, 0);
	if (showsPlayerTwoRow)
	{
		RenderTuning(font, windowSize, Player::Two, 1);
	}

}

void DropPedal::Overlay::RenderTuning(
	ID3DXFont* font,
	const Resolution& windowSize,
	Player player,
	int row)
{
	UpdateTuningCache(font, player);
	const size_t playerIndex = GetPlayerIndex(player);
	const int top = static_cast<int>(windowSize.height / 54.0f)
		+ row * static_cast<int>(windowSize.height / 36.0f);

	// Locked-mode flash (see Render): alternate orange and the normal colour every 250 ms.
	unsigned int textColor = tuningTextColors[playerIndex];
	const unsigned long long lockedTick = DropPedalState::GetModeLockedNoticeTick();
	const unsigned long long now = GetTickCount64();
	if (player == Player::One && lockedTick != 0 && now - lockedTick < LOCKED_FLASH_MS
		&& ((now - lockedTick) / 250) % 2 == 0)
	{
		textColor = LOCKED_FLASH_COLOR;
	}

	DrawShadowedText(
		font,
		windowSize,
		tuningLines[playerIndex],
		textColor,
		static_cast<int>(windowSize.width / 96.0f),
		top,
		static_cast<int>(windowSize.width / 3.0f),
		top + static_cast<int>(windowSize.height / 27.0f));
}

void DropPedal::Overlay::UpdateTuningCache(ID3DXFont* font, Player player)
{
	const size_t playerIndex = GetPlayerIndex(player);
	const auto pitchMode = DropPedal::GetPitchMode();
	const int targetSemitones = DropPedal::GetTargetSemitones(player);
	const int baseTuningSemitones = DropPedal::GetBaseTuningSemitones(player);
	const bool isInputUnavailable = pitchMode == PitchMode::DropPedal
		&& !DropPedal::IsInputShifterActive();

	if (hasCachedTuningState[playerIndex]
		&& pitchMode == cachedPitchModes[playerIndex]
		&& isInputUnavailable == cachedInputUnavailable[playerIndex]
		&& targetSemitones == cachedTargetSemitones[playerIndex]
		&& baseTuningSemitones == cachedBaseTuningSemitones[playerIndex]
		&& font == cachedTuningFonts[playerIndex]
		&& cachedTuningColorRevisions[playerIndex] == overlayTextColorRevision)
	{
		return;
	}

	// Speaker Mode moves the one song mix along Player One's route; it renders as
	// a single global row, so only Player One's cache ever sees that mode.
	std::string& tuningLine = tuningLines[playerIndex];
	unsigned int& tuningTextColor = tuningTextColors[playerIndex];
	if (pitchMode == PitchMode::Off)
	{
		tuningLine = "Pitch: Off";
	}
	else if (pitchMode == PitchMode::SpeakerMode)
	{
		tuningLine = "Speaker: " + DropPedal::GetPitchRouteName();
	}
	else if (isInputUnavailable)
	{
		tuningLine = "Drop: Input unavailable";
	}
	else
	{
		tuningLine = "Drop: " + DropPedal::GetTuningName(player);
	}
	tuningTextColor = overlayTextColors.usesCustomColors
		? overlayTextColors.status
		: DROP_PEDAL_DISABLED_TEXT;

	if (pitchMode != PitchMode::Off && !isInputUnavailable)
	{
		const int direction = DropPedal::GetShiftDirection(player);
		tuningTextColor = direction < 0
			? overlayTextColors.down
			: (direction > 0 ? overlayTextColors.up : overlayTextColors.status);
	}

	cachedPitchModes[playerIndex] = pitchMode;
	cachedInputUnavailable[playerIndex] = isInputUnavailable;
	cachedTargetSemitones[playerIndex] = targetSemitones;
	cachedBaseTuningSemitones[playerIndex] = baseTuningSemitones;
	cachedTuningFonts[playerIndex] = font;
	cachedTuningColorRevisions[playerIndex] = overlayTextColorRevision;
	hasCachedTuningState[playerIndex] = true;
	font->PreloadTextA(tuningLine.c_str(), static_cast<int>(tuningLine.length()));
}

void DropPedal::Overlay::DrawShadowedText(
	ID3DXFont* font,
	const Resolution& windowSize,
	const std::string& text,
	unsigned int textColor,
	int topLeftX,
	int topLeftY,
	int bottomRightX,
	int bottomRightY) const
{
	RECT textRectangle{ topLeftX, topLeftY, bottomRightX, bottomRightY };
	RECT shadowRectangle = textRectangle;
	int shadowOffset = static_cast<int>(windowSize.height / 540);
	if (shadowOffset < 1) shadowOffset = 1;
	OffsetRect(&shadowRectangle, shadowOffset, shadowOffset);

	font->DrawTextA(
		nullptr,
		text.c_str(),
		-1,
		&shadowRectangle,
		DT_LEFT | DT_NOCLIP,
		D3DCOLOR_ARGB(230, 0, 0, 0));
	font->DrawTextA(nullptr, text.c_str(), -1, &textRectangle, DT_LEFT | DT_NOCLIP, textColor);
}
