#pragma once

// Round-trip latency DSP, shared by the proxy (which injects the probe into the output) and whatever
// captures the loopback (the input hook or an offline test). Pure and header-only so it can
// be unit-tested with synthetic signals, no game or hardware. The measurement is: inject a known probe
// on the output, capture it after it loops back to the input (electrical or acoustic), and cross-correlate
// to find the delay in samples -> milliseconds at the stream sample rate.
#include <cmath>

namespace LatencyDsp
{
	// A short bipolar impulse train. Alternating-sign spikes give a single sharp cross-correlation peak
	// (no ambiguity from a periodic tone) while staying cheap to detect. Writes exactly `len` samples.
	inline void GenerateProbe(float* out, int len)
	{
		for (int i = 0; i < len; ++i) out[i] = 0.0f;
		const int spacing = 7;
		float sign = 1.0f;
		for (int i = 0; i < len; i += spacing) { out[i] = sign; sign = -sign; }
	}

	struct Match
	{
		int lag;          // best delay in samples, or -1 if nothing matched
		float confidence; // normalized correlation at that lag, [0,1]
	};

	// Slide `reference` (length refLen) over `recorded` (length recLen) for lags in [0, maxLag] and return
	// the lag with the highest normalized cross-correlation. Normalization makes the confidence independent
	// of loopback level, so a quiet return still locks on. O(maxLag * refLen): fine for a one-shot measure,
	// not for a per-buffer path.
	inline Match FindLag(const float* reference, int refLen, const float* recorded, int recLen, int maxLag)
	{
		Match best{ -1, 0.0f };
		double refEnergy = 0.0;
		for (int i = 0; i < refLen; ++i) refEnergy += static_cast<double>(reference[i]) * reference[i];
		if (refEnergy <= 0.0) return best;

		for (int lag = 0; lag <= maxLag && lag + refLen <= recLen; ++lag)
		{
			double dot = 0.0, recEnergy = 0.0;
			for (int i = 0; i < refLen; ++i)
			{
				const double r = recorded[lag + i];
				dot += static_cast<double>(reference[i]) * r;
				recEnergy += r * r;
			}
			const double denom = std::sqrt(refEnergy * recEnergy);
			const double corr = denom > 0.0 ? dot / denom : 0.0;
			if (corr > best.confidence) { best.confidence = static_cast<float>(corr); best.lag = lag; }
		}
		return best;
	}

	inline double LagToMilliseconds(int lagSamples, double sampleRate)
	{
		return (sampleRate > 0.0 && lagSamples >= 0) ? (1000.0 * lagSamples / sampleRate) : -1.0;
	}
}
