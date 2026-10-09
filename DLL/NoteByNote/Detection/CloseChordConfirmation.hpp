#pragma once

#include <cmath>
#include <cstdint>

namespace NoteByNote
{
	inline bool SupportsCloseChord(const int* tones, int count)
	{
		return tones && count == 2 && tones[0] >= 57 && tones[0] <= 88
			&& tones[1] >= 57 && tones[1] <= 88
			&& std::abs(tones[0] - tones[1]) >= 1 && std::abs(tones[0] - tones[1]) <= 2;
	}

	inline bool ConfirmCloseChord(const float* samples, uint32_t sampleCount, uint32_t sampleRate,
		const int* tones, int toneCount)
	{
		if (!samples || !SupportsCloseChord(tones, toneCount) || sampleRate < 8000 || sampleRate > 96000) return false;
		const uint32_t count = sampleRate / 4;
		if (count > sampleCount || count > 24000) return false;
		const float* source = samples + sampleCount - count;
		float window[24000];
		double energy = 0.0;
		for (uint32_t i = 0; i < count; ++i)
		{
			if (!std::isfinite(source[i])) return false;
			energy += source[i] * source[i];
			window[i] = static_cast<float>(source[i] * (0.5 - 0.5 * std::cos(6.283185307179586 * i / (count - 1))));
		}
		energy /= count;
		if (energy < 1e-6) return false;
		auto powerAt = [&](double frequency)
		{
			const double coefficient = 2.0 * std::cos(6.283185307179586 * frequency / sampleRate);
			double previous = 0.0, beforePrevious = 0.0;
			for (uint32_t i = 0; i < count; ++i)
			{
				const double next = window[i] + coefficient * previous - beforePrevious;
				beforePrevious = previous;
				previous = next;
			}
			return (previous * previous + beforePrevious * beforePrevious - coefficient * previous * beforePrevious)
				* 16.0 / (static_cast<double>(count) * count);
		};
		// Normalized Goertzel power over a sub-window [start, start+length) of the raw source,
		// each with its own Hann so a partial-window tone cannot borrow the full-window peak.
		auto powerIn = [&](uint32_t start, uint32_t length, double frequency)
		{
			if (length < 2) return 0.0;
			const double coefficient = 2.0 * std::cos(6.283185307179586 * frequency / sampleRate);
			double previous = 0.0, beforePrevious = 0.0;
			for (uint32_t i = 0; i < length; ++i)
			{
				const double windowed = source[start + i]
					* (0.5 - 0.5 * std::cos(6.283185307179586 * i / (length - 1)));
				const double next = windowed + coefficient * previous - beforePrevious;
				beforePrevious = previous;
				previous = next;
			}
			return (previous * previous + beforePrevious * beforePrevious - coefficient * previous * beforePrevious)
				* 16.0 / (static_cast<double>(length) * length);
		};
		for (int tone = 0; tone < toneCount; ++tone)
		{
			double bestPower = 0.0, bestFrequency = 0.0;
			for (int cents = -30; cents <= 30; cents += 10)
			{
				const double frequency = 440.0 * std::pow(2.0, (tones[tone] - 69.0 + cents / 100.0) / 12.0);
				const double power = powerAt(frequency);
				if (power > bestPower) { bestPower = power; bestFrequency = frequency; }
			}
			if (bestPower < 1e-5 || bestPower < energy * 0.03) return false;
			// Each note needs its own spectral peak. Merely exempting a neighbouring
			// chord tone from a dominance test lets one note's leakage count twice.
			const double shoulderRatio = std::pow(2.0, 40.0 / 1200.0);
			if (bestPower < 1.15 * powerAt(bestFrequency / shoulderRatio)
				|| bestPower < 1.15 * powerAt(bestFrequency * shoulderRatio)) return false;
			// A note that only occupies part of the window (one note gliding or re-fretted
			// from N+1 to N partway through) integrates into two peaks over 250 ms but is absent from one half. A
			// sounding dyad holds both tones through the whole window; each stays within
			// ~6 dB across the two 125 ms halves, far above this balance floor.
			constexpr double CLOSE_CHORD_HALF_BALANCE = 0.25;
			const uint32_t half = count / 2;
			const double early = powerIn(0, half, bestFrequency);
			const double late = powerIn(half, count - half, bestFrequency);
			const double lesser = early < late ? early : late;
			const double greater = early < late ? late : early;
			if (greater <= 0.0 || lesser < greater * CLOSE_CHORD_HALF_BALANCE) return false;
		}
		return true;
	}
}
