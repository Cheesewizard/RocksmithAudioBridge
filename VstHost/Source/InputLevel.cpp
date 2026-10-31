#include "InputLevel.hpp"

#include <algorithm>
#include <cmath>

namespace VstHost
{
	void InputLevel::Feed(const float* guitar, int frames)
	{
		for (int i = 0; i < frames; ++i)
		{
			windowPeak = std::max(windowPeak, std::fabs(guitar[i]));
			if (++windowFrames < WINDOW_FRAMES) continue;
			const float db = windowPeak > 0.0f ? 20.0f * std::log10(windowPeak) : LOWEST_DB;
			const int bin = std::clamp(static_cast<int>((db - LOWEST_DB) / BIN_DB), 0, BINS - 1);
			histogram[bin].fetch_add(1, std::memory_order_relaxed);
			windowPeak = 0.0f;
			windowFrames = 0;
		}
	}

	InputLevel::Reading InputLevel::Measure()
	{
		std::array<uint32_t, BINS> counts{};
		uint64_t total = 0;
		for (int b = 0; b < BINS; ++b) total += counts[b] = histogram[b].load(std::memory_order_relaxed);
		Reading reading;
		if (total == 0) return reading;

		// Fade old windows (see the header).
		if (total > FADE_WINDOWS)
			for (auto& bin : histogram) bin.store(bin.load(std::memory_order_relaxed) / 2, std::memory_order_relaxed);

		const auto binDb = [](int b) { return LOWEST_DB + (b + 0.5f) * BIN_DB; };
		// The background: the 20th percentile of every window.
		uint64_t seen = 0;
		int noiseBin = 0;
		for (; noiseBin < BINS; ++noiseBin)
			if ((seen += counts[noiseBin]) >= total / 5) break;
		reading.noiseDb = binDb(noiseBin);

		// The playing windows, and the 97th percentile of them.
		const float threshold = std::max(FLOOR_DB, reading.noiseDb + OVER_NOISE_DB);
		const int first = std::clamp(static_cast<int>(std::ceil((threshold - LOWEST_DB) / BIN_DB)), 0, BINS);
		uint64_t playing = 0;
		for (int b = first; b < BINS; ++b) playing += counts[b];
		reading.playingSeconds = playing * (WINDOW_FRAMES / 48000.0f);
		if (playing == 0) return reading;
		const uint64_t wanted = static_cast<uint64_t>(std::ceil(playing * PERCENTILE));
		seen = 0;
		for (int b = first; b < BINS; ++b)
			if ((seen += counts[b]) >= wanted) { reading.levelDb = binDb(b); break; }
		return reading;
	}

	void InputLevel::Reset()
	{
		for (auto& bin : histogram) bin.store(0, std::memory_order_relaxed);
	}
}
