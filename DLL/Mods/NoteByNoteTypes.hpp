#pragma once

#include <cstddef>
#include <cstdint>
#include "../Audio/RawAttackEvidence.hpp"
#include "DetectionFeedback.hpp"
namespace ResearchProtocol
{
	enum class LogLevel : uint32_t
	{
		Debug,
		Info,
		Warning,
		Error
	};

	enum class ExpectedAttackEventKind : uint32_t
	{
		CandidateChanged,
		HoldEstablished,
		ScoringStateChanged
	};

	enum class GatePhase : uint32_t
	{
		Idle,
		Armed,
		WaitingForInputRelease,
		Holding,
		CommitBeforeRelease,
		DenseRebuildPending,
		DenseRecommitAfterRebuild,
		DensePlayerSongStartPending,
		PostRelease,
		RecommitAfterRelease
	};

	struct ScoringNote
	{
		uintptr_t nativeNoteAddress = 0;
		uintptr_t recordAddress = 0;
		uint32_t noteMask = 0;
		uint32_t noteFlags = 0;
		uint32_t noteHash = 0;
		float authoredTime = 0.0f;
		float nativeEventTime = 0.0f;
		int32_t stringIndex = -1;
		int32_t fret = -1;
		int32_t chordId = -1;
		int32_t chordNotesId = -1;
		int32_t phraseIterationId = -1;
		float rangeA4 = 0.0f;
		float rangeA8 = 0.0f;
		uint8_t stateC0 = 0;
		uint8_t stateC1 = 0;
		uint8_t stateC2 = 0;
		uint8_t stateC3 = 0;
		int32_t bendSemitones = 0;
	};

	struct ExpectedAttackEvent
	{
		ExpectedAttackEventKind kind = ExpectedAttackEventKind::CandidateChanged;
		uintptr_t ownerAddress = 0;
		uint64_t epoch = 0;
		float updateTime = 0.0f;
		uint8_t isAfterUpdate = 0;
		uint8_t isNotePresent = 1;
		ScoringNote note;
	};

	struct RenderedNote
	{
		int32_t stringIndex = -1;
		int32_t fret = -1;
	};

	struct RenderedAttack
	{
		uint8_t isTransition = 0;
		uint64_t renderFrame = 0;
		float songTime = 0.0f;
		float longitudinalPosition = 0.0f;
		const RenderedNote* notes = nullptr;
		size_t noteCount = 0;
	};
	struct NativeDrawObservation
	{
		uint64_t renderFrame = 0;
		float songTime = 0.0f;
		float greyNoteCutoff = 0.0f;
		uint32_t primitiveType = 0;
		int32_t baseVertexIndex = 0;
		uint32_t minimumVertexIndex = 0;
		uint32_t vertexCount = 0;
		uint32_t startIndex = 0;
		uint32_t primitiveCount = 0;
		uint32_t stride = 0;
		uint32_t startRegister = 0;
		uint32_t vectorCount = 0;
		uint32_t declarationType = 0;
		uint32_t declarationElementCount = 0;
		uintptr_t streamIdentity = 0;
		uintptr_t nativeCaller = 0;
		uintptr_t devicePointer = 0;
		uintptr_t shaderConstantShadow = 0;
		uint32_t drawSite = 0;
		uintptr_t userPointerData = 0;
	};

	struct NoteByNoteState
	{
		uint32_t structSize = sizeof(NoteByNoteState);
		uint8_t isInitialized = 0;
		uint8_t isEpochConfirmed = 0;
		uint8_t ownsNativeHold = 0;
		GatePhase gatePhase = GatePhase::Idle;
		uintptr_t trackedOwner = 0;
		uintptr_t selectedRecord = 0;
		uint64_t epoch = 0;
		uint64_t holdTickCount = 0;
		float lastUpdateTime = 0.0f;
		float selectedRecordTime = 0.0f;
		float selectedHoldTime = 0.0f;
		float heldEpoch = 0.0f;
		int32_t selectedString = -1;
		int32_t selectedFret = -1;
		int32_t selectedChordId = -1;
		int32_t selectedChordNotesId = -1;
		int32_t expectedMidi = -1;
		uintptr_t visualRecord = 0;
		int32_t visualString = -1;
		int32_t visualFret = -1;
		int32_t visualChordId = -1;
		int32_t visualChordNotesId = -1;
		static constexpr uint32_t MaxVisualGroup = 8;
		uint32_t visualGroupCount = 0;
		uintptr_t visualGroupRecords[MaxVisualGroup] = {};
		int32_t visualGroupStrings[MaxVisualGroup] = {};
		int32_t visualGroupFrets[MaxVisualGroup] = {};
		int32_t bendBaseMidi = -1;
		int32_t bendTargetMidi = -1;
		float soundingMidi = -1.0f;
		float soundingQuality = 0.0f;
		float detectorLevelDb = -120.0f;
		float detectorQuality = 0.0f;
		int32_t detectorLoudestMidi = -1;
		uint8_t detectorSampleValid = 0;
		static constexpr uint32_t MaxChordTones = 6;
		uint32_t expectedChordToneCount = 0;
		int32_t expectedChordTones[MaxChordTones] = {};
		uint8_t isBendTarget = 0;
		int32_t bendAcceptMidi = -1;
		uint8_t chordAuthorityIsMl = 0;
		uint8_t noteAuthorityIsMl = 0;
		uint8_t bendAuthorityIsMl = 0;
		uint32_t compareAgreeCount = 0;
		uint32_t compareDisagreeCount = 0;
		int32_t compareLastTechnique = -1;    // 0 single, 1 chord, 2 bend, -1 none yet
		uint8_t compareLastNativeMatch = 0;
		uint8_t compareLastMlMatch = 0;
		uint8_t compareLastMlHadOpinion = 0;
		uint8_t compareLastValid = 0;
		static constexpr uint32_t CompareHistoryLength = 24;
		uint32_t compareHistoryCount = 0;
		uint8_t compareHistory[CompareHistoryLength] = {};
		NoteByNote::DetectionFeedback detectionFeedback;
		uint8_t detectorPassesLevel = 0;
		char holdRefusal[24] = {};
		char holdPhase[32] = {};
		uint8_t bendReachedTarget = 0;
	};

	using ScoringUpdate = void(__stdcall*)(void* owner, float updateTime);
	using HitDecision = bool(__fastcall*)(void* owner, void* unusedEdx, void* note);
	using NoteHeadDraw = void(__fastcall*)(void* renderCtx, void* unusedEdx, int* noteArray);
	struct RawToneEvidence
	{
		uint32_t structSize = sizeof(RawToneEvidence);
		uint32_t sampleRate = 0;
		uint32_t windowSampleCount = 0;
		float totalRms = 0.0f;
		float targetPower = 0.0f;
		float minusOnePower = 0.0f;   // one semitone below the target
		float plusOnePower = 0.0f;    // one semitone above
		float minusTwoPower = 0.0f;
		float plusTwoPower = 0.0f;
	};

	enum class MlNoteVerdict : uint32_t
	{
		Unavailable,
		Pending,
		Unknown,
		Confirmed,
		Conflicting
	};

	struct MlNoteEvidence
	{
		uint64_t analyzedSampleIndex = 0;
		uint32_t sampleRate = 0;
		MlNoteVerdict verdict = MlNoteVerdict::Unavailable;
		int32_t observedMidi = -1;
		float confidence = 0.0f;
		double ageSeconds = 0.0;
		float targetConfidence = -1.0f;
	};

	struct MlChordEvidence
	{
		uint64_t analyzedSampleIndex = 0;
		uint32_t sampleRate = 0;
		MlNoteVerdict verdict = MlNoteVerdict::Unavailable;
		uint8_t requiredStringMask = 0;
		uint8_t matchedStringMask = 0;
		double ageSeconds = 0.0;
	};

	struct RawToneComb
	{
		uint64_t endSampleIndex = 0;
		uint32_t sampleRate = 0;
		uint32_t sampleCounts[3] = {};
		float rms[3] = {};
		float powers[3][17] = {};
	};

	struct RawNoteConfirmation
	{
		uint64_t endSampleIndex = 0;
		uint32_t sampleRate = 0;
		float targetPower = 0.0f;
		float attackChange = 0.0f;
		float attackPower = 0.0f;
		float attackMinusPower = 0.0f;
		float attackPlusPower = 0.0f;
		float neighbourPower = 0.0f;
		uint32_t confirmed = 0;
	};
	constexpr uint32_t HOST_API_VERSION = 12;
	struct HostApi
	{
		uint32_t version = HOST_API_VERSION;
		uint32_t structSize = sizeof(HostApi);
		uint8_t(__cdecl* IsNoteByNoteEnabled)() = nullptr;
		void(__cdecl* HandleControllerFault)(const char* reason) = nullptr;
		void(__cdecl* PublishExpectedAttackEvent)(const ExpectedAttackEvent* event) = nullptr;
		void(__cdecl* Log)(LogLevel level, const char* message) = nullptr;
		int(__cdecl* GetInputOnsetShiftSemitones)() = nullptr;
		uint8_t(__cdecl* QueryRawToneEvidence)(
			double frequencyHz, float windowSeconds, RawToneEvidence* out) = nullptr;
		uint8_t(__cdecl* QueryMlPitch)(
			float* outMidi, float* outConfidence, double* outAgeSeconds) = nullptr;
		uint8_t(__cdecl* IsMlPitchServiceAlive)() = nullptr;
		uint64_t(__cdecl* GetMlAudioSampleIndex)() = nullptr;
		uint8_t(__cdecl* QueryMlNoteEvidence)(int expectedMidi, float minConfidence,
			uint64_t minimumSampleIndex, MlNoteEvidence* evidence) = nullptr;
		uint8_t(__cdecl* QueryMlChordEvidence)(const int32_t* expectedMidiByString,
			float minConfidence, uint64_t minimumSampleIndex, MlChordEvidence* evidence) = nullptr;
		uint8_t(__cdecl* QueryRawToneComb)(double frequencyHz, RawToneComb* out) = nullptr;
		uint8_t(__cdecl* QueryRawNoteConfirmation)(double frequencyHz, uint64_t minimumSampleIndex, uint64_t maximumSampleIndex, RawNoteConfirmation* out) = nullptr;
		uint8_t(__cdecl* QueryRawAttacks)(uint64_t afterSampleIndex, RawPitchVerifier::RawAttackBatch* out) = nullptr;
		uint8_t(__cdecl* CaptureRawSnapshot)(RawPitchVerifier::AudioSnapshot* out) = nullptr;
		float(__cdecl* GetSongSpeedPercent)() = nullptr;
		void(__cdecl* SetSongSpeedPercent)(float percent) = nullptr;
		uint8_t(__cdecl* IsFlowModeEnabled)() = nullptr;
		float(__cdecl* GetPlayerSpeedRealPercent)() = nullptr;
		int32_t(__cdecl* ConsumeNoteNavigation)() = nullptr;
	};
}
