#pragma once

#include <windows.h>
#include <cstdint>

// Guitar-to-ear delay of the game's own tones in Cable mode, measured while the player plays.
//
// Why: the Tone designer's delay is measured sample by sample (CableAmpLink), but the game's tones pass through Wwise,
// where a sample cannot be followed. The player could not tell what game tones cost, nor compare settings.
//
// How: the cable input (stamped as it arrives) and the game's output (stamped with when it will be heard) are kept as
// 1 kHz high-passed 1 ms peaks on one clock. Each clear pick in the input (a 10 dB rise to above -42 dBFS) is timed
// to the first sharp rise in the output after it (6 dB over its level just before, within 150 ms), as measured by
// hand on recordings; the median of the last 7 is shown. With nothing to measure (no clear playing, the game amp
// silenced) Valid() is false.
namespace Audio::GameToneLatency
{
	// Cable thread: Player 1's raw input as it arrives; the last sample came in at `lastSample` (QPC, 100 ns).
	void Input(const float* mono, uint32_t frames, UINT64 lastSample) noexcept;
	// Output session: the game's packet as handed to the backend; its first sample is heard at `heard` (QPC, 100 ns).
	void Output(const float* stereo, uint32_t frames, UINT64 heard) noexcept;
	// ASIO mode: both sides from the bridge driver, in its buffer switch (no timestamps: stamped on arrival, so the lag is
	// the game's processing inside the PC; the driver's input and output latency are added by the readouts). Input:
	// Player 1's raw device channel. Output: the game's output as tapped; it only counts while SetAsioGameAudible(true)
	// (the tap includes an amp sim's return, which must not be timed as the game).
	void AsioInput(const float* mono, uint32_t frames) noexcept;
	void AsioOutput(const float* stereo, uint32_t frames) noexcept;
	void SetAsioGameAudible(bool audible);
	bool Valid();      // measured from the playing in the last 60 s (ASIO mode: the game's processing only)
	float MedianMs();
	// Measured, or else estimated: what the game itself adds (its two blocks at the current block size, plus a part
	// measured once with its amp playing and remembered for every block size, or a built-in guess before that) plus what
	// the output holds now (always live). So the game tones' figure shows from the start, stays right while the Tone
	// designer plays (the game's amp is silenced then, nothing to time) and after a device, mode or block switch.
	bool Available();
	bool Guessed();    // the estimate still rests on the built-in guess: never measured on this PC
	// What the game itself adds at a game block of `blockMs` (its two blocks plus the remembered fixed part), for a figure
	// built from somewhere else's numbers: ASIO mode, where nothing is timed and the driver reports the rest.
	float GameOwnMs(float blockMs);
	bool GameOwnGuessed();   // that fixed part is still the built-in guess (never measured on the cable on this PC)
	float CurrentMs();
	// LatencyLoopback's measured total (cable sample to speaker, ms), from any thread: taken as 7 agreeing picks on the
	// overlay's next comparison, and its fixed part (beyond the output part and the two blocks) is remembered.
	void NoteMeasuredMs(float totalMs);
	UINT64 Now100ns();
}
