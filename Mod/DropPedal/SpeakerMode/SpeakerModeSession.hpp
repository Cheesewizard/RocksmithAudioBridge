#pragma once

#include "SpeakerModeController.hpp"
#include "SpeakerTargetSync.hpp"

namespace AudioBridge::SpeakerMode
{
	// The framework mod's Speaker Mode owner for the Wwise music hook: state in DropPedalState, the chart sync here.
	class SpeakerModeSession final : public Audio::SongShift::ISpeakerModeController
	{
	public:
		bool IsEnabled() const override;
		int GetShiftSemitones() const override;
		void Disable() override;
		bool TrySynchronizeTarget(const std::string& songKey) override;

		// MainThread: a new song (or none) starts the sync over.
		void ResetSong();

		// The synced chart's shape, for the readout's names (4.x's GetPitchRouteName with a synced chart). False until
		// the chart is synced for the current song. Any thread.
		struct Chart
		{
			ChartTuningShape shape = ChartTuningShape::Uniform;
			int offsets[6] = {};
			int reference = 0;
		};
		static bool TryGetSyncedChart(Chart& chart);

	private:
		void PublishChart();

		SpeakerTargetSync targetSync;
	};
}
