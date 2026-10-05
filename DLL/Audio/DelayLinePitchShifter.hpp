#pragma once

#include "IInputProcessor.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace Audio
{
	// The jump a splice will commit, and how well the two crossfaded segments actually
	// match (0 = identical, ~1 = uncorrelated). Poor quality means no alignment exists
	// in the searched range - a transient or an unresolvable chord - and the splicer
	// uses that to defer or soften the splice instead of popping.
	struct AlignedJump
	{
		double jump;
		float quality;
	};

	// Pitch shifter using period-synchronous splicing, the technique real drop pedals use.
	// A single read tap trails the write head at the pitch ratio; whenever the tap drifts
	// one detected pitch period behind (or ahead), it jumps by exactly that period under a
	// short crossfade. Because the jump equals the waveform's own period, the output stays
	// periodic at the shifted pitch, unlike free-running dual-tap designs whose crossfade
	// cycle amplitude-modulates the signal at a rate guitar pitch trackers cannot ignore.
	//
	// The period comes from an AMDF detector running on a 4x-decimated copy of the input,
	// but the detector only sizes and centers each jump. The committed splice offset comes
	// from a phase-alignment search on the ring content itself (AlignJump), the Eventide
	// H949 de-glitcher approach, so splices do not inherit detector error. When no good
	// alignment exists, down-shifts commit with a longer crossfade instead of adding
	// input delay. Up-shifts may defer briefly while their tap still has write-head
	// clearance, so chords and transients degrade to softness rather than pops.
	class DelayLinePitchShifter final : public IInputProcessor
	{
	public:
		explicit DelayLinePitchShifter(int semitones);

		void SetSemitones(int semitones);
		void SetPitchDetectionEnabled(bool enabled);
		bool TryGetDetectedMidi(int& midi) const;

		void Prepare(const CaptureFormat& format) override;
		bool Process(float* samples, uint32_t frameCount) override;
		uint32_t GetLatencyFrames() const override;
		// The delay the shifter is adding right now (smoothed), in frames; 0 when passing through.
		float GetLiveDelayFrames() const { return liveDelayFrames.load(std::memory_order_relaxed); }

	private:
		std::atomic<float> ratio;
		// Smoothed distance between the read tap and the write point, in frames: the delay
		// the shifter is adding RIGHT NOW. Drifts with the shift amount and the note's
		// period and splices back by whole periods, so it is averaged over ~0.5 s for the
		// overlay. 0 while passing audio through.
		std::atomic<float> liveDelayFrames{ 0.0f };
		std::atomic<bool> isPitchDetectionEnabled{ false };
		std::atomic<int> detectedMidi{ -1 };
		std::atomic<uint32_t> missedPitchDetections{ 0 };
		uint32_t sampleRate = 48000;

		// Splicer state
		std::vector<float> ring;
		uint32_t writePosition = 0;
		double readDelay = 0.0;
		double fadeFromDelay = 0.0;
		int fadeLength = 64;
		int fadeRemaining = 0;
		int spliceHoldoff = 0;

		// Period detector state. Periods are fractional: a string's true period is not a
		// whole number of samples, and splicing by a rounded period lands every splice up
		// to half a sample out of phase, heard as signal-gated buzz at the splice rate.
		std::vector<float> decimated;
		uint32_t decimatedPosition = 0;
		float decimationAccumulator = 0.0f;
		uint32_t decimationPhase = 0;
		uint32_t samplesSinceDetect = 0;
		double periodSamples = 0.0;
		double candidatePeriod = 0.0;
		int candidateVotes = 0;

		void StoreInputSample(float sample);
		float ReadTap(double delay) const;
		void DetectPeriod();
		double RefineAtFullRate(int coarsePeriod) const;
		AlignedJump AlignJump(double fromDelay, double nominalJump, double maxJump) const;
	};
}
