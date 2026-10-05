#include "../../stdafx.h"
#include "DropPedalState.hpp"

namespace
{
	constexpr float CENTS_PER_SEMITONE = 100.0f;
	constexpr int MIN_TARGET_SEMITONES = -24;
	constexpr int MAX_TARGET_SEMITONES = 24;
	constexpr int SEMITONES_PER_OCTAVE = 12;

	// Session state is written by WndProc key commands and read by Wwise, rendering and
	// game-loop threads.
	std::atomic<int> targetSemitones[DropPedal::PLAYER_COUNT] = {};

	// From [Drop Pedal] in RSMods.ini, read once at startup.
	bool isConfiguredEnabled = false;

	// Pitch mode lives in the session rather than in the settings map, because the
	// settings reload during boot and would otherwise wipe a hotkey transition.
	// Pitch processing starts Off so the player chooses the route explicitly.
	std::atomic<DropPedal::PitchMode> pitchMode{ DropPedal::PitchMode::Off };
	std::atomic<bool> isGameplayInProgress{ false };
	std::atomic<bool> isSpeakerTargetSynchronized{ false };
	std::atomic<unsigned long long> modeNoticeTick{ 0 };
	// When F7 was refused because Speaker Mode is locked for the song (the overlay flashes).
	std::atomic<unsigned long long> modeLockedNoticeTick{ 0 };

	// The tuning the player's guitar is physically in, as semitones from E standard.
	// Everything the mod shows is relative to this, so a player who lives in Eb sees
	// tunings named from Eb rather than being told to do the arithmetic themselves.
	// Session state like the shift itself: a player in Eb sets it once per launch.
	std::atomic<int> baseTuningSemitones[DropPedal::PLAYER_COUNT] = {};

	const char* GetTuningNameAtIndex(int index)
	{
		// The names read downwards from E, so a downward step indexes straight into them.
		static const char* tuningNames[SEMITONES_PER_OCTAVE] = {
			"E", "Eb", "D", "C#", "C", "B", "Bb", "A", "Ab", "G", "F#", "F"
		};

		return tuningNames[index];
	}

	const char* GetTuningNameForSemitones(int semitonesFromE)
	{
		int stepsBelowE = (-semitonesFromE) % SEMITONES_PER_OCTAVE;
		if (stepsBelowE < 0)
		{
			stepsBelowE += SEMITONES_PER_OCTAVE;
		}

		return GetTuningNameAtIndex(stepsBelowE);
	}
}

void DropPedalState::Configure(const std::string& enabledSetting)
{
	isConfiguredEnabled = enabledSetting == "on";
	if (isConfiguredEnabled) return;

	pitchMode.store(DropPedal::PitchMode::Off, std::memory_order_relaxed);
	isSpeakerTargetSynchronized.store(false, std::memory_order_relaxed);
}

bool DropPedalState::IsConfiguredEnabled()
{
	return isConfiguredEnabled;
}

bool DropPedalState::IsEnabled()
{
	return GetPitchMode() == DropPedal::PitchMode::DropPedal;
}

bool DropPedalState::IsSpeakerModeEnabled()
{
	return GetPitchMode() == DropPedal::PitchMode::SpeakerMode;
}

DropPedal::PitchMode DropPedalState::GetPitchMode()
{
	return pitchMode.load(std::memory_order_relaxed);
}

bool DropPedalState::TryCyclePitchMode(DropPedal::PitchMode& nextMode)
{
	const auto currentMode = GetPitchMode();
	const bool isGameplay = isGameplayInProgress.load(std::memory_order_relaxed);
	if (isGameplay && currentMode == DropPedal::PitchMode::SpeakerMode)
	{
		nextMode = currentMode;
		modeLockedNoticeTick.store(GetTickCount64(), std::memory_order_relaxed);
		return false;
	}
	if (isGameplay && currentMode == DropPedal::PitchMode::DropPedal)
	{
		nextMode = DropPedal::PitchMode::Off;
	}
	else
	{
		switch (currentMode)
		{
			case DropPedal::PitchMode::DropPedal:
				nextMode = DropPedal::PitchMode::SpeakerMode;
				break;
			case DropPedal::PitchMode::SpeakerMode:
				nextMode = DropPedal::PitchMode::Off;
				break;
			case DropPedal::PitchMode::Off:
			default:
				nextMode = DropPedal::PitchMode::DropPedal;
				break;
		}
	}

	pitchMode.store(nextMode, std::memory_order_relaxed);
	isSpeakerTargetSynchronized.store(false, std::memory_order_relaxed);
	modeNoticeTick.store(GetTickCount64(), std::memory_order_relaxed);
	return true;
}

bool DropPedalState::DisableSpeakerMode()
{
	auto expectedMode = DropPedal::PitchMode::SpeakerMode;
	if (!pitchMode.compare_exchange_strong(
		expectedMode,
		DropPedal::PitchMode::Off,
		std::memory_order_relaxed)) return false;

	modeNoticeTick.store(GetTickCount64(), std::memory_order_relaxed);
	isSpeakerTargetSynchronized.store(false, std::memory_order_relaxed);
	return true;
}

void DropPedalState::SetGameplayInProgress(bool isGameplay)
{
	isGameplayInProgress.store(isGameplay, std::memory_order_relaxed);
}

bool DropPedalState::AreSpeakerControlsLocked()
{
	return isGameplayInProgress.load(std::memory_order_relaxed)
		&& IsSpeakerModeEnabled();
}

void DropPedalState::SetSpeakerTargetSynchronized(bool isSynchronized)
{
	isSpeakerTargetSynchronized.store(isSynchronized, std::memory_order_relaxed);
}

bool DropPedalState::IsSpeakerTargetSynchronized()
{
	return isSpeakerTargetSynchronized.load(std::memory_order_relaxed);
}

/// <summary>
/// Move the target immediately, so the on-screen tuning tracks the player's key
/// presses without lag. Does nothing while the mod is off, so the pitch keys are
/// inert until toggled on.
/// </summary>
bool DropPedalState::AdjustTarget(DropPedal::Player player, int semitoneDelta)
{
	if (GetPitchMode() == DropPedal::PitchMode::Off
		|| AreSpeakerControlsLocked()
		|| (IsSpeakerModeEnabled() && IsSpeakerTargetSynchronized()))
	{
		return false;
	}

	const int adjusted = DropPedalState::GetTargetSemitones(player) + semitoneDelta;
	return SetTargetSemitones(player, adjusted);
}

bool DropPedalState::SetTargetSemitones(DropPedal::Player player, int semitones)
{
	if (semitones < MIN_TARGET_SEMITONES || semitones > MAX_TARGET_SEMITONES)
	{
		return false;
	}

	targetSemitones[DropPedal::GetPlayerIndex(player)].store(semitones, std::memory_order_relaxed);
	return true;
}

/// <summary>
/// Cycle the physical base tuning down one name per press, wrapping after F
/// back to E. One octave reaches every tuning name; Speaker Mode's octave
/// adjustment anchors the absolute octave to the selected target.
/// </summary>
bool DropPedalState::CycleBaseTuning(DropPedal::Player player)
{
	if (GetPitchMode() == DropPedal::PitchMode::Off || AreSpeakerControlsLocked())
	{
		return false;
	}

	const size_t playerIndex = DropPedal::GetPlayerIndex(player);
	int next = baseTuningSemitones[playerIndex].load(std::memory_order_relaxed) - 1;
	if (next <= -SEMITONES_PER_OCTAVE)
	{
		next = 0;
	}

	baseTuningSemitones[playerIndex].store(next, std::memory_order_relaxed);
	return true;
}

int DropPedalState::GetTargetSemitones(DropPedal::Player player)
{
	return targetSemitones[DropPedal::GetPlayerIndex(player)].load(std::memory_order_relaxed);
}

int DropPedalState::GetBaseTuningSemitones(DropPedal::Player player)
{
	return baseTuningSemitones[DropPedal::GetPlayerIndex(player)].load(std::memory_order_relaxed);
}

float DropPedalState::GetTargetCents(DropPedal::Player player)
{
	return static_cast<float>(DropPedalState::GetTargetSemitones(player)) * CENTS_PER_SEMITONE;
}

/// <summary>
/// Name the tuning the player's guitar is heard in: their physical tuning moved by
/// the current shift, in the form a tuner would show it.
/// </summary>
std::string DropPedalState::GetTuningName(DropPedal::Player player)
{
	const int semitones = DropPedalState::GetTargetSemitones(player);
	const int baseSemitones = DropPedalState::GetBaseTuningSemitones(player);
	const char* baseName = GetTuningNameForSemitones(baseSemitones);

	std::ostringstream name;
	if (semitones == 0)
	{
		name << baseName;
	}
	else
	{
		name << baseName << " -> " << GetTuningNameForSemitones(baseSemitones + semitones)
			<< " (" << (semitones > 0 ? "+" : "") << semitones << ")";
	}

	return name.str();
}

/// <summary>
/// Name the tuning the player's guitar is physically in, with no shift applied.
/// </summary>
std::string DropPedalState::GetBaseTuningName(DropPedal::Player player)
{
	return GetAbsoluteTuningName(DropPedalState::GetBaseTuningSemitones(player)) + " standard";
}

std::string DropPedalState::GetAbsoluteTuningName(int semitonesFromE)
{
	return GetTuningNameForSemitones(semitonesFromE);
}

int DropPedalState::GetShiftDirection(DropPedal::Player player)
{
	const int semitones = DropPedalState::GetTargetSemitones(player);
	if (semitones < 0)
	{
		return -1;
	}

	return semitones > 0 ? 1 : 0;
}

unsigned long long DropPedalState::GetModeNoticeTick()
{
	return modeNoticeTick.load(std::memory_order_relaxed);
}

unsigned long long DropPedalState::GetModeLockedNoticeTick()
{
	return modeLockedNoticeTick.load(std::memory_order_relaxed);
}
