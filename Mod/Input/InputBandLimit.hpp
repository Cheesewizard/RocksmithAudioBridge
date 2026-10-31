#pragma once

// Header-only and standard library only: the 4.x DLL's capture hook and InputConditioner run the same code.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace AudioBridge::Input
{
	// Soft knee above 0.7 so a hot make-up gain asymptotes toward full scale instead of hard clipping or wrapping on
	// transients.
	inline float SoftClipSample(float value)
	{
		if (value > 0.7f) return 0.7f + 0.3f * std::tanh((value - 0.7f) / 0.3f);
		if (value < -0.7f) return -0.7f + 0.3f * std::tanh((value + 0.7f) / 0.3f);
		return value;
	}

	// One channel's filter memory.
	struct BandLimitChannelState
	{
		float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
	};

	// Second-order Butterworth low-pass at 6 kHz (RBJ cookbook, Q = 1/sqrt(2)) run per channel over the post-gain
	// input. It is linear and cheap: it strips the broadband hiss and >19 kHz interface whine that the flat make-up
	// gain amplifies, and also smooths the soft-clip's transient harmonics. Audio thread only; state carries across
	// buffers and resets when the sample rate changes.
	class InputBandLimit final
	{
	public:
		static constexpr size_t MAX_CHANNELS = 8;

		void Configure(uint32_t sampleRate)
		{
			if (sampleRate == 0 || sampleRate == coeffRate) return;
			coeffRate = sampleRate;
			const double w0 = 2.0 * 3.14159265358979323846 * LOWPASS_HZ / static_cast<double>(sampleRate);
			const double cosw0 = std::cos(w0);
			const double sinw0 = std::sin(w0);
			const double alpha = sinw0 / 1.4142135623730951; // sinw0 / (2Q) with Q = 1/sqrt(2)
			const double a0 = 1.0 + alpha;
			b0 = static_cast<float>(((1.0 - cosw0) * 0.5) / a0);
			b1 = static_cast<float>((1.0 - cosw0) / a0);
			b2 = b0;
			a1 = static_cast<float>((-2.0 * cosw0) / a0);
			a2 = static_cast<float>((1.0 - alpha) / a0);
			channels = {};
		}

		float Process(size_t channel, float input)
		{
			BandLimitChannelState& c = channels[channel];
			const float output = b0 * input + b1 * c.x1 + b2 * c.x2 - a1 * c.y1 - a2 * c.y2;
			c.x2 = c.x1; c.x1 = input;
			c.y2 = c.y1; c.y1 = output;
			return output;
		}

	private:
		static constexpr double LOWPASS_HZ = 6000.0;

		float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
		uint32_t coeffRate = 0;
		std::array<BandLimitChannelState, MAX_CHANNELS> channels{};
	};
}
