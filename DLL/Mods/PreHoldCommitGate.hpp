#pragma once

namespace NoteByNote
{
	inline bool ShouldBlockPreHoldHitDecision(
		bool isFlowUntilMissEnabled,
		bool isHoldSuppressed)
	{
		return !isFlowUntilMissEnabled && !isHoldSuppressed;
	}

	inline bool ShouldRejectNaturalPreHoldCommit(
		bool isFlowUntilMissEnabled,
		bool isHoldSuppressed,
		bool hasTargetOwnedFreshAttack)
	{
		return ShouldBlockPreHoldHitDecision(isFlowUntilMissEnabled, isHoldSuppressed)
			&& !hasTargetOwnedFreshAttack;
	}
}
