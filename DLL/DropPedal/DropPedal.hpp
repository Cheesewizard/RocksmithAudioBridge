#pragma once

#include "DropPedalPlayer.hpp"

// Owns the session pitch route shared by the Drop Pedal input shifter and
// Speaker Mode's Wwise music-output shifter.
//
// A pitch shifter moves the whole signal, so this covers uniform tunings such as
// Eb or D standard. It cannot produce drop tunings, where only one string differs.
//
namespace DropPedal
{
	enum class PitchMode
	{
		Off,
		DropPedal,
		SpeakerMode
	};

	void LoadSettings();
	bool IsConfiguredEnabled();
	bool ShouldInstallInputHooks();
	void ReportInputShifterUnavailable();
	void InstallInputHooks();
	void UpdateInputShifterPitch(Player player);
	// Semitones the input shifter currently applies to the Player 1 route: the frame
	// divergence between the player's physical pitch and post-processing route audio.
	int GetAppliedInputShiftSemitones();
	bool SetInputPitchDetectionEnabled(bool enabled);
	bool TryGetDetectedInputMidi(Player player, int& midi);

	void InstallHooks();
	void Poll();

	// Tracks the active arrangement's authored tuning reference through tuner and song.
	void HandleArrangementTuning(bool isGameplay);
	void ResetSongState();

	bool IsEnabled();
	bool IsSpeakerModeEnabled();
	void DisableSpeakerMode();
	bool TrySynchronizeSpeakerTarget(const std::string& songKey);
	PitchMode GetPitchMode();
	std::string GetPitchModeName();
	int GetTargetSemitones(Player player);
	int GetShiftSemitones();
	// Shift that brings the physical guitar into the chart's authored tuning (chart
	// reference minus base), computed live from the chart tuning. This is the shift at
	// which the retuned input is in tune with the song; it equals the dialed shift when
	// the pedal is set correctly and diverges only when the player is out of tune.
	// False when the chart tuning is not readable yet.
	bool TryGetChartMatchShiftSemitones(int& shiftSemitones);
	bool TryGetPhysicalOpenStringMidi(int stringIndex, int& midi);
	std::string GetTuningName(Player player);
	std::string GetPitchRouteName();
	std::string GetPhysicalTuningName();
	std::string GetTargetTuningName();
	bool TryGetAuthoredTrueTuning(float& trueTuning);

	// The input itself is retuned before Rocksmith receives it, so audio and detection
	// hear the same shifted notes with either RS_ASIO or a Real Tone Cable.
	void SetInputShifterActive(bool active);
	bool IsInputShifterActive();
	// Frames the Player 1 input shifter currently adds to the path (0 when it is passing
	// audio through), for the audio diagnostics overlay.
	uint32_t GetInputShifterLatencyFrames();
	bool IsPlayerShiftAvailable(Player player);

	// The tuning the guitar is physically in, and which way the shift is going, so the
	// overlay can name and colour the state without duplicating the arithmetic.
	int GetBaseTuningSemitones(Player player);
	std::string GetBaseTuningName(Player player);
	int GetShiftDirection(Player player);
}
