#pragma once

#include "PedalControls.hpp"

// The overlay's Drop Pedal page: the pedal's controls (shift, Match song, mode, each player's tuning) and its readout's
// settings.
namespace AudioBridge::DropPedal
{
	// MainThread, from DropPedalMod::OnInitialize. The page keeps `controls` for the mod's lifetime.
	void RegisterPage(PedalControls& controls);
}
