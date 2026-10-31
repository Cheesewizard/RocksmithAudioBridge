#include "GameView.hpp"

#include "GameState.hpp"

#include <atomic>
#include <mutex>

namespace AudioBridge::GameView
{
	namespace
	{
		std::atomic<bool> inSong{ false };
		std::atomic<bool> inTuningMenus{ false };
		std::mutex textMutex;
		std::string currentMenu;   // under textMutex
		std::string songKey;       // under textMutex
	}

	void Publish()
	{
		// MainThread, so these read upstream's menu cache after ModManager has refreshed it this tick.
		std::string menu = GameState::GetCurrentMenu();
		const bool tuning = GameState::Menus::IsInTuningMenus();
		const bool song = GameState::IsInSong();
		std::string key = GameState::GetSongKey();
		{
			std::lock_guard<std::mutex> lock(textMutex);
			currentMenu.swap(menu);
			songKey.swap(key);
		}
		inTuningMenus.store(tuning, std::memory_order_release);
		inSong.store(song, std::memory_order_release);
	}

	bool IsInSong() { return inSong.load(std::memory_order_acquire); }
	bool IsInTuningMenus() { return inTuningMenus.load(std::memory_order_acquire); }

	std::string CurrentMenu()
	{
		std::lock_guard<std::mutex> lock(textMutex);
		return currentMenu;
	}

	std::string SongKey()
	{
		std::lock_guard<std::mutex> lock(textMutex);
		return songKey;
	}
}
