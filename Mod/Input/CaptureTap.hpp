#pragma once

// Feeds the game's guitar capture streams through the framework's input chain (Framework::AudioInput()).
//
// The game reads its input through PortAudio's WASAPI host, RS_ASIO or not. When PortAudio opens a stream it
// unmarshals the stream's COM pointers; the tap hooks that function (chaining RS_ASIO's own patch when RS_ASIO is
// on, detouring the game's function in Cable mode), takes the stream's IAudioCaptureClient, assigns it to a player
// from RS_ASIO.ini and patches IAudioCaptureClient::GetBuffer. Each buffer's first channel goes through the chain
// as mono float (input index = player) and, when a processor changed it, back to every channel.
//
// Functions run on MainThread unless noted.
#include <cstdint>

namespace AudioBridge::Input::CaptureTap
{
	// Reads RS_ASIO.ini and installs the unmarshal hook, or arms a retry while RS_ASIO hasn't patched yet. Once.
	void Install();
	// Retries the hook install, and prepares (or re-prepares) each player's stream in the chain once it's bound.
	// The chain is fixed at the first prepare, so this must not run before every mod's OnInitialize.
	void Tick();
	// Inactive, buffers pass untouched and every prepared stream is released.
	void SetActive(bool active);
	void Shutdown();

	// Any thread. RS_ASIO.ini (or Cable mode) gives this player an input.
	bool IsPlayerConfigured(std::uint32_t player);
	// Any thread. This player's stream is bound and running through the chain.
	bool IsPlayerLive(std::uint32_t player);
}
