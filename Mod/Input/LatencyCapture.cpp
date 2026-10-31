#include "LatencyCapture.hpp"

#include <algorithm>

namespace AudioBridge::Input
{
	namespace
	{
		constexpr std::uint32_t CAPTURED_INPUT = 0;   // Player 1, the input the loopback cable is plugged into
	}

	void LatencyCapture::Prepare(std::uint32_t, const Framework::CaptureFormat&)
	{
	}

	bool LatencyCapture::Process(std::uint32_t input, float* const* channels, std::uint32_t channelCount,
		std::uint32_t frames, bool active) noexcept
	{
		if (!active || input != CAPTURED_INPUT || channelCount == 0) return false;
		const int wanted = target.load(std::memory_order_relaxed);
		if (wanted <= 0) return false;

		const int have = filled.load(std::memory_order_relaxed);
		if (have >= wanted) return false;
		const int count = (std::min)(static_cast<int>(frames), wanted - have);
		std::copy_n(channels[0], count, captured.data() + have);
		filled.store(have + count, std::memory_order_release);
		return false;
	}

	std::uint32_t LatencyCapture::GetLatencyFrames(std::uint32_t) const
	{
		return 0;
	}

	// Disarm before the reset, so the audio thread stops appending; arm last.
	void LatencyCapture::Start(int frames)
	{
		frames = std::clamp(frames, 1, MAX_FRAMES);
		target.store(0, std::memory_order_relaxed);
		filled.store(0, std::memory_order_relaxed);
		target.store(frames, std::memory_order_release);
	}

	bool LatencyCapture::IsDone() const
	{
		const int wanted = target.load(std::memory_order_acquire);
		return wanted > 0 && filled.load(std::memory_order_acquire) >= wanted;
	}

	int LatencyCapture::Get(const float** out) const
	{
		if (out) *out = captured.data();
		return filled.load(std::memory_order_acquire);
	}

	LatencyCapture& SharedLatencyCapture()
	{
		static LatencyCapture capture;
		return capture;
	}
}
