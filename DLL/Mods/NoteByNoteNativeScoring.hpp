#pragma once

#include "NoteByNoteProbe.hpp"
#include "../Research/ResearchProtocol.hpp"

namespace NoteByNoteNativeScoring
{
	void Initialize();
	bool IsAvailable();
	void SetRequestedGenericHooks(const ResearchProtocol::HookRequest* requests, uint32_t count);
	ResearchProtocol::NoteByNoteState GetResearchState();
	void ObserveRenderedAttack(const NoteByNoteProbe::NativeRenderedAttack& attack);
	bool ProcessHitDecision(
		void* owner,
		void* unusedEdx,
		void* note,
		ResearchProtocol::HitDecision original);
	void ProcessScoringUpdate(
		void* owner,
		float updateTime,
		ResearchProtocol::ScoringUpdate original);
	void Shutdown();
	void Stop();
	void RequestReArm();
	bool TryGetResearchState(ResearchProtocol::NoteByNoteState& state);
	void SetChordHoldsEnabled(bool enabled);
	void SetFreezePromptSoundEnabled(bool enabled);
	void SetFlowUntilMissEnabled(bool enabled);
	bool GetFlowUntilMissEnabled();
	void SetFlowLateGraceSeconds(float seconds);
	float GetFlowLateGraceSeconds();

	bool GetChordHoldsEnabled();
	void SetVerboseTrace(bool enabled);
	bool GetVerboseTrace();
	enum class DetectionStrategy { Blend = 0, NativeOnly = 1, MlOnly = 2 };
	void SetChordDetectionStrategy(DetectionStrategy strategy);
	void SetNoteDetectionStrategy(DetectionStrategy strategy);
	void SetBendDetectionStrategy(DetectionStrategy strategy);
	void SetChordWindowSlideEnabled(bool enabled);
	bool GetChordWindowSlideEnabled();
	void SetChordTier0RescueEnabled(bool enabled);   // per-tone energy chord rescue (default off)
	bool GetChordTier0RescueEnabled();
	void SetRepeatStrumHoldsEnabled(bool enabled);
	bool GetRepeatStrumHoldsEnabled();
	void SetNativeFreezeFlagEnabled(bool enabled);
	bool GetNativeFreezeFlagEnabled();
	void SetNativeChordPanelEnabled(bool enabled);
	bool GetNativeChordPanelEnabled();
	void SetSafetyReleaseEnabled(bool enabled);
	void SetScheduleShiftEnabled(bool enabled);
	void SetNativeReleaseEnabled(bool enabled);
	void SetNdAcceptEnabled(bool enabled);
	bool TryDescribeSelectedChordTarget(uintptr_t record, char* buffer, size_t bufferLength);
	bool TryDescribeChordTarget(uintptr_t noteAddress, char* buffer, size_t bufferLength);
	bool TryDescribeChordLabel(uintptr_t noteAddress, const int* tones, int toneCount,
		char* buffer, size_t bufferLength);
	bool TryDescribeSelectedChordFingering(
		uintptr_t record,
		char* buffer,
		size_t bufferLength,
		int& lowestPlayedString);
	bool IsBassArrangementActive();
}
