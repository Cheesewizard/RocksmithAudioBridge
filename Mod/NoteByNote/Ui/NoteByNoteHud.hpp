#pragma once

// Note by Note's on-screen HUD, 4.x's D3DX blocks from DLL/D3DOverlay.cpp drawn with ImGui on the background draw list
// from NoteByNoteMod's overlay callback:
//  - the chord finger numerals over a frozen chord hold (DisplayNoteByNoteStatus), from the highway's anchors
//    (HighwaySeam.hpp; none until the highway renderer ports),
//  - the bend meter at the right while a bend is the target (DisplayNoteByNoteBendMeter),
//  - the detection readout (Native / Enhanced / ML) and the target block in the Detailed, Simple and Tab styles, at the
//    built-in places or where the player dragged them (DisplayMlStringFretOverlay; NoteByNoteHudLayout.hpp).
// 4.x's OverlayToggles "bend_meter" and "ml_fret" were research switches that shipped on and only its research bridge
// flipped; both are always on here. The detection readout keeps its setting (NoteByNoteDetectionOverlay).
namespace NoteByNote::Hud
{
	// MainThread, outside the Loading phase: the game state the HUD and the look-ahead tab read on the render thread
	// (each string's open pitch in the player's physical tuning, the string colours), which 4.x read there directly.
	void PublishGameState();

	// Render thread, inside the ImGui frame.
	void Draw();

	// Any thread, from the last PublishGameState. 4.x's DropPedal::TryGetPhysicalOpenStringMidi: the MIDI note of an
	// open string in the player's physical tuning (the chart's tuning moved by the pedal's base); false when the chart's
	// tuning isn't readable.
	bool TryGetPhysicalOpenStringMidi(int stringIndex, int& midi);
	// 4.x's ERMode::TryGetActiveStringColor: the string's colour on the highway as opaque ARGB.
	bool TryGetStringColor(int stringIndex, unsigned int& argb);
}
