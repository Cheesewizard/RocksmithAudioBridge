#pragma once

// Header-only and standard library only: the 4.x DLL's capture hook and InputConditioner run the same code.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace AudioBridge::Input
{
	// Feed-forward compressor run per frame over the guitar input (channel 0 as the sidechain), applied to every
	// channel. A single "strength" macro (0..1) sweeps threshold, ratio and make-up together: at 0 it is a bypass,
	// toward 1 it clamps down harder. Fast attack / medium release track and flatten the slow (1-6 Hz) string-beat
	// wobble so the game amp receives a steadier envelope and cannot magnify it into a warble. Audio thread only;
	// state carries across buffers.
	class InputCompressor final
	{
	public:
		void Configure(uint32_t sampleRate, float strength)
		{
			if (sampleRate == 0) return;
			if (sampleRate != coeffRate)
			{
				coeffRate = sampleRate;
				const double fs = static_cast<double>(sampleRate);
				attackCoef = static_cast<float>(1.0 - std::exp(-1.0 / (ATTACK_SECONDS * fs)));
				releaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / (RELEASE_SECONDS * fs)));
			}
			if (strength != configuredStrength)
			{
				configuredStrength = strength;
				const float s = std::clamp(strength, 0.0f, 1.0f);
				ratio = 1.0f + 5.0f * s;                 // 1:1 .. 6:1
				thresholdDb = -6.0f - 24.0f * s;         // -6 .. -30 dBFS
				// Modest auto make-up: restore roughly what a -6 dBFS peak loses, so smoothing the
				// wobble does not just quieten the note. Kept gentle to limit noise lift on the decay.
				const float lossAtRef = (thresholdDb - (-6.0f)) * (1.0f - 1.0f / ratio); // <= 0
				makeupLinear = std::pow(10.0f, (-lossAtRef * 0.6f) / 20.0f);
			}
		}

		float NextGain(float sidechain)
		{
			const float levelDb = 20.0f * std::log10(std::fabs(sidechain) + 1e-9f);
			const float target = levelDb > thresholdDb
				? (thresholdDb - levelDb) * (1.0f - 1.0f / ratio) : 0.0f; // <= 0
			const float coef = target < gainReductionDb ? attackCoef : releaseCoef;
			gainReductionDb += (target - gainReductionDb) * coef;
			return std::pow(10.0f, gainReductionDb / 20.0f) * makeupLinear;
		}

	private:
		static constexpr float ATTACK_SECONDS = 0.008f;
		static constexpr float RELEASE_SECONDS = 0.090f;

		float ratio = 1.0f;
		float thresholdDb = 0.0f;
		float makeupLinear = 1.0f;
		float attackCoef = 0.0f;
		float releaseCoef = 0.0f;
		float gainReductionDb = 0.0f;   // smoothed, <= 0
		uint32_t coeffRate = 0;
		float configuredStrength = -1.0f;
	};
}
