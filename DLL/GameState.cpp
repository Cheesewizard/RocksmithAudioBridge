#include "stdafx.h"
#include "GameState.hpp"

namespace
{
	constexpr size_t MAX_SONG_EVENT_LENGTH = 50;
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
	const uintptr_t address = MemUtil::FindDMAAddy(
		Offsets::baseHandle + Offsets::ptr_multiplayer,
		Offsets::ptr_multiplayerOffsets);
	if (address == 0 || MemUtil::IsBadReadPtr(reinterpret_cast<void*>(address))) return false;

	return *reinterpret_cast<int*>(address) != 0;
}

/// <summary>
/// Get the current selected profile name. **Only works on Profile Selection screen**
/// </summary>
/// <returns>Profile Name</returns>
std::string GameState::CurrentSelectedUser() {
	uintptr_t badValue = MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_selectedProfileName, Offsets::ptr_selectedProfileNameOffsets);

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

		badValue = MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_selectedProfileName, Offsets::ptr_selectedProfileNameOffsets);
	}

	return std::string((const char*)badValue);
}

/// <summary>
/// Gets the SongKey of the current playing song, based on the initial preview.
/// </summary>
/// <returns>Last played Song Key</returns>
std::string GameState::GetSongKey() {
	const uintptr_t previewEventAddress = MemUtil::FindDMAAddy(
		Offsets::baseHandle + Offsets::ptr_previewName,
		Offsets::ptr_previewNameOffsets);
	std::string currentSongKey;
	const bool hasCurrentSongKey = previewEventAddress != 0
		&& TryReadSongKey(reinterpret_cast<const char*>(previewEventAddress), currentSongKey);

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
		uintptr_t preMainMenuAdr = MemUtil::FindDMAAddy(Offsets::ptr_currentMenu, Offsets::ptr_preMainMenuOffsets, GameNotLoaded);

		if (preMainMenuAdr)
		{
			// I.e. check if its neither one of the possible states
			std::string currentMenu((char*)preMainMenuAdr);

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


	// If game hasn't loaded, take the safer, but possibly slower route

	uintptr_t currentMenuAddr = MemUtil::FindDMAAddy(Offsets::ptr_currentMenu, Offsets::ptr_currentMenuOffsets, GameNotLoaded);
	if (!currentMenuAddr) {
		//LOG_ERROR("Invalid Pointer: GetCurrentMenu(" << std::boolalpha << GameNotLoaded << ") @ LVL 3" << std::endl);
		return "where are we actually";
	}

	std::string currentMenu((char*)currentMenuAddr);
	return currentMenu;
}

/// <summary>
/// Should we turn on / off ColorBlind colors
/// </summary>
/// <param name="enabled"> - Should we turn on colors or turn off?</param>
void GameState::ToggleCB(bool enabled) {
	uintptr_t addrTimer = MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_timer, Offsets::ptr_timerBaseOffsets);
	uintptr_t cbEnabled = MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_colorBlindMode, Offsets::ptr_colorBlindModeOffsets);

	if (!addrTimer || !cbEnabled) {
		// LOG_ERROR("Invalid Pointers: ToggleCB(" << std::boolalpha << enabled << ")" << std::endl); // Disabled because it causes log to get huge real quick
		return;
	}

	// JIC, no need to write the same value constantly
	if (*(byte*)cbEnabled != (byte)enabled)
		*(byte*)cbEnabled = enabled;
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
