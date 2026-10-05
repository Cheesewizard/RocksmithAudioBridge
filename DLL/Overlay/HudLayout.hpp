#pragma once
// Where the Note by Note HUD blocks were drawn, published by the D3D HUD (GameOverlay::DisplayMlStringFretOverlay)
// and read by the in-game layout editor (Overlay::DrawHudEditor) so a block can be dragged and resized in place.
// Both run on the render thread inside EndScene (ImGui first, HUD after), so the editor sees the previous frame's
// rectangles; plain globals are enough. Coordinates are backbuffer pixels, the same space as ImGui's mouse.

#include "../Settings.hpp"

namespace Overlay::HudLayout
{
	struct Block
	{
		float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		unsigned long long drawnTick = 0;   // GetTickCount64 of the last frame it was drawn; 0 = never
	};

	inline Block blocks[2];   // index = Settings::NoteByNoteHudBlock
	// The window size the HUD laid out against (GameOverlay::WindowSize); placements are fractions of this.
	inline float frameWidth = 0, frameHeight = 0;

	inline void Publish(Settings::NoteByNoteHudBlock block, float x0, float y0, float x1, float y1)
	{
		blocks[static_cast<int>(block)] = { x0, y0, x1, y1, GetTickCount64() };
	}

	// Drawn within the last few frames (the HUD hides the readout when detection is off or outside a song).
	inline bool IsLive(Settings::NoteByNoteHudBlock block)
	{
		const unsigned long long tick = blocks[static_cast<int>(block)].drawnTick;
		return tick != 0 && GetTickCount64() - tick < 250;
	}
}
