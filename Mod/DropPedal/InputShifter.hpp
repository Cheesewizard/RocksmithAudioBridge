#pragma once

#include "Framework/AudioInput.hpp"

#include "DelayLinePitchShifter.hpp"
#include "DropPedalTypes.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace AudioBridge::DropPedal
{
	// Drop Pedal's pitch shifter on each player's guitar input, in the framework's input chain. The tap feeds each
	// player's stream as mono; only channel 0 is shifted.
	class InputShifter final : public Framework::IInputProcessor
	{
	public:
		void Prepare(std::uint32_t input, const Framework::CaptureFormat& format) override;
		bool Process(std::uint32_t input, float* const* channels, std::uint32_t channelCount, std::uint32_t frames,
			bool active) noexcept override;
		std::uint32_t GetLatencyFrames(std::uint32_t input) const override;

		// MainThread.
		void SetSemitones(::DropPedal::Player player, int semitones);

		// Any thread: the chain ran this player's input through the shifter, with the pedal Active, in the last
		// second (silent packets come through as zeros, so a quiet guitar still counts). What 4.x's "input shifter
		// active" flag stood for: the shifter is in the input path and applying whatever shift is dialled.
		bool IsApplying(::DropPedal::Player player) const;

	private:
		std::array<std::atomic<std::uint64_t>, ::DropPedal::PLAYER_COUNT> lastActiveTick{};
		std::array<Audio::DelayLinePitchShifter, ::DropPedal::PLAYER_COUNT> shifters{
			Audio::DelayLinePitchShifter(0), Audio::DelayLinePitchShifter(0) };
	};
}
