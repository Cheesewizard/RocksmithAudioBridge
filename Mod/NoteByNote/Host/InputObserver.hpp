#pragma once

#include "Framework/AudioInput.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace NoteByNote
{
	// Note by Note's view of the guitar input: hands each buffer to the raw pitch verifier and the ML service's audio
	// export (4.x fed them from DLL/AudioBridge/Input/InputCaptureHook.cpp). Last in the chain, so it hears what the
	// game's detection hears: conditioned and, with the Drop Pedal on, shifted (expected pitches are graded in that
	// shifted frame, DropPedalShift.hpp). Never changes the audio and adds no latency.
	class InputObserver final : public Framework::IInputProcessor
	{
	public:
		void Prepare(std::uint32_t input, const Framework::CaptureFormat& format) override;
		bool Process(std::uint32_t input, float* const* channels, std::uint32_t channelCount, std::uint32_t frames,
			bool active) noexcept override;
		std::uint32_t GetLatencyFrames(std::uint32_t input) const override;

	private:
		std::array<std::atomic<std::uint32_t>, Framework::kMaxAudioInputs> sampleRates{};
	};
}
