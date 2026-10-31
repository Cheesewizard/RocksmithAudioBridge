#pragma once

#include <cstdint>

namespace NoteByNote
{
	class ChordAttackGate
	{
	public:
		static bool IsFretHandMuted(uint32_t noteMask)
		{
			return (noteMask & 0x00000008u) != 0;
		}

		bool TryConsumeForNote(bool pitchesMatch, uint64_t now, uint32_t sampleRate)
		{
			return TryConsume(pitchesMatch, now, sampleRate);
		}

		void Clear()
		{
			attackSample = 0;
			lastObservedSample = 0;
			consumedThroughSample = 0;
			inheritedAttackSample = 0;
			inheritedHoldSample = 0;
		}

		void BeginHold(uint64_t sample)
		{
			attackSample = 0;
			if (sample > consumedThroughSample) consumedThroughSample = sample;
		}

		uint64_t GetAttackSample() const { return attackSample; }

		// A strum played BEFORE this hold that the single-note predecessor confirmed off but did
		// not own (StrumHandoff.hpp). It becomes the pending attack, aged from the hold sample so
		// the chord gets its normal fresh-attack and corroboration budget from the first tick it
		// can be evaluated. Idempotent across the two hold resets a dense successor performs;
		// refused once consumed, and never displaces a newer real strum.
		bool Inherit(uint64_t sample, uint64_t holdSample)
		{
			if (sample == 0 || sample <= consumedThroughSample || holdSample < sample) return false;
			if (attackSample != 0 && attackSample > sample) return false;
			if (sample > lastObservedSample) lastObservedSample = sample;
			attackSample = sample;
			inheritedAttackSample = sample;
			inheritedHoldSample = holdSample;
			return true;
		}

		// Where the pending attack's age is measured from: the hold for an inherited strum, the
		// strum itself otherwise. Raw-audio matchers still read at GetAttackSample().
		uint64_t GetAgeOriginSample() const
		{
			return attackSample != 0 && attackSample == inheritedAttackSample
				? inheritedHoldSample : attackSample;
		}

		uint64_t GetConsumedThroughSample() const { return consumedThroughSample; }

		bool Observe(uint64_t sample)
		{
			if (sample <= lastObservedSample || sample <= consumedThroughSample) return false;
			// Pre-hold audio after an inherited strum is that strum's own tail re-flagged, not a
			// new attack; letting it replace the strum would age the chord from the tail.
			if (attackSample != 0 && attackSample == inheritedAttackSample
				&& sample <= inheritedHoldSample) return false;
			lastObservedSample = sample;
			attackSample = sample;
			return true;
		}

		bool HasFreshAttack(uint64_t now, uint32_t sampleRate) const
		{
			const uint64_t origin = GetAgeOriginSample();
			return sampleRate != 0 && origin != 0 && now >= origin
				&& now - origin <= sampleRate * 3ULL / 10;
		}

		bool TryConsume(bool chordMatches, uint64_t now, uint32_t sampleRate)
		{
			if (!chordMatches || !HasFreshAttack(now, sampleRate)) return false;
			consumedThroughSample = now;
			attackSample = 0;
			return true;
		}

	private:
		uint64_t attackSample = 0;
		uint64_t lastObservedSample = 0;
		uint64_t consumedThroughSample = 0;
		uint64_t inheritedAttackSample = 0;
		uint64_t inheritedHoldSample = 0;
	};
}
