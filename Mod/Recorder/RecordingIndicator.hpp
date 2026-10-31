#pragma once

// The red "REC" badge in the top-right corner while a take records (4.x's GameOverlay::DisplayRecordingIndicator,
// DLL/D3DOverlay.cpp). Drawn whatever the game shows, loading included, as 4.x drew it before its GameLoaded check.
namespace AudioBridge::RecordingIndicator
{
	// Render thread, inside the ImGui frame (AudioRecorderMod's overlay callback).
	void Draw();
}
