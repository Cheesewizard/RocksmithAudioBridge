#pragma once

// The overlay's Note by Note page (4.x's OverlayShell.cpp NoteByNotePage) and, while it shows, the on-screen layout
// editor for the HUD's readout and target blocks (4.x's HudEditor). Practice group, before Drop Pedal. Offered while
// Note by Note is on, or while its switch is missing from the Riff Repeater menu (the page carries the repair message).
namespace NoteByNote::Ui
{
	// MainThread, from NoteByNoteMod::OnInitialize.
	void RegisterNoteByNotePage();
}
