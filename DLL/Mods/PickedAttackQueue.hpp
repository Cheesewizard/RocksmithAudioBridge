#pragma once

#include <array>
#include <cstdint>
#include "DetectionFeedback.hpp"

namespace NoteByNote
{
	struct PickedAttack
	{
		double time = 0.0;
		uint64_t minimumSample = 0;
		// The unresolved attack this one ended, when that attack never had its confirmation
		// window (StrumHandoff.hpp); 0 otherwise.
		uint64_t cutShortPredecessorSample = 0;
		// Set at confirmation: this attack confirmed the note but is the successor's strum.
		bool strumBelongsToSuccessor = false;
		// A kept cut-short attack (see Push) whose full confirmation window has been evaluated
		// without confirming; Take discards it instead of waiting on it.
		bool rejected = false;
		uint64_t minimumMlSample = 0;
		uint64_t confirmedSample = 0;
		uint32_t sampleRate = 0;
		int candidateMidi = -1;
		int confirmedMidi = -1;
		// Nonzero: this pick was made after the record named here had already taken its pick, so
		// it belongs to the NEXT note. It is not judged until the target moves off that record.
		uintptr_t awaitingTargetAfter = 0;
		// The secured note a play-ahead pick followed (its pitch, still ringing); -1 otherwise.
		int playAheadFrom = -1;
		// What the game's detector heard during this attack's first ~300 ms, sampled every tick.
		// The loudest-note query and sounding table are live-only, so an attack judged later
		// (buffered, played ahead, or while the previous note was still loudest) is judged on
		// what was heard during it rather than what the detector reports at that moment.
		struct HeardPitch
		{
			int16_t midi = -1;
			uint16_t loudestTicks = 0;     // ticks the loudest-note query named this pitch
			float firstStrength = 0.0f;    // sounding-table strength at the first sample (0 = absent)
			float peakStrength = 0.0f;     // highest sounding-table strength seen
		};
		std::array<HeardPitch, 16> heard = {};
		uint8_t heardCount = 0;
		bool heardSampled = false;
		float heardThreshold = 0.0f;

		HeardPitch* FindOrAddHeard(int midi)
		{
			for (uint8_t index = 0; index < heardCount; ++index)
				if (heard[index].midi == midi) return &heard[index];
			if (heardCount == heard.size()) return nullptr;
			heard[heardCount].midi = static_cast<int16_t>(midi);
			return &heard[heardCount++];
		}

		const HeardPitch* FindHeard(int midi) const
		{
			for (uint8_t index = 0; index < heardCount; ++index)
				if (heard[index].midi == midi) return &heard[index];
			return nullptr;
		}
		DetectionFeedback feedback;
		int baselineMidi[2] = { -1, -1 };
		float baselinePower[2] = {};
		float baselineMinusPower[2] = {};
		float baselinePlusPower[2] = {};
	};

	class PickedAttackQueue
	{
	public:
		static constexpr unsigned CAPACITY = 32;
		static constexpr double MAX_AGE_SECONDS = 0.75;

		void Clear()
		{
			count = 0;
		}

		unsigned Count() const { return count; }

		PickedAttack* Latest()
		{
			return count == 0 ? nullptr : &attacks[count - 1];
		}

		PickedAttack* At(unsigned index)
		{
			return index < count ? &attacks[index] : nullptr;
		}

		unsigned Expire(double now)
		{
			unsigned expired = 0;
			while (count != 0 && now - attacks[0].time > MAX_AGE_SECONDS)
			{
				RemoveFirst();
				++expired;
			}
			return expired;
		}

		bool Push(const PickedAttack& attack)
		{
			if (count != 0 && attack.time <= attacks[count - 1].time) return false;
			// An unresolved attack that already had its confirmation window cannot borrow the next
			// attack's pitch evidence, so it is replaced. One the new attack CUT SHORT (it never had
			// the window a low note needs) is kept instead and confirmed once its window has passed:
			// fast repeated picking of a low note (~100 ms apart against a 150 ms window) would
			// otherwise discard every pick until the player paused.
			// For a repeated note the borrowed audio is the same note, so the evidence still holds.
			if (count != 0 && attacks[count - 1].confirmedMidi < 0
				&& (attack.cutShortPredecessorSample == 0
					|| attack.cutShortPredecessorSample != attacks[count - 1].minimumSample)) --count;
			if (count == CAPACITY) RemoveFirst();
			attacks[count++] = attack;
			return true;
		}

		// keepMismatched (bass): an attack confirmed as another pitch stays queued until
		// it expires, so the NEXT target can re-judge it (a note played early). The first attack
		// confirmed as the target is taken, with everything older than it.
		bool Take(int expectedMidi, PickedAttack& out, bool keepMismatched = false)
		{
			if (keepMismatched)
			{
				for (unsigned index = 0; index < count; ++index)
				{
					if (attacks[index].rejected || attacks[index].confirmedMidi != expectedMidi) continue;
					out = attacks[index];
					for (unsigned drop = 0; drop <= index; ++drop) RemoveFirst();
					return true;
				}
				return false;
			}
			while (count != 0 && (attacks[0].confirmedMidi >= 0 || attacks[0].rejected))
			{
				const PickedAttack attack = attacks[0];
				RemoveFirst();
				if (attack.rejected || attack.confirmedMidi != expectedMidi) continue;
				out = attack;
				return true;
			}
			return false;
		}

		// Drops the first n attacks (oldest first).
		void RemoveFirstCount(unsigned n)
		{
			while (n-- != 0 && count != 0) RemoveFirst();
		}

	private:
		std::array<PickedAttack, CAPACITY> attacks = {};
		unsigned count = 0;

		void RemoveFirst()
		{
			for (unsigned index = 1; index < count; ++index) attacks[index - 1] = attacks[index];
			--count;
		}
	};
}
