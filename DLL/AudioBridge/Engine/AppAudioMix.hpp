#pragma once

#include <cstdint>

// Other programs' sound (YouTube, Discord, Spotify) in the device the bridge holds exclusively or through ASIO
// (2026-10-13).
//
// Why: exclusive mode and an ASIO interface take a device away from Windows' mixer, so while Rocksmith runs no other
// program can play there. A virtual audio driver of our own would fix that but needs paid signing (ruled out).
//
// How: Windows 10 2004+ process loopback captures every program's sound except Rocksmith's own (this process tree),
// with no driver. The player sets Windows' default output to a spare device (laptop speakers) and mutes it: the
// capture is taken before the device's mute (measured: -16.5 dBFS of YouTube from muted speakers, app-loopback-probe
// in the atlas). A thread captures it into a FIFO; the output's fill (exclusive or ASIO, never shared, where programs
// can play on the device already) mixes it in, read through a rate matcher steered to ~20 ms of fill (the spare
// device and the output run on different clocks; 20 ms more on YouTube is not heard).
//
// HKCU\Software\RSMods\AsioProxy\MixAppAudio (default on).
namespace Audio::AppAudioMix
{
	bool Enabled();
	void SetEnabled(bool enabled);
	// The output's fill thread, exclusive or ASIO only: adds what other programs played to `stereo`.
	void Mix(float* stereo, uint32_t frames) noexcept;
	// For the overlay: the capture is running and something came through lately.
	bool Hearing();
	bool Running();
}
