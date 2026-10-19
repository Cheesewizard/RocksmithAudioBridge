#pragma once

// The input noise suppressor: a soft downward expander keyed to the signal it processes. Header-only and standard
// library only, so the game DLL's input chain (InputCaptureHook.cpp) and the amp sim's feed (AmpFeedConditioner.hpp,
// also built into the bridge driver) run the same code. Moved here unchanged from InputCaptureHook.hpp/.cpp.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Audio::AsioHook::Detail
{
	class NoiseSuppressor final
	{
	public:
		void Configure(uint32_t sampleRate, float threshold)
		{
			if (sampleRate == 0) return;
			if (sampleRate == configuredRate && threshold == configuredThreshold) return;

			configuredRate = sampleRate;
			configuredThreshold = threshold;
			const double fs = static_cast<double>(sampleRate);
			detectorAttackCoef = static_cast<float>(1.0 - std::exp(-1.0 / (DETECTOR_ATTACK_SECONDS * fs)));
			detectorReleaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / (DETECTOR_RELEASE_SECONDS * fs)));
			gainAttackCoef = static_cast<float>(1.0 - std::exp(-1.0 / (GAIN_ATTACK_SECONDS * fs)));
			gainReleaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / (GAIN_RELEASE_SECONDS * fs)));
			rangeGain = std::pow(10.0f, EXPANDER_RANGE_DB / 20.0f);
			attackThreshold = threshold * std::pow(10.0f, ATTACK_ABOVE_THRESHOLD_DB / 20.0f);
			openConfirmSamples = (std::max)(uint32_t{ 1 }, static_cast<uint32_t>(std::lround(OPEN_CONFIRM_SECONDS * sampleRate)));
			energy = 0.0f;
			appliedGain = rangeGain;
			openConfirmCount = 0;
			isOpen = false;
		}

		float NextGain(float sidechain, float threshold)
		{
			if (!std::isfinite(sidechain) || threshold <= 0.0f) return 1.0f;
			const float magnitude = std::fabs(sidechain);
			const float sampleEnergy = magnitude * magnitude;
			const float detectorCoef = sampleEnergy > energy ? detectorAttackCoef : detectorReleaseCoef;
			energy += (sampleEnergy - energy) * detectorCoef;
			const float envelope = std::sqrt((std::max)(energy, 0.0f));

			if (!isOpen)
			{
				if (envelope >= attackThreshold)
					openConfirmCount = (std::min)(openConfirmCount + 1, openConfirmSamples);
				else
					openConfirmCount = 0;
				if (openConfirmCount >= openConfirmSamples)
					isOpen = true;
			}
			else if (envelope < threshold * CLOSE_HYSTERESIS)
			{
				isOpen = false;
				openConfirmCount = 0;
			}

			float target = 1.0f;
			if (!isOpen)
			{
				if (envelope < threshold)
				{
					const float belowDb = 20.0f * std::log10((std::max)(envelope, 1e-9f) / threshold);
					const float reductionDb = belowDb * (EXPANDER_RATIO - 1.0f);
					target = reductionDb <= EXPANDER_RANGE_DB
						? rangeGain
						: std::pow(10.0f, reductionDb / 20.0f);
				}
				else
					target = rangeGain;
			}

			const float coefficient = target > appliedGain ? gainAttackCoef : gainReleaseCoef;
			appliedGain += (target - appliedGain) * coefficient;
			return appliedGain;
		}

		void Process(float* samples, size_t sampleCount, uint32_t sampleRate, float threshold)
		{
			if (samples == nullptr || sampleCount == 0 || threshold <= 0.0f) return;
			Configure(sampleRate, threshold);
			for (size_t index = 0; index < sampleCount; ++index)
				samples[index] *= NextGain(samples[index], threshold);
		}

		void Reset()
		{
			configuredThreshold = -1.0f;
			energy = 0.0f;
			appliedGain = rangeGain;
			openConfirmCount = 0;
			isOpen = false;
		}

	private:
		static constexpr float EXPANDER_RATIO = 6.0f;
		static constexpr float EXPANDER_RANGE_DB = -80.0f;
		static constexpr float DETECTOR_ATTACK_SECONDS = 0.002f;
		static constexpr float DETECTOR_RELEASE_SECONDS = 0.080f;
		static constexpr float GAIN_ATTACK_SECONDS = 0.002f;
		static constexpr float GAIN_RELEASE_SECONDS = 0.180f;
		static constexpr float OPEN_CONFIRM_SECONDS = 0.003f;
		static constexpr float CLOSE_HYSTERESIS = 0.65f;
		// Opening level, relative to the configured threshold. An absolute level would hard-mute every
		// note between the threshold and that level, leaving the input dead after a pause until one
		// hit is loud enough. The 3 ms confirm still rejects spikes.
		static constexpr float ATTACK_ABOVE_THRESHOLD_DB = 6.0f;

		float energy = 0.0f;
		float appliedGain = 0.0f;
		float detectorAttackCoef = 0.0f;
		float detectorReleaseCoef = 0.0f;
		float gainAttackCoef = 0.0f;
		float gainReleaseCoef = 0.0f;
		float rangeGain = 0.0001f;
		float configuredThreshold = -1.0f;
		float attackThreshold = 0.0316228f;
		uint32_t openConfirmSamples = 1;
		uint32_t openConfirmCount = 0;
		uint32_t configuredRate = 0;
		bool isOpen = false;
	};
}
