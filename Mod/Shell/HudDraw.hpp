#pragma once

#include <cstdint>

// What 4.x's D3DX HUD (DLL/D3DOverlay.cpp, DX9DrawText / DX9DrawTextW) becomes on upstream's ImGui: text in the
// player's on-screen font (RSMods.ini OnScreenFont, as 4.x and upstream's HUD use) at any pixel height, drawn on the
// background draw list from a Menu().RegisterOverlay callback, so it sits under every ImGui window and over the game.
//
// ImGui bakes a face at fixed sizes, and upstream's ImGuiHud::AcquireFont bakes at most 8 for the whole game (its HUD
// default size counts too). 4.x asked D3DX for a font per size (readout, target, numerals, REC...), so here a size
// is rounded up to one of a few baked sizes and drawn scaled down from it. A size is baked the frame after it is first
// asked for (until then ImGui's default font stands in), because baking has to happen outside the ImGui frame.
//
// D3DX's FW_BOLD has no ImGui equivalent; `bold` draws the text twice, a pixel apart. Glyphs outside Latin-1 (4.x's
// U+25A0 swatch, U+2588 meter blocks, U+25CF REC dot) aren't in the baked range, so callers draw those as shapes.
namespace AudioBridge::HudDraw
{
	// Render thread, OUTSIDE the ImGui frame: from a Draw().RegisterFrame callback of every mod that draws with this
	// (more than one call a frame is harmless). Bakes the sizes Font was asked for and reads the OnScreenFont face.
	void PrepareFonts();

	// Render thread, inside the frame. The font to draw `pixelHeight` with: a baked size at or above it, or ImGui's
	// default font until that size is baked. Draw with the pixel height asked for, not the font's own size.
	ImFont* Font(float pixelHeight);
	ImVec2 Measure(float pixelHeight, const char* text);
	void Text(ImDrawList* list, float pixelHeight, ImVec2 position, ImU32 color, const char* text, bool bold = false);

	// D3DCOLOR (0xAARRGGBB) to ImGui's packed colour. An alpha of 0 is opaque, as upstream's HUD treats it.
	ImU32 Argb(std::uint32_t argb);
}
