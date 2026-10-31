#include "InputConditioner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace AudioBridge::Input
{
	namespace
	{
		constexpr std::int64_t METER_STALE_MS = 500;

		float ClampSample(float value)
		{
			if (!std::isfinite(value)) return 0.0f;
			return std::clamp(value, -1.0f, 1.0f);
		}

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}
	}

	void InputConditioner::Prepare(std::uint32_t input, const Framework::CaptureFormat& format)
	{
		if (input != CONDITIONED_INPUT) return;
		sampleRate = format.sampleRate;
		noiseGate.Reset();
		processedGateRevision = gateThresholdRevision.load(std::memory_order_acquire);
	}

	bool InputConditioner::Process(std::uint32_t input, float* const* channels, std::uint32_t channelCount,
		std::uint32_t frames, bool active) noexcept
	{
		if (!active || input != CONDITIONED_INPUT || channelCount == 0 || frames == 0 || sampleRate == 0) return false;

		bool changed = false;

		const std::uint32_t revision = gateThresholdRevision.load(std::memory_order_acquire);
		if (revision != processedGateRevision)
		{
			noiseGate.Reset();
			processedGateRevision = revision;
		}
		const float threshold = gateThresholdLinear.load(std::memory_order_relaxed);
		if (threshold > 0.0f)
		{
			RunNoiseGate(channels, channelCount, frames, threshold);
			changed = true;
		}

		// After the gate and before the make-up gain: it flattens the string-beat wobble so the game amp downstream
		// cannot magnify it into a warble.
		const float strength = compressorStrength.load(std::memory_order_relaxed);
		if (strength > 0.0f)
		{
			RunCompressor(channels, channelCount, frames, strength);
			changed = true;
		}

		const float gain = inputGainLinear.load(std::memory_order_relaxed);
		if (gain > 1.0001f || gain < 0.9999f)
		{
			RunGain(channels, channelCount, frames, gain);
			changed = true;
		}
		return changed;
	}

	std::uint32_t InputConditioner::GetLatencyFrames(std::uint32_t) const
	{
		// Every stage works on the current sample.
		return 0;
	}

	void InputConditioner::RunNoiseGate(float* const* channels, std::uint32_t channelCount, std::uint32_t frames,
		float threshold)
	{
		Audio::AsioHook::Detail::NoiseGateShape shape;
		shape.decayMs = gateDecayMs.load(std::memory_order_relaxed);
		shape.reductionDb = gateReductionDb.load(std::memory_order_relaxed);
		shape.attackMs = gateAttackMs.load(std::memory_order_relaxed);
		noiseGate.Configure(sampleRate, threshold, shape);

		for (std::uint32_t frame = 0; frame < frames; ++frame)
		{
			const float gain = noiseGate.NextGain(channels[0][frame], threshold);
			for (std::uint32_t channel = 0; channel < channelCount; ++channel)
				channels[channel][frame] = ClampSample(channels[channel][frame] * gain);
		}

		meterDetectorDb.store(noiseGate.DetectorDb(), std::memory_order_relaxed);
		meterOpen.store(noiseGate.IsOpen(), std::memory_order_relaxed);
		meterUpdatedMs.store(NowMs(), std::memory_order_relaxed);
	}

	void InputConditioner::RunCompressor(float* const* channels, std::uint32_t channelCount, std::uint32_t frames,
		float strength)
	{
		compressor.Configure(sampleRate, strength);
		for (std::uint32_t frame = 0; frame < frames; ++frame)
		{
			const float gain = compressor.NextGain(channels[0][frame]);
			for (std::uint32_t channel = 0; channel < channelCount; ++channel)
				channels[channel][frame] = ClampSample(channels[channel][frame] * gain);
		}
	}

	// Band-limit after the gain, so the stage that smooths amplified hiss also tames the soft clip's transient
	// harmonics. Tied to the gain, so the input at unity gain is untouched.
	void InputConditioner::RunGain(float* const* channels, std::uint32_t channelCount, std::uint32_t frames, float gain)
	{
		bandLimit.Configure(sampleRate);
		const std::uint32_t filtered = (std::min)(channelCount, static_cast<std::uint32_t>(InputBandLimit::MAX_CHANNELS));
		for (std::uint32_t channel = 0; channel < channelCount; ++channel)
		{
			float* samples = channels[channel];
			for (std::uint32_t frame = 0; frame < frames; ++frame)
			{
				const float lifted = ClampSample(SoftClipSample(samples[frame] * gain));
				samples[frame] = channel < filtered ? ClampSample(bandLimit.Process(channel, lifted)) : lifted;
			}
		}
	}

	void InputConditioner::SetInputGainDb(float decibels)
	{
		if (!std::isfinite(decibels)) return;
		decibels = std::clamp(decibels, -24.0f, 24.0f);
		inputGainLinear.store(std::pow(10.0f, decibels / 20.0f), std::memory_order_relaxed);
	}

	void InputConditioner::SetNoiseGateThresholdDb(float decibels, bool resetGate)
	{
		// A threshold at or above 0 dBFS is meaningless, so it doubles as "off". The revision makes the audio thread
		// reset its detector, including off then on again at the same threshold.
		if (!std::isfinite(decibels) || decibels >= 0.0f)
		{
			gateThresholdLinear.store(0.0f, std::memory_order_relaxed);
			gateThresholdRevision.fetch_add(1, std::memory_order_release);
			return;
		}
		decibels = std::clamp(decibels, -90.0f, -20.0f);
		gateThresholdLinear.store(std::pow(10.0f, decibels / 20.0f), std::memory_order_relaxed);
		if (resetGate) gateThresholdRevision.fetch_add(1, std::memory_order_release);
	}

	void InputConditioner::SetNoiseGateShape(const Audio::AsioHook::Detail::NoiseGateShape& requested)
	{
		const Audio::AsioHook::Detail::NoiseGateShape shape = requested.Clamped();
		gateDecayMs.store(shape.decayMs, std::memory_order_relaxed);
		gateReductionDb.store(shape.reductionDb, std::memory_order_relaxed);
		gateAttackMs.store(shape.attackMs, std::memory_order_relaxed);
	}

	void InputConditioner::SetCompressorStrength(float strength)
	{
		if (!std::isfinite(strength)) return;
		compressorStrength.store(std::clamp(strength, 0.0f, 1.0f), std::memory_order_relaxed);
	}

	float InputConditioner::GetInputGainDb() const
	{
		const float gain = inputGainLinear.load(std::memory_order_relaxed);
		return gain > 0.0f ? 20.0f * std::log10(gain) : -24.0f;
	}

	float InputConditioner::GetNoiseGateThresholdDb() const
	{
		const float threshold = gateThresholdLinear.load(std::memory_order_relaxed);
		return threshold > 0.0f ? 20.0f * std::log10(threshold) : 0.0f;
	}

	Audio::AsioHook::Detail::NoiseGateShape InputConditioner::GetNoiseGateShape() const
	{
		Audio::AsioHook::Detail::NoiseGateShape shape;
		shape.decayMs = gateDecayMs.load(std::memory_order_relaxed);
		shape.reductionDb = gateReductionDb.load(std::memory_order_relaxed);
		shape.attackMs = gateAttackMs.load(std::memory_order_relaxed);
		return shape;
	}

	float InputConditioner::GetCompressorStrength() const
	{
		return compressorStrength.load(std::memory_order_relaxed);
	}

	InputConditioner& SharedConditioner()
	{
		static InputConditioner conditioner;
		return conditioner;
	}

	bool InputConditioner::TryGetNoiseGateMeter(NoiseGateMeter& meter) const
	{
		const std::int64_t updated = meterUpdatedMs.load(std::memory_order_relaxed);
		if (updated == 0 || NowMs() - updated > METER_STALE_MS
			|| gateThresholdLinear.load(std::memory_order_relaxed) <= 0.0f)
		{
			meter = {};
			return false;
		}
		meter.detectorDb = meterDetectorDb.load(std::memory_order_relaxed);
		meter.open = meterOpen.load(std::memory_order_relaxed);
		return true;
	}
}
