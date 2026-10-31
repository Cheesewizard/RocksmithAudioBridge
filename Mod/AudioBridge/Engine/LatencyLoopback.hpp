#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

// Measured game tones latency in Cable mode, on demand (prototype).
//
// Why: GameToneLatency times picks against attacks in the game's output. On an amped, gated, compressed tone the
// attack is fuzzy, so its figure drifts with the tone and the playing, and it needs the player to play.
//
// How: a digital loopback inside the game. While armed, Player 1's cable packets that the game reads are replaced by
// three short chirps (CableCaptureSession::Publish), stamped with the same capture clock GameToneLatency uses. The
// game's output is recorded as it is handed to the backend, stamped with when it will be heard (AudioBridgeEngine
// Pump), and nothing reaches the speakers meanwhile (the player hears silence for about a second). A worker then
// finds each chirp in the output by normalized cross-correlation; the median lag is the game tones' delay from the
// cable sample to the speaker, excluding only the cable's own converter and USB and the output device's DAC.
// The game's amp must be the one playing (Compare on, or the Tone designer off): a muted game amp answers nothing.
//
// Checked offline (atlas tools/latency-loopback/loopback_sim.py: a stand-in amp of 80 Hz high-pass, tanh at gain 1 to
// 300 and a 3-4 kHz cab low-pass, music up to -12 dB, packet stamps jittered by up to 1 ms): within 0.15 ms of the true
// delay in every case, the remainder being the cab filter's own delay, which is real. Laying the output packets end to
// end matters: placed at their own stamps, the jitter gave 1-2 ms errors. Options weighed: an envelope (quadrature)
// correlation was no better than the plain one; a 150 Hz-4 kHz sweep no better than 300 Hz-2.5 kHz.
namespace Audio::LatencyLoopback
{
	// Overlay: start a measurement. False (with `why`) when one is running or it cannot run now.
	bool Start(std::string& why);
	bool Running();
	// One line for the overlay: what it is doing, or the last result.
	std::string Status();
	// The last measured total in ms, or -1 when none this launch.
	float LastMs();

	// Cable capture thread, Player 1 only: replaces `samples` (frames of mono float) in place while injecting.
	// `firstSample` is the first sample's capture time (QPC, 100 ns). Returns true when it replaced them.
	bool Inject(float* samples, uint32_t frames, UINT64 firstSample) noexcept;
	// Output session: the game's packet (interleaved stereo) whose first sample is heard at `heard` (QPC, 100 ns).
	void Capture(const float* stereo, uint32_t frames, UINT64 heard) noexcept;
	// Output session: true while what goes to the speakers must be silence.
	bool MuteOutput() noexcept;
}
