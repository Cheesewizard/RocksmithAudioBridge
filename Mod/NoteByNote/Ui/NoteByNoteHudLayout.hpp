#pragma once
// Where the Note by Note HUD blocks were drawn, published by the HUD (NoteByNoteHud.cpp, 4.x's
// GameOverlay::DisplayMlStringFretOverlay) and read by the in-game layout editor (NoteByNotePage.cpp's HudEditor) so a
// block can be dragged and resized in place. Both run on the render thread inside the same ImGui frame; the editor
// floats after the shell's window and the HUD's overlay may run before or after it, so the editor can see this frame's
// rectangles or the previous frame's. Plain globals are enough. Coordinates are ImGui display pixels, the same space
// as ImGui's mouse.

#include "../NoteByNoteSettings.hpp"

#include <Windows.h>

namespace NoteByNote::HudLayout
{
	struct Block
	{
		float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		unsigned long long drawnTick = 0;   // GetTickCount64 of the last frame it was drawn; 0 = never
	};

	inline Block blocks[2];   // index = ModSettings::HudBlock
	// The display size the HUD laid out against; placements are fractions of this.
	inline float frameWidth = 0, frameHeight = 0;

	inline void Publish(ModSettings::HudBlock block, float x0, float y0, float x1, float y1)
	{
		blocks[static_cast<int>(block)] = { x0, y0, x1, y1, GetTickCount64() };
	}

	// Drawn within the last few frames (the HUD hides the readout when detection is off or outside a song).
	inline bool IsLive(ModSettings::HudBlock block)
	{
		const unsigned long long tick = blocks[static_cast<int>(block)].drawnTick;
		return tick != 0 && GetTickCount64() - tick < 250;
	}
}
