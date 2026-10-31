#pragma once

// One spelling for every pitch class the mod puts on screen.
//
// Guitarists read a flat song in flats: with the Drop Pedal taking an E standard
// guitar into an Eb song, the open low string is "Eb", never "D#".
// These are the names the mod already uses for tunings
// (DropPedalState's tuning list) and for Guitar Speak (flats where guitar
// practice spells flats: Eb, Ab, Bb; sharps where it spells sharps: C#, F#),
// so the detection HUD, the bend meter and the ML readout agree with the tuning
// line drawn above them instead of each choosing its own accidental.
//
// The spelling is FIXED rather than derived from the active tuning on purpose: a
// context-sensitive rule would rename the same pitch between songs, and the
// player reads these rows against the tuning name, which is already spelled this
// way whatever the song.
namespace PitchNames
{
	// Any integer pitch class or MIDI number, negative included.
	inline const char* ForPitchClass(int pitch)
	{
		static const char* const NAMES[12] =
			{ "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
		return NAMES[((pitch % 12) + 12) % 12];
	}
}
