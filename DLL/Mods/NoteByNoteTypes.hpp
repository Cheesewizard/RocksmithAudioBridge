#pragma once

#include <cstddef>
#include <cstdint>
#include "../Audio/RawAttackEvidence.hpp"
#include "DetectionFeedback.hpp"

// Note by Note domain types: the plain data structures the scoring engine, the host, and the
// overlay all exchange. These are shippable and carry no dependency on the research bridge or
// the reloadable-probe ABI; that ABI (HostApi / ProbeApi) lives in Research/ResearchProtocol.hpp,
// which includes this header. The namespace remains ResearchProtocol for existing call sites.
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
		// The chart's bend amount in rounded semitones (record+0x40): 0 = not a bend,
		// >0 = bend by that many semitones, -1 = bend flagged but amount unreadable. The
		// target cue displays it because a frozen hold repaints the bend visual as a plain note.
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

	// One note-head draw seen at the host's DrawIndexedPrimitive hook. The load-bearing field is
	// nativeCaller: _ReturnAddress() read inside the D3D hook, the native function that issued the
	// draw. A plain POD so the host can fill it from render-thread locals with no shared allocator.
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

		// The IDirect3DDevice9 issuing the draw, so read-only device methods can be called
		// synchronously on the render thread inside the native draw call.
		uintptr_t devicePointer = 0;

		// Host-maintained shadow of vertex-shader constants c0..c7 (a float[8][4]); the fallback
		// matrix source when the device rejects GetVertexShaderConstantF.
		uintptr_t shaderConstantShadow = 0;

		// Which device draw entry issued this observation: 0 = DrawIndexedPrimitive,
		// 1 = DrawPrimitive, 2 = DrawPrimitiveUP, 3 = DrawIndexedPrimitiveUP.
		uint32_t drawSite = 0;

		// The user-pointer vertex data for the UP draw sites; zero for the stream sites. Valid
		// only for the duration of the observation call.
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

		// The target's legato gesture: the contiguous run of hammer-on / pull-off notes
		// immediately following visualRecord on the same string. Never targets in their own
		// right, but played as part of the target's gesture, so they stay visible.
		static constexpr uint32_t MaxVisualGroup = 8;
		uint32_t visualGroupCount = 0;
		uintptr_t visualGroupRecords[MaxVisualGroup] = {};

		// The same gesture as string/fret pairs, for the neck diagram, which gates on
		// coordinates rather than a note pointer. Parallel to visualGroupRecords.
		int32_t visualGroupStrings[MaxVisualGroup] = {};
		int32_t visualGroupFrets[MaxVisualGroup] = {};

		// Bend visualizer: the sounding pitch as a tuner needle against the bend target.
		// soundingMidi is fractional when a continuous tracker is live and the integer ungated
		// detector pitch otherwise; -1 when nothing credible is sounding. Bend fields are -1
		// outside a bend gesture.
		int32_t bendBaseMidi = -1;
		int32_t bendTargetMidi = -1;
		float soundingMidi = -1.0f;
		float soundingQuality = 0.0f;

		// Input-health feed: the latest detector gate sample, drawn under the overlay's status
		// text so a dead capture session is visible. A live chain moves with every strum.
		float detectorLevelDb = -120.0f;
		float detectorQuality = 0.0f;
		int32_t detectorLoudestMidi = -1;
		uint8_t detectorSampleValid = 0;

		// The current chord target's tones, in the player's physical MIDI frame. count 0 when
		// the target is a single note or the tones are not yet resolved.
		static constexpr uint32_t MaxChordTones = 6;
		uint32_t expectedChordToneCount = 0;
		int32_t expectedChordTones[MaxChordTones] = {};

		// The current single-note target's bend, if any: isBendTarget is 1 when the target is a
		// bend and bendAcceptMidi is the pitch the bend must reach.
		uint8_t isBendTarget = 0;
		int32_t bendAcceptMidi = -1;

		// Per-technique detection authority (0 = native decides, 1 = ML decides) and a live
		// native-vs-ML agreement snapshot the host overlay draws as a color-coded strip.
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
		// Ring of the most recent samples, oldest at [0]: 0 = disagree, 1 = agree, 2 = one-sided.
		uint8_t compareHistory[CompareHistoryLength] = {};
		NoteByNote::DetectionFeedback detectionFeedback;
		uint8_t detectorPassesLevel = 0;

		// Veto readout for the HUD (appended last; host and probe are built and deployed together).
		// holdRefusal: DescribeDetectorRefusal of the latest gate sample ("none-visible" = the game's
		// hidden onset/confidence gate). holdPhase: DescribeGatePhase of the mod's transport state.
		char holdRefusal[24] = {};
		char holdPhase[32] = {};

		// Bend visualizer "reached" verdict (appended after the veto readout; structSize-gated
		// like every appended field): 1 when the probe's own bend evaluation has counted the
		// sounding pitch at the bend target this gesture - the same verdict that advances the
		// note - so the meter colors green from the engine's decision instead of re-deriving
		// "on target" from pitch alone. Green-and-no-progress then cannot happen by
		// construction (the meter's needle position still comes from the pitch feed).
		uint8_t bendReachedTarget = 0;
	};

	using ScoringUpdate = void(__stdcall*)(void* owner, float updateTime);
	using HitDecision = bool(__fastcall*)(void* owner, void* unusedEdx, void* note);
	using NoteHeadDraw = void(__fastcall*)(void* renderCtx, void* unusedEdx, int* noteArray);

	// Tier-0 raw-audio tone evidence: Goertzel powers measured on the raw Player 1 input route at
	// exact frequencies (cent resolution), upstream of the engine's semitone-quantized features.
	// Powers are normalized so a full-scale sine at the bin reads about 1.0 for any window length.
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
		// Highest confidence any string read the EXPECTED note, in either the shifted (expected)
		// or physical (expected - appliedShift) frame - independent of whether a louder OTHER
		// string made the reduced verdict Conflicting. -1 when the expected note is not present.
		// A ringing neighbour must not veto this; the direct ML-expected-note accept uses it.
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
		// Windows: 50, 100, 150 ms. Bins: -50 through +150 cents in 12.5-cent steps.
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

	// Bumped on every HostApi or shared-structure change. These structures cross the
	// host/controller boundary in lockstep, so a stale host or controller must fail
	// initialization instead of silently losing scoring or presentation state.
	constexpr uint32_t HOST_API_VERSION = 12;

	// The services the host provides to the scoring engine (pitch-shift frame, tier-0 raw tone
	// evidence, the ML companion, logging). The scoring engine reaches these through
	// ResearchProbeRuntime, which stores this table; the host fills it in shippable code. This
	// is the host contract, not debug plumbing, so it ships with the domain types.
	struct HostApi
	{
		uint32_t version = HOST_API_VERSION;
		uint32_t structSize = sizeof(HostApi);
		uint8_t(__cdecl* IsNoteByNoteEnabled)() = nullptr;
		void(__cdecl* HandleControllerFault)(const char* reason) = nullptr;
		void(__cdecl* PublishExpectedAttackEvent)(const ExpectedAttackEvent* event) = nullptr;
		void(__cdecl* Log)(LogLevel level, const char* message) = nullptr;
		// Semitones to ADD to a native onset (reported in the arrangement's authored tuning
		// frame) to bring it into the display/target tuning frame expectedMidi is computed in.
		// Non-zero only while DropPedal's input pitch shifter is retuning the input (Speaker
		// Mode with the ASIO shifter): the input is retuned to the authored tuning for detection
		// while the game's tuning table is shifted to the target, so the two frames diverge by
		// exactly this. 0 when no shift is active. See DropPedal.hpp SetInputShifterActive.
		int(__cdecl* GetInputOnsetShiftSemitones)() = nullptr;
		// Tier-0 query over the raw Player 1 route; returns 0 when the route has not observed
		// enough audio yet. The pointer is null-guarded on the scoring side.
		uint8_t(__cdecl* QueryRawToneEvidence)(
			double frequencyHz, float windowSeconds, RawToneEvidence* out) = nullptr;
		// Tier-1 ML pitch from the 64-bit companion service over the shared-memory audio export.
		// outMidi is a FLOAT midi in the observed (post-shifter) route frame; outAgeSeconds is
		// audio-clock age. Returns 0 when the mailbox was never written or could not be read.
		uint8_t(__cdecl* QueryMlPitch)(
			float* outMidi, float* outConfidence, double* outAgeSeconds) = nullptr;
		// True when the companion updated the mailbox within the last 1.0 s of audio.
		uint8_t(__cdecl* IsMlPitchServiceAlive)() = nullptr;
		// Source-audio timestamps keep a previous target's model frame out of a new acceptance
		// decision. Host and scoring require this protocol together.
		uint64_t(__cdecl* GetMlAudioSampleIndex)() = nullptr;
		uint8_t(__cdecl* QueryMlNoteEvidence)(int expectedMidi, float minConfidence,
			uint64_t minimumSampleIndex, MlNoteEvidence* evidence) = nullptr;
		uint8_t(__cdecl* QueryMlChordEvidence)(const int32_t* expectedMidiByString,
			float minConfidence, uint64_t minimumSampleIndex, MlChordEvidence* evidence) = nullptr;
		uint8_t(__cdecl* QueryRawToneComb)(double frequencyHz, RawToneComb* out) = nullptr;
		uint8_t(__cdecl* QueryRawNoteConfirmation)(double frequencyHz, uint64_t minimumSampleIndex, uint64_t maximumSampleIndex, RawNoteConfirmation* out) = nullptr;
		uint8_t(__cdecl* QueryRawAttacks)(uint64_t afterSampleIndex, RawPitchVerifier::RawAttackBatch* out) = nullptr;
		// v9: the raw route snapshot behind chord, dyad, unison and fret-hand-mute attack
		// confirmation. The reloadable probe has no audio tap of its own; its
		// RawPitchVerifier::CaptureSnapshot stub must reach the host ring through here, or every
		// chord/dyad/mute attack in a loaded probe reads as sustain and never confirms.
		uint8_t(__cdecl* CaptureRawSnapshot)(RawPitchVerifier::AudioSnapshot* out) = nullptr;
		// v10: the song speed in percent (100 = normal), read and written through the same
		// Time_Stretch RTPC the song speed hotkey uses. Flow caps the speed to what detection can
		// keep up with in the current section.
		float(__cdecl* GetSongSpeedPercent)() = nullptr;
		void(__cdecl* SetSongSpeedPercent)(float percent) = nullptr;
		// v11: the FLOW MODE switch as the host holds it. The Riff Repeater menu row and the overlay
		// toggle write the host's own copy, which a reloaded controller never sees, so the controller
		// follows this every tick.
		uint8_t(__cdecl* IsFlowModeEnabled)() = nullptr;
		// v12: the real song speed (percent) the player's Riff Repeater SPEED slider asks for, last
		// read on the Settings screen; -1 when unknown. The slider -> real curve is the game's
		// (0x409400) or RSMods' Linear Riff Repeater, so the controller does not have to guess it.
		float(__cdecl* GetPlayerSpeedRealPercent)() = nullptr;
		// v12: a queued arrow-key note navigation (+1 next, -1 previous, 0 none), cleared by the read.
		int32_t(__cdecl* ConsumeNoteNavigation)() = nullptr;
	};
}
