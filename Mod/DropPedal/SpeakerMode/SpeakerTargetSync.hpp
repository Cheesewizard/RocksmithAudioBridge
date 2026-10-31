#pragma once

#include <string>

// Speaker Mode's shift follows the song: the selected chart's tuning sets how far the music moves. Shared by the 4.x
// DLL and the framework mod; the game reads go through GameTuning.hpp.
namespace AudioBridge::SpeakerMode
{
	// Any chart shape is supported: the player physically tunes each string to chart + shift -- the pre-song tuner
	// guides that, because its per-string targets carry the shift -- and the uniform song shift plus the global
	// detection transpose then match every string by the same interval. A drop chart needs one string retuned, an open
	// tuning a few more; that trade is the player's to make.
	enum class ChartTuningShape
	{
		Uniform,
		Drop,
		Custom
	};

	// The selected chart's per-string offsets: the tuner's while the pre-song tuner is open, else the loaded
	// arrangement's. MainThread. False when nothing is readable, or a string is outside -24..24 (a failed read).
	bool TryReadChartTuning(int (&stringTunings)[6]);
	// The offset the most strings share, so the player's base maps onto it and the fewest strings need retuning. Ties
	// favour the upper strings, which keeps drop shapes anchored to the uniform five.
	int GetChartReference(const int (&stringTunings)[6]);
	ChartTuningShape ClassifyChartTuning(const int (&stringTunings)[6], int reference);

	// Turns Speaker Mode off and queues the detection reference's restore. Safe on Wwise's threads.
	void DisableSpeakerMode();

	// The per-song state of the chart sync. MainThread.
	class SpeakerTargetSync final
	{
	public:
		// Reads the chart (accepted once two consecutive reads, 250 ms apart, agree) and sets Player 1's shift so the
		// song lands on the player's base tuning, the octave chosen nearest the shift already dialled. True once synced
		// for songKey; false while still reading, and when the chart is out of range (Speaker Mode is then turned off).
		bool TrySynchronize(const std::string& songKey);
		void Reset();

		bool HasSynchronized() const { return hasSynchronized; }
		ChartTuningShape Shape() const { return shape; }
		const int (&ChartOffsets() const)[6] { return chartOffsets; }
		// The chart's reference offset from E.
		int ChartReference() const { return chartReference; }

	private:
		std::string songKey;
		bool hasSynchronized = false;
		ChartTuningShape shape = ChartTuningShape::Uniform;
		int chartOffsets[6] = {};
		int chartReference = 0;
		int octaveAdjustment = 0;
		int pendingOffsets[6] = {};
		bool hasPending = false;
		unsigned long long readAfterTick = 0;
		unsigned long long modeTick = 0;
	};
}
