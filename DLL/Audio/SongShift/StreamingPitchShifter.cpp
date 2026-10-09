#include "stdafx.h"
#include "StreamingPitchShifter.hpp"

#include <algorithm>
#include <cmath>

#pragma optimize("t", on)

namespace
{
	constexpr float INT16_NORMALIZER = 1.0f / 32768.0f;
	constexpr float INT16_SCALE = 32768.0f;
	constexpr uint32_t ANALYSIS_WINDOW_MILLISECONDS = 60;
	constexpr uint32_t ANALYSIS_INTERVAL_MILLISECONDS = 15;

	int16_t FloatToInt16(float sample)
	{
		const float scaled = sample * INT16_SCALE;
		const float clipped = std::max(-32768.0f, std::min(32767.0f, scaled));
		return static_cast<int16_t>(std::lround(clipped));
	}
}

Audio::SongShift::StreamingPitchShifter::StreamingPitchShifter()
	: stretch(0)
{
}

void Audio::SongShift::StreamingPitchShifter::ResetGeneration()
{
	if (isConfigured)
	{
		stretch.reset();
	}

	expectedPosition = 0;
	currentSemitones = 0;
	isProcessing = false;
	hasExpectedPosition = false;
	hasFailed = false;
}

void Audio::SongShift::StreamingPitchShifter::Bypass()
{
	isProcessing = false;
	hasExpectedPosition = false;
}

Audio::SongShift::StreamProcessResult Audio::SongShift::StreamingPitchShifter::Process(
	int16_t* interleavedSamples,
	uint16_t frameCount,
	uint16_t maximumFrameCount,
	uint32_t sampleRate,
	uint32_t positionStart,
	int semitones)
{
	if (hasFailed || interleavedSamples == nullptr || frameCount == 0
		|| maximumFrameCount == 0 || frameCount > maximumFrameCount)
	{
		return StreamProcessResult::Failed;
	}

	if (semitones == 0)
	{
		Bypass();
		return StreamProcessResult::Bypassed;
	}

	bool didConfigure = false;
	if (!isConfigured || configuredSampleRate != sampleRate
		|| inputLeft.size() < maximumFrameCount)
	{
		if (!Configure(maximumFrameCount, sampleRate))
		{
			hasFailed = true;
			return StreamProcessResult::Failed;
		}

		didConfigure = true;
	}

	bool didReset = false;
	if (!isProcessing || (hasExpectedPosition && positionStart != expectedPosition))
	{
		ResetProcessingPosition(positionStart);
		didReset = true;
	}

	if (currentSemitones != semitones)
	{
		stretch.setTransposeSemitones(static_cast<float>(semitones));
		currentSemitones = semitones;
	}

	for (uint16_t frame = 0; frame < frameCount; frame++)
	{
		const size_t sampleIndex = static_cast<size_t>(frame) * 2;
		inputLeft[frame] = interleavedSamples[sampleIndex] * INT16_NORMALIZER;
		inputRight[frame] = interleavedSamples[sampleIndex + 1] * INT16_NORMALIZER;
	}

	float* inputChannels[] = { inputLeft.data(), inputRight.data() };
	float* outputChannels[] = { outputLeft.data(), outputRight.data() };

	try
	{
		stretch.process(inputChannels, frameCount, outputChannels, frameCount);
	}
	catch (...)
	{
		hasFailed = true;
		return StreamProcessResult::Failed;
	}

	for (uint16_t frame = 0; frame < frameCount; frame++)
	{
		const size_t sampleIndex = static_cast<size_t>(frame) * 2;
		interleavedSamples[sampleIndex] = FloatToInt16(outputLeft[frame]);
		interleavedSamples[sampleIndex + 1] = FloatToInt16(outputRight[frame]);
	}

	expectedPosition = positionStart + frameCount;
	hasExpectedPosition = true;
	isProcessing = true;

	if (didConfigure) return StreamProcessResult::Configured;
	if (didReset) return StreamProcessResult::Reset;
	return StreamProcessResult::Processed;
}

int Audio::SongShift::StreamingPitchShifter::GetInputLatency() const
{
	return isConfigured ? stretch.inputLatency() : 0;
}

int Audio::SongShift::StreamingPitchShifter::GetOutputLatency() const
{
	return isConfigured ? stretch.outputLatency() : 0;
}

bool Audio::SongShift::StreamingPitchShifter::Configure(
	uint16_t maximumFrameCount,
	uint32_t sampleRate)
{
	try
	{
		const int blockSamples = static_cast<int>(
			static_cast<uint64_t>(sampleRate) * ANALYSIS_WINDOW_MILLISECONDS / 1000);
		const int intervalSamples = static_cast<int>(
			static_cast<uint64_t>(sampleRate) * ANALYSIS_INTERVAL_MILLISECONDS / 1000);
		stretch.configure(2, blockSamples, intervalSamples);
		inputLeft.resize(maximumFrameCount);
		inputRight.resize(maximumFrameCount);
		outputLeft.resize(maximumFrameCount);
		outputRight.resize(maximumFrameCount);
	}
	catch (...)
	{
		return false;
	}

	configuredSampleRate = sampleRate;
	currentSemitones = 0;
	isConfigured = true;
	isProcessing = false;
	hasExpectedPosition = false;
	return true;
}

void Audio::SongShift::StreamingPitchShifter::ResetProcessingPosition(uint32_t positionStart)
{
	stretch.reset();
	expectedPosition = positionStart;
	hasExpectedPosition = false;
	isProcessing = false;
}

#pragma optimize("", on)
