#include "SpeakerModeSession.hpp"

#include "../DropPedalState.hpp"

#include <mutex>

namespace AudioBridge::SpeakerMode
{
	namespace
	{
		std::mutex chartMutex;
		bool chartSynced = false;
		SpeakerModeSession::Chart syncedChart;
	}

	bool SpeakerModeSession::IsEnabled() const
	{
		return DropPedalState::IsConfiguredEnabled() && DropPedalState::IsSpeakerModeEnabled();
	}

	// Speaker Mode shifts the one song mix; its route rides on Player 1's state.
	int SpeakerModeSession::GetShiftSemitones() const
	{
		return DropPedalState::GetTargetSemitones(DropPedal::Player::One);
	}

	void SpeakerModeSession::Disable()
	{
		DisableSpeakerMode();
	}

	bool SpeakerModeSession::TrySynchronizeTarget(const std::string& songKey)
	{
		const bool synced = targetSync.TrySynchronize(songKey);
		PublishChart();
		return synced;
	}

	void SpeakerModeSession::ResetSong()
	{
		targetSync.Reset();
		PublishChart();
	}

	void SpeakerModeSession::PublishChart()
	{
		std::lock_guard<std::mutex> lock(chartMutex);
		chartSynced = targetSync.HasSynchronized();
		if (!chartSynced) return;
		syncedChart.shape = targetSync.Shape();
		for (int string = 0; string < 6; string++) syncedChart.offsets[string] = targetSync.ChartOffsets()[string];
		syncedChart.reference = targetSync.ChartReference();
	}

	bool SpeakerModeSession::TryGetSyncedChart(Chart& chart)
	{
		std::lock_guard<std::mutex> lock(chartMutex);
		if (!chartSynced) return false;
		chart = syncedChart;
		return true;
	}
}
