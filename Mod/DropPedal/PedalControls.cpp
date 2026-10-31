#include "PedalControls.hpp"

#include "DropPedalShift.hpp"
#include "DropPedalState.hpp"
#include "SpeakerMode/SpeakerTargetSync.hpp"

#include "Log.hpp"

#include <cstdlib>

using DropPedal::PitchMode;
using DropPedal::Player;

namespace AudioBridge::DropPedal
{
	namespace
	{
		// DropPedalState's target range, either way.
		constexpr int SHIFT_LIMIT = 24;

		const char* ModeName(PitchMode mode)
		{
			switch (mode)
			{
			case PitchMode::DropPedal: return "Drop Pedal";
			case PitchMode::SpeakerMode: return "Speaker Mode";
			case PitchMode::Off:
			default: return "Off";
			}
		}

		const char* PlayerName(Player player)
		{
			return player == Player::One ? "Player 1" : "Player 2";
		}
	}

	PedalControls::PedalControls(InputShifter& inputShifter) : shifter(inputShifter)
	{
	}

	bool PedalControls::CanSelectMode(PitchMode mode) const
	{
		return DropPedalState::IsConfiguredEnabled() && DropPedalState::CanSelectPitchMode(mode);
	}

	bool PedalControls::SelectMode(PitchMode mode)
	{
		if (!DropPedalState::IsConfiguredEnabled()) return false;
		if (mode == DropPedalState::GetPitchMode()) return true;
		if (!DropPedalState::SetPitchMode(mode))
		{
			LOG_INFO("[DropPedal] Speaker Mode can't be switched on or off during a song." << std::endl);
			return false;
		}
		ApplyShifts();
		LOG_INFO("[DropPedal] " << ModeName(mode) << "." << std::endl);
		return true;
	}

	bool PedalControls::CycleMode()
	{
		if (!DropPedalState::IsConfiguredEnabled()) return false;
		PitchMode next = PitchMode::Off;
		if (!DropPedalState::TryCyclePitchMode(next))
		{
			LOG_INFO("[DropPedal] Speaker Mode can't be switched on or off during a song." << std::endl);
			return false;
		}
		ApplyShifts();
		LOG_INFO("[DropPedal] " << ModeName(next) << "." << std::endl);
		return true;
	}

	bool PedalControls::CanAdjustShift() const
	{
		return DropPedalState::IsConfiguredEnabled() && DropPedalState::CanAdjustTarget();
	}

	bool PedalControls::AdjustShift(Player player, int semitoneDelta)
	{
		return SetShift(player, DropPedalState::GetTargetSemitones(player) + semitoneDelta);
	}

	bool PedalControls::SetShift(Player player, int semitones)
	{
		if (!CanAdjustShift() || !DropPedalState::SetTargetSemitones(player, semitones)) return false;
		ApplyShift(player);
		LOG_INFO("[DropPedal] " << PlayerName(player) << " target now " << DropPedalState::GetTuningName(player) << std::endl);
		return true;
	}

	void PedalControls::ApplyShifts()
	{
		ApplyShift(Player::One);
		ApplyShift(Player::Two);
	}

	void PedalControls::ApplyShift(Player player)
	{
		const bool shifting = DropPedalState::IsConfiguredEnabled() && DropPedalState::IsEnabled();
		shifter.SetSemitones(player, shifting ? DropPedalState::GetTargetSemitones(player) : 0);
	}

	// In the menus the arrangement pointer can still hold the last song, so the chart only counts in a song or its tuner.
	void PedalControls::UpdateChartMatch(bool songOrTunerUp)
	{
		int tunings[6];
		const bool known = songOrTunerUp && SpeakerMode::TryReadChartTuning(tunings);
		const int shift = known ? SpeakerMode::GetChartReference(tunings) - DropPedalState::GetBaseTuningSemitones(Player::One) : 0;
		const bool inRange = known && std::abs(shift) <= SHIFT_LIMIT;
		chartMatchShift.store(shift, std::memory_order_relaxed);
		chartMatchKnown.store(inRange, std::memory_order_release);
		Shift::PublishChartMatch(inRange, shift);   // Note by Note grades against it (DropPedalShift.hpp)
	}

	bool PedalControls::TryGetChartMatch(int& shiftSemitones) const
	{
		if (!chartMatchKnown.load(std::memory_order_acquire)) return false;
		shiftSemitones = chartMatchShift.load(std::memory_order_relaxed);
		return true;
	}
}
