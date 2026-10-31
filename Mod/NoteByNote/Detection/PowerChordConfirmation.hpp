#pragma once

#include <cstdint>

// Power chords (root + fifth, optionally octaves) cannot accept on the game's evidence alone.
// The game's sounding table almost never lists the FIFTH, because the fifth sits among the root's own overtones, so a two-note chord never reaches the
// "both tones sounding" bar even while the game's own vote and chord matcher say yes.
//
// The raw audio can settle it: the fifth's own pitch is never on the root's harmonic series
// (it sits at 1.5x the root), so energy there, with a fresh attack, means the fifth was really
// played, and a root played alone cannot produce it. These helpers pick which tones to check.
namespace NoteByNote
{
	// Root position (every tone an octave or a fifth above the lowest) or fifth in the bass
	// (every tone an octave or a fourth above the lowest), with at least one non-octave tone.
	inline bool IsPowerChordShape(const int* tones, int count)
	{
		if (tones == nullptr || count < 2 || count > 6) return false;
		int lowest = tones[0];
		for (int i = 1; i < count; ++i) if (tones[i] < lowest) lowest = tones[i];
		if (lowest < 0) return false;
		bool hasFifth = false, hasFourth = false;
		for (int i = 0; i < count; ++i)
		{
			const int interval = (tones[i] - lowest) % 12;
			if (interval == 7) hasFifth = true;
			else if (interval == 5) hasFourth = true;
			else if (interval != 0) return false;
		}
		return hasFifth != hasFourth;
	}

	// Below this the raw verifier measures a note's 2nd/3rd harmonics instead of its own pitch
	// (RawPitchVerifier.cpp: frequencyHz < 160.0), and a low fifth's 2nd harmonic is shared with
	// the root's 3rd, so it cannot separate the fifth from a root played alone. midi 52 = 164.8 Hz.
	constexpr int POWER_CHORD_MIN_PROBE_MIDI = 52;

	// The non-octave tones worth checking in the raw audio: at or above the harmonic-only
	// limit, and not themselves a harmonic of the lowest tone (a fifth an octave up, +19, IS the
	// root's 3rd harmonic). Returns how many were written to out (at most 6).
	inline int SelectPowerChordProbeTones(const int* tones, int count, int* out)
	{
		if (!IsPowerChordShape(tones, count) || out == nullptr) return 0;
		int lowest = tones[0];
		for (int i = 1; i < count; ++i) if (tones[i] < lowest) lowest = tones[i];
		int written = 0;
		for (int i = 0; i < count && written < 6; ++i)
		{
			const int interval = tones[i] - lowest;
			if (interval % 12 == 0) continue;
			if (interval == 19 || interval == 31 || interval == 43) continue;
			if (tones[i] < POWER_CHORD_MIN_PROBE_MIDI) continue;
			out[written++] = tones[i];
		}
		return written;
	}
}
