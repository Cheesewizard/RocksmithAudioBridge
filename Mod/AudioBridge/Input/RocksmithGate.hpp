#pragma once

// Live manual control of Rocksmith's own amp noise gate (the P1_NoiseFloor RTPC the game sets from
// calibration). On a noisy interface input the game calibrates that gate high and chops decaying notes
// early; forcing it low here stops the game cutting sustain and leaves the residual hiss for the
// front-of-chain pre-signal noise gate to handle. When the override is off the game keeps its own
// calibrated value untouched.
namespace RocksmithGate {
	// enabled = take over the game's gate; thresholdDb = the P1_NoiseFloor value to hold while enabled
	// (lower opens the gate for longer sustain; the game default is about -59.3 dB).
	void SetOverride(bool enabled, float thresholdDb);
	bool IsOverrideEnabled();
	float GetThresholdDb();

	// Re-asserts the forced value once per presented frame while enabled, because the game rewrites
	// P1_NoiseFloor on calibration and song transitions. Safe to call before Wwise is initialised.
	void ApplyPerFrame();
}
