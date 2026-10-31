#pragma once

// The Look ahead tab (the TabTimeline target style): a scrollable tablature window over the song, drawn every frame
// in a song with Note by Note on, whether the Audio Bridge overlay is open or not (4.x drew it from Menu.cpp's ImGui
// frame, beside the overlay). NoteByNoteMod's overlay callback calls DrawOverlay.
//
// The VexFlow licence notice 4.x linked in with TablatureNotices.rc (the renderer's engraving rules follow VexFlow)
// isn't compiled into the framework build, which builds no resource scripts for external mods; the notice ships
// with the installer.
namespace NoteByNote::Ui
{
	bool IsTablaturePreviewVisible();
	// Whether ImGui should draw its cursor for the tab this frame. Inside the ImGui frame.
	bool TablaturePreviewWantsCursor();
	void DrawTablaturePreview();

	// Render thread, inside the ImGui frame: claims the overlay input on the frames 4.x captured it for the tab (all
	// the while it shows: ImGui then gets the mouse and keys, and the game's mouse reads come back empty only while
	// ImGui wants the mouse, over the window), votes for the cursor, and draws the tab.
	void DrawOverlay();
}
