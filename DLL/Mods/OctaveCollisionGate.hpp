#pragma once
#include <cmath>
#include <cstdlib>
#include "NoteTechniqueClassification.hpp"   // CHORD_STRING_SENTINEL

// Below 160 Hz the raw pitch verifier confirms a note on its 2nd and 3rd harmonics only,
// because a magnetic pickup can leave the low-string fundamental ~30 dB down (so useHarmonics
// deliberately skips the fundamental check in RawPitchVerifier::ConfirmSnapshot). The failure
// this gate handles: a chord that committed a moment earlier is still ringing an octave above a queued low
// note (Fbsus2's F3 + C4 sit exactly on F2's 2f + 3f). That decay is spectrally identical to a
// fresh pick of the low note, so the raw verifier confirms a phantom the player never played and
// the pick buffer force-commits it. Raw evidence cannot separate the two; only the game's own
// (native) detector can, because it reads the ringing chord's actual tones, not the octave below.
namespace NoteByNote
{
	// Mirrors RawPitchVerifier.cpp (frequencyHz < 160.0): the harmonic-only regime where the
	// fundamental is unavailable and an octave-up chord can masquerade as the note.
	inline bool IsHarmonicOnlyTarget(int midi)
	{
		return midi >= 0 && 440.0 * std::pow(2.0, (midi - 69.0) / 12.0) < 160.0;
	}

	// A ringer BELOW the target by 12/19/24 puts its 2f/3f/4f on the target's fundamental band,
	// which ~-30 dB fundamentals stop lowerFundamental from rejecting (a ringing E2 can confirm
	// E3). A ringer
	// ABOVE by the same intervals masks only a harmonic-only target, whose 2f/3f are the very
	// bands the verifier measures.
	inline bool RingingNoteMasksTarget(int midi, int ringerMidi)
	{
		if (midi < 0 || ringerMidi < 0) return false;
		const int below = midi - ringerMidi;
		const int above = ringerMidi - midi;
		if (below == 12 || below == 19 || below == 24) return true;
		return IsHarmonicOnlyTarget(midi) && (above == 12 || above == 19 || above == 24);
	}

	// Any still-ringing tone (a chord's authored tones) that masks the target.
	inline bool RingingTonesMaskTarget(int midi, const int* tones, int count)
	{
		for (int i = 0; tones != nullptr && i < count; ++i)
			if (RingingNoteMasksTarget(midi, tones[i])) return true;
		return false;
	}

	// A slow strum's later strings arrive as separate HFC attacks well after the strum starts
	// (e.g. an Fbsus2's B-string C4, F2's 3f, can register as an "attack" ~150 ms later and
	// confirm F2, with native reading the ringing F3 an octave low as F). When the chord just
	// committed rings the target's measured harmonic bands, an attack this soon after its strum is
	// the strum itself, not a pick. Real chord->single picks come later than this, so a rushed
	// real pick inside the window only costs a re-pick.
	constexpr uint32_t CHORD_STRUM_TAIL_MS = 200;

	inline bool IsInsideChordStrumTail(uint64_t attackSample, uint64_t chordStrumSample, uint32_t sampleRate)
	{
		return chordStrumSample != 0 && sampleRate != 0 && attackSample >= chordStrumSample
			&& attackSample - chordStrumSample < static_cast<uint64_t>(sampleRate) * CHORD_STRUM_TAIL_MS / 1000;
	}

	// A chord followed by a plain single note always needs its own attack. The raw stream cannot
	// tell that pick from the chord strum's tail or a sloppy strum ringing the next note, so native
	// must hear the note. Legato successors (hammer/pull/tap) are exempt: they continue a ringing
	// string with no pick.
	inline bool ChordToSingleBoundaryRequiresNative(int previousString, bool isLegatoTarget)
	{
		return previousString == CHORD_STRING_SENTINEL && !isLegatoTarget;
	}

	// ...but only inside the chord strum's tail, where the raw stream cannot tell a pick from the
	// strum. Past it a raw-confirmed attack IS the pick, and requiring native there would stall
	// re-picks of the chord's own tones: native keeps reporting the chord's OTHER tone. With no
	// strum known (after a legato run) native is always required.
	inline bool ChordToSingleAttackNeedsNative(int previousString, bool isLegatoTarget,
		uint64_t attackSample, uint64_t chordStrumSample, uint32_t sampleRate)
	{
		return ChordToSingleBoundaryRequiresNative(previousString, isLegatoTarget)
			&& (chordStrumSample == 0 || IsInsideChordStrumTail(attackSample, chordStrumSample, sampleRate));
	}

	// The native detector can read one semitone off expectedMidi in Speaker Mode (the E/Eb
	// chart-1 frame), so tolerate +/-1. A ringing octave or fifth reads +11..+19 away and never
	// falls inside this window.
	inline bool NativeCorroboratesPick(int nativeMidi, int midi)
	{
		return nativeMidi >= 0 && std::abs(nativeMidi - midi) <= 1;
	}
}
