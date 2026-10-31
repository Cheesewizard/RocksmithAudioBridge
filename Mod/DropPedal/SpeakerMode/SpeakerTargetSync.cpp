#include "stdafx.h"
#include "SpeakerTargetSync.hpp"

#include "GameState.hpp"

#include "../DropPedalHooks.hpp"
#include "../DropPedalState.hpp"
#include "../GameTuning.hpp"

namespace AudioBridge::SpeakerMode
{
	namespace
	{
		constexpr int MIN_CHART_TUNING_SEMITONES = -24;
		constexpr int MAX_CHART_TUNING_SEMITONES = 24;
		constexpr int SEMITONES_PER_OCTAVE = 12;
		constexpr unsigned long long READ_INTERVAL_MS = 250;

		int GetClosestOctaveAdjustment(int selectedShiftSemitones, int chartShiftSemitones)
		{
			const int difference = selectedShiftSemitones - chartShiftSemitones;
			const int roundingAdjustment = difference >= 0
				? SEMITONES_PER_OCTAVE / 2
				: -(SEMITONES_PER_OCTAVE / 2);
			return ((difference + roundingAdjustment) / SEMITONES_PER_OCTAVE) * SEMITONES_PER_OCTAVE;
		}

		// The tuner's tuning is authoritative while the pre-song tuner is up. Once it's gone (a guitar already in tune
		// passes it in under a second), the loaded arrangement's stands in, so a quick tuner can't starve the sync.
		bool TryReadRawChartTuning(int (&stringTunings)[6])
		{
			return GameState::Menus::IsInPreSongTuner()
				? GameTuning::TryGetTunerTuning(stringTunings)
				: GameTuning::TryGetArrangementTuning(stringTunings);
		}

		bool IsChartTuningInRange(const int (&stringTunings)[6])
		{
			for (const int stringTuning : stringTunings)
			{
				if (stringTuning < MIN_CHART_TUNING_SEMITONES || stringTuning > MAX_CHART_TUNING_SEMITONES) return false;
			}
			return true;
		}
	}

	bool TryReadChartTuning(int (&stringTunings)[6])
	{
		return TryReadRawChartTuning(stringTunings) && IsChartTuningInRange(stringTunings);
	}

	int GetChartReference(const int (&stringTunings)[6])
	{
		const int candidateOrder[6] = { 1, 2, 3, 4, 5, 0 };
		int reference = stringTunings[1];
		int referenceCount = 0;
		for (const int candidate : candidateOrder)
		{
			int count = 0;
			for (const int stringTuning : stringTunings)
			{
				if (stringTuning == stringTunings[candidate]) count++;
			}
			if (count > referenceCount)
			{
				referenceCount = count;
				reference = stringTunings[candidate];
			}
		}
		return reference;
	}

	ChartTuningShape ClassifyChartTuning(const int (&stringTunings)[6], int reference)
	{
		for (int string = 1; string < 6; string++)
		{
			if (stringTunings[string] != reference) return ChartTuningShape::Custom;
		}

		if (stringTunings[0] == reference) return ChartTuningShape::Uniform;
		if (stringTunings[0] == reference - 2) return ChartTuningShape::Drop;
		return ChartTuningShape::Custom;
	}

	void DisableSpeakerMode()
	{
		if (!DropPedalState::DisableSpeakerMode()) return;

		// Runs on Wwise decoder threads as well as MainThread, so only queue the restore here: taking the true-tuning
		// lock or logging would stall audio decode. The in-song arrangement pass reapplies the reference within a frame.
		DropPedalHooks::QueuePitchRestore();
	}

	bool SpeakerTargetSync::TrySynchronize(const std::string& requestedSongKey)
	{
		if (requestedSongKey.empty() || !DropPedalState::IsSpeakerModeEnabled()) return false;

		bool didSynchronize = false;
		const auto currentModeTick = DropPedalState::GetModeNoticeTick();
		if (songKey != requestedSongKey || modeTick != currentModeTick)
		{
			songKey = requestedSongKey;
			modeTick = currentModeTick;
			hasSynchronized = false;
			hasPending = false;
			DropPedalState::SetSpeakerTargetSynchronized(false);
			readAfterTick = 0;
		}

		if (!hasSynchronized)
		{
			// The tuner populates its tuning data while the screen builds, so no single reading is trusted; the shape
			// is accepted once two consecutive reads agree. Reads start immediately: a fixed initial delay loses the
			// race against a guitar that is already in tune and passes the tuner in under a second.
			const auto now = GetTickCount64();
			if (now < readAfterTick) return false;
			readAfterTick = now + READ_INTERVAL_MS;

			// A failed read keeps the pending reading; one out of range (the tuner still populating) drops it.
			int stringTunings[6];
			if (!TryReadRawChartTuning(stringTunings)) return false;
			if (!IsChartTuningInRange(stringTunings))
			{
				hasPending = false;
				return false;
			}

			bool confirmsPending = hasPending;
			for (int string = 0; string < 6; string++)
			{
				confirmsPending = confirmsPending && pendingOffsets[string] == stringTunings[string];
			}

			if (!confirmsPending)
			{
				hasPending = true;
				for (int string = 0; string < 6; string++)
				{
					pendingOffsets[string] = stringTunings[string];
				}
				return false;
			}

			chartReference = GetChartReference(stringTunings);
			shape = ClassifyChartTuning(stringTunings, chartReference);
			for (int string = 0; string < 6; string++)
			{
				chartOffsets[string] = stringTunings[string];
			}
			DropPedalState::SetSpeakerTargetSynchronized(true);
			const int chartShiftSemitones = chartReference - DropPedalState::GetBaseTuningSemitones(DropPedal::Player::One);
			octaveAdjustment = GetClosestOctaveAdjustment(
				DropPedalState::GetTargetSemitones(DropPedal::Player::One), chartShiftSemitones);
			hasSynchronized = true;
			didSynchronize = true;
		}

		const int shiftSemitones = chartReference - DropPedalState::GetBaseTuningSemitones(DropPedal::Player::One)
			+ octaveAdjustment;
		if (!DropPedalState::SetTargetSemitones(DropPedal::Player::One, shiftSemitones))
		{
			LOG_ERROR("Speaker Mode chart tuning is outside the supported pitch range for " << songKey << std::endl);
			DisableSpeakerMode();
			return false;
		}
		if (didSynchronize)
		{
			LOG_INFO("Speaker Mode chart target synchronized (chart " << chartReference
				<< (shape == ChartTuningShape::Drop ? ", drop shape" : "")
				<< (shape == ChartTuningShape::Custom ? ", custom shape" : "")
				<< ", octave adjustment " << octaveAdjustment << ") for " << songKey << std::endl);
		}
		return true;
	}

	void SpeakerTargetSync::Reset()
	{
		DropPedalState::SetSpeakerTargetSynchronized(false);
		songKey.clear();
		hasSynchronized = false;
		hasPending = false;
		shape = ChartTuningShape::Uniform;
		chartReference = 0;
		octaveAdjustment = 0;
		readAfterTick = 0;
		modeTick = 0;
	}
}
