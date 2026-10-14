#pragma once

#include <windows.h>
#include <cstdint>

// Guitar-to-ear delay of the game's own tones in Cable mode, measured while the player plays.
//
// Why: the Tone designer's delay is measured sample by sample (CableAmpLink), but the game's tones pass through Wwise,
// where a sample cannot be followed. The player could not tell what game tones cost, nor compare settings.
//
// How: the cable input (stamped as it arrives) and the game's output (stamped with when it will be heard) are kept as
// 1 kHz high-passed 1 ms peaks on one clock. Each clear pick in the input (a 15 dB rise to above -35 dBFS) is timed
// to the first sharp rise in the output after it (8 dB over its level just before, within 150 ms), as measured by
// hand on recordings; the median of the last 7 is shown. With nothing to measure (no clear playing, the game amp
// silenced) Valid() is false.
namespace Audio::GameToneLatency
{
	// Cable thread: Player 1's raw input as it arrives; the last sample came in at `lastSample` (QPC, 100 ns).
	void Input(const float* mono, uint32_t frames, UINT64 lastSample) noexcept;
	// Output session: the game's packet as handed to the backend; its first sample is heard at `heard` (QPC, 100 ns).
	void Output(const float* stereo, uint32_t frames, UINT64 heard) noexcept;
	bool Valid();      // measured from the playing in the last 60 s
	float MedianMs();
	// Measured, or else estimated: what the game itself adds (measured once with its amp playing, remembered per game
	// block size) plus what the output holds now (always live). So the game tones' figure stays right while the Tone
	// designer plays (the game's amp is silenced then, nothing to time) and after a device or mode switch.
	bool Available();
	float CurrentMs();
	UINT64 Now100ns();
}
