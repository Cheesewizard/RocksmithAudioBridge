#pragma once

#include <string>

// Game state for code off MainThread (the Tone designer on the render thread, the External amp worker, latency
// calibration), published on MainThread by AudioBridgeRoutingMod every tick. 4.x read GameState straight from those
// threads; upstream's readers are not all null-guarded and crashed while the game was still loading, so here only
// MainThread reads them, and never in the Loading phase. Before the first publish (and while loading) everything reads
// empty / false.
//
// GameState::GameLoaded and GameState::GameClosing are plain atomics upstream and are read directly.
namespace AudioBridge::GameView
{
	// MainThread, outside the Loading phase.
	void Publish();

	// Any thread.
	bool IsInSong();               // GameState::IsInSong
	bool IsInTuningMenus();        // GameState::Menus::IsInTuningMenus
	std::string CurrentMenu();     // GameState::GetCurrentMenu
	std::string SongKey();         // GameState::GetSongKey: the song playing, or the song list's highlighted (previewed) one
}
