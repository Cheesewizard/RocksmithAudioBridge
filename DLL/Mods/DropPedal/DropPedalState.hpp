#pragma once

#include "DropPedalPlayer.hpp"

#include <string>
#include "DropPedal.hpp"

namespace DropPedalState
{
	void Configure(const std::string& enabledSetting);
	bool IsConfiguredEnabled();
	bool IsEnabled();
	bool IsSpeakerModeEnabled();
	DropPedal::PitchMode GetPitchMode();
	bool TryCyclePitchMode(DropPedal::PitchMode& nextMode);
	bool DisableSpeakerMode();
	void SetGameplayInProgress(bool isGameplay);
	bool AreSpeakerControlsLocked();
	void SetSpeakerTargetSynchronized(bool isSynchronized);
	bool IsSpeakerTargetSynchronized();
	bool AdjustTarget(DropPedal::Player player, int semitoneDelta);
	bool SetTargetSemitones(DropPedal::Player player, int semitones);
	bool CycleBaseTuning(DropPedal::Player player);
	int GetTargetSemitones(DropPedal::Player player);
	int GetBaseTuningSemitones(DropPedal::Player player);
	float GetTargetCents(DropPedal::Player player);
	std::string GetTuningName(DropPedal::Player player);
	std::string GetBaseTuningName(DropPedal::Player player);
	std::string GetAbsoluteTuningName(int semitonesFromE);
	int GetShiftDirection(DropPedal::Player player);
	unsigned long long GetModeNoticeTick();
	// Tick of the last pedal key refused in Speaker Mode (F7 mid-song, or a Player 2 key); 0 if none.
	unsigned long long GetModeLockedNoticeTick();
	// Flash the Speaker Mode line to show a pedal key was refused.
	void NotifyModeLocked();
}
