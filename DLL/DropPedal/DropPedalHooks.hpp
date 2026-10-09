#pragma once

#include "DropPedalPlayer.hpp"

namespace DropPedalHooks
{
	void Install();
	void Poll();
	// Decoder-thread-safe request; the game-loop poll restores tuning state.
	void QueuePitchRestore();
	void SetInputShifterActive(bool active);
	bool IsInputShifterActive();
	bool TryGetAuthoredTrueTuning(float& trueTuning);
	void ReportInputShifterUnavailable();
	void HandleArrangementTuning();
	void ResetSongState();
}
