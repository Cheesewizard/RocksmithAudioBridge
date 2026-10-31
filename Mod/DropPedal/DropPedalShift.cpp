#include "DropPedalShift.hpp"

#include "DropPedalHooks.hpp"
#include "DropPedalState.hpp"

#include <atomic>

using ::DropPedal::Player;

namespace AudioBridge::DropPedal::Shift
{
	namespace
	{
		std::atomic<bool> chartMatchKnown{ false };
		std::atomic<int> chartMatchShift{ 0 };

		bool IsShifting()
		{
			return DropPedalState::IsConfiguredEnabled() && DropPedalState::IsEnabled();
		}
	}

	// As 4.x: the pedal is on and its shifter is applying on Player 1's input (DropPedalHooks::IsInputShifterActive,
	// set each tick by DropPedalMod from InputShifter::IsApplying).
	int AppliedInputShiftSemitones()
	{
		if (!IsShifting()) return 0;
		if (!DropPedalHooks::IsInputShifterActive()) return 0;
		return DropPedalState::GetTargetSemitones(Player::One);
	}

	int InputOnsetShiftSemitones()
	{
		if (DropPedalState::GetPitchMode() != ::DropPedal::PitchMode::DropPedal) return 0;
		if (chartMatchKnown.load(std::memory_order_acquire)) return chartMatchShift.load(std::memory_order_relaxed);
		return DropPedalState::GetTargetSemitones(Player::One);
	}

	void PublishChartMatch(bool known, int shiftSemitones)
	{
		chartMatchShift.store(shiftSemitones, std::memory_order_relaxed);
		chartMatchKnown.store(known, std::memory_order_release);
	}
}
