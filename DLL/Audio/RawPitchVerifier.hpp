#pragma once

#include <cstdint>
#include "RawAttackEvidence.hpp"

// Tier 0 of the detection stack: a Goertzel measurement bank over the raw input route
// samples, observed at the same GetBuffer choke point the input processors run at (the
// exact audio the game's detector receives). The game's own spectral features are
// semitone-quantized with an inconsistent +/-1 read bias, which makes adjacent-fret
// discrimination impossible downstream of them; this module measures energy at exact
// frequencies (cent resolution) upstream of that quantization, answering whether the
// expected note's fundamental is sounding versus its one-fret neighbours. Deterministic
// DSP, no model; the Note by Note freeze makes it latency-tolerant.
namespace RawPitchVerifier
{
	// AudioSnapshot is defined in RawAttackEvidence.hpp so it can cross the host/probe
	// boundary as a shared POD.

	bool CaptureSnapshot(AudioSnapshot& out);
	bool QueryAttacks(uint64_t afterSampleIndex, RawAttackBatch& out);
	bool MeasureSnapshot(const AudioSnapshot& snapshot, double frequencyHz,
		float windowSeconds, float& power, float& rms, uint32_t& sampleCount);

	struct NoteConfirmation
	{
		uint64_t endSampleIndex = 0;
		uint32_t sampleRate = 0;
		float targetPower = 0.0f;
		float attackChange = 0.0f;
		float attackPower = 0.0f;
		float attackMinusPower = 0.0f;
		float attackPlusPower = 0.0f;
		float neighbourPower = 0.0f;
		bool confirmed = false;
	};

	bool ConfirmSnapshot(const AudioSnapshot& snapshot, double frequencyHz, NoteConfirmation& out);
	bool QueryNoteConfirmation(double frequencyHz, NoteConfirmation& out, uint64_t minimumSampleIndex = 0, uint64_t maximumSampleIndex = 0);

	// Goertzel powers around a target fundamental. Powers are normalized by
	// (windowSampleCount/2)^2 so a full-scale sine at the bin reads ~1.0 regardless of
	// window length; compare them against each other, not against absolute thresholds.
	struct ToneEvidence
	{
		uint32_t sampleRate = 0;
		uint32_t windowSampleCount = 0;
		float totalRms = 0.0f;
		float targetPower = 0.0f;
		float minusOnePower = 0.0f;   // one semitone below the target
		float plusOnePower = 0.0f;    // one semitone above
		float minusTwoPower = 0.0f;
		float plusTwoPower = 0.0f;
	};

	// Audio-thread feed: append post-processing route samples (mono float) for Player 1
	// (routeIndex 0). Other routes are ignored for now. Silent buffers should be fed as
	// zeros so the ring's timeline stays continuous.
	void Observe(uint32_t routeIndex, const float* samples, uint32_t count, uint32_t sampleRate);

	// True once the ring holds at least one full window at a known sample rate.
	bool IsReady();

	// Measure the last windowSeconds of route audio at frequencyHz and its +/-1 and
	// +/-2 semitone neighbours. Safe from any thread; returns false when not enough
	// audio has been observed yet. windowSeconds is clamped to [0.05, 0.3].
	bool QueryToneEvidence(double frequencyHz, float windowSeconds, ToneEvidence& out);
}
