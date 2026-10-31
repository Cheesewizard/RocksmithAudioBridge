#pragma once

// Standard library and Windows only: the framework mod and the 4.x DLL compile this state.

#include "DropPedalTypes.hpp"

#include <string>

namespace DropPedalState
{
	void Configure(const std::string& enabledSetting);
	bool IsConfiguredEnabled();
	bool IsEnabled();
	bool IsSpeakerModeEnabled();
	DropPedal::PitchMode GetPitchMode();
	bool TryCyclePitchMode(DropPedal::PitchMode& nextMode);
	// Direct selection for the overlay's mode control. Same song locks as the F7 cycle: Speaker Mode can be
	// neither entered nor left mid-song.
	bool CanSelectPitchMode(DropPedal::PitchMode mode);
	bool SetPitchMode(DropPedal::PitchMode mode);
	// Whether AdjustTarget would accept a step right now (mode on, Speaker Mode not locked or following the song).
	bool CanAdjustTarget();
	bool DisableSpeakerMode();
	void SetGameplayInProgress(bool isGameplay);
	bool AreSpeakerControlsLocked();
	void SetSpeakerTargetSynchronized(bool isSynchronized);
	bool IsSpeakerTargetSynchronized();
	bool AdjustTarget(DropPedal::Player player, int semitoneDelta);
	bool SetTargetSemitones(DropPedal::Player player, int semitones);
	bool CanChangeBaseTuning();
	// semitonesFromE: 0 (E) down to -11 (F). Works while the pedal is Off too.
	bool SetBaseTuningSemitones(DropPedal::Player player, int semitonesFromE);
	// Parses a GetAbsoluteTuningName name ("Eb") back to semitones from E; false for anything else.
	bool TryParseTuningName(const std::string& name, int& semitonesFromE);
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
