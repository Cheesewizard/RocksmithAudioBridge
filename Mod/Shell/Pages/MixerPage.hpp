#pragma once

// The overlay's Mixer page: the game's playback buses (ops 7..13), seeded from the engine's status.
namespace AudioBridge::Pages
{
	// MainThread, from AudioBridgeRoutingMod::OnInitialize.
	void RegisterMixerPage();
}
