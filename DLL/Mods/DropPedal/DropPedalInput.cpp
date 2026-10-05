#include "../../stdafx.h"
#include "DropPedal.hpp"
#include "DropPedalInput.hpp"
#include "DropPedalState.hpp"

namespace
{
	DropPedal::Player GetCommandPlayer()
	{
		return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
			? DropPedal::Player::Two
			: DropPedal::Player::One;
	}

	const char* GetPlayerName(DropPedal::Player player)
	{
		return player == DropPedal::Player::One ? "Player 1" : "Player 2";
	}

	bool RejectUnavailablePlayerTwo(DropPedal::Player player)
	{
		if (player != DropPedal::Player::Two) return false;
		if (DropPedal::IsPlayerShiftAvailable(DropPedal::Player::Two)) return false;

		LOG_ERROR("Drop pedal Player 2 controls require a configured second input route." << std::endl);
		return true;
	}
}

void DropPedalInput::AdjustTarget(int semitoneDelta)
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	const DropPedal::Player player = GetCommandPlayer();
	if (RejectUnavailablePlayerTwo(player)) return;

	if (!DropPedalState::AdjustTarget(player, semitoneDelta)) return;

	DropPedal::UpdateInputShifterPitch(player);

	LOG_INFO("Drop pedal " << GetPlayerName(player) << " target now "
		<< DropPedalState::GetTuningName(player) << std::endl);

}

void DropPedalInput::ToggleEnabled()
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	DropPedal::PitchMode nextMode;
	if (!DropPedalState::TryCyclePitchMode(nextMode))
	{
		LOG_INFO("Speaker Mode cannot be selected or disabled during gameplay" << std::endl);
		return;
	}

	DropPedal::UpdateInputShifterPitch(DropPedal::Player::One);
	DropPedal::UpdateInputShifterPitch(DropPedal::Player::Two);

	LOG_INFO("Pitch mode changed to " << DropPedal::GetPitchModeName()
		<< ", route " << DropPedal::GetPitchRouteName() << std::endl);
}

void DropPedalInput::CycleBaseTuning()
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	const DropPedal::Player player = GetCommandPlayer();
	if (RejectUnavailablePlayerTwo(player)) return;

	if (!DropPedalState::CycleBaseTuning(player)) return;

	LOG_INFO("Drop pedal " << GetPlayerName(player) << " base tuning now "
		<< DropPedalState::GetBaseTuningName(player) << std::endl);
}
