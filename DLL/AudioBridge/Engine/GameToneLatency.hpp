#pragma once

#include <windows.h>
#include <cstdint>

// Guitar-to-ear delay of the game's own tones in Cable mode, measured while the player plays (2026-10-13).
//
// Why: the Tone designer's delay is measured sample by sample (CableAmpLink), but the game's tones pass through Wwise,
// where a sample cannot be followed. The player could not tell what game tones cost, nor compare settings.
//
// How: the same as reading a recording by eye. Picks are found in Player 1's cable input as it arrives (a sharp rise in
// level after quiet), and attacks in the game's output as it leaves for the device, each stamped with QPC; the output's
// stamp is when it will be heard (what the backend and device still hold ahead of it). Each output attack is matched to
// the latest unmatched pick 2-200 ms before it; the median of the last 15 matches is the delay. A song's backing track
// makes attacks of its own, so a match needs quiet output before the attack, and the median rides over the odd wrong
// pairing; with nothing to measure (no playing, or the game amp silenced) Valid() is false.
namespace Audio::GameToneLatency
{
	// Cable thread: Player 1's raw input as it arrives; the last sample came in at `lastSample` (QPC, 100 ns).
	void Input(const float* mono, uint32_t frames, UINT64 lastSample) noexcept;
	// Output session: the game's packet as handed to the backend; its first sample is heard at `heard` (QPC, 100 ns).
	void Output(const float* stereo, uint32_t frames, UINT64 heard) noexcept;
	bool Valid();
	float MedianMs();
	UINT64 Now100ns();
}
