#pragma once

#include "DropPedalTypes.hpp"
#include "InputShifter.hpp"

#include <atomic>

namespace AudioBridge::DropPedal
{
	// What the pedal can be told to do, by its keys (MainThread) and by the overlay page (render thread). The pedal's
	// state is atomic (DropPedalState) and so is the shifter's ratio, so every call applies at once from either thread.
	class PedalControls final
	{
	public:
		explicit PedalControls(InputShifter& inputShifter);

		bool CanSelectMode(::DropPedal::PitchMode mode) const;
		// False, with nothing changed, when the pedal refuses (Speaker Mode is fixed for a song).
		bool SelectMode(::DropPedal::PitchMode mode);
		// F7: Off -> Drop Pedal -> Speaker Mode -> Off (in a song, Drop Pedal -> Off).
		bool CycleMode();

		bool CanAdjustShift() const;
		bool AdjustShift(::DropPedal::Player player, int semitoneDelta);
		bool SetShift(::DropPedal::Player player, int semitones);

		// The pedal's shift onto both players' shifters (zero unless Drop Pedal is on).
		void ApplyShifts();

		// MainThread, every tick: the shift that puts the player's guitar in the song's tuning, read from the chart
		// while a song or its tuner is up. The page's "Match song" reads it from the render thread.
		void UpdateChartMatch(bool songOrTunerUp);
		bool TryGetChartMatch(int& shiftSemitones) const;

	private:
		void ApplyShift(::DropPedal::Player player);

		InputShifter& shifter;
		std::atomic<bool> chartMatchKnown{ false };
		std::atomic<int> chartMatchShift{ 0 };
	};
}
