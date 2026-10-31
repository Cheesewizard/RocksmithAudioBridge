#include "SourceRouter.hpp"

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>

namespace VstHost
{
	SourceRouter::SourceRouter()
	{
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		ticksToMicroseconds = 1000000.0 / frequency.QuadPart;
	}

	void SourceRouter::SetSource(AudioSink* sink, bool limit)
	{
		shouldLimit.store(limit, std::memory_order_release);
		activeSink.store(sink, std::memory_order_release);
	}

	BlockTiming SourceRouter::TakeTiming()
	{
		std::array<uint32_t, HISTOGRAM_BINS> counts{};
		BlockTiming result;
		for (int index = 0; index < HISTOGRAM_BINS; ++index)
		{
			counts[index] = histogram[index].exchange(0, std::memory_order_relaxed);
			result.blocks += counts[index];
		}
		result.maximumMicroseconds = maximumTicks.exchange(0, std::memory_order_relaxed) * ticksToMicroseconds;
		if (result.blocks == 0) return result;
		auto percentile = [&](double quantile)
		{
			const auto target = static_cast<uint64_t>(std::ceil(result.blocks * quantile));
			uint64_t sum = 0;
			for (int index = 0; index < HISTOGRAM_BINS; ++index)
			{
				sum += counts[index];
				if (sum >= target) return index == HISTOGRAM_BINS - 1 ? result.maximumMicroseconds : (index + 1) * BIN_MICROSECONDS;
			}
			return result.maximumMicroseconds;
		};
		result.medianMicroseconds = percentile(0.5);
		result.percentile99Microseconds = percentile(0.99);
		result.percentile999Microseconds = percentile(0.999);
		return result;
	}

	void SourceRouter::Process(const float* guitar, float* left, float* right, int frames)
	{
		juce::ScopedNoDenormals noDenormals;
		LARGE_INTEGER start;
		QueryPerformanceCounter(&start);
		inputLevel.Feed(guitar, frames);
		auto* sink = activeSink.load(std::memory_order_acquire);
		if (sink) sink->Process(guitar, left, right, frames);
		else
		{
			std::fill_n(left, frames, 0.0f);
			std::fill_n(right, frames, 0.0f);
		}
		float minimumGain = 1.0f;
		if (shouldLimit.load(std::memory_order_acquire))
		{
			constexpr float CEILING = 0.891250938f;
			constexpr float RELEASE_FACTOR = 0.00041658f;
			for (int frame = 0; frame < frames; ++frame)
			{
				if (!std::isfinite(left[frame]) || !std::isfinite(right[frame]))
				{
					left[frame] = right[frame] = 0.0f;
					minimumGain = 0.0f;
					continue;
				}
				const float peak = std::max(std::abs(left[frame]), std::abs(right[frame]));
				const float target = peak > CEILING ? CEILING / peak : 1.0f;
				limiterGain = std::min(target, limiterGain + (1.0f - limiterGain) * RELEASE_FACTOR);
				left[frame] *= limiterGain;
				right[frame] *= limiterGain;
				minimumGain = std::min(minimumGain, limiterGain);
			}
		}
		limiterReduction.store(1.0f - minimumGain, std::memory_order_relaxed);
		LARGE_INTEGER end;
		QueryPerformanceCounter(&end);
		const auto ticks = static_cast<uint64_t>(end.QuadPart - start.QuadPart);
		const auto bin = static_cast<size_t>(std::min<double>(HISTOGRAM_BINS - 1, ticks * ticksToMicroseconds / BIN_MICROSECONDS));
		histogram[bin].fetch_add(1, std::memory_order_relaxed);
		auto maximum = maximumTicks.load(std::memory_order_relaxed);
		while (ticks > maximum && !maximumTicks.compare_exchange_weak(maximum, ticks, std::memory_order_relaxed)) { }
		blockCounter.fetch_add(1, std::memory_order_release);
	}
}
