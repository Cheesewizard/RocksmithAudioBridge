#pragma once

#include <cmath>
#include <cstdint>
#include "OctaveCollisionGate.hpp"

// A low note needs 150 ms of audio to confirm, so when a chord is strummed shortly after a picked
// single note, the single's own attack can still be unresolved and gets dropped
// (PickedAttackQueue::Push). The strum may then confirm the single (e.g. the chord's octave sits
// on the note's 2f band) while native loudest pitch says the fresh energy belongs to the chord.
// These rules keep such a strum claimable by the successor chord (ChordAttackGate::Inherit), so a
// correct progression clears only through the attack it owned.
namespace NoteByNote
{
	// Mirrors RawPitchVerifier::ConfirmSnapshot attackCount: below 330 Hz nothing can confirm
	// until 150 ms after the attack; above it, 50 ms.
	inline uint32_t PickConfirmationWindowSamples(int midi, uint32_t sampleRate)
	{
		const double hz = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
		return static_cast<uint32_t>((hz < 330.0 ? 0.15 : 0.05) * sampleRate);
	}

	// The older unresolved attack was ended before it had the audio a confirmation needs.
	inline bool PredecessorAttackWasCutShort(uint64_t olderSample, uint64_t newerSample,
		int midi, uint32_t sampleRate)
	{
		return olderSample != 0 && sampleRate != 0 && newerSample > olderSample
			&& newerSample - olderSample < PickConfirmationWindowSamples(midi, sampleRate);
	}

	// The note had a cut-short attack of its own, and native hears the loudest fresh energy at
	// another pitch: the attack that confirmed the note is the successor's strum.
	inline bool StrumBelongsToSuccessor(uint64_t cutShortPredecessorSample, int nativeLoudest, int midi)
	{
		return cutShortPredecessorSample != 0 && nativeLoudest >= 0
			&& !NativeCorroboratesPick(nativeLoudest, midi);
	}

	// Only worth inheriting while a chord could still consume it (same 300 ms as HasFreshAttack).
	inline bool StrumHandoffIsFresh(uint64_t strumSample, uint64_t nowSample, uint32_t sampleRate)
	{
		return strumSample != 0 && sampleRate != 0 && nowSample > strumSample
			&& nowSample - strumSample <= sampleRate * 3ULL / 10;
	}
}
