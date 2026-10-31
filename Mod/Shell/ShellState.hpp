#pragma once

#include <string>

// The Audio Bridge overlay's state for other mods, set by AudioBridgeShellMod. The Tone designer's VST host shows its
// plugin windows only while the overlay is open, and needs the overlay key to pass it through to the game.
namespace AudioBridge::Shell
{
	// Any thread. True while the overlay window is open.
	bool IsOverlayOpen();
	// The OverlayKey setting's virtual-key code; 0 when the setting names no known key.
	unsigned OverlayKeyCode();

	// Render thread, inside the ImGui frame: every overlay that may want ImGui's software cursor (Rocksmith hides the
	// system one) votes every frame it runs, true or false, and the cursor shows when any of this frame's votes was true.
	// Each vote writes io.MouseDrawCursor as the OR of the frame's votes so far, so the last voter's write is the
	// answer whatever order the overlays run in. The shell votes while its window is open, the Look ahead tab while the
	// mouse is in use over the song (4.x's Menu.cpp: menu || overlay || TablaturePreviewWantsCursor).
	void VoteMouseCursor(bool wanted);

	// Any thread. Opens the overlay on the page registered as `pageId` (ShellPages.hpp; "general" has the overlay key
	// picker). The window switches to it the next frame it draws and remembers it like a sidebar pick; an id no page
	// has leaves the selection as it was.
	void OpenOverlayAt(const char* pageId);

	// ShellWindow only, render thread: the page OpenOverlayAt asked for since the last call, else empty.
	std::string TakeRequestedPage();

	// AudioBridgeShellMod only.
	void SetOverlayOpen(bool open);
	void SetOverlayKeyCode(unsigned code);
}
