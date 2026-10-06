#include "stdafx.h"
#include "Settings.hpp"
#include <atomic>

namespace
{
	std::atomic<bool> noteByNoteDetectionVisible{ true };
	std::atomic<bool> noteByNoteCustomColours{ false };
	std::atomic<int> noteByNoteUiSize{ 100 };
	std::atomic<int> noteByNoteTargetSize{ 150 };
	std::atomic<Settings::NoteByNoteTargetPosition> noteByNoteTargetPosition{ Settings::NoteByNoteTargetPosition::Left };
	std::atomic<uint32_t> noteByNoteColors[] = { 0xFFFFFFFF, 0xFF55DD77, 0xFFFFAA44, 0xFFFF5555 };
	std::atomic<int> noteByNoteLineSpacing{ 100 };
	std::atomic<Settings::NoteByNoteTargetStyle> noteByNoteTargetStyle{ Settings::NoteByNoteTargetStyle::Detailed };
	const char* const kNoteByNoteTargetStyles[] = { "Detailed", "Simple", "Tab" };
	// HUD block placements in 1/10000ths of the window (-1 = built-in layout); index = NoteByNoteHudBlock.
	// Packed x << 16 | y so a reader never sees a new x with an old y.
	std::atomic<uint32_t> noteByNotePlacements[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
	const char* const kNoteByNotePlacementKeys[2] = { "NoteByNoteReadoutPlacement", "NoteByNoteTargetPlacement" };

	uint32_t PackPlacement(Settings::HudPlacement placement)
	{
		if (!placement.IsSet()) return 0xFFFFFFFFu;
		const auto unit = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 10000.0f + 0.5f); };
		return (unit(placement.x) << 16) | unit(placement.y);
	}
}

namespace
{
	// Values changed at runtime by the in-game overlay (keybinds, Drop Pedal overlay options). The upstream settings
	// maps (modSettings / customSettings) are plain std::maps read by several threads without a lock; upstream only
	// rebuilds them on "update all", and that race is inherited. Our overlay writes must not add to it, so they never
	// touch those maps: they go here, under our own mutex, and to RSMods.ini (surgical per-key write). The readers
	// below check this table first. A reload from the ini (UpdateSettings) clears it, since the ini is then current.
	std::mutex runtimeOverrideMutex;
	std::map<std::string, std::string, std::less<>> runtimeOverrides;

	bool TryRuntimeOverride(const std::string& name, std::string& value)
	{
		std::lock_guard<std::mutex> guard(runtimeOverrideMutex);
		const auto found = runtimeOverrides.find(name);
		if (found == runtimeOverrides.end()) return false;
		value = found->second;
		return true;
	}

	void SetRuntimeOverride(const std::string& name, const std::string& value)
	{
		std::lock_guard<std::mutex> guard(runtimeOverrideMutex);
		runtimeOverrides[name] = value;
	}
}

bool Settings::IsNoteByNoteDetectionVisible()
{
	return noteByNoteDetectionVisible.load();
}

bool Settings::SetNoteByNoteDetectionVisible(bool visible)
{
	char executablePath[MAX_PATH]{};
	GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
	const auto iniPath = (std::filesystem::path(executablePath).parent_path() / "RSMods.ini").string();
	if (!WritePrivateProfileStringA("Note by Note", "NoteByNoteDetectionOverlay", visible ? "on" : "off", iniPath.c_str()))
	{
		LOG_ERROR("[SETTINGS] Could not persist NoteByNoteDetectionOverlay" << std::endl);
		return false;
	}
	noteByNoteDetectionVisible.store(visible);
	return true;
}

bool Settings::SetNoteByNoteTargetPosition(NoteByNoteTargetPosition position)
{
	const char* value = position == NoteByNoteTargetPosition::Center ? "Center"
		: position == NoteByNoteTargetPosition::Custom ? "Custom" : "Left";
	char executablePath[MAX_PATH]{};
	GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
	const auto iniPath = (std::filesystem::path(executablePath).parent_path() / "RSMods.ini").string();
	if (!WritePrivateProfileStringA("Note by Note", "NoteByNoteTargetPosition", value, iniPath.c_str()))
	{
		LOG_ERROR("[SETTINGS] Could not persist NoteByNoteTargetPosition" << std::endl);
		return false;
	}
	noteByNoteTargetPosition.store(position);
	return true;
}

int Settings::GetNoteByNoteUiSize()
{
	return noteByNoteUiSize.load();
}

int Settings::GetNoteByNoteTargetSize()
{
	return noteByNoteTargetSize.load();
}

Settings::NoteByNoteTargetPosition Settings::GetNoteByNoteTargetPosition()
{
	return noteByNoteTargetPosition.load();
}

NoteByNote::DetectionPalette Settings::GetNoteByNoteDetectionPalette()
{
	if (!noteByNoteCustomColours.load()) return {};
	return { noteByNoteColors[0].load(), noteByNoteColors[1].load(),
		noteByNoteColors[2].load(), noteByNoteColors[3].load() };
}

namespace
{
	// Same keys, section and formats ReadModSettings loads, so an overlay edit survives the next launch and the
	// desktop GUI shows it. Surgical per-key writes (never a whole-file rewrite) so nothing else in RSMods.ini moves.
	const char* const kNoteByNoteColorKeys[] = { "NoteByNoteNeutralColor", "NoteByNoteConfirmedColor", "NoteByNotePartialColor", "NoteByNoteRejectedColor" };
	const uint32_t kNoteByNoteColorDefaults[] = { 0xFFFFFF, 0x55DD77, 0xFFAA44, 0xFF5555 };

	bool WriteNoteByNoteKey(const char* key, const std::string& value)
	{
		char executablePath[MAX_PATH]{};
		GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
		const auto iniPath = (std::filesystem::path(executablePath).parent_path() / "RSMods.ini").string();
		if (WritePrivateProfileStringA("Note by Note", key, value.c_str(), iniPath.c_str())) return true;
		LOG_ERROR("[SETTINGS] Could not persist " << key << std::endl);
		return false;
	}

	std::string HexColor(uint32_t rgb)
	{
		char text[8]{};
		sprintf_s(text, "%06X", rgb & 0xFFFFFFu);
		return text;
	}
}

bool Settings::IsNoteByNoteCustomColours()
{
	return noteByNoteCustomColours.load();
}

bool Settings::SetNoteByNoteCustomColours(bool enabled)
{
	noteByNoteCustomColours.store(enabled);
	return WriteNoteByNoteKey("NoteByNoteCustomColours", enabled ? "on" : "off");
}

uint32_t Settings::GetNoteByNoteColor(int index)
{
	if (index < 0 || index > 3) return 0xFFFFFF;
	return noteByNoteColors[index].load() & 0xFFFFFFu;
}

bool Settings::SetNoteByNoteColor(int index, uint32_t rgb, bool persist)
{
	if (index < 0 || index > 3) return false;
	noteByNoteColors[index].store(0xFF000000u | (rgb & 0xFFFFFFu));
	return !persist || WriteNoteByNoteKey(kNoteByNoteColorKeys[index], HexColor(rgb));
}

bool Settings::ResetNoteByNoteColors()
{
	bool saved = true;
	for (int index = 0; index < 4; ++index)
		saved = SetNoteByNoteColor(index, kNoteByNoteColorDefaults[index], true) && saved;
	return saved;
}

bool Settings::SetNoteByNoteUiSize(int percent, bool persist)
{
	percent = std::clamp(percent, 50, 300);   // the range ReadModSettings accepts
	noteByNoteUiSize.store(percent);
	return !persist || WriteNoteByNoteKey("NoteByNoteUiSize", std::to_string(percent));
}

bool Settings::SetDropPedalSetting(const char* modKey, const char* iniKey, const std::string& value, bool persist)
{
	SetRuntimeOverride(modKey, value);   // never the shared map (see runtimeOverrides)
	if (!persist) return true;
	char executablePath[MAX_PATH]{};
	GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
	const auto iniPath = (std::filesystem::path(executablePath).parent_path() / "RSMods.ini").string();
	if (WritePrivateProfileStringA("Drop Pedal", iniKey, value.c_str(), iniPath.c_str())) return true;
	LOG_ERROR("[SETTINGS] Could not persist Drop Pedal " << iniKey << std::endl);
	return false;
}

bool Settings::SetNoteByNoteTargetSize(int percent, bool persist)
{
	percent = std::clamp(percent, 50, 300);
	noteByNoteTargetSize.store(percent);
	return !persist || WriteNoteByNoteKey("NoteByNoteTargetSize", std::to_string(percent));
}

Settings::HudPlacement Settings::GetNoteByNoteHudPlacement(NoteByNoteHudBlock block)
{
	const uint32_t packed = noteByNotePlacements[static_cast<int>(block)].load();
	if (packed == 0xFFFFFFFFu) return {};
	return { (packed >> 16) / 10000.0f, (packed & 0xFFFFu) / 10000.0f };
}

bool Settings::SetNoteByNoteHudPlacement(NoteByNoteHudBlock block, HudPlacement placement, bool persist)
{
	const int index = static_cast<int>(block);
	noteByNotePlacements[index].store(PackPlacement(placement));
	if (!persist) return true;
	std::string value = "Default";
	if (placement.IsSet())
	{
		const HudPlacement stored = GetNoteByNoteHudPlacement(block);
		char text[32]{};
		sprintf_s(text, "%.4f,%.4f", stored.x, stored.y);
		value = text;
	}
	return WriteNoteByNoteKey(kNoteByNotePlacementKeys[index], value);
}

int Settings::GetNoteByNoteLineSpacing()
{
	return noteByNoteLineSpacing.load();
}

bool Settings::SetNoteByNoteLineSpacing(int percent, bool persist)
{
	percent = std::clamp(percent, 50, 300);
	noteByNoteLineSpacing.store(percent);
	return !persist || WriteNoteByNoteKey("NoteByNoteLineSpacing", std::to_string(percent));
}

Settings::NoteByNoteTargetStyle Settings::GetNoteByNoteTargetStyle()
{
	return noteByNoteTargetStyle.load();
}

bool Settings::SetNoteByNoteTargetStyle(NoteByNoteTargetStyle style)
{
	noteByNoteTargetStyle.store(style);
	return WriteNoteByNoteKey("NoteByNoteTargetStyle", kNoteByNoteTargetStyles[static_cast<int>(style)]);
}

/// <summary>
/// Load Default Settings.
/// Used if the user has the DLL but no INI.
/// </summary>
void Settings::Initialize()
{
	modSettings = {
		{"CustomSongListTitles", "K"},
		{"ToggleLoftKey", "T"},
		{"ShowSongTimerKey", "S"},
		{"ForceReEnumerationKey", "F"},
		{"RainbowStringsKey", "V"},
		{"RainbowNotesKey", "N"},
		{"RemoveLyricsKey", "L"},
		{"RRSpeedKey", "R"},
		{"MenuToggleKey", "M"},
		{"TuningOffsetKey", "O"},
		{"ToggleExtendedRangeKey", "E"},
		{"LoopStartKey", "Y"},
		{"LoopEndKey", "U"},
		{"RewindKey", "Z"},
		{"DropPedalPitchDownKey", "VK_OEM_COMMA"},
		{"DropPedalPitchUpKey", "VK_OEM_PERIOD"},
		{"DropPedalToggleKey", "VK_F7"},
		{"DropPedalBaseTuningKey", "VK_F9"},
		{"RecordingHotkey", "VK_F6"},

		{"MasterVolumeKey", "5"},
		{"SongVolumeKey", "6"},
		{"Player1VolumeKey", "7"},
		{"Player2VolumeKey", "8"},
		{"MicrophoneVolumeKey", "9"},
		{"VoiceOverVolumeKey", "0"},
		{"SFXVolumeKey", "S"},
		{"DisplayMixerKey", "P"},
		{"MutePlayer1Key", "X"},
		{"MutePlayer2Key", "C"},

		{"ForceReEnumerationEnabled", "automatic"},

		{"ToggleLoftEnabled", "off"},
		{"VolumeControlEnabled", "off"},
		{"ShowSongTimerEnabled", "on"},
		{"ForceReEnumerationEnabled", "off"},
		{"RainbowStringsEnabled", "off"},
		{"ExtendedRangeEnabled", "on"},
		{"ExtendedRangeDropTuning", "off"},
		{"ExtendedRangeFixBassTuning", "off"},
		{"SeparateNoteColors", "off"},
		{"DiscoModeEnabled", "off"},
		{"RemoveHeadstockEnabled", "off"},
		{"RemoveSkylineEnabled", "off"},
		{"GreenScreenWallEnabled", "off"},
		{"ForceProfileEnabled", "off"},
		{"FretlessModeEnabled", "off"},
		{"RemoveInlaysEnabled", "off"},
		{"ToggleLoftWhen", "manual"},
		{"ToggleSkylineWhen", "song"},
		{"RemoveLaneMarkersEnabled", "off"},
		{"RemoveLyrics", "off"},
		{"RemoveLyricsWhen", "manual"},
		{"GuitarSpeak", "off"},
		{"GuitarSpeakWhileTuning", "off"},
		{"RemoveHeadstockWhen", "song"},
		{"ScreenShotScores", "off"},
		{"RRSpeedAboveOneHundred", "off"},
		{"AutoTuneForSong", "off"},
		{"AutoTuneForSongDevice", ""},
		{"MidiInDevice", ""},
		{"AutoTuneForSongWhen", "manual"},
		{"AutoTuneForSoftwareSemitoneSettings", ""},
		{"AutoTuneForSoftwareSemitoneTriggers", ""},
		{"AutoTuneForSoftwareTrueTuningSettings", ""},
		{"AutoTuneForSoftwareTrueTuningTriggers", ""},
		{"ChordsMode", "off"},
		{"ShowCurrentNoteOnScreen", "off"},
		{"OnScreenFont", "Arial"},
		{"ProfileToLoad", ""},
		{"ShowSongTimerWhen", "manual"},
		{"ShowSelectedVolumeWhen", "manual"},
		{"SecondaryMonitor", "off"},
		{"SongPreviews", "off"},
		{"OverrideInputVolumeEnabled", "off"},
		{"OverrideInputVolumeDevice", ""},
		{"AllowAudioInBackground", "off"},
		{"BypassTwoRTCMessageBox", "off"},
		{"LinearRiffRepeater", "off"},
		{"AltOutputSampleRate", "off"},
		{"AllowLooping", "off"},
		{"AllowRewind", "off"},
		{"FixOculusCrash", "off"},
		{"FixBrokenTones", "off"},
		{"UseCustomNSPTimer", "off"},
		{"DisplayCurrentAccuracy", "off"},
		{"PreventMidSongPause", "off"},
		{"RemoveFingerprints", "off"},
		{"EnableDropPedal", "on"},   // ships with the mod; toggled in game with F7. "off" is a revert switch only.
		{"DropPedalShowOverlay", "on"},
		{"DropPedalCustomOverlayColors", "off"},
		{"DropPedalOverlayDownColor", "6BE06B"},
		{"DropPedalOverlayUpColor", "FFC24D"},
		{"DropPedalOverlayStatusColor", "FFFFFF"},
	};

	customSettings = {
		{"ExtendedRangeMode", -5},
		{"CheckForNewSongsInterval", 5000},
		{"RRSpeedInterval", 0},
		{"TuningPedal", 0},
		{"TuningOffset", 0},
		{"VolumeControlInterval", 5},
		{"SecondaryMonitorXPosition", 0},
		{"SecondaryMonitorYPosition", 0},
		{"SeparateNoteColorsMode", 0},
		{"OverrideInputVolume", 17},
		{"AsioInputGain", 0},
		{"NoiseGateThreshold", 0},
		{"CompressorStrength", 0},
		{"HumFilter", 0},
		{"RocksmithGateOverride", 0},
		{"RocksmithGateThreshold", -593},
		{"AudioBridgeLimiter", 0},
		{"AudioBridgeLimiterLevel", -60},
		{"AudioBridgeLoudnessMatch", 0},
		{"AudioBridgeLoudnessTarget", -200},
		{"CustomStringColors", 0},
		{"AlternativeOutputSampleRate", 48000},
		{"LoopingLeadUp", 0},
		{"RewindBy", 0},
		{"RewindLeadup", 0},
		{"CustomNSPTimeLimit", 10000},
		{"OnScreenFontSize", 24},

		{"GuitarSpeakDelete", 0},
		{"GuitarSpeakSpace", 0},
		{"GuitarSpeakEnter", 0},
		{"GuitarSpeakTab", 0},
		{"GuitarSpeakPageUp", 0},
		{"GuitarSpeakPageDown", 0},
		{"GuitarSpeakUpArrow", 0},
		{"GuitarSpeakDownArrow", 0},
		{"GuitarSpeakEscape", 0},
		{"GuitarSpeakClose", 0},
		{"GuitarSpeakOBracket", 0},
		{"GuitarSpeakCBracket", 0},
		{"GuitarSpeakTildea", 0},
		{"GuitarSpeakForSlash", 0},
		{"GuitarSpeakAlt", 0}
	};

	twitchSettings = {
		{"RainbowStrings", "off"},
		{"RemoveNotes", "off"},
		{"TransparentNotes", "off"},
		{"SolidNotes", "off"},
		{"DrunkMode", "off"},
		{"FYourFC", "off"},
	};
}


// Read INI

/// <summary>
/// Parse INI for SongLists
/// </summary>
/// <returns>List of Custom Songlist Names</returns>
std::vector<std::string> Settings::GetCustomSongTitles() {
	std::vector<std::string> retList(20);
	CSimpleIniA reader;
	if (reader.LoadFile("RSMods.ini") < 0)
		return retList;

	for (int i = 0; i < 20; i++) {
		std::string songListName = "SongListTitle_" + std::to_string(i + 1);
		retList[i] = reader.GetValue("SongListTitles", songListName.c_str(), "SONG LIST");
	}

	return retList;
}

/// <summary>
/// Parse Mod / Volume Keybind Toggles
/// </summary>
void Settings::ReadKeyBinds() {
	CSimpleIniA reader;
	if (reader.LoadFile("RSMods.ini") < 0) {
		LOG_ERROR("Error reading saved settings" << std::endl);
		return;
	}

	modSettings = {
		{ "ToggleLoftKey", reader.GetValue("Keybinds", "ToggleLoftKey", "T") },
		{ "CustomSongListTitles", reader.GetValue("Keybinds", "CustomSongListTitles", "K")},
		{ "ShowSongTimerKey", reader.GetValue("Keybinds", "ShowSongTimerKey", "N")},
		{ "ForceReEnumerationKey", reader.GetValue("Keybinds", "ForceReEnumerationKey", "F")},
		{ "MenuToggleKey", reader.GetValue("Keybinds", "MenuToggleKey", "M")},
		{ "RainbowStringsKey", reader.GetValue("Keybinds", "RainbowStringsKey", "V")},
		{ "RainbowNotesKey", reader.GetValue("Keybinds", "RainbowNotesKey", "N")},
		{ "RemoveLyricsKey", reader.GetValue("Keybinds", "RemoveLyricsKey", "L")},
		{ "RRSpeedKey", reader.GetValue("Keybinds", "RRSpeedKey", "R")},
		{ "TuningOffsetKey", reader.GetValue("Keybinds", "TuningOffsetKey", "O")},
		{ "ToggleExtendedRangeKey", reader.GetValue("Keybinds", "ToggleExtendedRangeKey", "E")},
		{ "LoopStartKey", reader.GetValue("Keybinds", "LoopStartKey", "Y")},
		{ "LoopEndKey", reader.GetValue("Keybinds", "LoopEndKey", "U")},
		{ "RewindKey", reader.GetValue("Keybinds", "RewindKey", "Z")},
		{ "DropPedalPitchDownKey", reader.GetValue("Keybinds", "DropPedalPitchDownKey", "VK_OEM_COMMA")},
		{ "DropPedalPitchUpKey", reader.GetValue("Keybinds", "DropPedalPitchUpKey", "VK_OEM_PERIOD")},
		{ "DropPedalToggleKey", reader.GetValue("Keybinds", "DropPedalToggleKey", "VK_F7")},
		{ "DropPedalBaseTuningKey", reader.GetValue("Keybinds", "DropPedalBaseTuningKey", "VK_F9")},
		{ "RecordingHotkey", reader.GetValue("Keybinds", "RecordingHotkey", "VK_F6")},

		{ "MasterVolumeKey", reader.GetValue("Audio Keybindings", "MasterVolumeKey", "5") },
		{ "SongVolumeKey", reader.GetValue("Audio Keybindings", "SongVolumeKey", "6") },
		{ "Player1VolumeKey", reader.GetValue("Audio Keybindings", "Player1VolumeKey", "7") },
		{ "Player2VolumeKey", reader.GetValue("Audio Keybindings", "Player2VolumeKey", "8") },
		{ "MicrophoneVolumeKey", reader.GetValue("Audio Keybindings", "MicrophoneVolumeKey", "9") },
		{ "VoiceOverVolumeKey", reader.GetValue("Audio Keybindings", "VoiceOverVolumeKey", "0") },
		{ "SFXVolumeKey", reader.GetValue("Audio Keybindings", "SFXVolumeKey", "S") },
		{ "DisplayMixerKey", reader.GetValue("Audio Keybindings", "DisplayMixerKey", "P") },
		{ "MutePlayer1Key", reader.GetValue("Audio Keybindings", "MutePlayer1Key", "X")},
		{ "MutePlayer2Key", reader.GetValue("Audio Keybindings", "MutePlayer2Key", "C")}
	};

	// A recording hotkey on the Drop Pedal base-tuning key (older configs defaulted both to F9) would make
	// every press toggle a recording as well. Base tuning keeps its key; the recording key moves to F6 (the
	// default; F8 has known issues, F11 is the game's fullscreen key) and is saved, or is left unbound if
	// base tuning itself is on F6. The overlay's Record page can rebind it.
	if (modSettings["RecordingHotkey"] == modSettings["DropPedalBaseTuningKey"]) {
		const bool f6Free = modSettings["DropPedalBaseTuningKey"] != "VK_F6";
		LOG_WARNING("RecordingHotkey " << modSettings["RecordingHotkey"] << " collides with DropPedalBaseTuningKey; recording "
			<< (f6Free ? "moved to VK_F6." : "left unbound; pick a key in the overlay's Record page.") << std::endl);
		if (f6Free) SetKeyBind("RecordingHotkey", 0x75, true);   // VK_F6
		else modSettings["RecordingHotkey"] = "";
	}
}

/// <summary>
/// Parse Settings For Mods
/// </summary>
void Settings::ReadModSettings() {
	CSimpleIniA reader;
	if (reader.LoadFile("RSMods.ini") < 0) {
		LOG_ERROR("Error reading saved settings" << std::endl);
		return;
	}

	customSettings = {
		{"ExtendedRangeMode", reader.GetLongValue("Mod Settings", "ExtendedRangeModeAt", -5)},
		{"CheckForNewSongsInterval", reader.GetLongValue("Mod Settings", "CheckForNewSongsInterval", 5000)},
		{"RRSpeedInterval", reader.GetLongValue("Mod Settings", "RRSpeedInterval", 0)},
		{"TuningPedal", reader.GetLongValue("Mod Settings", "TuningPedal", 0)},
		{"TuningOffset", reader.GetLongValue("Mod Settings", "TuningOffset", 0)},
		{"VolumeControlInterval", reader.GetLongValue("Mod Settings", "VolumeControlInterval", 5)},
		{"SecondaryMonitorXPosition", reader.GetLongValue("Mod Settings", "SecondaryMonitorXPosition", 0)},
		{"SecondaryMonitorYPosition", reader.GetLongValue("Mod Settings", "SecondaryMonitorYPosition", 0)},
		{"SeparateNoteColorsMode", reader.GetLongValue("Mod Settings", "SeparateNoteColorsMode", 0)}, // 0 = same as strings, 1 = default, 2 = custom
		{"CustomStringColors", reader.GetLongValue("Toggle Switches", "CustomStringColors", 0)}, //0 = default, 1 = Zag, 2 = custom colors
		{"OverrideInputVolume", reader.GetLongValue("Mod Settings", "OverrideInputVolume", 17)}, // 17 is what Rocksmith calls default.
		{"AsioInputGain", reader.GetLongValue("Mod Settings", "AsioInputGain", 0)}, // tenths of a dB of guitar input make-up gain (0 = off)
		{"NoiseGateThreshold", reader.GetLongValue("Mod Settings", "NoiseGateThreshold", 0)}, // guitar input suppressor open threshold in tenths of a dB (0 = off, else negative)
		{"CompressorStrength", reader.GetLongValue("Mod Settings", "CompressorStrength", 0)}, // guitar input compressor strength, 0-100 (0 = off); flattens string-beat wobble before the game amp
		{"HumFilter", reader.GetLongValue("Mod Settings", "HumFilter", 0)}, // mains-hum notch base frequency (0 = off, else 50 or 60); notches out the 50/60 Hz ground-loop hum comb on a grounded interface
		{"RocksmithGateOverride", reader.GetLongValue("Mod Settings", "RocksmithGateOverride", 0)}, // 1 = take over the game's own amp noise gate (P1_NoiseFloor); 0 = leave the game's calibrated gate alone
		{"RocksmithGateThreshold", reader.GetLongValue("Mod Settings", "RocksmithGateThreshold", -593)}, // forced P1_NoiseFloor in tenths of a dB while the override is on (-593 = game default; lower opens the gate for longer sustain)
		{"AudioBridgeLimiter", reader.GetLongValue("Mod Settings", "AudioBridgeLimiter", 0)}, // output limiter / safety ceiling (0 = off, 1 = on)
		{"AudioBridgeLimiterLevel", reader.GetLongValue("Mod Settings", "AudioBridgeLimiterLevel", -60)}, // limiter ceiling in tenths of a dBFS (-60 = -6.0 dBFS); nothing leaves above this
		{"AudioBridgeLoudnessMatch", reader.GetLongValue("Mod Settings", "AudioBridgeLoudnessMatch", 0)}, // loudness equalisation / AGC (0 = off, 1 = on)
		{"AudioBridgeLoudnessTarget", reader.GetLongValue("Mod Settings", "AudioBridgeLoudnessTarget", -200)}, // AGC target loudness in tenths of a dBFS RMS (-200 = -20.0 dBFS)
		{"AlternativeOutputSampleRate", reader.GetLongValue("Mod Settings", "AlternativeOutputSampleRate", 48000)},
		{"LoopingLeadUp", reader.GetLongValue("Mod Settings", "LoopingLeadUp", 0)},
		{"RewindBy", reader.GetLongValue("Mod Settings", "RewindBy", 0)},
		{"RewindLeadup", reader.GetLongValue("Mod Settings", "RewindLeadup", 0)},
		{"CustomNSPTimeLimit", reader.GetLongValue("Mod Settings", "CustomNSPTimeLimit", 10000)},
		{"OnScreenFontSize", reader.GetLongValue("Mod Settings", "OnScreenFontSize", 24)},

		{"GuitarSpeakDelete", reader.GetLongValue("Guitar Speak", "GuitarSpeakDeleteWhen", 0)},
		{"GuitarSpeakSpace", reader.GetLongValue("Guitar Speak", "GuitarSpeakSpaceWhen", 0)},
		{"GuitarSpeakEnter", reader.GetLongValue("Guitar Speak", "GuitarSpeakEnterWhen", 0)},
		{"GuitarSpeakTab", reader.GetLongValue("Guitar Speak", "GuitarSpeakTabWhen", 0)},
		{"GuitarSpeakPageUp", reader.GetLongValue("Guitar Speak", "GuitarSpeakPGUPWhen", 0)},
		{"GuitarSpeakPageDown", reader.GetLongValue("Guitar Speak", "GuitarSpeakPGDNWhen", 0)},
		{"GuitarSpeakUpArrow", reader.GetLongValue("Guitar Speak", "GuitarSpeakUPWhen", 0)},
		{"GuitarSpeakDownArrow", reader.GetLongValue("Guitar Speak", "GuitarSpeakDNWhen", 0)},
		{"GuitarSpeakEscape", reader.GetLongValue("Guitar Speak", "GuitarSpeakESCWhen", 0)},
		{"GuitarSpeakClose", reader.GetLongValue("Guitar Speak", "GuitarSpeakCloseWhen", 0)},
		{"GuitarSpeakOBracket", reader.GetLongValue("Guitar Speak", "GuitarSpeakOBracketWhen", 0)},
		{"GuitarSpeakCBracket", reader.GetLongValue("Guitar Speak", "GuitarSpeakCBracketWhen", 0)},
		{"GuitarSpeakTildea", reader.GetLongValue("Guitar Speak", "GuitarSpeakTildeaWhen", 0)},
		{"GuitarSpeakForSlash", reader.GetLongValue("Guitar Speak", "GuitarSpeakForSlashWhen", 0)},
		{"GuitarSpeakAlt", reader.GetLongValue("Guitar Speak", "GuitarSpeakAltWhen", 0)},
	};

	// Mods Enabled / Disabled
	modSettings["ToggleLoftEnabled"] = reader.GetValue("Toggle Switches", "ToggleLoft", "on");
	modSettings["VolumeControlEnabled"] = reader.GetValue("Toggle Switches", "VolumeControl", "off");
	modSettings["ShowSongTimerEnabled"] = reader.GetValue("Toggle Switches", "ShowSongTimer", "off");
	modSettings["ForceReEnumerationEnabled"] = reader.GetValue("Toggle Switches", "ForceReEnumeration", "automatic");
	modSettings["RainbowStringsEnabled"] = reader.GetValue("Toggle Switches", "RainbowStrings", "off");
	modSettings["RainbowNotesEnabled"] = reader.GetValue("Toggle Switches", "RainbowNotes", "off");
	modSettings["ExtendedRangeEnabled"] = reader.GetValue("Toggle Switches", "ExtendedRange", "off");
	modSettings["ExtendedRangeDropTuning"] = reader.GetValue("Toggle Switches", "ExtendedRangeDropTuning", "off");
	modSettings["ExtendedRangeFixBassTuning"] = reader.GetValue("Toggle Switches", "ExtendedRangeFixBassTuning", "off");
	modSettings["SeparateNoteColors"] = reader.GetValue("Toggle Switches", "SeparateNoteColors", "off");
	modSettings["DiscoModeEnabled"] = reader.GetValue("Toggle Switches", "DiscoMode", "off");
	modSettings["RemoveHeadstockEnabled"] = reader.GetValue("Toggle Switches", "Headstock", "off");
	modSettings["RemoveSkylineEnabled"] = reader.GetValue("Toggle Switches", "Skyline", "off");
	modSettings["GreenScreenWallEnabled"] = reader.GetValue("Toggle Switches", "GreenScreenWall", "off");
	modSettings["ForceProfileEnabled"] = reader.GetValue("Toggle Switches", "ForceProfileLoad", "off");
	modSettings["FretlessModeEnabled"] = reader.GetValue("Toggle Switches", "Fretless", "off");
	modSettings["RemoveInlaysEnabled"] = reader.GetValue("Toggle Switches", "Inlays", "off");
	modSettings["ToggleLoftWhen"] = reader.GetValue("Toggle Switches", "ToggleLoftWhen", "manual");
	modSettings["ToggleSkylineWhen"] = reader.GetValue("Toggle Switches", "ToggleSkylineWhen", "song");
	modSettings["RemoveLaneMarkersEnabled"] = reader.GetValue("Toggle Switches", "LaneMarkers", "off");
	modSettings["RemoveLyrics"] = reader.GetValue("Toggle Switches", "Lyrics", "off");
	modSettings["RemoveLyricsWhen"] = reader.GetValue("Toggle Switches", "RemoveLyricsWhen", "manual");
	modSettings["GuitarSpeak"] = reader.GetValue("Toggle Switches", "GuitarSpeak", "off");
	modSettings["GuitarSpeakWhileTuning"] = reader.GetValue("Guitar Speak", "GuitarSpeakWhileTuning", "off");
	modSettings["RemoveHeadstockWhen"] = reader.GetValue("Toggle Switches", "RemoveHeadstockWhen", "song");
	modSettings["ScreenShotScores"] = reader.GetValue("Toggle Switches", "ScreenShotScores", "off");
	modSettings["RRSpeedAboveOneHundred"] = reader.GetValue("Toggle Switches", "RRSpeedAboveOneHundred", "off");
	modSettings["AutoTuneForSong"] = reader.GetValue("Toggle Switches", "AutoTuneForSong", "off");
	modSettings["AutoTuneForSongDevice"] = reader.GetValue("Toggle Switches", "AutoTuneForSongDevice", "");
	modSettings["MidiInDevice"] = reader.GetValue("Toggle Switches", "MidiInDevice", "");
	modSettings["AutoTuneForSongWhen"] = reader.GetValue("Toggle Switches", "AutoTuneForSongWhen", "manual");
	modSettings["AutoTuneForSoftwareSemitoneSettings"] = reader.GetValue("Toggle Switches", "AutoTuneForSoftwareSemitoneSettings", "");
	modSettings["AutoTuneForSoftwareSemitoneTriggers"] = reader.GetValue("Toggle Switches", "AutoTuneForSoftwareSemitoneTriggers", "");
	modSettings["AutoTuneForSoftwareTrueTuningSettings"] = reader.GetValue("Toggle Switches", "AutoTuneForSoftwareTrueTuningSettings", "");
	modSettings["AutoTuneForSoftwareTrueTuningTriggers"] = reader.GetValue("Toggle Switches", "AutoTuneForSoftwareTrueTuningTriggers", "");
	modSettings["ChordsMode"] = reader.GetValue("Toggle Switches", "ChordsMode", "off");
	modSettings["ShowCurrentNoteOnScreen"] = reader.GetValue("Toggle Switches", "ShowCurrentNoteOnScreen", "off");
	modSettings["OnScreenFont"] = reader.GetValue("Toggle Switches", "OnScreenFont", "Arial");
	modSettings["ProfileToLoad"] = reader.GetValue("Toggle Switches", "ProfileToLoad", "");
	modSettings["CustomHighwayColors"] = reader.GetValue("Highway Colors", "CustomHighwayColors", "");
	modSettings["ShowSongTimerWhen"] = reader.GetValue("Toggle Switches", "ShowSongTimerWhen", "manual");
	modSettings["ShowSelectedVolumeWhen"] = reader.GetValue("Toggle Switches", "ShowSelectedVolumeWhen", "manual");
	modSettings["SecondaryMonitor"] = reader.GetValue("Toggle Switches", "SecondaryMonitor", "off");
	modSettings["SongPreviews"] = reader.GetValue("Toggle Switches", "SongPreviews", "off");
	modSettings["OverrideInputVolumeEnabled"] = reader.GetValue("Toggle Switches", "OverrideInputVolumeEnabled", "off");
	modSettings["OverrideInputVolumeDevice"] = reader.GetValue("Toggle Switches", "OverrideInputVolumeDevice", "");
	modSettings["AllowAudioInBackground"] = reader.GetValue("Toggle Switches", "AllowAudioInBackground", "off");
	modSettings["BypassTwoRTCMessageBox"] = reader.GetValue("Toggle Switches", "BypassTwoRTCMessageBox", "off");
	modSettings["LinearRiffRepeater"] = reader.GetValue("Toggle Switches", "LinearRiffRepeater", "off");
	modSettings["AltOutputSampleRate"] = reader.GetValue("Toggle Switches", "AltOutputSampleRate", "off");
	modSettings["AllowLooping"] = reader.GetValue("Toggle Switches", "AllowLooping", "off");
	modSettings["AllowRewind"] = reader.GetValue("Toggle Switches", "AllowRewind", "off");
	modSettings["FixOculusCrash"] = reader.GetValue("Toggle Switches", "FixOculusCrash", "off");
	modSettings["FixBrokenTones"] = reader.GetValue("Toggle Switches", "FixBrokenTones", "off");
	modSettings["UseCustomNSPTimer"] = reader.GetValue("Toggle Switches", "UseCustomNSPTimer", "off");
	modSettings["DisplayCurrentAccuracy"] = reader.GetValue("Toggle Switches", "DisplayCurrentAccuracy", "off");
	modSettings["PreventMidSongPause"] = reader.GetValue("Toggle Switches", "PreventMidSongPause", "off");
	modSettings["RemoveFingerprints"] = reader.GetValue("Toggle Switches", "RemoveFingerprints", "off");
	modSettings["EnableDropPedal"] = reader.GetValue("Drop Pedal", "EnableDropPedal", "on");
	modSettings["DropPedalShowOverlay"] = reader.GetValue("Drop Pedal", "ShowOverlay", "on");
	modSettings["DropPedalCustomOverlayColors"] = reader.GetValue("Drop Pedal", "CustomOverlayColors", "off");
	modSettings["DropPedalOverlayDownColor"] = reader.GetValue("Drop Pedal", "OverlayDownColor", "6BE06B");
	modSettings["DropPedalOverlayUpColor"] = reader.GetValue("Drop Pedal", "OverlayUpColor", "FFC24D");
	modSettings["DropPedalOverlayStatusColor"] = reader.GetValue("Drop Pedal", "OverlayStatusColor", "FFFFFF");

	noteByNoteCustomColours.store(std::string(reader.GetValue("Note by Note", "NoteByNoteCustomColours", "off")) == "on");
	noteByNoteDetectionVisible.store(std::string(reader.GetValue("Note by Note", "NoteByNoteDetectionOverlay", "on")) != "off");
	const std::string targetPosition = reader.GetValue("Note by Note", "NoteByNoteTargetPosition", "Left");
	if (targetPosition == "Left")
		noteByNoteTargetPosition.store(Settings::NoteByNoteTargetPosition::Left);
	else if (targetPosition == "Center")
		noteByNoteTargetPosition.store(Settings::NoteByNoteTargetPosition::Center);
	else if (targetPosition == "Custom")
		noteByNoteTargetPosition.store(Settings::NoteByNoteTargetPosition::Custom);
	else
		LOG_ERROR("Invalid NoteByNoteTargetPosition: expected Left, Center or Custom; keeping Left." << std::endl);
	const char* sizeKeys[] = { "NoteByNoteUiSize", "NoteByNoteTargetSize" };
	// Target text ships larger than the UI readout: at 100% the target label was too small to
	// read comfortably, so its default is 150%. The UI readout stays at 100%.
	const char* sizeDefaults[] = { "100", "150" };
	std::atomic<int>* sizes[] = { &noteByNoteUiSize, &noteByNoteTargetSize };
	for (unsigned index = 0; index < 2; ++index)
	{
		const std::string value = reader.GetValue("Note by Note", sizeKeys[index], sizeDefaults[index]);
		if (value.empty() || value.size() > 3 || value.find_first_not_of("0123456789") != std::string::npos)
		{
			LOG_ERROR("Invalid " << sizeKeys[index] << ": expected a whole percentage from 50 to 300; keeping the current size." << std::endl);
			continue;
		}
		const int size = std::stoi(value);
		if (size < 50 || size > 300)
		{
			LOG_ERROR("Invalid " << sizeKeys[index] << ": expected 50 to 300; keeping the current size." << std::endl);
			continue;
		}
		sizes[index]->store(size);
	}
	{
		const std::string value = reader.GetValue("Note by Note", "NoteByNoteLineSpacing", "100");
		const bool numeric = !value.empty() && value.size() <= 3 && value.find_first_not_of("0123456789") == std::string::npos;
		const int spacing = numeric ? std::stoi(value) : -1;
		if (spacing >= 50 && spacing <= 300) noteByNoteLineSpacing.store(spacing);
		else LOG_ERROR("Invalid NoteByNoteLineSpacing: expected 50 to 300; keeping the current spacing." << std::endl);
	}
	{
		const std::string style = reader.GetValue("Note by Note", "NoteByNoteTargetStyle", "Detailed");
		bool known = false;
		for (int index = 0; index < 3; ++index)
			if (style == kNoteByNoteTargetStyles[index]) { noteByNoteTargetStyle.store(static_cast<NoteByNoteTargetStyle>(index)); known = true; }
		if (!known) LOG_ERROR("Invalid NoteByNoteTargetStyle: expected Detailed, Simple or Tab; keeping Detailed." << std::endl);
	}
	for (int index = 0; index < 2; ++index)
	{
		// "Default" (or absent) = built-in layout; otherwise "x,y" as fractions of the game window.
		const std::string value = reader.GetValue("Note by Note", kNoteByNotePlacementKeys[index], "Default");
		HudPlacement placement;
		if (value != "Default" && sscanf_s(value.c_str(), "%f,%f", &placement.x, &placement.y) != 2)
		{
			LOG_ERROR("Invalid " << kNoteByNotePlacementKeys[index] << ": expected Default or x,y fractions; using the default layout." << std::endl);
			placement = {};
		}
		if (placement.IsSet() && (placement.x > 1.0f || placement.y > 1.0f)) placement = {};
		noteByNotePlacements[index].store(PackPlacement(placement));
	}
	const char* colorKeys[] = { "NoteByNoteNeutralColor", "NoteByNoteConfirmedColor", "NoteByNotePartialColor", "NoteByNoteRejectedColor" };
	const char* colorDefaults[] = { "FFFFFF", "55DD77", "FFAA44", "FF5555" };
	for (unsigned index = 0; index < 4; ++index)
	{
		const std::string value = reader.GetValue("Note by Note", colorKeys[index], colorDefaults[index]);
		if (value.size() != 6 || value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
		{
			LOG_ERROR("Invalid Note by Note colour for " << colorKeys[index] << ": expected six hexadecimal digits; keeping the current colour." << std::endl);
			continue;
		}
		noteByNoteColors[index].store(0xFF000000u | static_cast<uint32_t>(std::stoul(value, nullptr, 16)));
	}

}

/// <summary>
/// Parse String Colors From INI -> Color List
/// </summary>
void Settings::ReadStringColors() {
	CSimpleIniA reader;
	if (reader.LoadFile("RSMods.ini") < 0)
		return;

	customStringColorsNormal.clear();
	customStringColorsCB.clear();
	customNoteColorsNormal.clear();
	customNoteColorsCB.clear();

	for (int stringIdx = 0; stringIdx < 6; stringIdx++) {
		std::string strKey = "";
		std::string val;

		// Read string colors (normal)
		strKey = "string" + std::to_string(stringIdx) + "_N";
		val = reader.GetValue("String Colors", strKey.c_str(), defaultStrColors[stringIdx].c_str());
		customStringColorsNormal.push_back(ConvertHexToColor(val));

		// Read string colors (colorblind)
		strKey = "string" + std::to_string(stringIdx) + "_CB";
		val = reader.GetValue("String Colors", strKey.c_str(), defaultStrColorsCB[stringIdx].c_str());
		customStringColorsCB.push_back(ConvertHexToColor(val));

		// Read note colors (normal)
		strKey = "note" + std::to_string(stringIdx) + "_N";
		val = reader.GetValue("String Colors", strKey.c_str(), defaultStrColors[stringIdx].c_str());
		customNoteColorsNormal.push_back(ConvertHexToColor(val));

		// Read note colors (colorblind)
		strKey = "note" + std::to_string(stringIdx) + "_CB";
		val = reader.GetValue("String Colors", strKey.c_str(), defaultStrColorsCB[stringIdx].c_str());
		customNoteColorsCB.push_back(ConvertHexToColor(val));
	}

	// Set the default colors for deactivated notes.
	for (int stringIdx = 6; stringIdx < 8; stringIdx++) {
		customStringColorsNormal.push_back(ConvertHexToColor(defaultStrColors[stringIdx]));
		customStringColorsCB.push_back(ConvertHexToColor(defaultStrColorsCB[stringIdx]));
		customNoteColorsNormal.push_back(ConvertHexToColor(defaultStrColors[stringIdx]));
		customNoteColorsCB.push_back(ConvertHexToColor(defaultStrColorsCB[stringIdx]));
	}
}

/// <summary>
/// Parse Noteway Colors From INI
/// </summary>
void Settings::ReadNotewayColors() {
	CSimpleIniA reader;
	if (reader.LoadFile("RSMods.ini") < 0) {
		LOG_ERROR("Error reading saved settings" << std::endl);
		return;
	}

	notewayColors = {
			{ "CustomHighwayNumbered", reader.GetValue("Highway Colors", "CustomHighwayNumbered", "") },
			{ "CustomHighwayUnNumbered", reader.GetValue("Highway Colors", "CustomHighwayUnNumbered", "") },
			{ "CustomHighwayGutter", reader.GetValue("Highway Colors", "CustomHighwayGutter", "") },
			{ "CustomFretNubmers", reader.GetValue("Highway Colors", "CustomFretNubmers", "") },
	};
}

/// <summary>
/// Turn ExtendedRangeEnabled off / on, when ToggleExtendedRangeKey is pressed.
/// </summary>
void Settings::ToggleExtendedRangeMode()
{
	modSettings["ExtendedRangeEnabled"] = (modSettings["ExtendedRangeEnabled"] == "on") ? "off" : "on";
}


/// <summary>
/// Read Keybind From INI
/// </summary>
/// <param name="name"> - std::map[key]</param>
/// <returns>Virtual Key | uint</returns>
unsigned int Settings::GetKeyBind(const std::string& name) {
	std::string value;
	if (TryRuntimeOverride(name, value)) return GetVKCodeForString(value);
	return GetVKCodeForString(modSettings[name]);
}

/// <summary>
/// Read Mod Setting (internally called customSettings)
/// </summary>
/// <param name="name"> - std::map[key]</param>
/// <returns>Int for mod setting</returns>
int Settings::GetModSetting(const std::string& name) {
	return customSettings[name];
}

/// <summary>
/// Read Mod Toggle On / Off (internally modSettings)
/// </summary>
/// <param name="name"> - std::map[key]</param>
/// <returns>Value of mod toggle</returns>
std::string Settings::ReturnSettingValue(const std::string& name) {
	std::string value;
	if (TryRuntimeOverride(name, value)) return value;
	return modSettings[name];
}

/// <summary>
/// Get Virtual Key code for input string
/// </summary>
/// <param name="vkString"> - std::map[key]</param>
/// <returns></returns>
namespace
{
	// Every entry ReadKeyBinds loads (Keybinds and Audio Keybindings), for the overlay picker's clash check.
	const char* const kAllKeyBindNames[] = {
		"ToggleLoftKey", "CustomSongListTitles", "ShowSongTimerKey", "ForceReEnumerationKey", "MenuToggleKey",
		"RainbowStringsKey", "RainbowNotesKey", "RemoveLyricsKey", "RRSpeedKey", "TuningOffsetKey",
		"ToggleExtendedRangeKey", "LoopStartKey", "LoopEndKey", "RewindKey", "DropPedalPitchDownKey",
		"DropPedalPitchUpKey", "DropPedalToggleKey", "DropPedalBaseTuningKey", "RecordingHotkey",
		"MasterVolumeKey", "SongVolumeKey", "Player1VolumeKey", "Player2VolumeKey", "MicrophoneVolumeKey",
		"VoiceOverVolumeKey", "SFXVolumeKey", "DisplayMixerKey", "MutePlayer1Key", "MutePlayer2Key",
	};
}

bool Settings::SetKeyBind(const char* name, unsigned int vk, bool persist) {
	// The ini stores names ("VK_F6", "T"); prefer the VK_ spelling when the table has both.
	std::string value;
	for (const auto& [text, code] : keyMap) {
		if (code != vk) continue;
		if (value.empty() || (text.rfind("VK_", 0) == 0 && value.rfind("VK_", 0) != 0)) value = text;
	}
	if (value.empty()) return false;
	SetRuntimeOverride(name, value);   // never the shared map (see runtimeOverrides)
	if (!persist) return true;
	char executablePath[MAX_PATH]{};
	GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
	const auto iniPath = (std::filesystem::path(executablePath).parent_path() / "RSMods.ini").string();
	if (WritePrivateProfileStringA("Keybinds", name, value.c_str(), iniPath.c_str())) return true;
	LOG_ERROR("[SETTINGS] Could not persist " << name << std::endl);
	return false;
}

std::string Settings::FindKeyBindUsing(unsigned int vk, const char* except) {
	for (const char* name : kAllKeyBindNames)
		if (std::string_view(name) != except && GetKeyBind(name) == vk) return name;
	return "";
}

int Settings::GetVKCodeForString(const std::string& vkString) {
	return keyMap[vkString];
}

/// <summary>
/// Is the twitch effect on
/// </summary>
/// <param name="name"> - std::map[key]</param>
bool Settings::IsTwitchSettingEnabled(const std::string& name) {
	if (twitchSettings.count(name) == 0) // JIC
		return false;

	return twitchSettings[name] == "on";
}

/// <summary>
/// Read Noteway Color
/// </summary>
/// <param name="name"> - std::map[key]</param>
/// <returns>HEX color</returns>
std::string Settings::ReturnNotewayColor(const std::string& name) {
	return notewayColors[name];
}

/// <summary>
/// Split input into list of strings, based on spaces
/// </summary>
/// <param name="input"> - Input string</param>
/// <returns>List of strings taken from input, that were seperated by spaces.</returns>
std::vector<std::string> Settings::SplitByWhitespace(const std::string& input) {
	std::regex re("\\s+");
	std::sregex_token_iterator first{ input.begin(), input.end(), re, -1 };
	std::sregex_token_iterator last;

	return { first, last };
}

/// <summary>
/// Change mod setting to new value
/// </summary>
/// <param name="name"> - std::map[key]</param>
/// <param name="newValue"> - new setting value</param>
void Settings::UpdateModSetting(const std::string& name, const std::string_view& newValue) {
	modSettings[name] = newValue;
}

/// <summary>
/// Change custom setting to new value
/// </summary>
/// <param name="name"> - std::map[key]</param>
/// <param name="newValue"> - new setting value</param>
void Settings::UpdateCustomSetting(const std::string& name, int newValue) {
	customSettings[name] = newValue;
}

/// <summary>
/// Change mod setting to new value
/// </summary>
/// <param name="name"> - std::map[key]</param>
/// <param name="newValue"> - new setting value</param>
void Settings::UpdateTwitchSetting(const std::string& name, const std::string_view& newValue) {
	twitchSettings[name] = newValue;
}

/// <summary>
/// Trigger single setting update.
/// </summary>
/// <param name="updateMessage"> - Format: update (custom|mod) name newValue</param>
void Settings::ParseSettingUpdate(const std::string& updateMessage) {
	auto msgParts = SplitByWhitespace(updateMessage);

	if (msgParts.size() < 4)
		return;

	std::string type = msgParts[1];
	std::string entry = msgParts[2];
	std::string value = msgParts[3];

	if (type == "custom") {
		int val = stoi(value);

		UpdateCustomSetting(entry, val);
	}
	else
		UpdateModSetting(entry, value);
}

/// <summary>
/// Read twitch message and edit effect
/// </summary>
/// <param name="twitchMsg"> - twitch message with mod</param>
/// <param name="toggleType"> - "enable" / "disable"</param>
void Settings::ParseTwitchToggle(const std::string& twitchMsg, const std::string_view& toggleType) {
	auto msgParts = SplitByWhitespace(twitchMsg);

	if (msgParts.size() < 2)
		return;

	std::string effectName = msgParts[1];

	twitchSettings[effectName] = toggleType == "enable" ? "on" : "off";
}

/// <summary>
/// Twitch: Solid note color
/// </summary>
/// <param name="twitchMsg"> - twitch message with new color</param>
void Settings::ParseSolidColorsMessage(const std::string& twitchMsg) {
	auto msgParts = SplitByWhitespace(twitchMsg);

	if (msgParts.size() < 3)
		return;

	UpdateModSetting("SolidNoteColor", msgParts[2]);
}

/// <summary>
/// Get color list of strings
/// </summary>
/// <param name="CB"> - colorblind or not</param>
/// <returns>List of all string colors</returns>
std::vector<RSColor> Settings::GetStringColors(bool CB) {
	if (CB)
		return customStringColorsCB;
	else
		return customStringColorsNormal;
}

/// <summary>
/// Get color list of notes
/// </summary>
/// <param name="CB"> - colorblind or not</param>
/// <returns>List of all note colors</returns>
std::vector<RSColor> Settings::GetNoteColors(bool CB) {
	if (CB)
		return customNoteColorsCB;
	else
		return customNoteColorsNormal;
}


/// <summary>
/// Change string color in color list
/// </summary>
/// <param name="strIndex"> - string number (zero-indexed)</param>
/// <param name="c"> - new color</param>
/// <param name="CB"> - colorblind or not</param>
void Settings::SetStringColors(int strIndex, RSColor c, bool CB) {
	if (CB)
		customStringColorsCB[strIndex] = c;
	else
		customStringColorsNormal[strIndex] = c;
}

/// <summary>
/// Change note color in color list
/// </summary>
/// <param name="strIndex"> - string number (zero-indexed)</param>
/// <param name="c"> - new color</param>
/// <param name="CB"> - colorblind or not</param>
void Settings::SetNoteColors(int strIndex, RSColor c, bool CB) {
	if (CB)
		customNoteColorsCB[strIndex] = c;
	else
		customNoteColorsNormal[strIndex] = c;
}

/// <summary>
/// Re-Parse INI
/// </summary>
void Settings::UpdateSettings() {
	{
		// Reloading from RSMods.ini: it now holds everything the overlay persisted, and anything newer the GUI saved.
		std::lock_guard<std::mutex> guard(runtimeOverrideMutex);
		runtimeOverrides.clear();
	}
	ReadKeyBinds();
	ReadModSettings();
	ReadStringColors();
	ReadNotewayColors();

	async_UpdateMidiSettings = true;
	D3DHooks::RecreateTextures = true;
}

/// <summary>
/// Convert HEX -> Color struct
/// </summary>
/// <param name="hexStr"> - String of hex, without #</param>
/// <returns>Color struct</returns>
RSColor Settings::ConvertHexToColor(const std::string& hexStr) {
	int r, g, b;
	if (sscanf_s(hexStr.c_str(), "%02x%02x%02x", &r, &g, &b) != EOF) {
		RSColor c((float)r / 255, (float)g / 255, (float)b / 255);

		return c;
	}
	else {
		RSColor nullColor(0.0f, 0.0f, 0.0f);
		return nullColor;
	}
}
