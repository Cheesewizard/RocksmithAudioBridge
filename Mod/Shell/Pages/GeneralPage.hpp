#pragma once

// The overlay's General page: silence while tuning, the overlay key, the measured latency (ASIO mode) and the song
// list refresh.
namespace AudioBridge::Pages
{
	// MainThread, from AudioBridgeRoutingMod::OnInitialize.
	void RegisterGeneralPage();
}
