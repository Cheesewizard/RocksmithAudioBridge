#pragma once
// High-frequency-content (HFC) onset detector for the raw attack evidence in RawPitchVerifier.
//
// Replaces the GPL aubio "hfc" onset object, which was compiled into xinput1_3.dll and would have
// made the whole DLL GPL. Written from the published HFC method, without aubio's source; the
// constants below were fixed by measuring aubio's outputs from the outside (synthetic clicks,
// steady noise, recorded takes) so Note by Note keeps the attack timing it was tuned on.
//
// Per hop (hop = window / 4):
//   1. Hann-windowed FFT of the last `window` samples.
//   2. Descriptor D = sum over bins k of (k + 1) * log(1 + |X_k|).
//   3. Adaptive threshold over the last 7 descriptors: smooth them forward and backward with a
//      fixed biquad, then T = smoothed[5] - median(smoothed) - 0.058 * mean(smoothed).
//   4. An onset is reported one hop after T peaks above zero, unless the current hop is quieter
//      than the silence gate or it follows the previous onset by less than the minimum gap.
//      Its position is refined inside the hop by a parabola through the peak and its neighbours,
//      minus a fixed 4.3-hop analysis delay.
// Process() does no allocation and no locking; Init() allocates.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

class HfcOnsetDetector
{
public:
	static float LevelDb(const float* samples, uint32_t count)
	{
		double energy = 0.0;
		for (uint32_t index = 0; index < count; ++index) energy += static_cast<double>(samples[index]) * samples[index];
		return static_cast<float>(10.0 * std::log10(energy / count));   // -inf for digital silence
	}

	// window must be a power of two of at least 16; hop = window / 4.
	bool Init(uint32_t windowSize, uint32_t sampleRate)
	{
		if (windowSize < 16 || (windowSize & (windowSize - 1)) != 0 || sampleRate == 0) return false;
		window = windowSize;
		hop = window / 4;
		rate = sampleRate;
		delay = static_cast<int64_t>(4.3 * hop);
		frame.assign(window, 0.0f);
		hann.resize(window);
		for (uint32_t index = 0; index < window; ++index)
			hann[index] = 0.5 - 0.5 * std::cos(2.0 * PI * index / (window - 1));
		spectrum.resize(window);
		twiddle.resize(window / 2);
		for (uint32_t index = 0; index < window / 2; ++index)
			twiddle[index] = std::polar(1.0, -2.0 * PI * index / window);
		bitReverse.resize(window);
		uint32_t bits = 0;
		while ((1u << bits) < window) ++bits;
		for (uint32_t index = 0; index < window; ++index)
		{
			uint32_t reversed = 0;
			for (uint32_t bit = 0; bit < bits; ++bit) if (index & (1u << bit)) reversed |= 1u << (bits - 1 - bit);
			bitReverse[index] = reversed;
		}
		SetMinimumGapMs(minimumGapMs);
		Reset();
		return true;
	}

	void Reset()
	{
		std::fill(frame.begin(), frame.end(), 0.0f);
		std::fill(std::begin(history), std::end(history), 0.0);
		thresholdPrevious = thresholdBeforePrevious = 0.0;
		hopIndex = 0;
		lastOnset = 0;
		haveOnset = false;
	}

	void SetMinimumGapMs(float milliseconds)
	{
		minimumGapMs = milliseconds;
		minimumGap = static_cast<int64_t>(std::floor(rate * milliseconds / 1000.0 + 0.5));
	}
	void SetSilenceDb(float decibels) { silenceDb = decibels; }
	uint32_t Hop() const { return hop; }

	// Feed exactly Hop() samples. Returns true when an onset was detected; LastOnset() then gives its
	// position in samples from the first sample fed since Init/Reset.
	bool Process(const float* samples)
	{
		std::move(frame.begin() + hop, frame.end(), frame.begin());
		std::copy(samples, samples + hop, frame.end() - hop);
		const double descriptor = Descriptor();
		std::move(std::begin(history) + 1, std::end(history), std::begin(history));
		history[HISTORY - 1] = descriptor;
		const double threshold = Thresholded();

		bool onset = false;
		const double peak = thresholdPrevious;
		if (peak > 0.0 && peak > thresholdBeforePrevious && peak >= threshold && LevelDb(samples, hop) >= silenceDb)
		{
			const double curvature = thresholdBeforePrevious - 2.0 * peak + threshold;
			const double offset = curvature != 0.0 ? 0.5 * (thresholdBeforePrevious - threshold) / curvature : 0.0;
			const int64_t position = static_cast<int64_t>(hopIndex) * hop - delay
				+ static_cast<int64_t>(std::floor((1.0 + offset) * hop + 0.5));
			if (!haveOnset || position - lastOnset >= minimumGap)
			{
				lastOnset = position;
				haveOnset = true;
				onset = true;
			}
		}
		thresholdBeforePrevious = thresholdPrevious;
		thresholdPrevious = threshold;
		++hopIndex;
		return onset;
	}

	uint64_t LastOnset() const { return lastOnset > 0 ? static_cast<uint64_t>(lastOnset) : 0; }

private:
	static constexpr double PI = 3.14159265358979323846;
	static constexpr int HISTORY = 7;
	static constexpr int PEAK_INDEX = 5;
	static constexpr double THRESHOLD_MEAN = 0.058;
	// Smoothing biquad, run forward then backward over the 7-value history (zero initial state).
	static constexpr double B0 = 0.16, B1 = 0.32, B2 = 0.16, A1 = 0.23484, A2 = 0.0;

	double Descriptor()
	{
		for (uint32_t index = 0; index < window; ++index)
			spectrum[bitReverse[index]] = std::complex<double>(frame[index] * hann[index], 0.0);
		for (uint32_t size = 2; size <= window; size <<= 1)
		{
			const uint32_t half = size / 2, step = window / size;
			for (uint32_t start = 0; start < window; start += size)
				for (uint32_t index = 0; index < half; ++index)
				{
					const std::complex<double> odd = spectrum[start + index + half] * twiddle[index * step];
					spectrum[start + index + half] = spectrum[start + index] - odd;
					spectrum[start + index] += odd;
				}
		}
		double sum = 0.0;
		for (uint32_t bin = 0; bin <= window / 2; ++bin) sum += (bin + 1.0) * std::log1p(std::abs(spectrum[bin]));
		return sum;
	}

	double Thresholded() const
	{
		double forward[HISTORY], smoothed[HISTORY];
		double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		for (int index = 0; index < HISTORY; ++index)
		{
			const double x = history[index];
			const double y = B0 * x + B1 * x1 + B2 * x2 - A1 * y1 - A2 * y2;
			x2 = x1; x1 = x; y2 = y1; y1 = y;
			forward[index] = y;
		}
		x1 = x2 = y1 = y2 = 0;
		for (int index = HISTORY - 1; index >= 0; --index)
		{
			const double x = forward[index];
			const double y = B0 * x + B1 * x1 + B2 * x2 - A1 * y1 - A2 * y2;
			x2 = x1; x1 = x; y2 = y1; y1 = y;
			smoothed[index] = y;
		}
		double sorted[HISTORY], mean = 0.0;
		for (int index = 0; index < HISTORY; ++index) { sorted[index] = smoothed[index]; mean += smoothed[index]; }
		mean /= HISTORY;
		std::nth_element(sorted, sorted + HISTORY / 2, sorted + HISTORY);
		return smoothed[PEAK_INDEX] - sorted[HISTORY / 2] - THRESHOLD_MEAN * mean;
	}

	uint32_t window = 0, hop = 0, rate = 48000;
	int64_t delay = 0;
	float minimumGapMs = 20.0f;
	int64_t minimumGap = 0;
	float silenceDb = -70.0f;
	std::vector<float> frame;
	std::vector<double> hann;
	std::vector<std::complex<double>> spectrum, twiddle;
	std::vector<uint32_t> bitReverse;
	double history[HISTORY] = {};
	double thresholdPrevious = 0.0, thresholdBeforePrevious = 0.0;
	uint64_t hopIndex = 0;
	int64_t lastOnset = 0;
	bool haveOnset = false;
};
