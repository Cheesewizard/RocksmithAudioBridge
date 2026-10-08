#include "../../stdafx.h"
#include "FYourFCEffect.hpp"

namespace CrowdControl::Effects { // Kills user's current note streak
	
	/// <summary>
	/// Test the twitch mod's requirements.
	/// </summary>
	/// <param name="request"> - JSON Request</param>
	/// <returns>EffectStatus::Success if test completed without any issues. EffectStatus::Retry if we have to retry.</returns>
	EffectStatus FYourFCEffect::Test(const Request& request)
	{
		LOG_INFO("FYourFC::Test()" << std::endl);

		if (!CanStart())
			return EffectStatus::Retry;

		return EffectStatus::Success;
	}

	/// <summary>
	/// Kills the player's current note streak for a certain duration
	/// </summary>
	/// <returns> EffectStatus::Retry if we aren't currently in a song or incompatible effects are running, or EffectStatus::Sucess if we are</returns>
	EffectStatus FYourFCEffect::Start(const Request& request)
	{
		LOG_INFO("FYourFC::Start()" << std::endl);

		if (!CanStart())
			return EffectStatus::Retry;

		uintptr_t currentNoteStreak = 0;

		// Crowd Control requests arrive at any time, including as the song ends, so every link is checked and the
		// walk and the write are guarded against a link freed under us.
		if (GameState::Menus::IsInLearnASongModes()) {
			currentNoteStreak = MemUtil::FindDMAAddyGuarded(Offsets::baseHandle + Offsets::ptr_currentNoteStreak, Offsets::ptr_currentNoteStreakLASOffsets);
		}
		else if (GameState::Menus::IsInScoreAttackModes()) {
			currentNoteStreak = MemUtil::FindDMAAddyGuarded(Offsets::baseHandle + Offsets::ptr_currentNoteStreak, Offsets::ptr_currentNoteStreakSAOffsets);
		}

		MemUtil::TryWrite<int32_t>(currentNoteStreak, 0);

		return EffectStatus::Success;
	}

	/// <summary>
	/// Stops the mod.
	/// </summary>
	/// <returns>EffectStatus::Success</returns>
	EffectStatus FYourFCEffect::Stop()
	{
		LOG_INFO("FYourFC::Stop()" << std::endl);

		return EffectStatus::Success;
	}
}
