#pragma once

namespace DropPedalInput
{
	void AdjustTarget(int semitoneDelta);
	void ToggleEnabled();
	// One key cycles the base tuning down a name per press, wrapping to E.
	void CycleBaseTuning();
}
