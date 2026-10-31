#include "InputShifter.hpp"

#include <windows.h>

namespace AudioBridge::DropPedal
{
	void InputShifter::Prepare(std::uint32_t input, const Framework::CaptureFormat& format)
	{
		if (input >= shifters.size()) return;
		shifters[input].Prepare(format.sampleRate);
	}

	bool InputShifter::Process(std::uint32_t input, float* const* channels, std::uint32_t channelCount,
		std::uint32_t frames, bool active) noexcept
	{
		if (!active || input >= shifters.size() || channelCount == 0) return false;
		lastActiveTick[input].store(GetTickCount64(), std::memory_order_relaxed);
		return shifters[input].Process(channels[0], frames);
	}

	// Constant, as the chain requires. Unshifted audio passes with no delay. Shifted audio trails by up to one
	// detected period, varying with the note like a hardware drop pedal, and is left out on purpose: counting it
	// would mean delaying unshifted audio by the same amount, ~10 ms on every note.
	std::uint32_t InputShifter::GetLatencyFrames(std::uint32_t) const
	{
		return 0;
	}

	void InputShifter::SetSemitones(::DropPedal::Player player, int semitones)
	{
		shifters[::DropPedal::GetPlayerIndex(player)].SetSemitones(semitones);
	}

	bool InputShifter::IsApplying(::DropPedal::Player player) const
	{
		constexpr std::uint64_t STALE_MS = 1000;
		const std::uint64_t last = lastActiveTick[::DropPedal::GetPlayerIndex(player)].load(std::memory_order_relaxed);
		return last != 0 && GetTickCount64() - last < STALE_MS;
	}
}
