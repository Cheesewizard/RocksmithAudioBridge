#pragma once

// Relabels the native Riff Repeater HUD "MISSED" row to "NOTE BY NOTE" while Note by Note
// is active, in the game's own font, by intercepting the localized-string resolver. In
// Note by Note the notes freeze and wait, so the miss count is always 0 and the slot is
// free to become the mode indicator. See NoteByNoteHudLabel.cpp for
// the resolver ABI and the reason this is a resolver hook and not an overlay.
namespace NoteByNoteHudLabel
{
	// Installs the localized-string resolver detour. Safe no-op if the game build does not
	// match the expected resolver prologue. Call once at startup on the game's main thread.
	void Initialize();
}
