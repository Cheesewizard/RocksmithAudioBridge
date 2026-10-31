#pragma once

// Static output trim, shared by the proxy (which applies it to the game output before the real driver
// plays it) and an offline test. A single linear gain multiplies every sample: the waveform is scaled,
// never clipped or dynamically compressed, so it adds no distortion and no latency. This is a clean
// "set the output level" control, not a peak limiter. Off by default (gain 1.0 = unity passthrough).
// Header-only and pure so it stays unit-testable with synthetic signals.
namespace LimiterDsp
{
	struct Trim
	{
		float gain = 1.0f;   // linear output gain; 1.0 = unity (no change)

		void Configure(float gainLinear) { gain = gainLinear; }

		// Active only when the gain differs from unity, so passthrough costs nothing (skipped by the caller).
		bool Active() const { return gain < 0.999f || gain > 1.001f; }

		float Apply(float x) const { return x * gain; }

		// Convenience for tests: scale an interleaved float block in place.
		void Process(float* x, int frames, int channels)
		{
			if (!Active()) return;
			const int n = frames * channels;
			for (int i = 0; i < n; ++i) x[i] *= gain;
		}
	};
}
