#pragma once

#include <cmath>
#include <cstdint>

// Strum-anchored chord presence. Answers "were the chord's tones in the raw audio right after
// the strum?" so a late native-matcher hit can be told apart from a slide.
//
// The unvoted chord path only accepts within 100 ms of the strum (a slide guard), but the
// game's chord matcher can need 90-145 ms to recognise a clean strum, and a wider window lets
// a wrong strum plus a slide onto the right chord pass. Age cannot separate the two; what was
// strummed can. A correct strum carries its tones from the attack onward; a
// wrong strum carries other pitches until the hand moves.
//
// Window: 80 ms starting 5 ms after the attack (skips the pick transient). 80 ms cannot
// separate semitones on low fundamentals (Hann main lobe half width 25 Hz), so each tone is
// measured at the lowest harmonic whose semitone spacing is at least 35 Hz.
namespace NoteByNote
{
	constexpr double STRUM_PRESENCE_START_SECONDS = 0.005;
	constexpr double STRUM_PRESENCE_WINDOW_SECONDS = 0.080;
	constexpr double STRUM_PRESENCE_MIN_SEMITONE_HZ = 35.0;
	constexpr double STRUM_PRESENCE_ENERGY_FRACTION = 0.002;
	constexpr double STRUM_PRESENCE_NEIGHBOUR_RATIO = 1.5;

	struct StrumToneReading
	{
		int midi = -1;
		int harmonic = 0;
		float power = 0.0f;
		float neighbour = 0.0f;
		bool present = false;
	};

	struct StrumChordPresence
	{
		int toneCount = 0;
		int presentCount = 0;
		int requiredCount = 0;
		float energy = 0.0f;
		bool confirmed = false;
		StrumToneReading tones[6];
	};

	inline uint32_t GetStrumPresenceSampleSpan(uint32_t sampleRate)
	{
		return static_cast<uint32_t>(sampleRate * (STRUM_PRESENCE_START_SECONDS + STRUM_PRESENCE_WINDOW_SECONDS)) + 1;
	}

	// samples[0] is the attack sample; sampleCount must reach GetStrumPresenceSampleSpan.
	inline bool MeasureStrumChordPresence(const float* samples, uint32_t sampleCount, uint32_t sampleRate,
		const int* tones, int toneCount, StrumChordPresence& out)
	{
		out = {};
		if (!samples || !tones || toneCount < 2 || toneCount > 6 || sampleRate < 8000 || sampleRate > 96000)
			return false;
		const uint32_t start = static_cast<uint32_t>(sampleRate * STRUM_PRESENCE_START_SECONDS);
		const uint32_t length = static_cast<uint32_t>(sampleRate * STRUM_PRESENCE_WINDOW_SECONDS);
		if (length < 64 || length > 8192 || start + length > sampleCount) return false;

		float window[8192];
		double energy = 0.0;
		for (uint32_t i = 0; i < length; ++i)
		{
			const float value = samples[start + i];
			if (!std::isfinite(value)) return false;
			energy += static_cast<double>(value) * value;
			window[i] = static_cast<float>(value * (0.5 - 0.5 * std::cos(6.283185307179586 * i / (length - 1))));
		}
		energy /= length;
		out.energy = static_cast<float>(energy);
		out.toneCount = toneCount;
		out.requiredCount = toneCount - (toneCount >= 5 ? 1 : 0);
		if (energy < 1e-7) return true;

		const double nyquistLimit = sampleRate * 0.45;
		auto powerAt = [&](double frequency)
		{
			if (frequency <= 0.0 || frequency >= nyquistLimit) return 0.0;
			const double coefficient = 2.0 * std::cos(6.283185307179586 * frequency / sampleRate);
			double previous = 0.0, beforePrevious = 0.0;
			for (uint32_t i = 0; i < length; ++i)
			{
				const double next = window[i] + coefficient * previous - beforePrevious;
				beforePrevious = previous;
				previous = next;
			}
			return (previous * previous + beforePrevious * beforePrevious - coefficient * previous * beforePrevious)
				* 16.0 / (static_cast<double>(length) * length);
		};
		auto midiHz = [](double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); };
		const double semitone = std::pow(2.0, 1.0 / 12.0);

		for (int t = 0; t < toneCount; ++t)
		{
			StrumToneReading& reading = out.tones[t];
			reading.midi = tones[t];
			if (tones[t] < 0) continue;
			const double f0 = midiHz(tones[t]);
			// A harmonic that coincides with a harmonic of another chord tone (a fifth's 2nd is
			// the root's 3rd; an octave shares all of them) is lit by that tone and proves
			// nothing. Use the lowest unshared harmonic with enough semitone spacing, else the
			// lowest unshared one. A tone with no unshared harmonic (an octave double) cannot be
			// told apart from its partner and is left out of the count.
			auto isShared = [&](double frequency)
			{
				for (int o = 0; o < toneCount; ++o)
				{
					if (o == t || tones[o] < 0) continue;
					const double other = midiHz(tones[o]);
					for (int k = 1; k <= 8; ++k)
						if (std::fabs(1200.0 * std::log2(frequency / (k * other))) <= 40.0) return true;
				}
				return false;
			};
			// Fallback = the HIGHEST unshared harmonic, not the lowest: on low strings (open E + A,
			// 82/110 Hz) h1 has a 5 Hz semitone, far inside the window's resolution, so every strum
			// would read as a neighbour rise. Up to h8 keeps low strings resolvable (E2 h7 = 34 Hz).
			int harmonic = 0, fallback = 0;
			for (int h = 1; h <= 8 && harmonic == 0; ++h)
			{
				if (h * f0 * semitone >= nyquistLimit || isShared(h * f0)) continue;
				fallback = h;
				if (h * f0 * (semitone - 1.0) >= STRUM_PRESENCE_MIN_SEMITONE_HZ) harmonic = h;
			}
			if (harmonic == 0) harmonic = fallback;
			if (harmonic == 0)
			{
				reading.harmonic = 0;   // octave double: not measurable, not counted
				--out.requiredCount;
				continue;
			}
			reading.harmonic = harmonic;
			const double target = harmonic * f0;
			double bestPower = 0.0, bestFrequency = target;
			for (int cents = -30; cents <= 30; cents += 10)
			{
				const double frequency = target * std::pow(2.0, cents / 1200.0);
				const double power = powerAt(frequency);
				if (power > bestPower) { bestPower = power; bestFrequency = frequency; }
			}
			// A semitone neighbour that sits on a harmonic of another chord tone is lit by
			// that tone; it must not veto this one.
			double neighbour = 0.0;
			for (int side = 0; side < 2; ++side)
			{
				const double frequency = side == 0 ? bestFrequency * semitone : bestFrequency / semitone;
				bool litByChord = false;
				for (int o = 0; o < toneCount && !litByChord; ++o)
				{
					if (o == t || tones[o] < 0) continue;
					const double other = midiHz(tones[o]);
					for (int k = 1; k <= 8; ++k)
					{
						if (std::fabs(1200.0 * std::log2(frequency / (k * other))) <= 40.0) { litByChord = true; break; }
					}
				}
				if (litByChord) continue;
				const double power = powerAt(frequency);
				if (power > neighbour) neighbour = power;
			}
			reading.power = static_cast<float>(bestPower);
			reading.neighbour = static_cast<float>(neighbour);
			reading.present = bestPower >= energy * STRUM_PRESENCE_ENERGY_FRACTION
				&& bestPower >= neighbour * STRUM_PRESENCE_NEIGHBOUR_RATIO;
			if (reading.present) ++out.presentCount;
		}
		out.confirmed = out.requiredCount >= 1 && out.presentCount >= out.requiredCount;
		return true;
	}
}
