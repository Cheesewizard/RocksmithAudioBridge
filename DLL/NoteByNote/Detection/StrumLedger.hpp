#pragma once

#include <cmath>
#include <cstdint>

// Strum Ledger. Per chord tone: how much did it RISE at this strum, compared with the moment
// just before it? The pick buffer's rule (RawPitchVerifier attack bands vs preceding bands)
// applied to every chord tone.
//
// Why: the game's sounding table and vote are wrong in both directions on some dyads. They can
// hear an upper tone from the lower one alone (64's 5th harmonic is ~10 cents from 68's 4th),
// and they miss real strums. StrumChordPresence compares levels, so a note still ringing from
// the previous chord next to a chord tone reads as a wrong neighbour. Comparing RISE to RISE
// fixes that without tolerating a wrong fret: a wrong fret played now rises; a ring does not.
//
// Reads (all anchored at the attack): early = 60 ms window from +5 ms with harmonics chosen for
// >= 50 Hz semitone spacing; standard = 80 ms window from +5 ms; late = 80 ms window ending at
// +120 ms (catches a rolled strum's later strings). Baseline = 80 ms ending 5 ms before the attack.
namespace NoteByNote
{
	constexpr double LEDGER_BASELINE_SECONDS = 0.080;
	constexpr double LEDGER_BASELINE_GAP_SECONDS = 0.005;
	constexpr double LEDGER_ENERGY_FRACTION = 0.002;
	constexpr double LEDGER_NEIGHBOUR_RATIO = 1.5;
	// A string re-struck over its own ring (the same chord again) adds energy but rarely doubles
	// it; a ring left alone only decays (post < baseline), which is what separates the ringing-tone
	// single-note false accepts.
	constexpr double LEDGER_BASELINE_RATIO = 1.2;

	// The ledger decides pitched chords (see NoteByNoteNativeScoring "STAGE 2 Strum Ledger").
	// Set false to use the sounding-table rules.
	constexpr bool STRUM_LEDGER_DECIDES = true;

	enum class LedgerRead { Early, Standard, Late };

	struct LedgerReadSpec
	{
		double startSeconds;
		double lengthSeconds;
		double minSemitoneHz;
	};

	inline LedgerReadSpec GetLedgerReadSpec(LedgerRead read)
	{
		switch (read)
		{
			case LedgerRead::Early: return { 0.005, 0.060, 50.0 };
			case LedgerRead::Standard: return { 0.005, 0.080, 35.0 };
			default: return { 0.040, 0.080, 35.0 };
		}
	}

	inline const char* GetLedgerReadName(LedgerRead read)
	{
		switch (read)
		{
			case LedgerRead::Early: return "65ms";
			case LedgerRead::Standard: return "85ms";
			default: return "120ms";
		}
	}

	struct LedgerToneReading
	{
		int midi = -1;
		int harmonic = 0;      // 0 = octave double, not measurable, not counted
		float baseline = 0.0f;
		float post = 0.0f;
		float rise = 0.0f;
		float neighbourRise = 0.0f;
		bool struck = false;
		bool wrongNeighbour = false;
	};

	struct LedgerReading
	{
		int toneCount = 0;
		int struckCount = 0;
		int requiredCount = 0;
		float energy = 0.0f;
		bool strummed = false;
		bool wrongStrum = false;
		LedgerToneReading tones[6];
	};

	inline uint32_t GetLedgerPreSamples(uint32_t sampleRate)
	{
		return static_cast<uint32_t>(sampleRate * (LEDGER_BASELINE_SECONDS + LEDGER_BASELINE_GAP_SECONDS)) + 1;
	}

	inline uint32_t GetLedgerPostSamples(LedgerRead read, uint32_t sampleRate)
	{
		const LedgerReadSpec spec = GetLedgerReadSpec(read);
		return static_cast<uint32_t>(sampleRate * (spec.startSeconds + spec.lengthSeconds)) + 1;
	}

	// samples[attackOffset] is the attack sample.
	inline bool MeasureStrumLedger(const float* samples, uint32_t sampleCount, uint32_t attackOffset,
		uint32_t sampleRate, const int* tones, int toneCount, LedgerRead read, LedgerReading& out)
	{
		out = {};
		if (!samples || !tones || toneCount < 2 || toneCount > 6 || sampleRate < 8000 || sampleRate > 96000)
			return false;
		const LedgerReadSpec spec = GetLedgerReadSpec(read);
		const uint32_t baseLength = static_cast<uint32_t>(sampleRate * LEDGER_BASELINE_SECONDS);
		const uint32_t baseGap = static_cast<uint32_t>(sampleRate * LEDGER_BASELINE_GAP_SECONDS);
		const uint32_t postStart = static_cast<uint32_t>(sampleRate * spec.startSeconds);
		const uint32_t postLength = static_cast<uint32_t>(sampleRate * spec.lengthSeconds);
		if (attackOffset < baseLength + baseGap || attackOffset + postStart + postLength > sampleCount
			|| postLength > 8192 || baseLength > 8192) return false;
		const float* base = samples + attackOffset - baseGap - baseLength;
		const float* post = samples + attackOffset + postStart;

		double energy = 0.0;
		for (uint32_t i = 0; i < postLength; ++i)
		{
			if (!std::isfinite(post[i]) ) return false;
			energy += static_cast<double>(post[i]) * post[i];
		}
		energy /= postLength;
		out.energy = static_cast<float>(energy);
		out.toneCount = toneCount;
		out.requiredCount = toneCount - (toneCount >= 5 ? 1 : 0);

		const double nyquistLimit = sampleRate * 0.45;
		auto powerAt = [&](const float* window, uint32_t length, double frequency)
		{
			if (frequency <= 0.0 || frequency >= nyquistLimit) return 0.0;
			const double coefficient = 2.0 * std::cos(6.283185307179586 * frequency / sampleRate);
			double previous = 0.0, beforePrevious = 0.0;
			for (uint32_t i = 0; i < length; ++i)
			{
				const double hann = 0.5 - 0.5 * std::cos(6.283185307179586 * i / (length - 1));
				const double next = window[i] * hann + coefficient * previous - beforePrevious;
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
			LedgerToneReading& reading = out.tones[t];
			reading.midi = tones[t];
			if (tones[t] < 0) continue;
			const double f0 = midiHz(tones[t]);
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
			// Fallback = the HIGHEST unshared harmonic, not the lowest: for low strings (open E + A,
			// 82/110 Hz) h1 has a 5 Hz semitone, far inside the window's resolution, so every strum
			// would read as a neighbour rise. Up to h8 keeps low strings resolvable (E2 h7 = 34 Hz).
			int harmonic = 0, fallback = 0;
			for (int h = 1; h <= 8 && harmonic == 0; ++h)
			{
				if (h * f0 * semitone >= nyquistLimit || isShared(h * f0)) continue;
				fallback = h;
				if (h * f0 * (semitone - 1.0) >= spec.minSemitoneHz) harmonic = h;
			}
			if (harmonic == 0) harmonic = fallback;
			if (harmonic == 0)
			{
				--out.requiredCount;
				continue;
			}
			reading.harmonic = harmonic;
			const double target = harmonic * f0;
			double bestPost = 0.0, bestFrequency = target;
			for (int cents = -30; cents <= 30; cents += 10)
			{
				const double frequency = target * std::pow(2.0, cents / 1200.0);
				const double power = powerAt(post, postLength, frequency);
				if (power > bestPost) { bestPost = power; bestFrequency = frequency; }
			}
			const double baseline = powerAt(base, baseLength, bestFrequency);
			const double rise = bestPost > baseline ? bestPost - baseline : 0.0;
			double neighbourRise = 0.0;
			for (int side = 0; side < 2; ++side)
			{
				const double frequency = side == 0 ? bestFrequency * semitone : bestFrequency / semitone;
				if (isShared(frequency)) continue;   // lit by another chord tone
				const double nPost = powerAt(post, postLength, frequency);
				const double nBase = powerAt(base, baseLength, frequency);
				const double nRise = nPost > nBase ? nPost - nBase : 0.0;
				if (nRise > neighbourRise) neighbourRise = nRise;
			}
			reading.baseline = static_cast<float>(baseline);
			reading.post = static_cast<float>(bestPost);
			reading.rise = static_cast<float>(rise);
			reading.neighbourRise = static_cast<float>(neighbourRise);
			const double floor = energy * LEDGER_ENERGY_FRACTION;
			reading.struck = rise >= floor && rise >= neighbourRise * LEDGER_NEIGHBOUR_RATIO
				&& bestPost >= baseline * LEDGER_BASELINE_RATIO;
			reading.wrongNeighbour = neighbourRise >= floor && neighbourRise >= rise;
			if (reading.struck) ++out.struckCount;
			if (reading.wrongNeighbour) out.wrongStrum = true;
		}
		out.strummed = out.requiredCount >= 1 && out.struckCount >= out.requiredCount;
		return true;
	}
}
