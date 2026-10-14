#pragma once

#include <windows.h>
#include <atomic>

// How much of the game's sound a Cable-mode exclusive or ASIO output keeps in hand.
//
// While Rocksmith boots and loads its menus its audio thread stalls for tens of ms at a time, for about a minute past
// MainMenu (FRAME PACING 21-22 fps there). The lean output queue (a game packet, a buffer and 1 ms, plus the adaptive
// cushion of at most 10 ms) ran dry at each stall, and when the game then paid back its owed wakes in a burst the
// overfull queue was cut: two clicks per stall, ~250-600 run-dries and ~10 cuts per boot. Then never again.
//
// So the output keeps a reserve of ~50 ms while nobody needs the game's sound fast, and drops to the lean queue (one
// crossfaded cut) when someone does:
//   - songs, tuners, calibration, Riff Repeater, lessons: lean (the music has to sit with the notes);
//   - menus while the player plays through the game's own amp: lean, until ~10 s without playing;
//   - everything else (boot, menus, loading, score screens, menus while Tone designer plays the guitar): reserve.
// Tone designer's guitar is mixed in after the queue (CableAmpLink::Render), so the reserve never delays it.
//
// Phase() is decided by the ExternalAmp thread (it reads the menu and knows whether the game's amp is muted); until
// that thread starts at MainMenu, the reserve applies. The input side (NoteInput) is fed from the cable capture.
namespace Audio::OutputReserve
{
	constexpr UINT32 kFrames = 2400;               // 50 ms at 48 kHz
	constexpr float kPlayingPeak = 0.0178f;        // -35 dBFS on the raw cable: a pick, not hum or hiss (idle peaks ~-60)
	constexpr ULONGLONG kPlayingHoldMs = 10000;    // the player counts as playing for this long after the last pick

	namespace Detail
	{
		inline std::atomic<bool> wanted{ true };
		inline std::atomic<ULONGLONG> lastPick{ 0 };
	}

	inline void SetWanted(bool value) { Detail::wanted.store(value, std::memory_order_relaxed); }
	inline bool Wanted() { return Detail::wanted.load(std::memory_order_relaxed); }

	// Player 1's raw cable input, a packet at a time.
	inline void NoteInput(const float* mono, UINT32 frames)
	{
		float peak = 0.0f;
		for (UINT32 i = 0; i < frames; ++i) { const float s = mono[i] < 0.0f ? -mono[i] : mono[i]; if (s > peak) peak = s; }
		if (peak > kPlayingPeak) Detail::lastPick.store(GetTickCount64(), std::memory_order_relaxed);
	}

	inline bool Playing()
	{
		const ULONGLONG at = Detail::lastPick.load(std::memory_order_relaxed);
		return at && GetTickCount64() - at < kPlayingHoldMs;
	}
}
