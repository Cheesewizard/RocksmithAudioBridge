#pragma once

// The overlay's Input page: one tab per player (and the mic while RS_ASIO.ini sends it through the bridge), each what
// the input is, its level, that source's own settings, then Player 1's processing. Also Player 1's Noise gate pedal,
// a stomp box that floats over the game while the overlay is open.
namespace AudioBridge::Pages
{
	// MainThread, from AudioBridgeRoutingMod::OnInitialize.
	void RegisterInputPage();
}
