#pragma once

#include <cstdint>
#include <vector>

#include "../../ThirdParty/SignalsmithStretch.hpp"

namespace Audio::SongShift
{
	enum class StreamProcessResult
	{
		Bypassed,
		Configured,
		Processed,
		Reset,
		Failed
	};

	class StreamingPitchShifter final
	{
	public:
		StreamingPitchShifter();

		void ResetGeneration();
		void Bypass();
		StreamProcessResult Process(
			int16_t* interleavedSamples,
			uint16_t frameCount,
			uint16_t maximumFrameCount,
			uint32_t sampleRate,
			uint32_t positionStart,
			int semitones);

		int GetInputLatency() const;
		int GetOutputLatency() const;

	private:
		bool Configure(uint16_t maximumFrameCount, uint32_t sampleRate);
		void ResetProcessingPosition(uint32_t positionStart);

		signalsmith::stretch::SignalsmithStretch<float> stretch;
		std::vector<float> inputLeft;
		std::vector<float> inputRight;
		std::vector<float> outputLeft;
		std::vector<float> outputRight;
		uint32_t configuredSampleRate = 0;
		uint32_t expectedPosition = 0;
		int currentSemitones = 0;
		bool isConfigured = false;
		bool isProcessing = false;
		bool hasExpectedPosition = false;
		bool hasFailed = false;
	};
}
