#pragma once

// Audio diagnostics (4.x's GameOverlay::DisplayAudioDiagnostics, DLL/D3DOverlay.cpp): the guitar input's signal bar,
// dB figure and packet rate in the top-right corner, a Player 2 row under it in multiplayer, and the latency readout
// under those. Switched by RSMods.ini [Mod Settings] AudioDiagnosticsOverlay (the overlay's General page, Display
// card; op 27 saves it) and, for the latency row, HKCU\Software\RSMods\Overlay LatencyReadout, both 4.x's. Drawn
// in menus and songs, on the background draw list, from AudioBridgeRoutingMod's overlay callback.
namespace AudioBridge::DiagnosticsHud
{
	// Render thread, inside the ImGui frame.
	void Draw();
	// True when Draw draws this frame (4.x's IsAudioDiagnosticsVisible). Any thread.
	bool IsVisible();

	// MainThread, outside the Loading phase: the game state Draw needs (upstream's GameState readers aren't all
	// null-guarded, so the render thread never calls them).
	void PublishGameState();

	// The Latency readout switch (HKCU, read once, saved on change). Any thread.
	bool IsLatencyReadoutEnabled();
	bool SetLatencyReadoutEnabled(bool enabled);
}
