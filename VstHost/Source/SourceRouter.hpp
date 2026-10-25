#pragma once

#include "BridgeAudio.hpp"

#include <array>
#include <cstdint>

namespace VstHost
{
	struct BlockTiming
	{
		uint64_t blocks = 0;
		double medianMicroseconds = 0.0;
		double percentile99Microseconds = 0.0;
		double percentile999Microseconds = 0.0;
		double maximumMicroseconds = 0.0;
	};

	class SourceRouter final : public AudioSink
	{
	public:
		SourceRouter();

		uint64_t BlockCounter() const { return blockCounter.load(std::memory_order_acquire); }
		float LimiterReduction() const { return limiterReduction.load(std::memory_order_relaxed); }
		void SetSource(AudioSink* sink, bool shouldLimit);
		BlockTiming TakeTiming();
		void Process(const float* guitar, float* left, float* right, int frames) override;

	private:
		static constexpr int HISTOGRAM_BINS = 65536;
		static constexpr double BIN_MICROSECONDS = 2.0;

		std::atomic<AudioSink*> activeSink{ nullptr };
		std::atomic<bool> shouldLimit{ false };
		std::atomic<uint64_t> blockCounter{ 0 };
		std::array<std::atomic<uint32_t>, HISTOGRAM_BINS> histogram{};
		std::atomic<uint64_t> maximumTicks{ 0 };
		std::atomic<float> limiterReduction{ 0.0f };
		double ticksToMicroseconds;
		float limiterGain = 1.0f;
	};
}
