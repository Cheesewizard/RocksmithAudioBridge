#pragma once

#include <cmath>
#include <cstdint>

namespace NoteByNote
{
	// Pitch-agnostic re-attack test for FRET-HAND-MUTED targets.
	//
	// A fret-hand mute has no reliable pitch, so the pitched validators (ConfirmChordAttack,
	// the single-note attackChange gate) are intentionally skipped for it. A bare onset flag
	// is not enough: a decaying ring can throw a spurious HFC onset (an open low E can produce
	// one ~2 s into its sustain, with zero energy rise) and satisfy the next muted target
	// with no fresh strike.
	//
	// A real mute IS a percussive strike: it raises BROADBAND energy across the attack. A
	// decaying ring only loses energy. So require the post-attack window to gain energy over
	// the louder of the two pre-attack windows, by the same relative-excitation margin the
	// chord validator uses (0.04 in power == 0.2 in amplitude). This is pitch-agnostic on
	// purpose, so a legitimately toneless mute still passes on its transient alone.
	//
	// Layout matches ConfirmChordAttack's call site: `samples` points at attackSample-2*window
	// and holds three adjacent windows of `window` samples (pre, pre, post), Hann-weighted.
	inline bool ConfirmMutedAttack(const float* samples, uint32_t window, uint32_t rate)
	{
		if (!samples || window < 8 || rate == 0) return false;
		constexpr double MINIMUM_RELATIVE_EXCITATION_AMPLITUDE = 0.2; // sqrt(0.04), as ConfirmChordAttack
		constexpr double TWO_PI = 6.283185307179586;
		double amplitude[3] = {};
		for (int frame = 0; frame < 3; ++frame)
		{
			double weightedPower = 0.0, weightSum = 0.0;
			for (uint32_t index = 0; index < window; ++index)
			{
				const float sample = samples[frame * window + index];
				if (!std::isfinite(sample)) return false;
				const double weight = 0.5 - 0.5 * std::cos(TWO_PI * index / (window - 1));
				weightedPower += weight * static_cast<double>(sample) * sample;
				weightSum += weight;
			}
			amplitude[frame] = weightSum > 0.0 ? std::sqrt(weightedPower / weightSum) : 0.0;
		}
		const double baseline = amplitude[0] > amplitude[1] ? amplitude[0] : amplitude[1];
		const double scale = baseline > amplitude[2] ? baseline : amplitude[2];
		if (scale < 1e-4) return false; // silence: no transient to confirm
		const double rise = amplitude[2] > baseline ? amplitude[2] - baseline : 0.0;
		return rise >= scale * MINIMUM_RELATIVE_EXCITATION_AMPLITUDE;
	}
}
