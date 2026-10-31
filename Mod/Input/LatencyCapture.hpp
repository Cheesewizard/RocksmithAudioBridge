#pragma once

#include "Framework/AudioInput.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace AudioBridge::Input
{
	// Round-trip latency measurement, the input half: while armed, copies Player 1's conditioned input (before any
	// pitch shift) into a fixed buffer until it's full; the engine then cross-correlates it against the probe the
	// bridge driver sent out through a physical out->in loop (LatencyLoopback). An observer in the input chain: it never
	// changes the audio and adds no latency.
	class LatencyCapture final : public Framework::IInputProcessor
	{
	public:
		static constexpr int MAX_FRAMES = 48000;   // up to 1 s at 48 kHz

		void Prepare(std::uint32_t input, const Framework::CaptureFormat& format) override;
		bool Process(std::uint32_t input, float* const* channels, std::uint32_t channelCount, std::uint32_t frames,
			bool active) noexcept override;
		std::uint32_t GetLatencyFrames(std::uint32_t input) const override;

		// Arms a one-shot capture of `frames` (1..MAX_FRAMES) samples.
		void Start(int frames);
		bool IsDone() const;
		// Frames captured so far; *out = the buffer.
		int Get(const float** out) const;

	private:
		std::array<float, MAX_FRAMES> captured{};
		std::atomic<int> target{ 0 };   // > 0 = capturing this many frames
		std::atomic<int> filled{ 0 };
	};

	// The one capture, owned by AudioBridgeRoutingMod (it registers it in the chain) and read by the engine's
	// control pipe.
	LatencyCapture& SharedLatencyCapture();
}
