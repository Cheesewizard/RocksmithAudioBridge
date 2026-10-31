#include "InputObserver.hpp"

#include "../Detection/RawPitchVerifier.hpp"
#include "../Ml/MlAudioExporter.hpp"

namespace NoteByNote
{
	void InputObserver::Prepare(std::uint32_t input, const Framework::CaptureFormat& format)
	{
		if (input >= sampleRates.size()) return;
		sampleRates[input].store(format.sampleRate, std::memory_order_release);
	}

	// Both observers keep only Player 1 (input 0) and neither allocates nor locks; 4.x called them with the same
	// arguments. Silent buffers arrive as zeros, as they did there.
	bool InputObserver::Process(std::uint32_t input, float* const* channels, std::uint32_t channelCount,
		std::uint32_t frames, bool active) noexcept
	{
		if (!active || input >= sampleRates.size() || channelCount == 0 || frames == 0) return false;
		const std::uint32_t sampleRate = sampleRates[input].load(std::memory_order_acquire);
		if (sampleRate == 0) return false;
		RawPitchVerifier::Observe(input, channels[0], frames, sampleRate);
		MlAudioExporter::Observe(input, channels[0], frames, sampleRate);
		return false;
	}

	std::uint32_t InputObserver::GetLatencyFrames(std::uint32_t) const
	{
		return 0;
	}
}
