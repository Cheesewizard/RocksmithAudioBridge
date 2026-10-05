#include "../stdafx.h"
#include "ExtendedRangeMode.hpp"
#include "../StringState.h"

/// <param name="stringnum"> - Number of string</param>
/// <param name="state"> - Structs::string_state</param>
/// <returns>Pointer to String Color</returns>
uintptr_t GetStringColor(uintptr_t stringnum, int state) {
	uintptr_t currentStringNum = stringnum; // EDX
	uintptr_t currentStringColor = 0; // EAX
	uintptr_t stringColorPointer = Offsets::ptr_stringColor; // Magic Number

	uintptr_t pointerValue = MemUtil::ReadPtr(stringColorPointer); // ECX

	if (!pointerValue)
		return NULL;

	currentStringColor = MemUtil::ReadPtr(pointerValue + currentStringColor * 0x4 + 0x348);

	if (currentStringColor >= 2)
		return NULL;

	currentStringColor = currentStringColor * 0xA8;
	currentStringColor += currentStringNum;

	currentStringColor = MemUtil::ReadPtr(pointerValue + currentStringColor * 0x4 + state);

	return currentStringColor;
}

bool ERMode::TryGetActiveStringColor(int stringIndex, RSColor& color)
{
	if (stringIndex < 0 || stringIndex >= 6) return false;
	__try
	{
		const auto address = GetStringColor(stringIndex, Enabled);
		if (address == 0) return false;
		color = *reinterpret_cast<const RSColor*>(address);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b)
		&& color.r >= 0.0f && color.r <= 1.0f
		&& color.g >= 0.0f && color.g <= 1.0f
		&& color.b >= 0.0f && color.b <= 1.0f;
}

/// <summary>
/// Store backup of original string color.
/// </summary>
/// <param name="strings"> - Pointers to string colors</param>
/// <param name="state"> - Structs::string_state</param>
void ERMode::InitStrings(std::vector<uintptr_t>& strings, int state) {
	strings.clear();
	for (int strIndex = 0; strIndex < 6;strIndex++)
		strings.push_back(GetStringColor(strIndex, state));
}

/// <summary>
/// Set strings to their respective colors
/// </summary>
/// <param name="strings"> - Pointers to string colors</param>
/// <param name="colors"> - New String Colors</param>
void ERMode::SetColors(std::vector<uintptr_t> strings, std::vector<RSColor> colors) {
	for (int strIndex = 0; strIndex < 6;strIndex++) {
		if (strings[strIndex] == NULL)
			return;
		*(RSColor*)strings[strIndex] = colors[strIndex];
	}
}

void ERMode::Initialize() {
	std::map<std::string, RSColor> emptyMap;

	for (int str = 0; str < 6; str++)
		customColors.emplace_back(emptyMap);
}

void ERMode::SetCustomColors(int strIdx, const ColorMap& customColorMap) {
	customColors[strIdx] = customColorMap;
}

/// <summary>
/// 
/// </summary>
/// <param name="strings"> - Pointers to string colors</param>
/// <param name="colorType"> - String of color type</param>
void ERMode::SetColors(std::vector<uintptr_t> strings, const std::string& colorType) {
	for (int strIndex = 0; strIndex < 6;strIndex++) {
		if (strings[strIndex] == NULL)
			return;

		*(RSColor*)strings[strIndex] = customColors[strIndex][colorType];
	}
}

std::vector<RSColor> oldNormal, oldDisabled, oldEnabled, oldGlow, oldAmb;

/// <summary>
/// Run ResetString on all six strings.
/// </summary>
bool ERMode::ResetAllStrings() {
	if (!ColorsSaved)
		return false;

	for (int str = 0; str < 6;str++)
		ResetString(str);
	return true;
}

/// <summary>
/// Reset a string back to the original color
/// </summary>
/// <param name="strIndex"> - Current String (zero indexed)</param>
void ERMode::ResetString(int strIndex) {
	std::vector<uintptr_t> stringsGlow, stringsDisabled, stringsAmb, stringsEnabled, stringsPegInTune, stringsPegNotInTune, stringsText, stringsPart, stringsBodyNorm, stringsBodyAcc, stringsBodyPrev;

	// Get the original values for a string.
	InitStrings(stringsGlow, Glow);
	InitStrings(stringsDisabled, Disabled);
	//InitStrings(stringsAmb, Ambient);
	InitStrings(stringsEnabled, Enabled);

	// Set the origial values.
	*(RSColor*)stringsGlow[strIndex] = oldGlow[strIndex];
	*(RSColor*)stringsDisabled[strIndex] = oldDisabled[strIndex];
	//*(Color*)stringsAmb[strIndex] = oldAmb[strIndex];
	*(RSColor*)stringsEnabled[strIndex] = oldEnabled[strIndex];

	//Settings::SetStringColors(strIndex, oldGlow[strIndex], false);
}

std::vector<std::vector<RSColor>> defaultColors;

/// <summary>
/// Set colors of strings depending on INI settings.
/// </summary>
void ERMode::Toggle7StringMode() {
	std::vector<uintptr_t> stringsTest, stringsGlow, stringsDisabled, stringsAmb, stringsEnabled, stringsPegInTune, stringsPegNotInTune, pegsTuning, stringsText, stringsPart, stringsBodyNorm, stringsBodyAcc, stringsBodyPrev;

	// Get the original values for the strings.
	InitStrings(stringsGlow, Glow);
	InitStrings(stringsDisabled, Disabled);
	InitStrings(stringsAmb, Ambient);
	InitStrings(stringsEnabled, Enabled);
	InitStrings(stringsPegInTune, PegsInTune);
	InitStrings(stringsPegNotInTune, PegsNotInTune);
	InitStrings(stringsText, Text);
	//InitStrings(stringsPart, Particles);
	InitStrings(stringsBodyNorm, BodyNorm);
	InitStrings(stringsBodyAcc, BodyAcc);
	InitStrings(pegsTuning, PegsTuning);
	//InitStrings(stringsBodyPrev, BodyPrev);

	if (ColorsSaved && Settings::IsTwitchSettingEnabled("SolidNotes")) {
		if (customSolidColor.size() != 6) // JIC
			return;

		// Don't change string colors with solid notes
		//SetColors(stringsGlow, customSolidColor);
		//SetColors(stringsEnabled, customSolidColor);
		//SetColors(stringsDisabled, customSolidColor);
		//SetColors(stringsAmb, customSolidColor);

		return;
	}

	if ((ERMode::AttemptedERInThisSong && ERMode::UseERExclusivelyInThisSong) || (AttemptedERInTuner && UseERInTuner)) {
		switch (Settings::GetModSetting("CustomStringColors")) {
			case 0: // User wants original Rocksmith colors
				break;
			case 1: // User wants ZZ / Zag's colors (normal colors, but shifted down one string with a dark green on the top for Extended Range).
				SetColors(stringsGlow, colorsGlow); // Zags custom low B color values manually entered; Glowed
				SetColors(stringsDisabled, colorsDisabled); // Zags custom low B color values manually entered; Disabled
				SetColors(stringsEnabled, colorsStrEna); // name="GuitarStringsEnabledColorBlind" id="237528906"
				SetColors(stringsPegNotInTune, colorsPegNotInTune); // name="GuitarPegsTuningBlind" id="1806691030"
				SetColors(stringsPegInTune, colorsPegInTune); // name="GuitarPegsInTuneBlind" id="2547441015"
				SetColors(stringsText, colorsText); // name="GuitarRegistrarTextIndicatorBlind" id="3186002004"
				SetColors(stringsBodyNorm, colorsNormal); // name="NotewayBodyPartsBodyNormBlind" id="3629363565"
				SetColors(stringsBodyAcc, colorsBodyAcc); // name="NotewayBodyPartsAccentBlind" id = "47948252"
				SetColors(pegsTuning, colorsPegsTuning);
				break;
			case 2: // User wants their own custom (ER) colors
				SetColors(stringsEnabled, "Enabled_CB");
				SetColors(stringsGlow, "Glow_CB");
				SetColors(stringsDisabled, "Disabled_CB");
				SetColors(stringsText, "TextIndicator_CB");
				SetColors(pegsTuning, "PegsTuning_CB");
				SetColors(stringsPegInTune, "PegsInTune_CB");
				SetColors(stringsPegNotInTune, "PegsOutTune_CB");
				SetColors(stringsBodyNorm, "BodyNorm_CB");
				SetColors(stringsBodyAcc, "BodyAcc_CB");
				break;
			default:
				break;
		}
	}
	else {
		if (!ColorsSaved) { //read only once, so it won't change defaults if you change to CB
			for (int i = 0; i < 6; i++) {
				oldDisabled.push_back(*(RSColor*)stringsDisabled[i]);
				oldEnabled.push_back(*(RSColor*)stringsEnabled[i]);
				oldGlow.push_back(*(RSColor*)stringsGlow[i]);
				oldAmb.push_back(*(RSColor*)stringsAmb[i]);
			}

			ColorsSaved = true;
		}

		if (Settings::GetModSetting("CustomStringColors") == 2) { // User wants their own custom (non-ER) colors
			SetColors(stringsEnabled, "Enabled_N");
			SetColors(stringsGlow, "Glow_N");
			SetColors(stringsDisabled, "Disabled_N");
			SetColors(stringsText, "TextIndicator_N");
			SetColors(pegsTuning, "PegsTuning_N");
			SetColors(stringsPegInTune, "PegsInTune_N");
			SetColors(stringsPegNotInTune, "PegsOutTune_N");
			SetColors(stringsBodyNorm, "BodyNorm_N");
			SetColors(stringsBodyAcc, "BodyAcc_N");
			//etc.
		} // If not enabled, colors will auto-reset upon entering a song
	}

	//NOTE: this overrides string colors, no matter if ER song or not
	if (Settings::GetModSetting("CustomStringColors") == 3) { // If you want the color testing menu to work
		if (saveDefaults) {
			defaultColors.clear();
			for (int idx = 0; idx < 17; idx++) {
				InitStrings(stringsTest, (idx * 0x18 + 0x350));

				std::vector<RSColor> defaults;
				for (int i = 0; i < 6; i++)
					defaults.push_back(*(RSColor*)stringsTest[i]);

				defaultColors.push_back(defaults);
			}

			saveDefaults = false;
		}

		// Restore previously saved defaults.
		if (restoreDefaults) {
			for (int idx = 0; idx < 17; idx++) {
				InitStrings(stringsTest, (idx * 0x18 + 0x350));

				for (int i = 0; i < 6; i++)
					*(RSColor*)stringsTest[i] = defaultColors[idx][i];
			}

			restoreDefaults = false;
		}
		else {
			stringsTest.clear();
			InitStrings(stringsTest, (currentOffsetIdx * 0x18 + 0x350));

			if (currColor == 0)
				SetColors(stringsTest, colorsBlack);
			else
				SetColors(stringsTest, colorsWhite);
		}

		InitStrings(stringsTest, Text);
		SetColors(stringsTest, colorsTest);
		LOG_INFO("Set custom colors" << std::endl);
	}
}

/// <summary>
/// Toggle RainbowEnabled on / off.
/// </summary>
void ERMode::ToggleRainbowMode() {
	RainbowEnabled = !RainbowEnabled;
}

/// <summary>
/// Toggle RainbowNotesEnabled on / off.
/// </summary>
void ERMode::ToggleRainbowNotes() {
	RainbowNotesEnabled = !RainbowNotesEnabled;
}

/// <summary>
/// RainbowEnabled Middleware.
/// </summary>
bool ERMode::IsRainbowEnabled() {
	return RainbowEnabled;
}

/// <summary>
/// RainbowNotesEnabled Middleware.
/// </summary>
bool ERMode::IsRainbowNotesEnabled() {
	return RainbowNotesEnabled;
}

/// <summary>
/// Toggle on Rainbow Strings / Rainbow Notes
/// </summary>
void ERMode::DoRainbow() {
	if (!RainbowEnabled && !RainbowNotesEnabled)
		return;

	std::vector<uintptr_t> stringsEnabled;
	std::vector<uintptr_t> stringsHigh;
	std::vector<uintptr_t> stringsDisabled;
	std::vector<RSColor> oldEnabledColors, oldHigh, oldDisabledColors;

	// Get original string colors.
	for (int i = 0; i < 6; i++) {
		stringsEnabled.push_back(GetStringColor(i, Enabled));
		stringsHigh.push_back(GetStringColor(i, Glow));
		stringsDisabled.push_back(GetStringColor(i, Disabled));
	}

	// Start with Red.
	RSColor c;
	c.r = 1.f;
	c.g = 0.f;
	c.b = 0.f;

	float h = 0.f;
	float speed = 2.f;
	float stringOffset = 20.f;
	bool didWeUseRainbowStrings = false; // If we don't use this, then the strings won't reset to default colors unless you end with rainbow strings.
	while (RainbowEnabled || RainbowNotesEnabled) {
		// Increment Hue by the speed we are trying to mimick.
		h += speed;

		// Hue can only be a value from 0-360, so reset it to 0 if it's over 360.
		if (h >= 360.f) { h = 0.f; }

		// For each string
		for (int i = 0; i < 6; i++) {
			// Save the previous colors
			if (RainbowEnabled) {
				oldEnabledColors.push_back(*(RSColor*)stringsEnabled[i]);
				oldHigh.push_back(*(RSColor*)stringsHigh[i]);
				oldDisabledColors.push_back(*(RSColor*)stringsDisabled[i]);
				didWeUseRainbowStrings = true;
			}
			
			int newH = h + (stringOffset * i);
			c.setH(newH);

			// Hue only goes from 0-360.
			if (newH > 360)
				newH -= 360;

			// Since we only make textures for 180 possibilities, we can narrow out results down to 180 different values (0-179).
			if (newH > 4)
				customNoteColorH = (newH / 2) - 1;
			else
				customNoteColorH = 1;

			// Set the new rainbow colors.
			if (RainbowEnabled) {
				*(RSColor*)stringsEnabled[i] = c;
				*(RSColor*)stringsHigh[i] = c;
				*(RSColor*)stringsDisabled[i] = c;
			}

			// Reset back to the original colors.
			if (!RainbowEnabled && didWeUseRainbowStrings) {
				if (oldEnabledColors.size() == 0)
					return;

				for (int i = 0; i < 6; i++) {
					*(RSColor*)stringsEnabled[i] = oldEnabledColors[i];
					*(RSColor*)stringsHigh[i] = oldHigh[i];
					*(RSColor*)stringsDisabled[i] = oldDisabledColors[i];
				}
			}
		}
		Sleep(16);
	}
}


/* IN CASE THIS IS EVER NEEDED */

/// <summary>
/// Do these colors match
/// </summary>
/// <param name="strings"> - List of strings</param>
/// <param name="R"> - Red (Color to look for)</param>
/// <param name="G"> - Green (Color to look for)</param>
/// <param name="B"> - Blue (Color to look for)</param>
/// <returns></returns>
bool IsMatch(std::vector<uintptr_t> strings, int R, int G, int B) {
	if (strings[0] == NULL)
		return false;

	int r = (int)std::round((*(RSColor*)strings[0]).r * 255); // Remember, just casting to int doesn't round up :(
	int g = (int)std::round((*(RSColor*)strings[0]).g * 255);
	int b = (int)std::round((*(RSColor*)strings[0]).b * 255);
	if (R == 63)
		LOG_INFO(std::dec << R << " " << r << " " << (R == r) << " " << G << " " << g << " " << (G == g) << " " << B << " " << b << " " << (B == b) << std::endl);

	if (R == r && G == g && B == b)
		return true;
	return false;
}
