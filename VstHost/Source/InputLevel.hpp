#pragma once

// How hard the player's guitar arrives, for the Tone designer's automatic input calibration. The stream thread feeds
// every block of the raw guitar (SourceRouter::Process, before any trim); each 50 ms window's peak goes into a
// histogram. The main thread asks for the level the player's hardest playing reaches (Measure):
//   - the background (hum, hiss, a guitar left plugged in) is the 20th percentile of all windows; a window counts as
//     playing only when it stands 12 dB over it and over -45 dBFS, so a humming cable is never taken for playing;
//   - the level is the 97th percentile of the playing windows: the hard notes, not a single stray spike.
// Tried on the owner's dry recordings (the game's Recordings folder): every take with 15 s or more of playing gave
// the same level within 1 dB (a trim of -7.5 to -8.5 dB), takes of hum alone gave none, and the fake guitar's note
// bank, normalised to about -11 dBFS, gave -0.5 dB.
//
// Older windows fade: once the histogram holds 5 minutes, every count is halved, so a new guitar, new strings or a
// changed pickup volume are followed. Threads: Feed on the stream thread only, the rest on the main thread; the
// histogram is atomic counters, so a count landing during a halving is at worst off by one.

#include <array>
#include <atomic>
#include <cstdint>

namespace VstHost
{
	class InputLevel
	{
	public:
		static constexpr float TARGET_PEAK_DB = -12.0f;   // where the starter tones expect the hardest playing
		static constexpr float FLOOR_DB = -45.0f;
		static constexpr float OVER_NOISE_DB = 12.0f;
		static constexpr float PERCENTILE = 0.97f;
		static constexpr int WINDOW_FRAMES = 2400;        // 50 ms at 48 kHz

		struct Reading
		{
			float playingSeconds = 0.0f;   // how much playing the level is from
			float levelDb = -120.0f;       // the hardest playing's window peak, dBFS
			float noiseDb = -120.0f;       // the background
		};

		// Stream thread: one block of the raw guitar.
		void Feed(const float* guitar, int frames);
		// Main thread.
		Reading Measure();
		void Reset();

	private:
		static constexpr float LOWEST_DB = -90.0f;
		static constexpr float BIN_DB = 0.5f;
		static constexpr int BINS = 180;                  // -90 to 0 dBFS in half dB
		static constexpr uint32_t FADE_WINDOWS = 6000;    // 5 minutes of windows

		std::array<std::atomic<uint32_t>, BINS> histogram{};
		// The window being filled: stream thread only.
		float windowPeak = 0.0f;
		int windowFrames = 0;
	};
}
