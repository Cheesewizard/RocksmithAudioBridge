#pragma once

// The overlay's Output page: where the game's sound goes (Playing to, Switch output, the output pair), the device's
// buffer and the game's own block size in Cable mode, and the volume cap.
namespace AudioBridge::Pages
{
	// MainThread, from AudioBridgeRoutingMod::OnInitialize.
	void RegisterOutputPage();
}
