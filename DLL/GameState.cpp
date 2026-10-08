#include "stdafx.h"
#include "GameState.hpp"

namespace
{
	constexpr size_t MAX_SONG_EVENT_LENGTH = 50;
	// Menu and profile names are short; anything longer is cut here instead of read to an unknown end.
	constexpr size_t MAX_GAME_STRING_LENGTH = 256;
	constexpr std::string_view SONG_EVENT_PREFIX = "Play_";
	constexpr std::string_view PREVIEW_EVENT_SUFFIX = "_Preview";
	constexpr std::string_view INVALID_EVENT_SUFFIX = "_Invalid";

	std::mutex songKeyMutex;
	std::string lastSongKey;

	bool HasSuffix(std::string_view text, std::string_view suffix)
	{
		return text.size() >= suffix.size()
			&& text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	bool TryReadSongKey(const char* eventName, std::string& songKey)
	{
		if (eventName == nullptr || MemUtil::IsBadReadPtr((void*)eventName)) return false;

		const size_t eventLength = strnlen_s(eventName, MAX_SONG_EVENT_LENGTH + 1);
		if (eventLength <= SONG_EVENT_PREFIX.size() + PREVIEW_EVENT_SUFFIX.size()
			|| eventLength > MAX_SONG_EVENT_LENGTH)
		{
			return false;
		}

		const std::string_view event(eventName, eventLength);
		if (!event._Starts_with(SONG_EVENT_PREFIX)) return false;

		const bool isPreview = HasSuffix(event, PREVIEW_EVENT_SUFFIX);
		const bool isInvalidPreview = HasSuffix(event, INVALID_EVENT_SUFFIX);
		if (!isPreview && !isInvalidPreview) return false;

		const size_t songKeyLength = eventLength
			- SONG_EVENT_PREFIX.size()
			- PREVIEW_EVENT_SUFFIX.size();
		songKey.assign(event.data() + SONG_EVENT_PREFIX.size(), songKeyLength);
		return true;
	}
}

/// <summary>
/// Are we in a song?
/// </summary>
bool GameState::IsInSong() {
	if (!GameLoaded)
	{
		return false;
	}

	return Contains(GetCurrentMenu(), songModes);
}

/// <summary>
/// Get the status of if the user is in multiplayer
/// </summary>
/// <returns>Is the user in multiplayer</returns>
bool GameState::IsMultiplayer() {
	// Called every frame by the HUD and overlay. The five-step chain is rebuilt while the game changes screens,
	// and the unchecked walk crashed on a freed middle link (a read of 0xFFEDC2B0 from DisplayAudioDiagnostics
	// on the main menu, 2026-10-08). Every link is checked, and a link freed between the check and the read
	// counts as "not multiplayer" instead of crashing.
	__try {
		const uintptr_t address = MemUtil::FindDMAAddy(
			Offsets::baseHandle + Offsets::ptr_multiplayer,
			Offsets::ptr_multiplayerOffsets,
			true);
		return address != 0 && *reinterpret_cast<int*>(address) != 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

/// <summary>
/// Get the current selected profile name. **Only works on Profile Selection screen**
/// </summary>
/// <returns>Profile Name</returns>
std::string GameState::CurrentSelectedUser() {
	// Polled by the auto profile loader while the profile select screen builds, so every link is checked and the walk
	// and the string copy are guarded against a link freed under us.
	uintptr_t badValue = MemUtil::FindDMAAddyGuarded(Offsets::baseHandle + Offsets::ptr_selectedProfileName, Offsets::ptr_selectedProfileNameOffsets);

	// If the pointer is invalid just return nothing
	if (!badValue) {
		LOG_ERROR("Invalid Pointer: CurrentSelectedUser" << std::endl);
		return (std::string)"";
	}

	// This value 90% of the time starts with an invalid pointer. We must wait ~2.5 seconds to guarantee that it is correct, or until the pointer changes (whichever comes first).
	for (int i = 0; i < 25; i++)
	{
		if (badValue >= 0x10000000)
			break;

		badValue = MemUtil::FindDMAAddyGuarded(Offsets::baseHandle + Offsets::ptr_selectedProfileName, Offsets::ptr_selectedProfileNameOffsets);
	}

	char profileName[MAX_GAME_STRING_LENGTH];
	if (!MemUtil::TryReadString(badValue, profileName, sizeof(profileName)))
		return (std::string)"";

	return std::string(profileName);
}

/// <summary>
/// Gets the SongKey of the current playing song, based on the initial preview.
/// </summary>
/// <returns>Last played Song Key</returns>
std::string GameState::GetSongKey() {
	// Polled every 100 ms by the Riff Repeater thread, in menus and songs alike, so every
	// link is checked and the event name is copied out under a guard before it is parsed. The copy holds one
	// character more than the longest accepted event, so an overlong name is still rejected by TryReadSongKey.
	const uintptr_t previewEventAddress = MemUtil::FindDMAAddyGuarded(
		Offsets::baseHandle + Offsets::ptr_previewName,
		Offsets::ptr_previewNameOffsets);
	char previewEvent[MAX_SONG_EVENT_LENGTH + 2];
	std::string currentSongKey;
	const bool hasCurrentSongKey = MemUtil::TryReadString(previewEventAddress, previewEvent, sizeof(previewEvent))
		&& TryReadSongKey(previewEvent, currentSongKey);

	std::lock_guard<std::mutex> lock(songKeyMutex);
	if (hasCurrentSongKey) lastSongKey = std::move(currentSongKey);
	return lastSongKey;
}

/// <param name="GameNotLoaded"> - Should we trust the pointer?</param>
/// <returns>Internal Menu Name</returns>
std::string GameState::GetCurrentMenu(bool GameNotLoaded) {
	bool failedToReadPreMainMenuAddr = false;

	// It seems like the third level of the pointer isn't initialized until you reach the UPLAY login screen,
	// but the second level actually is, and in there it keeps either an empty string, "TitleMenu", "MainOverlay"
	// (before you reach the login) or some gibberish that's always the same (after that) 
	if (GameNotLoaded) {
		uintptr_t preMainMenuAdr = MemUtil::FindDMAAddyGuarded(Offsets::ptr_currentMenu, Offsets::ptr_preMainMenuOffsets);
		char preMainMenuName[MAX_GAME_STRING_LENGTH];

		if (MemUtil::TryReadString(preMainMenuAdr, preMainMenuName, sizeof(preMainMenuName)))
		{
			// I.e. check if its neither one of the possible states
			std::string currentMenu(preMainMenuName);

			if (lastMenu == "TitleScreen" && lastMenu != currentMenu)
				canGetRealMenu = true;
			else {
				lastMenu = currentMenu;
				return "pre_enter_prompt";
			}
		}
		else
		{
			//_LOG_INFO("Invalid Pointer: GetCurrentMenu(" << std::boolalpha << GameNotLoaded << ") @ LVL 2" << std::endl);
			failedToReadPreMainMenuAddr = true;
		}
	}

	if (!canGetRealMenu)
		return "pre_enter_prompt";


	// If game hasn't loaded, take the safer, but possibly slower route.
	// Once it has, this runs on every draw call (IsInSong from Hook_DIP), so the links are not checked one by one
	// (each check is a VirtualQuery call); the guard turns a link freed during a screen change into a failed read.

	uintptr_t currentMenuAddr = MemUtil::FindDMAAddyGuarded(Offsets::ptr_currentMenu, Offsets::ptr_currentMenuOffsets, GameNotLoaded);
	char currentMenuName[MAX_GAME_STRING_LENGTH];
	if (!MemUtil::TryReadString(currentMenuAddr, currentMenuName, sizeof(currentMenuName))) {
		//LOG_ERROR("Invalid Pointer: GetCurrentMenu(" << std::boolalpha << GameNotLoaded << ") @ LVL 3" << std::endl);
		return "where are we actually";
	}

	std::string currentMenu(currentMenuName);
	return currentMenu;
}

/// <summary>
/// Should we turn on / off ColorBlind colors
/// </summary>
/// <param name="enabled"> - Should we turn on colors or turn off?</param>
void GameState::ToggleCB(bool enabled) {
	// Called from the draw hooks on every draw call while Extended Range or custom colors are on, so the links are not
	// checked one by one (each check is a VirtualQuery call); the guards turn a link freed as the song ends into a skip.
	uintptr_t addrTimer = MemUtil::FindDMAAddyGuarded(Offsets::baseHandle + Offsets::ptr_timer, Offsets::ptr_timerBaseOffsets, false);
	uintptr_t cbEnabled = MemUtil::FindDMAAddyGuarded(Offsets::baseHandle + Offsets::ptr_colorBlindMode, Offsets::ptr_colorBlindModeOffsets, false);

	byte currentValue = 0;
	if (!addrTimer || !MemUtil::TryRead(cbEnabled, currentValue)) {
		// LOG_ERROR("Invalid Pointers: ToggleCB(" << std::boolalpha << enabled << ")" << std::endl); // Disabled because it causes log to get huge real quick
		return;
	}

	// JIC, no need to write the same value constantly
	if (currentValue != (byte)enabled)
		MemUtil::TryWrite<byte>(cbEnabled, enabled);
}

namespace GameState {
	namespace Menus {
		bool IsOnMainMenu() {
			return currentMenu == "MainMenu";
		}

		bool IsInMultiplayerTunerMenus() {
			return Contains(currentMenu, learnASongModes);
		}

		bool IsInScoreMenus() {
			return Contains(currentMenu, scoreScreens);
		}

		bool IsInTuningMenus() {
			return Contains(currentMenu, tuningMenus);
		}

		bool IsInPreSongTuner() {
			return Contains(currentMenu, preSongTuners);
		}

		bool IsInSongModes() {
			return Contains(currentMenu, songModes);
		}

		bool IsInScoreAttackModes() {
			return Contains(currentMenu, scoreAttackModes);
		}

		bool IsInLearnASongModes() {
			return Contains(currentMenu, learnASongModes);
		}

		bool IsInLearnASongPauseModes() {
			return Contains(currentMenu, lasPauseMenus);
		}

		bool IsInModesWithAllowedFastRiffRepeater() {
			return Contains(currentMenu, fastRRModes);
		}

		bool IsInRiffRepeaterMenus() {
			return Contains(currentMenu, riffRepeaterMenus);
		}

		bool IsInOnlineModes() {
			return Contains(currentMenu, onlineModes);
		}

		bool IsInLASPlayingModes() {
			return Contains(currentMenu, learnASongPlaying);
		}

		bool IsOnScoreScreens() {
			return Contains(currentMenu, scoreScreens);
		}

		bool IsInLessonModes() {
			return Contains(currentMenu, lessonModes);
		}

		bool IsInMenusWithDisallowedAutoEnter() {
			return Contains(currentMenu, dontAutoEnter);
		}

		bool IsInCalibrationMenus() {
			return Contains(currentMenu, calibrationMenus);
		}
	}
}

bool Contains(std::string_view text, std::string_view key) {
	return text.find(key) != std::string_view::npos;
}
