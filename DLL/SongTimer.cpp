#include "stdafx.h"
#include "SongTimer.hpp"

namespace {
	bool TryReadTimerValue(uintptr_t address, float& value)
	{
		if (address == 0) return false;

		__try
		{
			value = *reinterpret_cast<float*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// SongTimer() runs every frame and the chains stop resolving for whole
	// stretches (the multiplayer pause menu is one), so each failure episode is
	// logged once.
	bool hasReportedBaseTimerFailure = false;
	bool hasReportedRareTimerFailure = false;
}

float SongTimer::SongTimer() {
	if (GameState::Menus::IsInPreSongTuner()) {
		return 0.f;
	}

	const uintptr_t addrTimerBase = MemUtil::FindDMAAddy(
		Offsets::baseHandle + Offsets::ptr_timer,
		Offsets::ptr_timerBaseOffsets,
		true);
	float baseTimer = 0.f;

	if (!TryReadTimerValue(addrTimerBase, baseTimer)) {
		if (!hasReportedBaseTimerFailure) {
			hasReportedBaseTimerFailure = true;
			LOG_ERROR("Invalid Pointer: (BASE) ShowSongTimer; further repeats suppressed until it resolves" << std::endl);
		}
		return 0.f;
	}
	hasReportedBaseTimerFailure = false;

	const uintptr_t addrTimerRare = MemUtil::FindDMAAddy(
		Offsets::baseHandle + Offsets::ptr_timerRare,
		Offsets::ptr_timerRareOffsets,
		true);
	float rareTimer = 0.f;

	if (!TryReadTimerValue(addrTimerRare, rareTimer)) {
		if (!hasReportedRareTimerFailure) {
			hasReportedRareTimerFailure = true;
			LOG_ERROR("Invalid Pointer: (RARE) ShowSongTimer; further repeats suppressed until it resolves" << std::endl);
		}
		return baseTimer;
	}
	hasReportedRareTimerFailure = false;

	// We entered a song where the base timer does not work.
	// Cause for this is unknown but we need to check, or time based mods (looping, song timer) will break.
	// Ex: Desolate Motion, or Rocksmith 2012 Theme.
	if (GameState::Menus::IsInSongModes() && baseTimer == 0.f && rareTimer != 0.f) {
		return rareTimer;
	}

	return baseTimer;
}

/// <summary>
/// When the user passes a note in the pause menu, notes become grey. This gets the time at which notes go from being grey (deactivated) to being colored (activated).
/// </summary>
/// <returns>Time where all notes before it are grey / deactivated.</returns>
float SongTimer::GetGreyNoteTimer() {
	// Read every frame by the loop overlay while paused and by the research draw feed, so every link is checked and
	// the walk and the read are guarded against a link freed as the song ends.
	uintptr_t greyNoteTimer = MemUtil::FindDMAAddyGuarded(Offsets::baseHandle + Offsets::ptr_greyOutNoteTimer, Offsets::ptr_greyOutNoteTimerOffsets);
	float greyNoteTime = 0.f;

	if (!MemUtil::TryRead(greyNoteTimer, greyNoteTime)) {
		LOG_ERROR("Invalid Pointer: GetGreyNoteTimer = NULL" << std::endl);
		return NULL;
	}

	return greyNoteTime;
}

/// <summary>
/// Sets the time at which all notes before it turn grey (deactivated) and all notes after it are colorful (activated)
/// </summary>
/// <param name="timeInSeconds"> - Time to set the "deactivate before" at.</param>
void SongTimer::SetGreyNoteTimer(float timeInSeconds) {
	// Written every frame by the loop overlay while paused, so it is guarded the same way as GetGreyNoteTimer.
	uintptr_t greyNoteTimer = MemUtil::FindDMAAddyGuarded(Offsets::baseHandle + Offsets::ptr_greyOutNoteTimer, Offsets::ptr_greyOutNoteTimerOffsets);

	if (!MemUtil::TryWrite(greyNoteTimer, timeInSeconds)) {
		LOG_ERROR("Invalid Pointer: SetGreyNoteTimer = NULL" << std::endl);
		return;
	}
}

/// <summary>
/// Gets how long the countdown will last for from start to end, between songs, in NSP.
/// </summary>
/// <returns>The amount of time it takes to go from song 1 to song 2.</returns>
double SongTimer::GetNonStopPlayTimer()
{
	return *(double*)Offsets::ptr_NonStopPlayPreSongTimer.Get();
}

/// <summary>
/// Sets the amount of time between song 1 and song 2 in NSP.
/// </summary>
/// <param name="NewTimer"> - New time to set</param>
void SongTimer::SetNonStopPlayTimer(double NewTimer)
{
	MemUtil::SetStaticValue(Offsets::ptr_NonStopPlayPreSongTimer, NewTimer, sizeof(NewTimer));
}
