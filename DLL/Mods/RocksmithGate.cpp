#include "../stdafx.h"
#include "RocksmithGate.hpp"
#include "../Wwise/SoundEngine.hpp"
#include <atomic>

namespace {
	// -59.3134 dB is Rocksmith's stock calibrated noise floor (matches the calibration defaults in
	// Menu.cpp), so enabling the override with an untouched slider is neutral until the user opens it.
	constexpr float kGameDefaultNoiseFloorDb = -59.3134f;
	std::atomic<bool> g_overrideEnabled{ false };
	std::atomic<float> g_thresholdDb{ kGameDefaultNoiseFloorDb };
	// Set once on the enabled -> disabled edge so turning the override off restores the game's default gate
	// right away, instead of leaving our forced value until the next song/calibration re-asserts it.
	std::atomic<bool> g_restorePending{ false };

	void WriteNoiseFloor(float value) {
		Wwise::SoundEngine::SetRTPCValue("P1_NoiseFloor", value, AK_INVALID_GAME_OBJECT, 0, AkCurveInterpolation_Linear);
		Wwise::SoundEngine::SetRTPCValue("P1_NoiseFloor", value, 0x1234, 0, AkCurveInterpolation_Linear);
	}
}

void RocksmithGate::SetOverride(bool enabled, float thresholdDb) {
	g_thresholdDb.store(thresholdDb, std::memory_order_release);
	const bool was = g_overrideEnabled.exchange(enabled, std::memory_order_acq_rel);
	if (was && !enabled) g_restorePending.store(true, std::memory_order_release);
}

bool RocksmithGate::IsOverrideEnabled() { return g_overrideEnabled.load(std::memory_order_acquire); }

float RocksmithGate::GetThresholdDb() { return g_thresholdDb.load(std::memory_order_acquire); }

void RocksmithGate::ApplyPerFrame() {
	if (!Wwise::SoundEngine::IsInitialized()) return;   // never touch RTPCs before Wwise is up
	if (g_overrideEnabled.load(std::memory_order_acquire)) {
		// Match the game's own calibration write: the global object and the player object (0x1234).
		WriteNoiseFloor(g_thresholdDb.load(std::memory_order_acquire));
	}
	else if (g_restorePending.exchange(false, std::memory_order_acq_rel)) {
		// One-shot revert to the stock gate on turn-off; the game re-asserts its real calibrated value on
		// the next song/calibration, so this only bridges the gap for the current song.
		WriteNoiseFloor(kGameDefaultNoiseFloorDb);
	}
}
