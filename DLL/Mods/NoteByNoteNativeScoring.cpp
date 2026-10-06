#include "ResearchProbeRuntime.hpp"
#include "MlConfirmationState.hpp"
#include "PickedAttackQueue.hpp"
#include "PreHoldCommitGate.hpp"
#include "OctaveCollisionGate.hpp"
#include "NoteTechniqueClassification.hpp"
#include "ChordAttackGate.hpp"
#include "ChordAttackConfirmation.hpp"
#include "ChordPitchDecision.hpp"
#include "MutedAttackConfirmation.hpp"
#include "ChordMatchWindow.hpp"
#include "CloseChordConfirmation.hpp"
#include "StrumChordPresence.hpp"
#include "PickClick.hpp"
#include "StrumLedger.hpp"
#include "StrummedChordAttack.hpp"
#include "StrumHandoff.hpp"
#include "PowerChordConfirmation.hpp"
#include "UnisonChord.hpp"
#include "../Audio/RawPitchVerifier.hpp"
#include "HeldPitchConfirmation.hpp"
#include "PlayedNotePitch.hpp"
#include "ArrangementInstrument.hpp"
#include "FlowSpeedCap.hpp"
#include "NoteByNoteNativeScoring.hpp"
#include "NoteByNoteScoringCore.hpp"

#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>
namespace
{
	void AppendNbnTraceLine(const std::string& text)
	{
		(void)text;
	}
}

#undef LOG_INFO
#undef LOG_ERROR
#define LOG_INFO(msg) do { std::ostringstream _log_ss; _log_ss << msg; \
	ResearchProbeRuntime::Log(ResearchProtocol::LogLevel::Info, _log_ss.str()); \
	AppendNbnTraceLine(_log_ss.str()); } while(0)
#define LOG_ERROR(msg) do { std::ostringstream _log_ss; _log_ss << msg; \
	ResearchProbeRuntime::Log(ResearchProtocol::LogLevel::Error, _log_ss.str()); \
	AppendNbnTraceLine(_log_ss.str()); } while(0)

namespace
{
	constexpr uintptr_t LAS_OWNER_VTABLE = 0x11D1430;      // GamePlaysongLAS
	constexpr uintptr_t PLAYER_SONG_VTABLE = 0x119F668;    // GameComponentPlayerSong
	constexpr uintptr_t NATIVE_HIT_DECISION = 0x7E2640;    // owner vtable +0xF0
	constexpr uintptr_t STOP_TMUSIC = 0x44C130;            // PlayerSong vtable +0x24
	constexpr uintptr_t SET_FIVE_CLOCKS = 0x7DE490;        // owner vtable +0x50
	constexpr uintptr_t COORDINATED_REBUILD = 0x7E87F0;    // owner vtable +0x54
	constexpr uintptr_t MARK_ACTIVE_NOTES = 0x7E8920;      // owner in EDX, plain ret
	constexpr uintptr_t PLAY_SOUND_CSTRING = 0x7CE8D0;     // event name in ESI
	constexpr uintptr_t ENGINE_COMPENSATION = 0x1224A20;   // double, 0.053 engine constant
	constexpr uintptr_t LAS_SECTION_START = 0x3C0;
	constexpr uintptr_t LAS_SECTION_START_MIRROR = 0x3C4;
	constexpr uintptr_t LAS_SECTION_END = 0x3C8;
	constexpr uintptr_t LAS_SECTION_END_MIRROR = 0x3CC;
	constexpr uintptr_t LAS_PHRASE_SECTION_CONTAINER = 0x78;
	constexpr uintptr_t PHRASE_SECTION_VECTOR_BEGIN = 0xF4;
	constexpr uintptr_t PHRASE_SECTION_VECTOR_END = 0xF8;
	constexpr uintptr_t PHRASE_SECTION_STRIDE = 0x58;
	constexpr uintptr_t PHRASE_SECTION_START = 0x24;
	constexpr uintptr_t PHRASE_SECTION_END = 0x28;
	constexpr uint32_t PHRASE_SECTION_MAX = 1024;
	constexpr uintptr_t NOTE_VFX_VTABLE = 0x119BFB0;
	constexpr uintptr_t SPECIALIZED_NOTE_VFX_VTABLE = 0x119BFD8;
	constexpr uintptr_t BEAT_VFX_VTABLE = 0x119BF28;
	constexpr uintptr_t ARM_NOTE_VFX_PROMPT = 0x40D2E0;    // NoteVfx vtable +0x20
	constexpr uintptr_t CLEAR_NOTE_VFX_PROMPT = 0x40D360;  // NoteVfx vtable +0x24
	constexpr uintptr_t ARM_SPECIALIZED_PROMPT_BC = 0x40D710; // Specialized NoteVfx vtable +0x14
	constexpr uintptr_t ARM_SPECIALIZED_PROMPT_C0 = 0x40D790; // Specialized NoteVfx vtable +0x18
	constexpr uintptr_t CLEAR_SPECIALIZED_PROMPT = 0x40D810;  // Specialized NoteVfx vtable +0x1C
	constexpr char FREEZE_NOTE_TRACK_EVENT[] = "Play_FreezeNoteTrack";
	constexpr uintptr_t ONSET_NOTE_QUERY = 0x48DC40;
	constexpr uintptr_t LOUDEST_PLAYED_NOTE_QUERY = 0x48E5F0;
	constexpr uintptr_t TUNING_OFFSETS = 0x1199D2C;
	constexpr uintptr_t MOTION_NOTE_SINGLETON = 0x0135F57C;
	constexpr uintptr_t MOTION_NOTE_SINGLETON_STEP = 0x10;
	constexpr uintptr_t MOTION_NOTE_CONTAINER_STEP = 0x04;
	constexpr uintptr_t MOTION_NOTE_ARRAY_POINTER = 0x1284;
	constexpr uintptr_t MOTION_NOTE_ARRAY_COUNT = 0x1288;
	constexpr uintptr_t MOTION_NOTE_RECORD_STRIDE = 0x50;
	constexpr uintptr_t MOTION_NOTE_RECORD_PITCH = 0x28;
	constexpr uintptr_t MOTION_NOTE_RECORD_ACTIVE = 0x3C;
	constexpr uintptr_t MOTION_NOTE_RECORD_LOCATED = 0x3D;
	constexpr uint32_t MOTION_NOTE_MAX_RECORDS = 64;
	constexpr float BEND_UNDERBEND_SEMITONES = 0.3f;
	constexpr float BEND_VARIANCE_SEMITONES = 0.5f;
	constexpr float BEND_OVERBEND_ALLOWANCE_SEMITONES = 1.5f;
	constexpr float BEND_VETO_ESTIMATOR_SLACK_SEMITONES = 0.15f;
	constexpr uintptr_t DETECTION_ROOT = 0x0135F57C;
	constexpr uintptr_t DETECTION_ARRANGEMENT_GUITAR = 0x10;   // Player 1, guitar and bass; +0x14 = player 2
	constexpr uintptr_t DETECTION_ENGINE = 0x08;
	constexpr uintptr_t DETECTION_DETECTOR = 0x04;
	constexpr uintptr_t DETECTOR_CURRENT_NOTE = 0x5F4;         // what 0x48E5F0 returns
	constexpr uintptr_t DETECTOR_ANALYSIS_CLOCK = 0xD08;
	constexpr uintptr_t DETECTOR_GATE_QUALITY = 0xD38;
	constexpr uintptr_t DETECTOR_RING_BUFFER = 0xDB8;
	constexpr uintptr_t DETECTOR_RING_INDEX = 0xDBC;
	constexpr uintptr_t DETECTOR_RING_CAPACITY = 0xDC0;
	constexpr uintptr_t DETECTOR_RING_VALID_COUNT = 0xDC8;
	constexpr uintptr_t RING_FRAME_TIMESTAMP = 0x730;
	constexpr uintptr_t DETECTOR_GATE_LEVEL = 0xDD8;
	constexpr uintptr_t DETECTOR_PITCH_MODE = 0x11EC;
	constexpr uintptr_t DETECTOR_RING_STRIDE = 0x7D0;
	constexpr uintptr_t RING_FRAME_PITCH_MODE_TWO = 0x0C;
	constexpr uintptr_t RING_FRAME_PITCH_DEFAULT = 0x14;
	constexpr uintptr_t RING_FRAME_SEQUENCE = 0x748;
	constexpr uintptr_t ONSET_DEDUPE_GLOBAL = 0x012F6920;
	constexpr uintptr_t DETECTOR_GATE_LEVEL_THRESHOLD = 0x01224418;
	constexpr uintptr_t DETECTOR_GATE_QUALITY_THRESHOLD = 0x012243A0;
	constexpr double DETECTOR_GATE_LEVEL_FALLBACK = -55.0;
	constexpr double DETECTOR_GATE_QUALITY_FALLBACK = 50.0;
	constexpr int32_t DETECTOR_RING_MAX_CAPACITY = 4096;
	constexpr uintptr_t DETECTOR_SOUNDING_TABLE = 0x604;
	constexpr uintptr_t DETECTOR_SOUNDING_COUNT = 0x6A4;
	constexpr uintptr_t ND_STRENGTH_THRESHOLD_GLOBAL = 0x01199DD4;
	constexpr float ND_STRENGTH_THRESHOLD_FALLBACK = 5.0f;
	constexpr double ND_SOUNDING_HOLD_SECONDS = 0.18;
	constexpr int32_t ND_SOUNDING_MAX_ENTRIES = 64;
	volatile bool isNdAcceptEnabled = true;
	constexpr double DETECTOR_SAMPLE_INTERVAL_SECONDS = 1.0;
	constexpr float DETECTOR_SPIKE_JUMP_DB = 2.5f;
	constexpr int32_t DETECTOR_SPIKE_BURST_TICKS = 12;
	constexpr double BEND_RELEASE_GUARD_SECONDS = 0.4;
	constexpr float ONSET_EVIDENCE_LEVEL_FLOOR_DB = -65.0f;
	constexpr uint32_t ONSET_EVIDENCE_MIN_FRAMES = 10;
	constexpr int32_t ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES = 4;
	constexpr float ONSET_EVIDENCE_RISE_DB = 1.5f;
	constexpr int32_t ONSET_SCAN_MAX_FRAMES_PER_TICK = 120;
	constexpr float DETECTOR_RAW_BEND_QUALITY_FLOOR = 15.0f;
	constexpr int32_t DETECTOR_RAW_BEND_STREAK_TICKS = 3;
	constexpr float DETECTOR_RAW_BEND_STRONG_QUALITY = 70.0f;
	constexpr int32_t BEND_ACCEPT_MIN_HOLD_TICKS = 2;
	constexpr int32_t REATTACK_WINDOW_TICKS = 15;
	constexpr int32_t REATTACK_STREAK_TICKS = 3;
	volatile bool g_mlRescueEnabled = true;
	volatile bool isMlStuckRescueEnabled = true;
	constexpr float ML_STUCK_RESCUE_CONF = 0.45f;
	constexpr int32_t BEND_RESCUE_STREAK = 4;
	constexpr uintptr_t OWNER_CLOCK_PRIMARY = 0x3B4;
	constexpr uintptr_t OWNER_CLOCK_SECONDARY = 0x3B8;
	constexpr uintptr_t OWNER_CLOCK_RENDER = 0x3D0;
	constexpr uintptr_t OWNER_CLOCK_EPOCH_LOW = 0x3D4;
	constexpr uintptr_t OWNER_CLOCK_EPOCH_HIGH = 0x3D8;
	constexpr uintptr_t OWNER_COMPONENTS = 0x18;
	constexpr uintptr_t OWNER_CHILD_FLAG = 0x395;
	constexpr uintptr_t OWNER_NOTES_BEGIN = 0x41C;
	constexpr uintptr_t OWNER_NOTES_END = 0x420;
	constexpr uintptr_t NOTE_RECORD = 0x2C;
	constexpr uintptr_t NOTE_EVENT_TIME = 0x38;
	constexpr uintptr_t NOTE_DETECT_WINDOW_END_DELTA = 0x94;
	constexpr uintptr_t NOTE_DETECT_WINDOW_START_DELTA = 0x98;
	constexpr uintptr_t NOTE_CHORD_RECORD = 0x30;
	struct ChordTemplateView
	{
		uint32_t mask;
		uint8_t frets[6];
		uint8_t fingers[6];
		int32_t notes[6];
		char name[32];
	};
	static_assert(sizeof(ChordTemplateView) == 0x48, "SNG chord template stride is 0x48");
	constexpr uintptr_t NOTE_WINDOW_ENTRY = 0xA4;
	constexpr uintptr_t NOTE_WINDOW_EXIT = 0xA8;
	constexpr uintptr_t NOTE_STATE_C0 = 0xC0;
	constexpr uintptr_t NOTE_SPECIALIZED_PROMPT_STATE = 0xC8;
	constexpr uintptr_t NOTE_SPECIALIZED_PROMPT_SUPPRESS = 0xD0;
	constexpr uintptr_t NOTE_VFX_WRAPPER = 0x154;
	constexpr uintptr_t NOTE_VFX_PROMPT_FORK = 0xA4;
	constexpr uintptr_t SPECIALIZED_PROMPT_BC_FORK = 0xBC;
	constexpr uintptr_t SPECIALIZED_PROMPT_C0_FORK = 0xC0;
	constexpr uintptr_t BEAT_VFX_ACTIVE = 0x04;
	constexpr uintptr_t BEAT_VFX_ENTITY = 0x10;
	constexpr uintptr_t BEAT_VFX_PROMPT_REQUEST = 0x25;
	constexpr uint32_t NOTE_MASK_HAMMERON = NoteByNote::NOTE_MASK_HAMMER_ON;
	constexpr uint32_t NOTE_MASK_PULLOFF = NoteByNote::NOTE_MASK_PULL_OFF;
	constexpr uint32_t NOTE_MASK_BEND = 0x00001000;
	constexpr uint32_t NOTE_MASK_TAP = NoteByNote::NOTE_MASK_TAP;
	constexpr uint32_t LEGATO_CONFIRMATION_TICKS = 2;
	constexpr double HOLD_SAFETY_RELEASE_SECONDS = 60.0;
	constexpr double CHORD_HOLD_SAFETY_RELEASE_SECONDS = 15.0;
	constexpr double COMMITTED_CHORD_RESELECT_GUARD_SECONDS = 2.0;
	constexpr int MAX_BEND_SEMITONES = 3;

	constexpr uintptr_t RECORD_MASK = 0x00;
	constexpr uintptr_t RECORD_FLAGS = 0x04;
	constexpr uintptr_t RECORD_HASH = 0x08;
	constexpr uintptr_t RECORD_TIME = 0x0C;
	constexpr uintptr_t RECORD_STRING = 0x10;
	constexpr uintptr_t RECORD_FRET = 0x11;
	constexpr uintptr_t RECORD_CHORD_ID = 0x14;
	constexpr uintptr_t RECORD_BEND_AMOUNT = 0x40;
	constexpr float BEND_AMOUNT_MIN_SEMITONES = 0.5f;
	constexpr float BEND_AMOUNT_MAX_SEMITONES = 4.0f;
	constexpr uintptr_t RECORD_CHORD_NOTES_ID = 0x18;
	constexpr uintptr_t RECORD_PHRASE_ITERATION = 0x20;

	constexpr uint32_t NOTE_MASK_IGNORE = 0x00040000;
	constexpr uint32_t NOTE_MASK_CHILD = 0x10000000;
	constexpr uintptr_t PLAYER_SONG_MODE = 0x18;
	constexpr uintptr_t PLAYER_SONG_PENDING_STATE = 0x14;
	constexpr uintptr_t PLAYER_SONG_PENDING_FLAG = 0x1C;
	constexpr uintptr_t PLAYER_SONG_RUNNING = 0xD9;
	constexpr uintptr_t PLAYER_SONG_STOPPED = 0xDA;
	constexpr uintptr_t PLAYER_SONG_CLOCK = 0x174;

	constexpr float ROLLBACK_THRESHOLD = 0.2f;
	constexpr float GREY_EPSILON = 0.001f;
	constexpr float BOUNDARY_EPSILON = 0.001f;
	constexpr float HELD_TIME_EPSILON = 0.0005f;
	constexpr float HOLD_BOUNDARY_MAX_OVERSHOOT = 0.25f;
	constexpr float FLOW_HOLD_BOUNDARY_MAX_OVERSHOOT = 0.34f;
	constexpr float PLAYER_SONG_RESTART_SECONDS = 0.27f;
	constexpr float CHORD_DENSE_WINDOW_SECONDS = 0.60f;
	constexpr uint32_t INPUT_RELEASE_CONFIRMATION_TICKS = 3;
	constexpr uint32_t BEND_WAIT_ON_TARGET_TICKS = 3;
	constexpr uint32_t DENSE_REBUILD_TIMEOUT_TICKS = 600;
	constexpr float BEND_DIAGNOSTIC_START_TIME = 14.55f;
	constexpr float BEND_DIAGNOSTIC_END_TIME = 14.75f;

	using ScoringUpdateFn = void(__stdcall*)(void* owner, float updateTime);
	using HitDecisionFn = bool(__fastcall*)(void* owner, void* unusedEdx, void* note);
	using ThiscallVoidFn = void(__fastcall*)(void* self, void* unusedEdx);
	using ThiscallFloatFn = void(__fastcall*)(void* self, void* unusedEdx, float value);
	using MarkNotesFn = void(__fastcall*)(void* unusedEcx, void* owner);

	ScoringUpdateFn originalScoringUpdate = nullptr;
	HitDecisionFn originalHitDecision = nullptr;

	enum class GatePhase
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

	const char* DescribeGatePhase(GatePhase phase)
	{
		switch (phase)
		{
		case GatePhase::Idle: return "idle";
		case GatePhase::Armed: return "armed";
		case GatePhase::WaitingForInputRelease: return "waiting-for-input-release";
		case GatePhase::Holding: return "holding";
		case GatePhase::CommitBeforeRelease: return "commit-before-release";
		case GatePhase::DenseRebuildPending: return "dense-rebuild-pending";
		case GatePhase::DenseRecommitAfterRebuild: return "dense-recommit-after-rebuild";
		case GatePhase::DensePlayerSongStartPending: return "dense-play-packet-pending";
		case GatePhase::PostRelease: return "post-release";
		case GatePhase::RecommitAfterRelease: return "recommit-after-release";
		}
		return "unknown";
	}

	enum class DenseChainResult
	{
		NotRequired,
		Advanced,
		Faulted
	};

	struct DenseSuccessor
	{
		uintptr_t record = 0;
		uintptr_t note = 0;
		float recordTime = 0.0f;
		float holdTime = 0.0f;
		int stringIndex = -1;
		int fret = -1;
		int chordId = -1;
		int chordNotesId = -1;
		int expectedMidi = -1;
		bool isBend = false;
		bool isBendChild = false;
		bool isLegato = false;
		bool isHammerOn = false;
	};

	struct BendDecisionObservation
	{
		uintptr_t record = 0;
		uint32_t packedStates = 0;
		int loudestMidi = -2;
		bool originalResult = false;
		bool hasObservation = false;
	};

	std::recursive_mutex controllerMutex;
	bool isInitialized = false;
	void* trackedOwner = nullptr;
	float lastUpdateTime = 0.0f;
	bool hasLastUpdateTime = false;
	bool isEpochConfirmed = false;
	bool confirmedViaGrid = false;
	uint32_t armDiagTicks = 0;
	bool hasNativeSectionRangeFailureLogged = false;
	uint32_t gridLatchRefusals = 0;
	std::string gridReadRejectReason;
	bool hasGridLatchRefusalLogged = false;
	uint32_t rollbackIdentityMisses = 0;
	bool hasRollbackIdentityMissLogged = false;
	bool hasPendingBoundary = false;
	float pendingBoundaryBeforeRollback = 0.0f;
	float pendingBoundaryAfterRollback = 0.0f;
	float greyCutoff = 0.0f;
	float sectionEndBoundary = 0.0f;
	bool hasSectionEndBoundary = false;
	struct TimelineSection { float start; float end; };
	std::vector<TimelineSection> timelineSections;
	void* timelineOwner = nullptr;
	uintptr_t timelineContainer = 0;
	uintptr_t timelineBegin = 0;
	uintptr_t timelineEnd = 0;
	std::chrono::steady_clock::time_point holdProgressAnchor{};
	bool hasHoldProgressAnchor = false;
	uint64_t epochIndex = 0;
	uint64_t sectionLatchCounter = 0;
	GatePhase gatePhase = GatePhase::Idle;
	uintptr_t selectedRecord = 0;
	float selectedRecordTime = 0.0f;
	int selectedString = -1;
	int selectedFret = -1;
	int selectedChordId = -1;
	int selectedChordNotesId = -1;
	float selectedHoldTime = 0.0f;
	double selectedCompensation = 0.0;
	double measuredSongSpeed = 1.0;
	bool hasMeasuredSongSpeed = false;
	float flowSpeedPassRequestedPercent = -1.0f;
	float songSpeedSampleTime = -1.0f;
	std::chrono::steady_clock::time_point songSpeedSampleAt{};
	int expectedMidi = -1;
	int researchChordTones[6] = {};
	int researchChordToneCount = 0;
	uintptr_t researchChordTonesRecord = 0;
	int previousExpectedMidi = -1;
	int previousSelectedString = -1;
	uint64_t pickAttackFloorSample = 0;
	NoteByNote::PickedAttack lastUnresolvedAttack;
	bool hasLastUnresolvedAttack = false;
	uint64_t lastTakenPickSample = 0;
	uintptr_t nextNoteMidiRecord = 0;
	int nextNoteMidi = -1;
	constexpr float SAME_TIME_GROUP_EPSILON_SECONDS = 0.002f;
	constexpr uint32_t SAME_TIME_SIBLING_CONFIRM_TICKS = 2;
	uintptr_t sameTimeTrackedRecord = 0;
	float sameTimeGroupTime = -1.0f;
	uint32_t sameTimeGroupStrings = 0;
	std::array<int, 6> sameTimeGroupPitches = {};
	int sameTimeGroupPitchCount = 0;
	std::array<std::pair<int32_t, float>, 64> sameTimeBaseline = {};
	int32_t sameTimeBaselineCount = 0;
	bool isSameTimeSibling = false;
	uint32_t sameTimeSiblingTicks = 0;
	uint64_t lastChordStrumSample = 0;
	uint64_t successorStrumSample = 0;
	uintptr_t successorStrumRecord = 0;
	bool previousWasBend = false;
	bool isBendTarget = false;
	int bendAcceptMidi = -1;
	bool isBendChildTarget = false;
	void ResolveBendAcceptance(uintptr_t record);
	void ResetBootstrap(const char* reason, void* liveOwner, bool releaseStoppedMusic);
	void BeginBendConfirmation();
	void StashPrimedOnset(int primedOnset);
	bool MatchesPickPitch(int sounding);
	void LogMotionTrackers(const char* context, int wantedMidi);
	std::chrono::steady_clock::time_point trackerSampleAnchor{};
	bool hasTrackerSampleAnchor = false;
	uint32_t visualGroupCount = 0;
	uintptr_t visualGroupRecords[ResearchProtocol::NoteByNoteState::MaxVisualGroup] = {};
	int32_t visualGroupStrings[ResearchProtocol::NoteByNoteState::MaxVisualGroup] = {};
	int32_t visualGroupFrets[ResearchProtocol::NoteByNoteState::MaxVisualGroup] = {};
	uintptr_t lastVisualGroupRecord = 0;
	bool isLegatoTarget = false;
	bool isHammerOnTarget = false;
	uint32_t legatoConfirmTickCount = 0;
	bool hasLegatoPitchDeparted = false;
	constexpr uint32_t MAX_LEGATO_RUN = 8;
	int legatoRunMidi[MAX_LEGATO_RUN] = {};
	bool legatoRunIsBend[MAX_LEGATO_RUN] = {};
	uint32_t legatoRunCount = 0;
	uint32_t legatoRunIndex = 0;
	bool isConfirmingLegatoRun = false;
	using DetectionStrategy = NoteByNoteNativeScoring::DetectionStrategy;
	enum class DetectionTechnique { Single = 0, Chord = 1, Bend = 2 };
	volatile int g_chordDetectionStrategy = static_cast<int>(DetectionStrategy::Blend);
	volatile int g_noteDetectionStrategy = static_cast<int>(DetectionStrategy::NativeOnly);
	volatile int g_bendDetectionStrategy = static_cast<int>(DetectionStrategy::Blend);
	constexpr float ML_BLEND_VETO_CONF = 0.70f;

	DetectionTechnique CurrentDetectionTechnique()
	{
		if (selectedChordId != -1) return DetectionTechnique::Chord;
		if (isBendTarget) return DetectionTechnique::Bend;
		return DetectionTechnique::Single;   // legato/tap ride the single-note native path
	}

	DetectionStrategy CurrentTechniqueStrategy()
	{
		switch (CurrentDetectionTechnique())
		{
			case DetectionTechnique::Chord: return static_cast<DetectionStrategy>(g_chordDetectionStrategy);
			case DetectionTechnique::Bend:  return static_cast<DetectionStrategy>(g_bendDetectionStrategy);
			default:                        return static_cast<DetectionStrategy>(g_noteDetectionStrategy);
		}
	}
	bool MlMayRescueNow() { return g_mlRescueEnabled && CurrentTechniqueStrategy() != DetectionStrategy::NativeOnly; }
	void SampleDetectionComparison();
	static constexpr uint32_t DETECTION_COMPARE_HISTORY = 24;
	struct DetectionComparison
	{
		uint32_t agree = 0;
		uint32_t disagree = 0;
		int lastTechnique = -1;
		bool lastNativeMatch = false;
		bool lastMlMatch = false;
		bool lastMlHadOpinion = false;
		bool lastValid = false;
		uint32_t historyCount = 0;
		uint8_t history[DETECTION_COMPARE_HISTORY] = {};   // 0 red, 1 green, 2 amber
	};
	DetectionComparison g_detectionComparison;
	bool isBendRunConfirmation = false;
	bool wasBendTrackerPitchLogged = false;
	bool isRawBendAcceptArmed = false;
	int32_t rawBendAcceptStreak = 0;
	int32_t rawEstimateBendStreak = 0;
	int32_t ndBendSightingStreak = 0;
	bool hasBendApproachBeenObserved = false;
	void BuildLegatoRun(void* owner, float runTime, int runString);
	bool isHoldSuppressed = false;
	volatile bool areChordHoldsEnabled = true;
	volatile bool isFlowUntilMissEnabled = true;
	volatile float flowLateGraceSeconds = 0.08f;
	int armedExpectedMidi = -1;
	uintptr_t armedBufferCommitRecord = 0;
	bool wasArmedBufferCommitLogged = false;
	volatile bool verboseTrace = false;   // Master has no probe command to turn it off, and no trace file
	volatile bool isSafetyReleaseEnabled = false;
	double lastStuckWarnHeldSeconds = 0.0;
	uint64_t chordDecisionEvalCount = 0;
	uint64_t chordSoundingPeakAttack = 0;
	uint64_t chordNativeAgreedAttack = 0;
	int chordSoundingPeak = 0;
	std::chrono::steady_clock::time_point chordDecisionLogAnchor;
	bool hasChordDecisionLogAnchor = false;
	std::chrono::steady_clock::time_point matcherObserveAnchor;
	bool hasMatcherObserveAnchor = false;
	uintptr_t matcherFreshnessRecord = 0;
	bool matcherFreshnessArmed = false;
	volatile bool isChordWindowSlideEnabled = true;
	uint64_t chordWindowSlideCount = 0;
	std::chrono::steady_clock::time_point chordWindowLogAnchor;
	bool hasChordWindowLogAnchor = false;
	volatile bool areRepeatStrumHoldsEnabled = true;
	uintptr_t lastCommittedChordRecord = 0;
	int lastCommittedChordId = -1;
	std::chrono::steady_clock::time_point lastCommittedChordAt{};
	bool sawSpikeDuringHold = false;
	bool sawLevelSpikeDuringHold = false;
	double holdLatchRingTime = 0.0;
	bool hasHoldLatchRingTime = false;
	int selectedNativeTone = -1;
	std::chrono::steady_clock::time_point lastBendCommitAt{};
	bool hasLastBendCommit = false;
	int32_t onsetScanRingIndex = -1;
	uint32_t onsetScanFramesSinceLatch = 0;
	bool hasOnsetScanAnchor = false;
	NoteByNote::PickedAttackQueue pickedAttacks;
	NoteByNote::PickedAttack acceptedPick;
	uintptr_t acceptedPickRecord = 0;
	NoteByNote::DetectionFeedback detectionFeedback;
	NoteByNote::DetectionFeedback chordAcceptFeedback;
	uint64_t feedbackMinimumMlSample = 0;
	uint64_t feedbackMaximumMlSample = 0;
	uintptr_t mlRescueRecord = 0;
	int mlRescueMidi = -1;
	NoteByNote::DetectionFeedback enhancedRescueFeedback;
	uint64_t pickDiagnosticSample = 0;
	NoteByNote::ChordAttackGate chordAttacks;
	uint64_t pickScanSample = 0;
	uint64_t latestRawAudioSample = 0;
	bool rawAttackStreamAvailable = false;
	bool chordHoldNeedsCaptureBoundary = false;
	uint32_t pickSampleRate = 0;


	float heldEpoch = 0.0f;
	void* heldPlayerSong = nullptr;
	uint64_t holdTickCount = 0;
	std::chrono::steady_clock::time_point holdTickRateAnchor;
	uint64_t holdTickRateAnchorTick = 0;
	std::chrono::steady_clock::time_point detectorSampleAnchor;
	bool hasDetectorSampleAnchor = false;
	int32_t lastSampledRingIndex = -1;
	bool hasLastSampledRingIndex = false;
	float lastTickDetectorLevel = 0.0f;
	bool hasLastTickDetectorLevel = false;
	int32_t spikeBurstTicksRemaining = 0;
	float reattackLastLevel = 0.0f;
	bool hasReattackLastLevel = false;
	int32_t reattackWindowTicks = 0;
	int32_t reattackStreak = 0;
	Research::MlConfirmationState mlConfirmationState;
	Research::MlConfirmationState mlBendConfirmationState;
	NoteByNote::HeldPitchConfirmation enhancedLegatoConfirmation;
	int32_t bendRescueStreak = 0;      // consecutive ticks tier-0/ML confirm the bend reached its target
	std::chrono::steady_clock::time_point denseRebuildQueuedAt{};
	bool hasDenseRebuildTiming = false;
	bool isFlowRedrawRebuild = false;
	std::chrono::steady_clock::time_point lastCommitWallClock{};
	bool hasLastCommitWallClock = false;
	int32_t spikeRecencyTicks = 0;
	int pendingPrimedOnset = -1;
	bool hasPickPitchDeparted = false;
	uint32_t pickPitchConfirmTicks = 0;
	uint64_t postReleaseTickCount = 0;
	uint64_t commitTickCount = 0;
	uint32_t inputReleaseTickCount = 0;
	int bendWaitPitchFloor = -1;
	uint32_t bendWaitOnTargetTicks = 0;
	bool wasCommitOverrideLogged = false;
	bool isReleaseRequested = false;
	bool reArmRequested = false;
	std::unordered_set<uintptr_t> consumedRecords;
	DenseSuccessor denseSuccessor;
	float observedScoringUpdateTime = 0.0f;
	std::array<BendDecisionObservation, 8> bendDecisionObservations = {};
	size_t bendDecisionObservationCount = 0;
	uint64_t renderFramesWhileHeld = 0;
	constexpr bool kApplyInputOnsetShift = false;
	int NativeFrameOffset();
	int ShiftNativePitchToDisplay(int nativeMidi)
	{
		if (!kApplyInputOnsetShift) return nativeMidi >= 0 ? nativeMidi + NativeFrameOffset() : nativeMidi;
		return nativeMidi >= 0 ? nativeMidi + ResearchProbeRuntime::GetInputOnsetShiftSemitones()
							   : nativeMidi;
	}
	float ShiftNativePitchToDisplay(float nativePitch)
	{
		if (!kApplyInputOnsetShift) return nativePitch;
		return nativePitch >= 0.0f
			? nativePitch + static_cast<float>(ResearchProbeRuntime::GetInputOnsetShiftSemitones())
			: nativePitch;
	}

	int QueryNativeOnsetNote()
	{
		int result = -1;
		__asm
		{
			xor eax, eax        // arrangement kind 0 (guitar)
			mov edx, 0x48DC40   // ONSET_NOTE_QUERY
			call edx
			mov result, eax
		}
		const int shifted = ShiftNativePitchToDisplay(result);
		if (shifted != result)
		{
			static uint32_t onsetShiftLogThrottle = 0;
			if ((onsetShiftLogThrottle++ % 120) == 0)
			{
				LOG_INFO("(NBN TRANSPOSE) Native onset " << result << " shifted -> " << shifted
					<< " to match the display-tuning expected pitch (DropPedal input shift active)."
					<< std::endl);
			}
		}
		return shifted;
	}

	int QueryNativeLoudestPlayedNote()
	{
		int result = -1;
		__asm
		{
			xor eax, eax        // arrangement kind 0 (guitar)
			mov edx, 0x48E5F0   // LOUDEST_PLAYED_NOTE_QUERY
			call edx
			mov result, eax
		}
		return result;
	}
	volatile bool isFreezePromptSoundEnabled = false;

	void PlayFreezeNoteTrack()
	{
		if (!isFreezePromptSoundEnabled)
		{
			LOG_INFO("(NBN LAS PROMPT) " << FREEZE_NOTE_TRACK_EVENT
				<< " skipped (prompt sound disabled)." << std::endl);
			return;
		}
		const char* eventName = FREEZE_NOTE_TRACK_EVENT;
		const uintptr_t playSound = PLAY_SOUND_CSTRING;
		__asm
		{
			push esi
			mov esi, eventName
			mov eax, playSound
			call eax
			pop esi
		}
		LOG_INFO("(NBN LAS PROMPT) Dispatched Rocksmith's native "
			<< FREEZE_NOTE_TRACK_EVENT << " event before the hold." << std::endl);
	}

	template <typename T>
	bool TryRead(uintptr_t address, T& value)
	{
		if (address == 0) return false;
		MEMORY_BASIC_INFORMATION memory = {};
		if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) == 0) return false;
		if (memory.State != MEM_COMMIT
			|| (memory.Protect & PAGE_GUARD) != 0
			|| (memory.Protect & PAGE_NOACCESS) != 0)
		{
			return false;
		}
		const auto regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
		if (address > regionEnd || sizeof(T) > regionEnd - address) return false;
		value = *reinterpret_cast<const T*>(address);
		return true;
	}
	bool TryWriteDedupeGlobal(int32_t value) noexcept
	{
		__try
		{
			*reinterpret_cast<volatile int32_t*>(ONSET_DEDUPE_GLOBAL) = value;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
	bool TryWriteGameFloat(uintptr_t address, float value) noexcept
	{
		__try
		{
			*reinterpret_cast<volatile float*>(address) = value;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool TryWriteGameUint32(uintptr_t address, uint32_t value) noexcept
	{
		__try
		{
			*reinterpret_cast<volatile uint32_t*>(address) = value;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool TryReadNativeSectionRange(void* owner, float& start, float& end)
	{
		const auto address = reinterpret_cast<uintptr_t>(owner);
		uintptr_t vtable = 0;
		float startMirror = 0.0f;
		float endMirror = 0.0f;
		if (!TryRead(address, vtable) || vtable != LAS_OWNER_VTABLE
			|| !TryRead(address + LAS_SECTION_START, start)
			|| !TryRead(address + LAS_SECTION_START_MIRROR, startMirror)
			|| !TryRead(address + LAS_SECTION_END, end)
			|| !TryRead(address + LAS_SECTION_END_MIRROR, endMirror))
		{
			return false;
		}

		return std::isfinite(start)
			&& std::isfinite(end)
			&& start >= 0.0f
			&& end > start
			&& std::fabs(start - startMirror) <= BOUNDARY_EPSILON
			&& std::fabs(end - endMirror) <= BOUNDARY_EPSILON;
	}
	std::string DescribeNativeSectionRange(void* owner)
	{
		const auto address = reinterpret_cast<uintptr_t>(owner);
		std::ostringstream text;
		uintptr_t vtable = 0;
		float start = 0.0f, startMirror = 0.0f, end = 0.0f, endMirror = 0.0f;
		text << "owner=0x" << std::hex << address;
		if (!TryRead(address, vtable)) return text.str() + " (owner unreadable)";
		text << " vtable=0x" << vtable << (vtable == LAS_OWNER_VTABLE ? " (GamePlaysongLAS)" : " (NOT GamePlaysongLAS)") << std::dec;
		const bool readable = TryRead(address + LAS_SECTION_START, start)
			&& TryRead(address + LAS_SECTION_START_MIRROR, startMirror)
			&& TryRead(address + LAS_SECTION_END, end)
			&& TryRead(address + LAS_SECTION_END_MIRROR, endMirror);
		if (!readable) return text.str() + " (range fields unreadable)";
		text << std::fixed << std::setprecision(3) << " start=" << start << " startMirror=" << startMirror
			<< " end=" << end << " endMirror=" << endMirror;
		return text.str();
	}
	bool EnsureTimelineForOwner(void* owner)
	{
		const auto address = reinterpret_cast<uintptr_t>(owner);
		uintptr_t vtable = 0;
		if (!TryRead(address, vtable) || vtable != LAS_OWNER_VTABLE)
		{
			gridReadRejectReason = "owner is not GamePlaysongLAS";
			return false;
		}

		uintptr_t container = 0;
		if (!TryRead(address + LAS_PHRASE_SECTION_CONTAINER, container) || container == 0)
		{
			gridReadRejectReason = "owner+0x78 section container is null or unreadable";
			return false;
		}

		uintptr_t begin = 0;
		uintptr_t end = 0;
		if (!TryRead(container + PHRASE_SECTION_VECTOR_BEGIN, begin) || begin == 0
			|| !TryRead(container + PHRASE_SECTION_VECTOR_END, end) || end < begin)
		{
			std::ostringstream reason;
			reason << "section vector unreadable (begin=0x" << std::hex << begin << " end=0x" << end << ")";
			gridReadRejectReason = reason.str();
			return false;
		}
		if (timelineOwner == owner && timelineContainer == container
			&& timelineBegin == begin && timelineEnd == end && !timelineSections.empty())
		{
			return true;
		}

		timelineSections.clear();
		timelineOwner = nullptr;
		timelineContainer = 0;
		timelineBegin = 0;
		timelineEnd = 0;

		const uintptr_t span = end - begin;
		if (span == 0 || (span % PHRASE_SECTION_STRIDE) != 0 || span / PHRASE_SECTION_STRIDE > PHRASE_SECTION_MAX)
		{
			std::ostringstream reason;
			reason << "section vector spans " << span << " bytes (stride " << PHRASE_SECTION_STRIDE << ", max "
				<< PHRASE_SECTION_MAX << " rows)";
			gridReadRejectReason = reason.str();
			return false;
		}
		const uint32_t count = static_cast<uint32_t>(span / PHRASE_SECTION_STRIDE);
		uint32_t skippedEmpty = 0;

		std::vector<TimelineSection> sections;
		sections.reserve(count);
		float previousEnd = -std::numeric_limits<float>::infinity();
		for (uint32_t index = 0; index < count; ++index)
		{
			const uintptr_t entry = begin + static_cast<uintptr_t>(index) * PHRASE_SECTION_STRIDE;
			float start = 0.0f;
			float sectionEnd = 0.0f;
			if (!TryRead(entry + PHRASE_SECTION_START, start)
				|| !TryRead(entry + PHRASE_SECTION_END, sectionEnd))
			{
				std::ostringstream reason;
				reason << "row " << index << " of " << count << " unreadable";
				gridReadRejectReason = reason.str();
				return false;
			}
			if (std::isfinite(start) && std::isfinite(sectionEnd) && sectionEnd <= start + BOUNDARY_EPSILON
				&& start >= previousEnd - BOUNDARY_EPSILON)
			{
				++skippedEmpty;
				continue;
			}
			if (!std::isfinite(start) || !std::isfinite(sectionEnd)
				|| sectionEnd <= start || start < previousEnd - BOUNDARY_EPSILON)
			{
				std::ostringstream reason;
				reason << std::fixed << std::setprecision(3) << "row " << index << " of " << count << " is invalid: start="
					<< start << " end=" << sectionEnd << " previous end=" << previousEnd;
				gridReadRejectReason = reason.str();
				return false;
			}
			previousEnd = sectionEnd;
			sections.push_back({ start, sectionEnd });
		}

		if (sections.empty())
		{
			gridReadRejectReason = "no row has a playable length";
			return false;
		}
		gridReadRejectReason.clear();
		timelineSections = std::move(sections);
		timelineOwner = owner;
		timelineContainer = container;
		timelineBegin = begin;
		timelineEnd = end;
		LOG_INFO("(NBN LAS TIMELINE) Cached " << timelineSections.size()
			<< " authored sections for owner 0x" << std::hex
			<< reinterpret_cast<uintptr_t>(owner) << std::dec << " ["
			<< std::fixed << std::setprecision(3) << timelineSections.front().start
			<< ".." << timelineSections.back().end << "s]"
			<< (skippedEmpty ? " (skipped " + std::to_string(skippedEmpty) + " row(s) with no playable length)" : std::string())
			<< "; arming reads the stable grid, not the marching +0x3C0/+0x3C8." << std::endl);
		return true;
	}
	int FindTimelineIndexByStart(float startTime)
	{
		for (size_t index = 0; index < timelineSections.size(); ++index)
		{
			if (std::fabs(timelineSections[index].start - startTime) <= BOUNDARY_EPSILON)
			{
				return static_cast<int>(index);
			}
		}
		return -1;
	}
	int FindTimelineIndexByEnd(float endTime)
	{
		for (size_t index = 0; index < timelineSections.size(); ++index)
		{
			if (std::fabs(timelineSections[index].end - endTime) <= BOUNDARY_EPSILON)
			{
				return static_cast<int>(index);
			}
		}
		return -1;
	}
	void NoteGridLatchRefusal(const char* reason, float loopEnd, float loopEndMirror)
	{
		if (hasGridLatchRefusalLogged || ++gridLatchRefusals < 180) return;
		hasGridLatchRefusalLogged = true;
		std::ostringstream detail;
		detail << std::fixed << std::setprecision(3);
		if (!timelineSections.empty())
		{
			size_t nearest = 0;
			for (size_t index = 1; index < timelineSections.size(); ++index)
				if (std::fabs(timelineSections[index].end - loopEnd) < std::fabs(timelineSections[nearest].end - loopEnd))
					nearest = index;
			detail << " grid has " << timelineSections.size() << " sections [" << timelineSections.front().start
				<< ".." << timelineSections.back().end << "]; nearest section end is row " << nearest << " at "
				<< timelineSections[nearest].end << " (off by " << (timelineSections[nearest].end - loopEnd) << " s).";
		}
		else detail << " grid is empty.";
		LOG_ERROR("(NBN LAS BOOTSTRAP) Grid latch still refused after " << gridLatchRefusals << " ticks: " << reason
			<< "; native loop end=" << std::fixed << std::setprecision(3) << loopEnd << " mirror=" << loopEndMirror
			<< "." << detail.str() << " Falling back to the rollback-identity bootstrap." << std::endl);
	}

	bool TryLatchSectionFromGrid(void* owner, float startHint = -1.0f)
	{
		if (!EnsureTimelineForOwner(owner))
		{
			const std::string reason = "the authored phrase grid could not be read (" + gridReadRejectReason + ")";
			NoteGridLatchRefusal(reason.c_str(), 0.0f, 0.0f);
			return false;
		}
		const auto address = reinterpret_cast<uintptr_t>(owner);
		float loopEnd = 0.0f;
		float loopEndMirror = 0.0f;
		if (!TryRead(address + LAS_SECTION_END, loopEnd)
			|| !TryRead(address + LAS_SECTION_END_MIRROR, loopEndMirror)
			|| !std::isfinite(loopEnd)
			|| std::fabs(loopEnd - loopEndMirror) > BOUNDARY_EPSILON)
		{
			NoteGridLatchRefusal("the native loop end was unreadable or disagreed with its mirror", loopEnd, loopEndMirror);
			return false; // transient; retry next tick, never inert.
		}

		const int endIndex = FindTimelineIndexByEnd(loopEnd);
		if (endIndex < 0)
		{
			NoteGridLatchRefusal("the native loop end matches no authored section end (1 ms tolerance)", loopEnd, loopEndMirror);
			return false; // loopEnd not yet a grid boundary; retry next tick.
		}
		int startIndex = -1;
		float loopStart = 0.0f;
		float loopStartMirror = 0.0f;
		if (TryRead(address + LAS_SECTION_START, loopStart)
			&& TryRead(address + LAS_SECTION_START_MIRROR, loopStartMirror)
			&& std::isfinite(loopStart)
			&& std::fabs(loopStart - loopStartMirror) <= BOUNDARY_EPSILON)
		{
			const int candidate = FindTimelineIndexByStart(loopStart);
			if (candidate >= 0 && candidate <= endIndex) startIndex = candidate;
		}
		if (startIndex < 0 && startHint >= 0.0f)
		{
			const int hinted = FindTimelineIndexByStart(startHint);
			if (hinted >= 0 && hinted <= endIndex) startIndex = hinted;
		}
		if (startIndex < 0) startIndex = endIndex;

		const float gridStart = timelineSections[static_cast<size_t>(startIndex)].start;
		const float gridEnd = timelineSections[static_cast<size_t>(endIndex)].end;
		if (!(gridEnd > gridStart))
		{
			NoteGridLatchRefusal("the matched grid rows give an empty range", loopEnd, loopEndMirror);
			return false;
		}

		greyCutoff = gridStart;
		sectionEndBoundary = gridEnd;
		hasSectionEndBoundary = true;
		isEpochConfirmed = true;
		confirmedViaGrid = true;
		epochIndex = 1;
		++sectionLatchCounter;
		songSpeedSampleTime = -1.0f;
		consumedRecords.clear();
		LOG_INFO("(NBN LAS BOOTSTRAP) Section latched from the authored grid: rows "
			<< startIndex << ".." << endIndex << " -> [" << std::fixed << std::setprecision(6)
			<< gridStart << ".." << gridEnd << "]; greyCutoff=" << gridStart
			<< " end=" << gridEnd << "; epoch 1 begins immediately (no rollback wait)."
			<< " Native loop read was [" << loopStart << ".." << loopEnd << "]." << std::endl);
		return true;
	}
	bool HasSelectionMovedToNewSection(void* owner)
	{
		if (!isEpochConfirmed || timelineSections.empty()) return false;
		const auto address = reinterpret_cast<uintptr_t>(owner);
		float currentEnd = 0.0f;
		float currentEndMirror = 0.0f;
		if (!TryRead(address + LAS_SECTION_END, currentEnd)
			|| !TryRead(address + LAS_SECTION_END_MIRROR, currentEndMirror)
			|| !std::isfinite(currentEnd)
			|| std::fabs(currentEnd - currentEndMirror) > BOUNDARY_EPSILON)
		{
			return false; // transient read; do not churn the latch.
		}
		if (std::fabs(currentEnd - sectionEndBoundary) <= BOUNDARY_EPSILON) return false;
		return FindTimelineIndexByEnd(currentEnd) >= 0;
	}
	bool TryGetSoundingPitchNear(float wantedMidi, float withinSemitones, float& pitchOut)
	{
		uintptr_t singleton = 0;
		if (!TryRead(MOTION_NOTE_SINGLETON, singleton) || singleton == 0) return false;

		uintptr_t step = 0;
		if (!TryRead(singleton + MOTION_NOTE_SINGLETON_STEP, step) || step == 0) return false;

		uintptr_t container = 0;
		if (!TryRead(step + MOTION_NOTE_CONTAINER_STEP, container) || container == 0) return false;

		uintptr_t records = 0;
		uint32_t count = 0;
		if (!TryRead(container + MOTION_NOTE_ARRAY_POINTER, records) || records == 0) return false;
		if (!TryRead(container + MOTION_NOTE_ARRAY_COUNT, count)) return false;
		if (count == 0 || count > MOTION_NOTE_MAX_RECORDS) return false;

		bool found = false;
		float bestPitch = 0.0f;
		float bestDistance = 0.0f;
		for (uint32_t index = 0; index < count; ++index)
		{
			const uintptr_t record = records + (index * MOTION_NOTE_RECORD_STRIDE);
			uint8_t active = 0;
			uint8_t located = 0;
			if (!TryRead(record + MOTION_NOTE_RECORD_ACTIVE, active) || active != 1) continue;
			if (!TryRead(record + MOTION_NOTE_RECORD_LOCATED, located) || located == 0) continue;

			float pitch = 0.0f;
			if (!TryRead(record + MOTION_NOTE_RECORD_PITCH, pitch)) continue;
			if (!std::isfinite(pitch) || pitch < 0.0f) continue;

			const float distance = std::fabs(pitch - wantedMidi);
			if (distance > withinSemitones) continue;
			if (!found || distance < bestDistance)
			{
				found = true;
				bestPitch = pitch;
				bestDistance = distance;
			}
		}

		if (found) pitchOut = bestPitch;
		return found;
	}
	void LogMotionTrackers(const char* context, int wantedMidi)
	{
		uintptr_t singleton = 0;
		uintptr_t step = 0;
		uintptr_t container = 0;
		uintptr_t records = 0;
		uint32_t count = 0;

		if (!TryRead(MOTION_NOTE_SINGLETON, singleton) || singleton == 0
			|| !TryRead(singleton + MOTION_NOTE_SINGLETON_STEP, step) || step == 0
			|| !TryRead(step + MOTION_NOTE_CONTAINER_STEP, container) || container == 0
			|| !TryRead(container + MOTION_NOTE_ARRAY_POINTER, records) || records == 0
			|| !TryRead(container + MOTION_NOTE_ARRAY_COUNT, count))
		{
			LOG_INFO("(NBN TRACKER) verdict=chain-unreadable context=" << context
				<< " wanted=" << wantedMidi
				<< " singleton=0x" << std::hex << singleton
				<< " arrangement=0x" << step
				<< " container=0x" << container
				<< " records=0x" << records << std::dec
				<< ". The tracker chain did not resolve, so every bend falls back to the"
				<< " integer query." << std::endl);
			return;
		}

		if (count == 0 || count > MOTION_NOTE_MAX_RECORDS)
		{
			LOG_INFO("(NBN TRACKER) verdict=bad-count context=" << context
				<< " wanted=" << wantedMidi << " count=" << count << "." << std::endl);
			return;
		}

		std::ostringstream detail;
		uint32_t activeCount = 0;
		uint32_t locatedCount = 0;
		uint32_t inBandCount = 0;
		for (uint32_t index = 0; index < count; ++index)
		{
			const uintptr_t record = records + (index * MOTION_NOTE_RECORD_STRIDE);
			uint8_t active = 0;
			uint8_t located = 0;
			float pitch = 0.0f;
			const bool readActive = TryRead(record + MOTION_NOTE_RECORD_ACTIVE, active);
			const bool readLocated = TryRead(record + MOTION_NOTE_RECORD_LOCATED, located);
			const bool readPitch = TryRead(record + MOTION_NOTE_RECORD_PITCH, pitch);
			if (readActive && active == 1) ++activeCount;
			if (readLocated && located != 0) ++locatedCount;
			if (readActive && active == 1 && readLocated && located != 0 && readPitch
				&& std::isfinite(pitch) && pitch >= 0.0f
				&& std::fabs(pitch - static_cast<float>(wantedMidi))
					<= BEND_UNDERBEND_SEMITONES)
			{
				++inBandCount;
			}
			detail << ' ' << index << '=' << static_cast<int>(active)
				<< '/' << static_cast<int>(located)
				<< '/' << std::fixed << std::setprecision(1) << (readPitch ? pitch : -99.0f);
		}

		const char* verdict = "in-band";
		if (activeCount == 0) verdict = "none-active";
		else if (locatedCount == 0) verdict = "none-located";
		else if (inBandCount == 0) verdict = "located-out-of-band";

		LOG_INFO("(NBN TRACKER) verdict=" << verdict << " context=" << context
			<< " wanted=" << wantedMidi
			<< " count=" << count << " active=" << activeCount
			<< " located=" << locatedCount << " inBand=" << inBandCount
			<< " |" << detail.str()
			<< " container=0x" << std::hex << container << std::dec
			<< "." << std::endl);
	}
	struct DetectorGateSample
	{
		uintptr_t detector = 0;
		float level = 0.0f;
		float quality = 0.0f;
		double levelMinimum = DETECTOR_GATE_LEVEL_FALLBACK;
		double qualityMinimum = DETECTOR_GATE_QUALITY_FALLBACK;
		bool passesLevel = false;
		bool passesQuality = false;
		int32_t currentNote = -1;
		int32_t ringIndex = -1;
		int32_t ringCapacity = 0;
		int32_t ringSequence = 0;
		int32_t pitchMode = 0;
		int32_t pitchNow = -1;
		int32_t pitchPrevious = -1;
		int32_t dedupe = -1;
		bool hasRing = false;
	};
	DetectorGateSample lastStateGateSample = {};
	bool hasLastStateGateSample = false;
	std::chrono::steady_clock::time_point lastStateGateSampleAt{};

	struct BendVisualizationSnapshot
	{
		int32_t baseMidi = -1;
		int32_t targetMidi = -1;
		float soundingMidi = -1.0f;
		float soundingQuality = 0.0f;
	};

	BendVisualizationSnapshot bendVisualizationSnapshot;
	bool bendVisualReached = false;
	uintptr_t bendVisualReachedRecord = 0;
	uint64_t bendVisualReachedEpoch = 0;
	constexpr float NBN_INPUT_PRESENT_FLOOR_DB = -60.0f;
	bool NbnInputPresent()
	{
		if (!hasLastStateGateSample || !std::isfinite(lastStateGateSample.level)) return false;
		if (std::chrono::duration<double>(
				std::chrono::steady_clock::now() - lastStateGateSampleAt).count() >= 2.0)
		{
			return false;
		}
		return lastStateGateSample.level >= NBN_INPUT_PRESENT_FLOOR_DB;
	}
	constexpr uintptr_t DETECTOR_OPEN_STRING_MIDI = ArrangementInstrument::DETECTOR_OPEN_STRING_MIDI;
	bool isBassArrangement = false;

	bool IsBassArrangement() { return isBassArrangement; }
	bool KeepsMismatchedPicks() { return true; }
	constexpr int BASS_HIGHEST_PLAYABLE_MIDI = 69;

	void RefreshArrangementInstrument()
	{
		uintptr_t root = 0, arrangement = 0, engine = 0, detector = 0;
		std::array<int16_t, 6> open = {};
		if (!TryRead(DETECTION_ROOT, root) || root == 0
			|| !TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0
			|| !TryRead(arrangement + DETECTION_ENGINE, engine) || engine == 0
			|| !TryRead(engine + DETECTION_DETECTOR, detector) || detector == 0
			|| !TryRead(detector + DETECTOR_OPEN_STRING_MIDI, open)) return;
		const bool bass = ArrangementInstrument::IsBassOpenStringTable(open.data());
		if (bass != isBassArrangement)
		{
			LOG_INFO("(NBN INSTRUMENT) " << (bass ? "Bass" : "Guitar") << " arrangement (detector open strings "
				<< open[0] << " " << open[1] << " " << open[2] << " " << open[3] << " " << open[4] << " " << open[5]
				<< ")." << std::endl);
		}
		isBassArrangement = bass;
	}
	int nativeFrameOffset = 0;
	bool hasNativeFrameOffset = false;
	std::chrono::steady_clock::time_point nativeFrameOffsetReadAt{};
	int NativeFrameOffset()
	{
		const auto now = std::chrono::steady_clock::now();
		if (hasNativeFrameOffset && now - nativeFrameOffsetReadAt < std::chrono::milliseconds(500)) return nativeFrameOffset;
		nativeFrameOffsetReadAt = now;
		uintptr_t root = 0, arrangement = 0, engine = 0, detector = 0;
		std::array<int16_t, 6> open = {};
		if (!TryRead(DETECTION_ROOT, root) || root == 0
			|| !TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0
			|| !TryRead(arrangement + DETECTION_ENGINE, engine) || engine == 0
			|| !TryRead(engine + DETECTION_DETECTOR, detector) || detector == 0
			|| !TryRead(detector + DETECTOR_OPEN_STRING_MIDI, open)) return nativeFrameOffset;
		const int stringIndex = (selectedString >= 0 && selectedString <= 5) ? selectedString : 0;
		int16_t tuningOffset = 0;
		int physicalOpen = -1;
		if (open[static_cast<size_t>(stringIndex)] <= 0
			|| !TryRead(TUNING_OFFSETS + static_cast<uintptr_t>(stringIndex) * 2, tuningOffset)
			|| !NoteByNote::TryResolvePlayedNotePitch(stringIndex, 0, tuningOffset,
				ResearchProbeRuntime::GetInputOnsetShiftSemitones(), physicalOpen, IsBassArrangement()))
		{
			return nativeFrameOffset;
		}
		const int measured = physicalOpen - open[static_cast<size_t>(stringIndex)];
		const int offset = (measured >= -2 && measured <= 2) ? measured : 0;
		if (offset != nativeFrameOffset || !hasNativeFrameOffset)
		{
			LOG_INFO("(NBN TRANSPOSE) Native detector frame differs from the played pitch by " << offset
				<< " semitone(s) (string " << stringIndex << " detector open " << open[static_cast<size_t>(stringIndex)]
				<< " played open " << physicalOpen << "); native readings are shifted by it." << std::endl);
		}
		hasNativeFrameOffset = true;
		nativeFrameOffset = offset;
		return offset;
	}

	bool TryReadDetectorGates(DetectorGateSample& sample)
	{
		uintptr_t root = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return false;

		uintptr_t arrangement = 0;
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0) return false;

		uintptr_t engine = 0;
		if (!TryRead(arrangement + DETECTION_ENGINE, engine) || engine == 0) return false;

		uintptr_t detector = 0;
		if (!TryRead(engine + DETECTION_DETECTOR, detector) || detector == 0) return false;
		sample.detector = detector;
		if (!TryRead(detector + DETECTOR_GATE_LEVEL, sample.level)) return false;
		if (!TryRead(detector + DETECTOR_GATE_QUALITY, sample.quality)) return false;
		if (!TryRead(DETECTOR_GATE_LEVEL_THRESHOLD, sample.levelMinimum)
			|| !std::isfinite(sample.levelMinimum))
		{
			sample.levelMinimum = DETECTOR_GATE_LEVEL_FALLBACK;
		}
		if (!TryRead(DETECTOR_GATE_QUALITY_THRESHOLD, sample.qualityMinimum)
			|| !std::isfinite(sample.qualityMinimum))
		{
			sample.qualityMinimum = DETECTOR_GATE_QUALITY_FALLBACK;
		}
		sample.passesLevel = std::isfinite(sample.level)
			&& static_cast<double>(sample.level) >= sample.levelMinimum;
		sample.passesQuality = std::isfinite(sample.quality)
			&& static_cast<double>(sample.quality) >= sample.qualityMinimum;

		TryRead(detector + DETECTOR_CURRENT_NOTE, sample.currentNote);
		TryRead(detector + DETECTOR_PITCH_MODE, sample.pitchMode);
		TryRead(detector + DETECTOR_RING_INDEX, sample.ringIndex);
		TryRead(detector + DETECTOR_RING_CAPACITY, sample.ringCapacity);
		TryRead(ONSET_DEDUPE_GLOBAL, sample.dedupe);

		uintptr_t ring = 0;
		if (TryRead(detector + DETECTOR_RING_BUFFER, ring) && ring != 0
			&& sample.ringIndex >= 0
			&& sample.ringCapacity > 0
			&& sample.ringCapacity <= DETECTOR_RING_MAX_CAPACITY
			&& sample.ringIndex < sample.ringCapacity)
		{
			const uintptr_t pitchField = sample.pitchMode == 2
				? RING_FRAME_PITCH_MODE_TWO
				: RING_FRAME_PITCH_DEFAULT;
			const int32_t previousIndex = sample.ringIndex > 0
				? sample.ringIndex - 1
				: sample.ringCapacity - 1;
			const uintptr_t current = ring
				+ static_cast<uintptr_t>(sample.ringIndex) * DETECTOR_RING_STRIDE;
			const uintptr_t previous = ring
				+ static_cast<uintptr_t>(previousIndex) * DETECTOR_RING_STRIDE;
			sample.hasRing = TryRead(current + pitchField, sample.pitchNow)
				&& TryRead(previous + pitchField, sample.pitchPrevious)
				&& TryRead(current + RING_FRAME_SEQUENCE, sample.ringSequence);
		}
		return true;
	}
	bool TryReadDetectorInputPresence(bool& isInputPresent)
	{
		DetectorGateSample sample;
		if (!TryReadDetectorGates(sample) || !sample.hasRing)
		{
			return false;
		}

		isInputPresent = sample.passesLevel
			&& sample.passesQuality
			&& sample.pitchNow >= 0
			&& sample.pitchNow == sample.pitchPrevious;
		return true;
	}
	std::chrono::steady_clock::time_point ndSoundingStreakStart{};
	bool hasNdSoundingStreak = false;
	float ndSoundingLastStrength = 0.0f;
	float ReadNdSoundingStrength(int pitch)
	{
		uintptr_t root = 0, arrangement = 0, detector = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return -1.0f;
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0) return -1.0f;
		if (!TryRead(arrangement + DETECTION_DETECTOR, detector) || detector == 0) return -1.0f;
		int32_t count = 0;
		if (!TryRead(detector + DETECTOR_SOUNDING_COUNT, count)
			|| count <= 0 || count > ND_SOUNDING_MAX_ENTRIES)
		{
			return -1.0f;
		}
		float threshold = ND_STRENGTH_THRESHOLD_FALLBACK;
		if (!TryRead(ND_STRENGTH_THRESHOLD_GLOBAL, threshold) || !std::isfinite(threshold))
		{
			threshold = ND_STRENGTH_THRESHOLD_FALLBACK;
		}
		for (int32_t index = 0; index < count; ++index)
		{
			int32_t midi = -1;
			if (!TryRead(detector + DETECTOR_SOUNDING_TABLE + static_cast<uintptr_t>(index) * 8, midi)) break;
			if (midi != pitch) continue;
			float strength = 0.0f;
			if (!TryRead(detector + DETECTOR_SOUNDING_TABLE + static_cast<uintptr_t>(index) * 8 + 4, strength)) break;
			return (std::isfinite(strength) && strength > threshold) ? strength : -1.0f;
		}
		return -1.0f;
	}
	float ReadNdRawSoundingStrength(int pitch)
	{
		uintptr_t root = 0, arrangement = 0, detector = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0)
			return std::numeric_limits<float>::quiet_NaN();
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0)
			return std::numeric_limits<float>::quiet_NaN();
		if (!TryRead(arrangement + DETECTION_DETECTOR, detector) || detector == 0)
			return std::numeric_limits<float>::quiet_NaN();
		int32_t count = 0;
		if (!TryRead(detector + DETECTOR_SOUNDING_COUNT, count)
			|| count <= 0 || count > ND_SOUNDING_MAX_ENTRIES)
		{
			return std::numeric_limits<float>::quiet_NaN();
		}
		for (int32_t index = 0; index < count; ++index)
		{
			int32_t midi = -1;
			if (!TryRead(detector + DETECTOR_SOUNDING_TABLE + static_cast<uintptr_t>(index) * 8, midi)) break;
			if (midi != pitch) continue;
			float strength = 0.0f;
			if (!TryRead(detector + DETECTOR_SOUNDING_TABLE + static_cast<uintptr_t>(index) * 8 + 4, strength)) break;
			return strength;
		}
		return std::numeric_limits<float>::quiet_NaN();
	}
	void SnapshotNdSoundingTable(std::array<std::pair<int32_t, float>, 64>& table, int32_t& tableCount)
	{
		tableCount = 0;
		uintptr_t root = 0, arrangement = 0, detector = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return;
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0) return;
		if (!TryRead(arrangement + DETECTION_DETECTOR, detector) || detector == 0) return;
		int32_t count = 0;
		if (!TryRead(detector + DETECTOR_SOUNDING_COUNT, count)
			|| count <= 0 || count > ND_SOUNDING_MAX_ENTRIES) return;
		for (int32_t index = 0; index < count && tableCount < static_cast<int32_t>(table.size()); ++index)
		{
			int32_t midi = -1;
			float strength = 0.0f;
			if (!TryRead(detector + DETECTOR_SOUNDING_TABLE + static_cast<uintptr_t>(index) * 8, midi)
				|| !TryRead(detector + DETECTOR_SOUNDING_TABLE + static_cast<uintptr_t>(index) * 8 + 4, strength)) break;
			if (!std::isfinite(strength)) continue;
			table[tableCount++] = { midi, strength };
		}
	}
	void TrackSameTimeGroup()
	{
		if (selectedRecord == 0 || selectedRecord == sameTimeTrackedRecord) return;
		sameTimeTrackedRecord = selectedRecord;
		sameTimeSiblingTicks = 0;
		const bool single = selectedChordId == -1 && selectedString >= 0 && selectedString < 6;
		const uint32_t stringBit = single ? (1u << selectedString) : 0u;
		if (single && sameTimeGroupTime >= 0.0f && sameTimeGroupPitchCount > 0
			&& std::fabs(selectedRecordTime - sameTimeGroupTime) < SAME_TIME_GROUP_EPSILON_SECONDS
			&& (sameTimeGroupStrings & stringBit) == 0)
		{
			isSameTimeSibling = true;
			sameTimeGroupStrings |= stringBit;
			LOG_INFO("(NBN SAME TIME) record=0x" << std::hex << selectedRecord << std::dec
				<< " string=" << selectedString << " fret=" << selectedFret << " shares time "
				<< std::fixed << std::setprecision(3) << selectedRecordTime << std::defaultfloat
				<< " with " << sameTimeGroupPitchCount << " committed note(s); accepted if its pitch rose"
				<< " in the sounding table since the group started." << std::endl);
			return;
		}
		isSameTimeSibling = false;
		sameTimeGroupTime = single ? selectedRecordTime : -1.0f;
		sameTimeGroupStrings = stringBit;
		sameTimeGroupPitchCount = 0;
		SnapshotNdSoundingTable(sameTimeBaseline, sameTimeBaselineCount);
	}
	void RecordSameTimeGroupCommit()
	{
		if (selectedChordId != -1 || sameTimeGroupTime < 0.0f || expectedMidi < 0
			|| std::fabs(selectedRecordTime - sameTimeGroupTime) >= SAME_TIME_GROUP_EPSILON_SECONDS
			|| sameTimeGroupPitchCount >= static_cast<int>(sameTimeGroupPitches.size())) return;
		sameTimeGroupPitches[sameTimeGroupPitchCount++] = expectedMidi;
	}
	bool ConfirmSameTimeSibling()
	{
		if (!isSameTimeSibling || sameTimeTrackedRecord != selectedRecord || expectedMidi < 0
			|| selectedChordId != -1 || isBendTarget || isConfirmingLegatoRun) return false;
		for (int index = 0; index < sameTimeGroupPitchCount; ++index)
		{
			const int member = sameTimeGroupPitches[index];
			if (member == expectedMidi || NoteByNote::RingingNoteMasksTarget(expectedMidi, member))
			{
				LOG_INFO("(NBN SAME TIME) Sibling pitch " << expectedMidi << " is masked by committed member "
					<< member << "; it needs its own attack." << std::endl);
				isSameTimeSibling = false;
				return false;
			}
		}
		const int nativePitch = expectedMidi - NativeFrameOffset();
		const float strength = ReadNdSoundingStrength(nativePitch);
		float baseline = 0.0f;
		for (int32_t index = 0; index < sameTimeBaselineCount; ++index)
		{
			if (sameTimeBaseline[index].first == nativePitch)
			{
				baseline = sameTimeBaseline[index].second > 0.0f ? sameTimeBaseline[index].second : 0.0f;
				break;
			}
		}
		if (strength < 0.0f || strength < 2.0f * baseline)
		{
			sameTimeSiblingTicks = 0;
			return false;
		}
		if (++sameTimeSiblingTicks < SAME_TIME_SIBLING_CONFIRM_TICKS) return false;
		LOG_INFO("(NBN SAME TIME) Accepted sibling pitch " << expectedMidi << " (native " << nativePitch
			<< ", sounding strength "
			<< strength << ", baseline " << baseline << ") with its same-time partner; no second attack needed."
			<< std::endl);
		isSameTimeSibling = false;
		return true;
	}

	double CurrentNdStreakSeconds()
	{
		if (!hasNdSoundingStreak) return 0.0;
		return std::chrono::duration<double>(
			std::chrono::steady_clock::now() - ndSoundingStreakStart).count();
	}

	int TickNdSoundingAcceptance(int expectedPitch)
	{
		if (!isNdAcceptEnabled || expectedPitch < 0) return -1;
		if (!sawSpikeDuringHold)
		{
			hasNdSoundingStreak = false;
			return -1;
		}
		const float strength = ReadNdSoundingStrength(expectedPitch);
		if (strength < 0.0f)
		{
			hasNdSoundingStreak = false;
			return -1;
		}
		ndSoundingLastStrength = strength;
		if (!hasNdSoundingStreak)
		{
			hasNdSoundingStreak = true;
			ndSoundingStreakStart = std::chrono::steady_clock::now();
			return -1;
		}
		const double held = CurrentNdStreakSeconds();
		if (held < ND_SOUNDING_HOLD_SECONDS) return -1;
		LOG_INFO("(NBN ND) Native sounding-table accept: expected " << expectedPitch
			<< " above threshold for " << std::fixed << std::setprecision(3) << held
			<< "s (strength " << ndSoundingLastStrength << "). The lesson primitive"
			<< " accepted where the onset edge did not." << std::endl);
		hasNdSoundingStreak = false;
		return expectedPitch;
	}
	constexpr uintptr_t RING_FRAME_PAIRS = 0x1C;
	constexpr uintptr_t RING_FRAME_PAIR_COUNT = 0xBC;
	constexpr uintptr_t RING_FRAME_LEVEL = 0x700;
	constexpr uintptr_t RING_FRAME_ONSET_FLAG = 0x7B5;

	bool TryDescribeCurrentAnalysisFrame(char* buffer, size_t bufferLength)
	{
		if (buffer == nullptr || bufferLength == 0) return false;
		buffer[0] = '\0';
		uintptr_t root = 0, arrangement = 0, engine = 0, detector = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return false;
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0) return false;
		if (!TryRead(arrangement + DETECTION_ENGINE, engine) || engine == 0) return false;
		if (!TryRead(engine + DETECTION_DETECTOR, detector) || detector == 0) return false;
		uintptr_t ring = 0;
		int32_t writeIndex = 0;
		int32_t capacity = 0;
		if (!TryRead(detector + DETECTOR_RING_BUFFER, ring) || ring == 0
			|| !TryRead(detector + DETECTOR_RING_INDEX, writeIndex)
			|| !TryRead(detector + DETECTOR_RING_CAPACITY, capacity)
			|| writeIndex < 0 || capacity <= 0 || writeIndex >= capacity
			|| capacity > DETECTOR_RING_MAX_CAPACITY)
		{
			return false;
		}
		const uintptr_t frame = ring
			+ static_cast<uintptr_t>(writeIndex) * DETECTOR_RING_STRIDE;
		float level = 0.0f;
		int32_t pairCount = 0;
		uint8_t onsetFlag = 0;
		TryRead(frame + RING_FRAME_LEVEL, level);
		TryRead(frame + RING_FRAME_PAIR_COUNT, pairCount);
		TryRead(frame + RING_FRAME_ONSET_FLAG, onsetFlag);
		size_t at = static_cast<size_t>(std::snprintf(buffer, bufferLength,
			"lvl=%.1f onset=%u pairs=%d", level, onsetFlag, pairCount));
		const int32_t reportCount = pairCount < 6 ? pairCount : 6;
		for (int32_t i = 0; i < reportCount && at < bufferLength; ++i)
		{
			int32_t pitch = -1;
			float energy = 0.0f;
			TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8, pitch);
			TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8 + 4, energy);
			at += std::snprintf(buffer + at, bufferLength - at, " %d:%.2f",
				pitch, energy);
		}
		return true;
	}
	bool TryResolveAnalysisRing(uintptr_t& ring, int32_t& writeIndex, int32_t& capacity)
	{
		uintptr_t root = 0, arrangement = 0, engine = 0, detector = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return false;
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0) return false;
		if (!TryRead(arrangement + DETECTION_ENGINE, engine) || engine == 0) return false;
		if (!TryRead(engine + DETECTION_DETECTOR, detector) || detector == 0) return false;
		return TryRead(detector + DETECTOR_RING_BUFFER, ring) && ring != 0
			&& TryRead(detector + DETECTOR_RING_INDEX, writeIndex)
			&& TryRead(detector + DETECTOR_RING_CAPACITY, capacity)
			&& writeIndex >= 0 && capacity > 0 && writeIndex < capacity
			&& capacity <= DETECTOR_RING_MAX_CAPACITY;
	}
	int JudgeMidi()
	{
		if (expectedMidi >= 0) return expectedMidi;
		return gatePhase == GatePhase::Armed ? armedExpectedMidi : -1;
	}

	bool IsPlainPickedTarget()
	{
		return expectedMidi >= 0 && selectedChordId == -1 && !isBendTarget
			&& !isBendChildTarget && (!isLegatoTarget || isHammerOnTarget) && !isConfirmingLegatoRun;
	}

	void TickPickedAttackStream();
	uint64_t ClaimSuccessorStrum()
	{
		if (successorStrumSample == 0) return 0;
		if ((successorStrumRecord != 0 && successorStrumRecord != selectedRecord)
			|| !rawAttackStreamAvailable
			|| !NoteByNote::StrumHandoffIsFresh(successorStrumSample, latestRawAudioSample, pickSampleRate))
		{
			successorStrumSample = 0;
			successorStrumRecord = 0;
			return 0;
		}
		successorStrumRecord = selectedRecord;
		return successorStrumSample;
	}

	void ResetChordOnsetEvidenceAnchor()
	{
		if (selectedChordId >= 0)
		{
			TickPickedAttackStream();
			chordHoldNeedsCaptureBoundary = !rawAttackStreamAvailable;
			const uint64_t inheritedStrum = ClaimSuccessorStrum();
			chordAttacks.BeginHold(inheritedStrum != 0 ? inheritedStrum - 1 : latestRawAudioSample);
			const bool inherited = inheritedStrum != 0
				&& chordAttacks.Inherit(inheritedStrum, latestRawAudioSample);
			LOG_INFO("(NBN CHORD HOLD) Fresh attack must start after sample=" << chordAttacks.GetConsumedThroughSample()
				<< " capturePending=" << chordHoldNeedsCaptureBoundary
				<< " inheritedStrum=" << (inherited ? inheritedStrum : 0)
				<< " record=0x" << std::hex << selectedRecord << std::dec << std::endl);
		}
		onsetScanFramesSinceLatch = 0;
		hasOnsetScanAnchor = false;
		onsetScanRingIndex = -1;
		uintptr_t ring = 0;
		int32_t writeIndex = 0;
		int32_t capacity = 0;
		if (!TryResolveAnalysisRing(ring, writeIndex, capacity)) return;
		onsetScanRingIndex = writeIndex;
		hasOnsetScanAnchor = true;
	}

	bool TryMeasureOnsetRise(uintptr_t ring, int32_t index, int32_t capacity,
		float level, float& rise)
	{
		rise = 0.0f;
		if (!std::isfinite(level) || capacity <= ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES) return false;
		const int32_t baseline = (index - ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES + capacity) % capacity;
		float priorLevel = 0.0f;
		if (!TryRead(ring + static_cast<uintptr_t>(baseline) * DETECTOR_RING_STRIDE
			+ RING_FRAME_LEVEL, priorLevel) || !std::isfinite(priorLevel)) return false;
		rise = level - priorLevel;
		return true;
	}

	bool ReadPickedAttackBaseline(NoteByNote::PickedAttack& attack, int midi, float& power)
	{
		if (midi < 0 || midi > 127 || attack.sampleRate == 0) return false;
		const int slot = midi == attack.candidateMidi ? 0 : 1;
		if (attack.baselineMidi[slot] == midi)
		{
			power = attack.baselinePower[slot];
			return true;
		}
		const double frequency = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
		const uint64_t count = static_cast<uint64_t>((frequency >= 330.0 ? 0.02f : 0.15f) * attack.sampleRate);
		if (attack.minimumSample <= count) return false;
		ResearchProtocol::RawNoteConfirmation raw;
		if (!ResearchProbeRuntime::QueryRawNoteConfirmation(frequency, raw,
			attack.minimumSample - count, attack.minimumSample)) return false;
		attack.baselineMidi[slot] = midi;
		attack.baselinePower[slot] = raw.attackPower;
		attack.baselineMinusPower[slot] = raw.attackMinusPower;
		attack.baselinePlusPower[slot] = raw.attackPlusPower;
		power = raw.attackPower;
		return true;
	}

	void ConfirmPickedAttack(NoteByNote::PickedAttack* attack, uint64_t maximumSample);
	constexpr double ATTACK_EVIDENCE_SECONDS = 0.30;
	void SampleAttackEvidence()
	{
		if (pickedAttacks.Count() == 0 || latestRawAudioSample == 0) return;
		bool needed = false;
		for (unsigned index = 0; index < pickedAttacks.Count() && !needed; ++index)
		{
			const auto* attack = pickedAttacks.At(index);
			needed = attack != nullptr && attack->sampleRate != 0
				&& latestRawAudioSample <= attack->minimumSample
					+ static_cast<uint64_t>(ATTACK_EVIDENCE_SECONDS * attack->sampleRate);
		}
		if (!needed) return;
		const int loudest = QueryNativeLoudestPlayedNote();
		std::array<std::pair<int32_t, float>, 64> table = {};
		int32_t tableCount = 0;
		SnapshotNdSoundingTable(table, tableCount);
		float threshold = ND_STRENGTH_THRESHOLD_FALLBACK;
		if (!TryRead(ND_STRENGTH_THRESHOLD_GLOBAL, threshold) || !std::isfinite(threshold))
			threshold = ND_STRENGTH_THRESHOLD_FALLBACK;
		for (unsigned index = 0; index < pickedAttacks.Count(); ++index)
		{
			auto* attack = pickedAttacks.At(index);
			if (attack == nullptr || attack->sampleRate == 0 || latestRawAudioSample > attack->minimumSample
				+ static_cast<uint64_t>(ATTACK_EVIDENCE_SECONDS * attack->sampleRate)) continue;
			const bool first = !attack->heardSampled;
			attack->heardSampled = true;
			attack->heardThreshold = threshold;
			for (int32_t entry = 0; entry < tableCount; ++entry)
			{
				auto* heard = attack->FindOrAddHeard(table[entry].first);
				if (heard == nullptr) continue;
				const float strength = table[entry].second > 0.0f ? table[entry].second : 0.0f;
				if (first) heard->firstStrength = strength;
				if (strength > heard->peakStrength) heard->peakStrength = strength;
			}
			if (loudest >= 0 && !first)
			{
				auto* heard = attack->FindOrAddHeard(loudest);
				if (heard != nullptr && heard->loudestTicks < 0xFFFF) ++heard->loudestTicks;
			}
		}
	}
	bool AttackHeardByNative(const NoteByNote::PickedAttack& attack, int midi)
	{
		const auto* heard = attack.FindHeard(midi);
		if (heard == nullptr) return false;
		const bool rose = heard->peakStrength > attack.heardThreshold
			&& heard->peakStrength >= 2.0f * heard->firstStrength;
		const bool isRinger = previousExpectedMidi >= 0 && midi == previousExpectedMidi;
		if (heard->loudestTicks > 0 && (!isRinger || rose)) return true;
		return rose && !NoteByNote::RingingNoteMasksTarget(midi, previousExpectedMidi);
	}
	uintptr_t retargetedAttacksRecord = 0;
	void RetargetBufferedAttacks()
	{
		if (selectedRecord == 0 || selectedRecord == retargetedAttacksRecord) return;
		retargetedAttacksRecord = selectedRecord;
		for (unsigned index = 0; index < pickedAttacks.Count(); ++index)
		{
			auto* attack = pickedAttacks.At(index);
			if (attack == nullptr || attack->rejected || attack->confirmedMidi < 0) continue;
			LOG_INFO("(NBN PICK BUFFER) Re-judging attack=" << attack->time << " (confirmed as "
				<< attack->confirmedMidi << ") against the new target from its recorded evidence." << std::endl);
			attack->confirmedMidi = -1;
			attack->confirmedSample = 0;
			attack->candidateMidi = -1;
			attack->strumBelongsToSuccessor = false;
		}
	}
	bool MeasurePickClickForAttack(uint64_t attackSample, NoteByNote::PickClickReading& reading)
	{
		if (attackSample == 0) return false;
		RawPitchVerifier::AudioSnapshot snapshot;
		if (!RawPitchVerifier::CaptureSnapshot(snapshot) || snapshot.sampleRate == 0
			|| snapshot.endSampleIndex < snapshot.sampleCount) return false;
		const uint64_t snapshotStart = snapshot.endSampleIndex - snapshot.sampleCount;
		if (attackSample < snapshotStart + NoteByNote::PickClickPreSamples(snapshot.sampleRate)
			|| attackSample + NoteByNote::PickClickPostSamples(snapshot.sampleRate) > snapshot.endSampleIndex) return false;
		return NoteByNote::MeasurePickClick(snapshot.samples, snapshot.sampleCount,
			static_cast<uint32_t>(attackSample - snapshotStart), snapshot.sampleRate, reading);
	}

	void LogPickClickShadow(uint64_t attackSample, int midi, const char* outcome)
	{
		static uint64_t loggedAttack = 0;
		if (attackSample == 0 || attackSample == loggedAttack) return;
		NoteByNote::PickClickReading reading;
		if (!MeasurePickClickForAttack(attackSample, reading)) return;
		loggedAttack = attackSample;
		LOG_INFO("(NBN PICK CLICK SHADOW) attack=" << attackSample << " midi=" << midi << " outcome=" << outcome
			<< std::fixed << std::setprecision(1) << " pre=" << reading.preDb << " post=" << reading.postDb
			<< " rise=" << reading.riseDb() << " preHf=" << reading.preHfDb << " postHf=" << reading.postHfDb
			<< " hfRise=" << reading.hfRiseDb() << " dB" << std::defaultfloat << std::endl);
	}
	void ConfirmLatestPickedAttack(uint64_t maximumSample = 0)
	{
		const unsigned count = pickedAttacks.Count();
		for (unsigned index = 0; index + 1 < count; ++index)
		{
			auto* older = pickedAttacks.At(index);
			if (older == nullptr || older->confirmedMidi >= 0 || older->rejected || older->sampleRate == 0) continue;
			if (older->awaitingTargetAfter != 0 && selectedRecord == older->awaitingTargetAfter) continue;
			const uint32_t window = NoteByNote::PickConfirmationWindowSamples(older->candidateMidi, older->sampleRate);
			if (latestRawAudioSample < older->minimumSample + window) continue;
			ConfirmPickedAttack(older, 0);
			if (older->confirmedMidi < 0)
			{
				older->rejected = true;
				LOG_INFO("(NBN PICK BUFFER) Kept cut-short attack=" << older->time
					<< " did not confirm over its full window; discarded." << std::endl);
			}
		}
		ConfirmPickedAttack(pickedAttacks.Latest(), maximumSample);
	}

	void ConfirmPickedAttack(NoteByNote::PickedAttack* attack, uint64_t maximumSample)
	{
		if (attack == nullptr || attack->confirmedMidi >= 0 || attack->rejected) return;
		bool isPlayAhead = attack->playAheadFrom >= 0;
		if (attack->awaitingTargetAfter != 0)
		{
			if (selectedRecord == 0 || selectedRecord == attack->awaitingTargetAfter || JudgeMidi() < 0) return;
			attack->awaitingTargetAfter = 0;
			attack->candidateMidi = JudgeMidi();
			LOG_INFO("(NBN PICK BUFFER) Play-ahead attack=" << attack->time
				<< " now judged against the next target midi=" << expectedMidi << std::endl);
		}
		if (attack->candidateMidi < 0 && JudgeMidi() >= 0) attack->candidateMidi = JudgeMidi();
		const int frameOffset = NativeFrameOffset();
		const int nativeRaw = QueryNativeLoudestPlayedNote();
		const int nativeMidi = nativeRaw >= 0 ? nativeRaw + frameOffset : nativeRaw;   // played frame
		const int candidates[] = { attack->candidateMidi, nativeMidi };
		static uint64_t loggedRejectAttack = 0;
		static const char* loggedRejectReason = nullptr;
		auto logTargetReject = [&](int index, const char* reason, const ResearchProtocol::RawNoteConfirmation& raw,
			float targetRise, float neighbourRise)
		{
			if (index != 0 || attack->sampleRate == 0 || raw.endSampleIndex < attack->minimumSample
				|| raw.endSampleIndex - attack->minimumSample
					< NoteByNote::PickConfirmationWindowSamples(candidates[0], attack->sampleRate)) return;
			if (loggedRejectAttack == attack->minimumSample && loggedRejectReason == reason) return;
			loggedRejectAttack = attack->minimumSample;
			loggedRejectReason = reason;
			LOG_INFO("(NBN PICK BUFFER) Not confirmed attack=" << attack->time << " midi=" << candidates[0]
				<< ": " << reason << " (confirmed=" << static_cast<int>(raw.confirmed)
				<< " change=" << raw.attackChange << " targetRise=" << targetRise
				<< " neighbourRise=" << neighbourRise << " native=" << nativeMidi << ")" << std::endl);
			LogPickClickShadow(attack->minimumSample, candidates[0], "refused");
		};
		for (int index = 0; index < 2; ++index)
		{
			const int midi = candidates[index];
			if (midi < 0 || midi > 127 || (index != 0 && midi == candidates[0])) continue;
			if (isPlayAhead && index != 0 && midi == attack->playAheadFrom) continue;
			if (index != 0 && IsBassArrangement() && midi > BASS_HIGHEST_PLAYABLE_MIDI) continue;
			if (index != 0 && IsBassArrangement() && attack->sampleRate != 0 && latestRawAudioSample
				< attack->minimumSample + static_cast<uint64_t>(ATTACK_EVIDENCE_SECONDS * attack->sampleRate)) continue;
			ResearchProtocol::RawNoteConfirmation raw;
			const double frequency = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
			if (!ResearchProbeRuntime::QueryRawNoteConfirmation(frequency, raw, attack->minimumSample, maximumSample))
				continue;
			const bool nativeHearsTarget = nativeMidi == midi || AttackHeardByNative(*attack, midi - frameOffset);
			const bool bassNativeHears = IsBassArrangement() && nativeHearsTarget;
			const bool guitarNativeHears = !IsBassArrangement() && nativeHearsTarget && raw.attackChange >= 0.2f;
			if (IsBassArrangement() && !bassNativeHears)
			{
				logTargetReject(index, "bass: the native detector does not hear this note", raw, 0.0f, 0.0f);
				continue;
			}
			if (raw.confirmed == 0 && !bassNativeHears && !guitarNativeHears)
			{
				logTargetReject(index, "pitch not confirmed in the raw audio", raw, 0.0f, 0.0f);
				continue;
			}
			if (raw.attackChange < 0.2f && !bassNativeHears)
			{
				logTargetReject(index, "no fresh attack at the pitch (change < 0.2)", raw, 0.0f, 0.0f);
				continue;
			}
			{
				const bool rePickOverRing = midi == previousExpectedMidi && raw.attackChange >= 0.5f;
				const bool slideIsPossible = previousSelectedString < 0 || selectedString == previousSelectedString;
				NoteByNote::PickClickReading click;
				if (NoteByNote::PICK_CLICK_REQUIRED && slideIsPossible && !rePickOverRing && !bassNativeHears
					&& MeasurePickClickForAttack(attack->minimumSample, click)
					&& !NoteByNote::HasPickClick(click))
				{
					LogPickClickShadow(attack->minimumSample, midi, "refused-no-click");
					logTargetReject(index, "no pick click at the attack (a slide into the note)", raw, 0.0f, 0.0f);
					continue;
				}
			}
			float baseline = 0.0f;
			if (!ReadPickedAttackBaseline(*attack, midi, baseline)) continue;
			const int slot = midi == attack->candidateMidi ? 0 : 1;
			const float targetRise = std::sqrt(raw.attackPower) - std::sqrt(baseline);
			const float minusRise = std::sqrt(raw.attackMinusPower) - std::sqrt(attack->baselineMinusPower[slot]);
			const float plusRise = std::sqrt(raw.attackPlusPower) - std::sqrt(attack->baselinePlusPower[slot]);
			const float neighbourRise = minusRise > plusRise ? minusRise : plusRise;
			const bool repeatsRingingNote = midi == previousExpectedMidi && raw.attackChange >= 0.5f;
			if (!repeatsRingingNote && !bassNativeHears
				&& neighbourRise > 1e-3f && neighbourRise > (targetRise > 0.0f ? targetRise : 0.0f))
			{
				logTargetReject(index, "a neighbouring semitone rose more than the target", raw,
					targetRise, neighbourRise);
				continue;
			}
			const bool chordRings = lastCommittedChordRecord != 0
				&& researchChordTonesRecord == lastCommittedChordRecord
				&& std::chrono::duration<double>(std::chrono::steady_clock::now()
					- lastCommittedChordAt).count() < COMMITTED_CHORD_RESELECT_GUARD_SECONDS;
			const bool noteRings = hasLastCommitWallClock && previousExpectedMidi >= 0
				&& std::chrono::duration<double>(std::chrono::steady_clock::now()
					- lastCommitWallClock).count() < COMMITTED_CHORD_RESELECT_GUARD_SECONDS;
			const bool chordMasks = chordRings
				&& NoteByNote::RingingTonesMaskTarget(midi, researchChordTones, researchChordToneCount);
			const bool insideChordStrumTail = NoteByNote::IsInsideChordStrumTail(attack->minimumSample,
				lastChordStrumSample, attack->sampleRate);
			if (chordMasks && insideChordStrumTail)
			{
				static uint64_t loggedStrumTailAttack = 0;
				if (loggedStrumTailAttack != attack->minimumSample)
				{
					loggedStrumTailAttack = attack->minimumSample;
					LOG_INFO("(NBN PICK BUFFER) Withheld attack=" << attack->time << " midi=" << midi
						<< ": inside the masking chord's strum tail ("
						<< (attack->minimumSample - lastChordStrumSample) * 1000 / attack->sampleRate
						<< " ms after strum=" << lastChordStrumSample << "); native loudest=" << nativeMidi
						<< " targetRise=" << targetRise << std::endl);
				}
				continue;
			}
			const char* nativeRequired = nullptr;
			if (chordMasks)
				nativeRequired = "the just-committed chord rings a masking partial";
			else if (noteRings && NoteByNote::RingingNoteMasksTarget(midi, previousExpectedMidi))
				nativeRequired = "the just-committed note rings a masking partial";
			else if (NoteByNote::ChordToSingleAttackNeedsNative(previousSelectedString, isLegatoTarget,
				attack->minimumSample, lastChordStrumSample, attack->sampleRate))
				nativeRequired = "chord->single inside the strum tail needs its own attack";
			if (nativeRequired != nullptr)
			{
				DetectorGateSample gateSample;
				const int pitchNow = TryReadDetectorGates(gateSample) ? gateSample.pitchNow : -1;
				if (!NoteByNote::NativeCorroboratesPick(nativeMidi, midi)
					&& !NoteByNote::NativeCorroboratesPick(pitchNow, midi))
				{
					static uint64_t loggedWithheldAttack = 0;
					if (loggedWithheldAttack != attack->minimumSample
						&& (loggedWithheldAttack = attack->minimumSample) != 0)
						LOG_INFO("(NBN PICK BUFFER) Withheld attack=" << attack->time << " midi=" << midi
							<< ": " << nativeRequired << "; native loudest=" << nativeMidi
							<< " pitchNow=" << pitchNow << " targetRise=" << targetRise << std::endl);
					continue;
				}
			}
			const auto strategy = CurrentTechniqueStrategy();
			NoteByNote::DetectionFeedback feedback;
			feedback.nativeMidi = nativeMidi;
			feedback.enhancedMidi = midi;
			if (strategy != DetectionStrategy::NativeOnly)
			{
				ResearchProtocol::MlNoteEvidence ml;
				const bool available = ResearchProbeRuntime::QueryMlNoteEvidence(
					midi, 0.5f, attack->minimumMlSample, ml);
				if (available)
				{
					feedback.mlMidi = ml.observedMidi;
					if (ml.verdict == ResearchProtocol::MlNoteVerdict::Confirmed)
					{
						feedback.mlMidi = midi;
						feedback.mlRole = NoteByNote::DetectorRole::Confirmed;
					}
				}
				if (strategy == DetectionStrategy::MlOnly
					&& (!available || ml.verdict != ResearchProtocol::MlNoteVerdict::Confirmed)) continue;
				if (available && ml.verdict == ResearchProtocol::MlNoteVerdict::Conflicting
					&& ml.confidence >= ML_BLEND_VETO_CONF && ml.observedMidi >= 0
					&& (ml.observedMidi - midi) % 12 != 0)
				{
					continue;
				}
			}
			NoteByNote::SetPickedDetectorRoles(feedback, NoteByNote::NativeCorroboratesPick(nativeMidi, midi),
				feedback.mlRole == NoteByNote::DetectorRole::Confirmed);
			attack->strumBelongsToSuccessor = NoteByNote::StrumBelongsToSuccessor(
				attack->cutShortPredecessorSample, nativeMidi, midi);
			attack->confirmedMidi = midi;
			attack->confirmedSample = raw.endSampleIndex;
			attack->feedback = feedback;
			LOG_INFO("(NBN PICK BUFFER) Confirmed attack=" << attack->time << " midi=" << midi
				<< " samples=" << attack->minimumSample << ".." << raw.endSampleIndex
				<< " change=" << raw.attackChange << " targetRise=" << targetRise << " neighbourRise=" << neighbourRise
				<< " native=" << nativeMidi << " queued=" << pickedAttacks.Count()
				<< (attack->strumBelongsToSuccessor ? " ownedBy=successor (cut-short predecessor pick; strum left for the next target)" : "")
				<< std::endl);
			LogPickClickShadow(attack->minimumSample, midi, "confirmed");
			return;
		}
	}

	void TickPickedAttackStream()
	{
		RawPitchVerifier::RawAttackBatch batch;
		if (!ResearchProbeRuntime::QueryRawAttacks(pickScanSample, batch))
		{
			if (rawAttackStreamAvailable)
				LOG_INFO("(NBN RAW ATTACK) Snapshot unavailable; preserving attack ownership until capture resumes." << std::endl);
			rawAttackStreamAvailable = false;
			return;
		}
		rawAttackStreamAvailable = true;
		latestRawAudioSample = batch.endSampleIndex;
		if (chordHoldNeedsCaptureBoundary)
		{
			chordAttacks.BeginHold(batch.endSampleIndex);
			chordHoldNeedsCaptureBoundary = false;
		}
		const double now = static_cast<double>(batch.endSampleIndex) / batch.sampleRate;
		if (batch.count != 0 && (batch.endSampleIndex < pickDiagnosticSample
			|| batch.endSampleIndex - pickDiagnosticSample >= batch.sampleRate))
		{
			const auto& latest = batch.frames[batch.count - 1];
			LOG_INFO("(NBN RAW ATTACK) sample=" << latest.endSampleIndex
				<< " inputDb=" << latest.inputLevelDb
				<< " queued=" << pickedAttacks.Count() << std::endl);
			pickDiagnosticSample = batch.endSampleIndex;
		}
		if (batch.reset != 0 || pickScanSample == 0 || pickSampleRate != batch.sampleRate || batch.endSampleIndex < pickScanSample
			|| (expectedMidi >= 0 && !IsPlainPickedTarget()))
		{
			pickedAttacks.Clear();
			chordAttacks.Clear();
			chordAttacks.BeginHold(batch.endSampleIndex);
			pickScanSample = batch.endSampleIndex;
			pickSampleRate = batch.sampleRate;
			return;
		}
		if (batch.count != 0 && batch.frames[0].endSampleIndex - pickScanSample > batch.sampleRate / 100)
		{
			LOG_INFO("(NBN PICK BUFFER) Raw attack scan overrun; clearing uncertain attacks." << std::endl);
			pickedAttacks.Clear();
			chordAttacks.Clear();
			chordAttacks.BeginHold(batch.endSampleIndex);
			pickScanSample = batch.endSampleIndex;
			return;
		}
		const unsigned expired = pickedAttacks.Expire(now);
		if (expired != 0) LOG_INFO("(NBN PICK BUFFER) Expired " << expired << " old attacks." << std::endl);
		uint32_t attackNoteMask = 0;
		if (selectedChordId >= 0 && !TryRead(selectedRecord + RECORD_MASK, attackNoteMask))
		{
			LOG_ERROR("(NBN CHORD ATTACK) Cannot read selected chord technique mask." << std::endl);
			return;
		}
		const bool fretHandMuted = selectedChordId >= 0 && NoteByNote::ChordAttackGate::IsFretHandMuted(attackNoteMask);
		for (uint32_t index = 0; index < batch.count; ++index)
		{
			const auto& frame = batch.frames[index];
			pickScanSample = frame.endSampleIndex;
			const uint64_t attackSample = frame.attackSampleIndex;
			if (attackSample == 0) continue;
			bool changed = false;
			const int singleCandidates[] = { expectedMidi, QueryNativeLoudestPlayedNote() };
			const bool isChord = selectedChordId >= 0;
			const int* candidates = isChord ? researchChordTones : singleCandidates;
			const int candidateCount = isChord
				? (researchChordTonesRecord == selectedRecord ? researchChordToneCount : 0) : 2;
			if (isChord && (fretHandMuted || candidateCount > 0))
			{
				RawPitchVerifier::AudioSnapshot snapshot;
				if (RawPitchVerifier::CaptureSnapshot(snapshot) && snapshot.sampleRate == batch.sampleRate)
				{
					const uint32_t window = snapshot.sampleRate / 20;
					const uint64_t start = snapshot.endSampleIndex - snapshot.sampleCount;
					if (attackSample >= start + window * 2 && attackSample + window <= snapshot.endSampleIndex)
					{
						const float* frames = snapshot.samples + attackSample - start - window * 2;
						changed = (fretHandMuted || IsBassArrangement())
							? NoteByNote::ConfirmMutedAttack(frames, window, snapshot.sampleRate)
							: NoteByNote::ConfirmChordAttack(frames, window, snapshot.sampleRate,
								candidates, candidateCount);
						if (changed && !fretHandMuted
							&& !NoteByNote::UsesNoPickLegatoAcceptance(attackNoteMask))
						{
							NoteByNote::ChordAttackEnvelope envelope;
							const uint32_t offset = static_cast<uint32_t>(attackSample - start);
							if (NoteByNote::MeasureChordAttackEnvelope(snapshot.samples, offset,
								snapshot.sampleCount, snapshot.sampleRate, envelope))
							{
								const bool hammer = NoteByNote::LooksLikeFretHandHammer(envelope);
								const bool noStrumRise = !hammer && NoteByNote::LacksStrumRise(envelope);
								LOG_INFO("(NBN CHORD ATTACK) envelope sample=" << attackSample
									<< " peakBefore=" << envelope.peakBeforeDb
									<< " floorBefore=" << envelope.floorBeforeDb
									<< " after=" << envelope.afterDb
									<< (hammer ? " REJECTED fret-hand hammer" : "")
									<< (noStrumRise ? " REJECTED no strum rise (slide or re-fret)" : "") << std::endl);
								if (hammer || noStrumRise) changed = false;
							}
						}
					}
				}
			}
			if (!isChord && IsBassArrangement()) changed = true;
			for (int candidate = 0; !isChord && !changed && candidate < candidateCount; ++candidate)
			{
				const int midi = candidates[candidate];
				if (midi < 0 || midi > 127) continue;
				ResearchProtocol::RawNoteConfirmation change;
				const double frequency = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
				ResearchProbeRuntime::QueryRawNoteConfirmation(frequency, change, attackSample,
					attackSample + batch.sampleRate / (frequency < 330.0 ? 20 : 50));
				if (change.attackChange >= 0.2f) { changed = true; break; }
			}
			if (!changed)
			{
				LOG_INFO("(NBN RAW ATTACK) Rejected sustain fluctuation sample=" << attackSample << std::endl);
				continue;
			}
			if (selectedChordId >= 0)
			{
				const bool queued = chordAttacks.Observe(attackSample);
				LOG_INFO("(NBN CHORD ATTACK) " << (queued ? "Queued" : "Rejected consumed audio")
					<< " sample=" << attackSample << " delivered=" << frame.endSampleIndex
					<< " consumedThrough=" << chordAttacks.GetConsumedThroughSample()
					<< " detector=hfc fretHandMuted=" << fretHandMuted << " record=0x" << std::hex
					<< selectedRecord << std::dec << std::endl);
				continue;
			}
			if (attackSample <= pickAttackFloorSample)
			{
				LOG_INFO("(NBN PICK BUFFER) Ignored attack sample=" << attackSample
					<< ": audio the chord commit already consumed (through=" << pickAttackFloorSample
					<< ")." << std::endl);
				continue;
			}
			NoteByNote::PickedAttack attack;
			attack.minimumSample = attackSample;
			attack.sampleRate = batch.sampleRate;
			const double time = static_cast<double>(attackSample) / batch.sampleRate;
			attack.time = time;
			ConfirmLatestPickedAttack(attack.minimumSample);
			mlRescueRecord = 0;
			enhancedRescueFeedback = {};
			attack.minimumMlSample = ResearchProbeRuntime::GetMlAudioSampleIndex();
			const bool targetAlreadyPicked = IsBassArrangement()
				&& acceptedPickRecord != 0 && acceptedPickRecord == selectedRecord;
			attack.candidateMidi = targetAlreadyPicked ? -1 : JudgeMidi();
			if (targetAlreadyPicked)
			{
				attack.awaitingTargetAfter = selectedRecord;
				attack.playAheadFrom = expectedMidi;
			}
			float baseline = 0.0f;
			ReadPickedAttackBaseline(attack, attack.candidateMidi, baseline);
			ReadPickedAttackBaseline(attack, QueryNativeLoudestPlayedNote(), baseline);
			const auto* pending = pickedAttacks.Latest();
			if (pending != nullptr && pending->confirmedMidi < 0)
			{
				if (NoteByNote::PredecessorAttackWasCutShort(pending->minimumSample,
					attack.minimumSample, attack.candidateMidi, batch.sampleRate))
					attack.cutShortPredecessorSample = pending->minimumSample;
				lastUnresolvedAttack = *pending;
				hasLastUnresolvedAttack = true;
				LOG_INFO("(NBN PICK BUFFER) Unresolved attack=" << pending->time
					<< " ended by next attack=" << time
					<< (attack.cutShortPredecessorSample != 0
						? " (cut short before its confirmation window; kept to confirm on its full window)" : "")
					<< std::endl);
			}
			if (!pickedAttacks.Push(attack))
			{
				LOG_INFO("(NBN PICK BUFFER) Attack queue full or timestamp repeated; rejected attack=" << time << std::endl);
			}
			else LOG_INFO("(NBN PICK BUFFER) Captured attack=" << time << " phase=" << static_cast<int>(gatePhase)
				<< (attack.awaitingTargetAfter != 0 ? " play-ahead" : "")
				<< " sample=" << attack.minimumSample << std::endl);
		}
		SampleAttackEvidence();
		ConfirmLatestPickedAttack();
	}
	bool TryInferFromFollowingPick();

	bool TakeBufferedPick()
	{
		if (acceptedPickRecord == selectedRecord && acceptedPick.confirmedMidi != expectedMidi)
		{
			LOG_INFO("(NBN PICK BUFFER) Dropped an accepted pick that does not match the target ("
				<< acceptedPick.confirmedMidi << " vs " << expectedMidi << ")." << std::endl);
			acceptedPickRecord = 0;
		}
		if (acceptedPickRecord == selectedRecord) return true;
		const unsigned before = pickedAttacks.Count();
		const bool taken = pickedAttacks.Take(expectedMidi, acceptedPick, KeepsMismatchedPicks());
		const unsigned mismatched = before - pickedAttacks.Count() - (taken ? 1 : 0);
		if (mismatched != 0)
		{
			LOG_INFO("(NBN PICK BUFFER) Discarded " << mismatched << " attacks with a different pitch; expected="
				<< expectedMidi << std::endl);
		}
		if (!taken && !TryInferFromFollowingPick()) return false;
		lastTakenPickSample = acceptedPick.minimumSample;
		acceptedPickRecord = selectedRecord;
		LOG_INFO("(NBN PICK BUFFER) Consumed attack=" << acceptedPick.time
			<< " midi=" << acceptedPick.confirmedMidi << " record=" << selectedRecord << std::endl);
		return true;
	}
	void TickOnsetEvidence()
	{
		uintptr_t ring = 0;
		int32_t writeIndex = 0;
		int32_t capacity = 0;
		if (!TryResolveAnalysisRing(ring, writeIndex, capacity)) return;
		if (!hasOnsetScanAnchor)
		{
			onsetScanRingIndex = writeIndex;
			hasOnsetScanAnchor = true;
			return;
		}
		int32_t advanced = writeIndex - onsetScanRingIndex;
		if (advanced < 0) advanced += capacity;
		if (advanced <= 0) return;
		if (advanced > ONSET_SCAN_MAX_FRAMES_PER_TICK)
		{
			onsetScanFramesSinceLatch += static_cast<uint32_t>(
				advanced - ONSET_SCAN_MAX_FRAMES_PER_TICK);
			onsetScanRingIndex += advanced - ONSET_SCAN_MAX_FRAMES_PER_TICK;
			if (onsetScanRingIndex >= capacity) onsetScanRingIndex -= capacity;
			advanced = ONSET_SCAN_MAX_FRAMES_PER_TICK;
		}
		bool loggedRingRejectThisTick = false;
		for (int32_t step = 1; step <= advanced; ++step)
		{
			int32_t index = onsetScanRingIndex + step;
			if (index >= capacity) index -= capacity;
			++onsetScanFramesSinceLatch;
			if (onsetScanFramesSinceLatch < ONSET_EVIDENCE_MIN_FRAMES) continue;
			const uintptr_t frame = ring
				+ static_cast<uintptr_t>(index) * DETECTOR_RING_STRIDE;
			uint8_t onsetFlag = 0;
			float level = 0.0f;
			if (!TryRead(frame + RING_FRAME_ONSET_FLAG, onsetFlag) || onsetFlag == 0) continue;

			if (!TryRead(frame + RING_FRAME_LEVEL, level) || !std::isfinite(level)
				|| level < ONSET_EVIDENCE_LEVEL_FLOOR_DB)
			{
				continue;
			}
			float rise = 0.0f;
			const bool haveBaseline = TryMeasureOnsetRise(ring, index, capacity, level, rise);
			if (!haveBaseline || rise < ONSET_EVIDENCE_RISE_DB)
			{
				if (!loggedRingRejectThisTick)
				{
					loggedRingRejectThisTick = true;
					float history[ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES] = {};
					for (int32_t offset = 1; offset <= ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES; ++offset)
					{
						const int32_t priorIndex = (index - offset + capacity) % capacity;
						if (!TryRead(ring + static_cast<uintptr_t>(priorIndex) * DETECTOR_RING_STRIDE
							+ RING_FRAME_LEVEL, history[offset - 1])) history[offset - 1] = std::nanf("");
					}
					LOG_INFO("(NBN LAS ONSET EVIDENCE) Rejected onset flag as a ring (no"
						<< " rising edge): level " << std::fixed << std::setprecision(1)
						<< level << " dB, rise " << std::showpos << rise << std::noshowpos
						<< " dB; baseline=" << (IsPlainPickedTarget() ? "local-valley" : "four-frames-back")
						<< " (need >= " << ONSET_EVIDENCE_RISE_DB << ") previousLevels=["
						<< history[0] << "," << history[1] << "," << history[2] << "," << history[3] << "], "
						<< onsetScanFramesSinceLatch << " frames after latch." << std::endl);
				}
				continue;
			}
			sawSpikeDuringHold = true;
			LOG_INFO("(NBN LAS ONSET EVIDENCE) Fresh attack frame accepted as attack"
				<< " evidence: onset flag set at level " << std::fixed
				<< std::setprecision(1) << level << " dB (rise " << std::showpos << rise
				<< std::noshowpos << " dB; baseline="
				<< (IsPlainPickedTarget() ? "local-valley" : "four-frames-back") << "), " << onsetScanFramesSinceLatch << " ring frames after the hold"
				<< " latched. The level-spike gate stayed silent (a re-strum over a loud"
				<< " ring moves the meter less than " << DETECTOR_SPIKE_JUMP_DB << " dB)."
				<< std::endl);
			break;
		}
		onsetScanRingIndex = writeIndex;
	}
	bool TryReadDetectorClock(double& clock)
	{
		uintptr_t root = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return false;
		uintptr_t arrangement = 0;
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0)
		{
			return false;
		}
		uintptr_t engine = 0;
		if (!TryRead(arrangement + DETECTION_ENGINE, engine) || engine == 0) return false;
		uintptr_t detector = 0;
		if (!TryRead(engine + DETECTION_DETECTOR, detector) || detector == 0) return false;
		return TryRead(detector + DETECTOR_ANALYSIS_CLOCK, clock);
	}
	const char* DescribeDetectorRefusal(const DetectorGateSample& sample)
	{
		if (!sample.passesLevel && !sample.passesQuality) return "level+quality";
		if (!sample.passesLevel) return "level";
		if (!sample.passesQuality) return "quality";
		if (!sample.hasRing) return "ring-unreadable";
		if (sample.pitchNow <= -1) return "no-pitch";
		if (sample.pitchNow != sample.pitchPrevious) return "unsettled";
		if (sample.pitchNow == sample.dedupe) return "already-consumed";
		return "none-visible";
	}
	void LogTier0ShadowEvidence(int expectedMidi, const char* context);
	void LogTier1ShadowPitch(int expectedMidi);
	bool Tier0ConfirmsExpected(int expectedMidi);
	extern volatile bool g_tier0Enforcement;
	ULONGLONG g_lastAttackSpikeTick = 0;
	ULONGLONG g_flowAdoptTick = 0;
	ULONGLONG g_flowArmedTick = 0;
	void LogDetectorGates(const char* context, bool force)
	{
		const auto now = std::chrono::steady_clock::now();
		DetectorGateSample sample;
		const bool sampleReadable = TryReadDetectorGates(sample);
		if (sampleReadable)
		{
			lastStateGateSample = sample;
			hasLastStateGateSample = true;
			lastStateGateSampleAt = now;
		}
		{
			static long consumedStreakTicks = 0;
			static bool streakSawSpike = false;
			const bool stuckOnExpected = sampleReadable
				&& gatePhase == GatePhase::Holding
				&& sample.passesLevel
				&& sample.passesQuality
				&& sample.hasRing
				&& sample.pitchNow >= 0
				&& sample.pitchNow == sample.dedupe
				&& sample.pitchNow == expectedMidi;
			if (stuckOnExpected)
			{
				++consumedStreakTicks;
				if (spikeBurstTicksRemaining > 0) streakSawSpike = true;
			}
			else
			{
				consumedStreakTicks = 0;
				streakSawSpike = false;
			}
			if (consumedStreakTicks >= 1 && streakSawSpike)
			{
				consumedStreakTicks = 0;
				streakSawSpike = false;
				if (TryWriteDedupeGlobal(-1))
				{
					LOG_INFO("(NBN LAS DEDUPE) reset: expected " << expectedMidi
						<< " was consumed without a commit, repluck reports were"
						<< " suppressed, and attack energy during the stall says the"
						<< " player is replucking; the next onset query re-reports it."
						<< std::endl);
				}
			}
		}

		if (sampleReadable)
		{
			const bool spiked = hasLastTickDetectorLevel
				&& std::isfinite(sample.level)
				&& sample.level - lastTickDetectorLevel >= DETECTOR_SPIKE_JUMP_DB;
			if (spiked)
			{
				if (gatePhase == GatePhase::Holding && holdTickCount >= 8)
				{
					sawSpikeDuringHold = true;
					sawLevelSpikeDuringHold = true;
				}
				spikeBurstTicksRemaining = DETECTOR_SPIKE_BURST_TICKS;
				g_lastAttackSpikeTick = GetTickCount64();
				if (!IsPlainPickedTarget())
				{
					mlRescueRecord = 0;
					enhancedRescueFeedback = {};
				}
				LOG_INFO("(NBN LAS SPIKE) jump=" << std::fixed << std::setprecision(1)
					<< (sample.level - lastTickDetectorLevel)
					<< " exp=" << expectedMidi
					<< " now=" << sample.pitchNow
					<< " prev=" << sample.pitchPrevious
					<< " dedupe=" << sample.dedupe
					<< " lvl=" << sample.level
					<< " q=" << std::setprecision(0) << sample.quality
					<< " loud=" << sample.currentNote
					<< std::endl);
			}
			else if (spikeBurstTicksRemaining > 0)
			{
				--spikeBurstTicksRemaining;
				LOG_INFO("(NBN LAS SPIKE+) exp=" << expectedMidi
					<< " now=" << sample.pitchNow
					<< " prev=" << sample.pitchPrevious
					<< " dedupe=" << sample.dedupe
					<< " lvl=" << std::fixed << std::setprecision(1) << sample.level
					<< " q=" << std::setprecision(0) << sample.quality
					<< " loud=" << sample.currentNote
					<< std::endl);
			}
			lastTickDetectorLevel = sample.level;
			hasLastTickDetectorLevel = std::isfinite(sample.level);
		}
		else
		{
			hasLastTickDetectorLevel = false;
		}
		if (gatePhase == GatePhase::Holding && !IsPlainPickedTarget() && !sawSpikeDuringHold)
		{
			TickOnsetEvidence();
		}
		if (gatePhase == GatePhase::Holding && NbnInputPresent())
		{
			static ULONGLONG lastCompareTick = 0;
			const ULONGLONG nowCompareTick = GetTickCount64();
			if (nowCompareTick - lastCompareTick >= 300)
			{
				lastCompareTick = nowCompareTick;
				SampleDetectionComparison();
			}
		}

		if (!force)
		{
			if (hasDetectorSampleAnchor
				&& std::chrono::duration<double>(now - detectorSampleAnchor).count()
					< DETECTOR_SAMPLE_INTERVAL_SECONDS)
			{
				return;
			}
			if (!verboseTrace || !NbnInputPresent()) return;
		}
		detectorSampleAnchor = now;
		hasDetectorSampleAnchor = true;

		if (!sampleReadable)
		{
			LOG_INFO("(NBN LAS DETECT) refusing=chain-unreadable context=" << context
				<< " expected=" << expectedMidi
				<< ". The detection engine could not be resolved from 0x0135F57C, so no"
				<< " statement can be made about the input gates." << std::endl);
			hasLastSampledRingIndex = false;
			return;
		}

		const char* ringMotion = "unknown";
		if (sample.hasRing)
		{
			if (!hasLastSampledRingIndex) ringMotion = "first-sample";
			else if (sample.ringIndex == lastSampledRingIndex) ringMotion = "STALLED";
			else ringMotion = "advancing";
		}
		lastSampledRingIndex = sample.ringIndex;
		hasLastSampledRingIndex = sample.hasRing;

		LOG_INFO("(NBN LAS DETECT) refusing=" << DescribeDetectorRefusal(sample)
			<< " ring=" << ringMotion
			<< " context=" << context
			<< " expected=" << expectedMidi
			<< " | level=" << std::fixed << std::setprecision(2) << sample.level
			<< " (needs >=" << sample.levelMinimum << ")"
			<< " quality=" << sample.quality
			<< " (needs >=" << sample.qualityMinimum << ")"
			<< " loudest=" << sample.currentNote
			<< " pitchNow=" << sample.pitchNow
			<< " pitchPrev=" << sample.pitchPrevious
			<< " dedupe=" << sample.dedupe
			<< " ringIndex=" << sample.ringIndex << "/" << sample.ringCapacity
			<< " seq=" << sample.ringSequence
			<< " pitchMode=" << sample.pitchMode
			<< " detector=0x" << std::hex << sample.detector << std::dec
			<< "." << std::endl);
		LOG_INFO("(NBN LAS DETECT2) ref=" << DescribeDetectorRefusal(sample)
			<< " exp=" << expectedMidi
			<< " now=" << sample.pitchNow
			<< " prev=" << sample.pitchPrevious
			<< " dedupe=" << sample.dedupe
			<< " lvl=" << std::fixed << std::setprecision(1) << sample.level
			<< " q=" << std::setprecision(0) << sample.quality
			<< " loud=" << sample.currentNote
			<< std::endl);
	}
	bool MlConfirmsHeldNote()
	{
		ResearchProtocol::MlNoteEvidence evidence;
		if (expectedMidi == previousExpectedMidi && !sawSpikeDuringHold)
		{
			mlConfirmationState.Observe(evidence);
			return false;
		}
		ResearchProbeRuntime::QueryMlNoteEvidence(expectedMidi, ML_STUCK_RESCUE_CONF,
			mlConfirmationState.GetMinimumSampleIndex(), evidence);
		return mlConfirmationState.Observe(evidence);
	}
	bool ConfirmHeldLegatoPitch()
	{
		const bool previousBendMayRing = previousWasBend && !sawSpikeDuringHold && hasLastBendCommit
			&& std::chrono::duration<double>(std::chrono::steady_clock::now() - lastBendCommitAt).count()
				< BEND_RELEASE_GUARD_SECONDS;
		if (expectedMidi == previousExpectedMidi || previousBendMayRing) return false;
		if (previousSelectedString == NoteByNote::CHORD_STRING_SENTINEL
			&& lastCommittedChordRecord != 0 && researchChordTonesRecord == lastCommittedChordRecord)
		{
			for (int i = 0; i < researchChordToneCount; ++i)
				if (researchChordTones[i] == expectedMidi) return false;
		}

		const bool mlConfirmed = MlMayRescueNow() && MlConfirmsHeldNote();
		ResearchProtocol::RawNoteConfirmation raw;
		const double frequency = 440.0 * std::pow(2.0, (expectedMidi - 69.0) / 12.0);
		const bool rawMatches = g_tier0Enforcement
			&& ResearchProbeRuntime::QueryRawNoteConfirmation(frequency, raw,
				enhancedLegatoConfirmation.GetMinimumSample()) && raw.confirmed != 0;
		const bool enhancedConfirmed = enhancedLegatoConfirmation.Observe(
			raw.endSampleIndex, raw.sampleRate, rawMatches);
		if (!mlConfirmed && !enhancedConfirmed) return false;
		if (mlConfirmed)
		{
			mlRescueRecord = selectedRecord;
			mlRescueMidi = expectedMidi;
		}
		if (enhancedConfirmed)
		{
			enhancedRescueFeedback.targetRecord = selectedRecord;
			enhancedRescueFeedback.enhancedMidi = expectedMidi;
			enhancedRescueFeedback.nativeMidi = QueryNativeLoudestPlayedNote();
			enhancedRescueFeedback.enhancedRole = NoteByNote::DetectorRole::Confirmed;
		}
		LOG_INFO("(NBN LEGATO CONFIRM) Fresh pitch evidence accepted target=" << expectedMidi
			<< " enhanced=" << enhancedConfirmed << " ml=" << mlConfirmed
			<< " native=" << QueryNativeLoudestPlayedNote() << std::endl);
		return true;
	}

	bool MlConfirmsBendTarget()
	{
		ResearchProtocol::MlNoteEvidence evidence;
		const bool mayBePreviousRing = !isBendChildTarget && !sawSpikeDuringHold
			&& (bendAcceptMidi == previousExpectedMidi
				|| (hasLastBendCommit && std::chrono::duration<double>(
					std::chrono::steady_clock::now() - lastBendCommitAt).count()
					< BEND_RELEASE_GUARD_SECONDS));
		if (!MlMayRescueNow() || mayBePreviousRing)
		{
			mlBendConfirmationState.Observe(evidence);
			return false;
		}
		ResearchProbeRuntime::QueryMlNoteEvidence(bendAcceptMidi, 0.5f,
			mlBendConfirmationState.GetMinimumSampleIndex(), evidence);
		return mlBendConfirmationState.Observe(evidence);
	}

	int TickSpikeReattackAcceptance(bool allowAccept)
	{
		DetectorGateSample sample;
		if (!TryReadDetectorGates(sample))
		{
			hasReattackLastLevel = false;
			reattackWindowTicks = 0;
			reattackStreak = 0;
			return -1;
		}

		if (isMlStuckRescueEnabled && MlMayRescueNow() && allowAccept && MlConfirmsHeldNote())
		{
			mlRescueRecord = selectedRecord;
			mlRescueMidi = expectedMidi;
			reattackWindowTicks = 0;
			reattackStreak = 0;
			LOG_INFO("(NBN LAS ML RESCUE) Stuck hold accepted: two distinct post-target"
				<< " audio predictions confirm expected " << expectedMidi << "." << std::endl);
			return expectedMidi;
		}
		const bool spiked = hasReattackLastLevel
			&& std::isfinite(sample.level)
			&& sample.level - reattackLastLevel >= DETECTOR_SPIKE_JUMP_DB;
		reattackLastLevel = sample.level;
		hasReattackLastLevel = std::isfinite(sample.level);
		const bool spikeAttributable = gatePhase != GatePhase::Holding
			|| holdTickCount >= 6;
		if (spiked && spikeAttributable)
		{
			reattackWindowTicks = REATTACK_WINDOW_TICKS;
			reattackStreak = 0;
			spikeRecencyTicks = REATTACK_WINDOW_TICKS;
		}
		else
		{
			if (reattackWindowTicks > 0) --reattackWindowTicks;
			if (spikeRecencyTicks > 0) --spikeRecencyTicks;
		}
		if (reattackWindowTicks <= 0)
		{
			reattackStreak = 0;
			return -1;
		}
		const bool detectorMatches = sample.passesQuality
			&& MatchesPickPitch(sample.currentNote);
		const bool nativeContradicts = sample.passesQuality
			&& sample.currentNote >= 0
			&& sample.currentNote != expectedMidi - 1;
		const bool rawConfirms = !detectorMatches
			&& !nativeContradicts
			&& g_tier0Enforcement
			&& Tier0ConfirmsExpected(expectedMidi);
		if (!detectorMatches && !rawConfirms)
		{
			if (!detectorMatches && nativeContradicts && g_tier0Enforcement
				&& reattackStreak == 0 && spiked)
			{
				LOG_INFO("(NBN LAS REATTACK) Raw route refused: the native detector confidently reads "
					<< sample.currentNote << " against expected " << expectedMidi
					<< " (quality=" << std::fixed << std::setprecision(0) << sample.quality
					<< "); tier-0 may only cover a missing or -1-biased native read." << std::endl);
			}
			reattackStreak = 0;
			return -1;
		}
		const int requiredStreak = rawConfirms ? 2 : REATTACK_STREAK_TICKS;
		if (++reattackStreak < requiredStreak) return -1;
		if (!allowAccept) return -1;
		if (rawConfirms)
		{
			enhancedRescueFeedback.targetRecord = selectedRecord;
			enhancedRescueFeedback.enhancedMidi = expectedMidi;
			enhancedRescueFeedback.nativeMidi = sample.currentNote;
			enhancedRescueFeedback.enhancedRole = NoteByNote::DetectorRole::Confirmed;
		}

		reattackWindowTicks = 0;
		reattackStreak = 0;
		LOG_INFO("(NBN LAS REATTACK) Pick accepted from the level spike: "
			<< (detectorMatches ? "raw pitch matched" : "tier-0 raw route confirmed")
			<< " expected " << expectedMidi
			<< " (detector read " << sample.currentNote
			<< ", level=" << std::fixed << std::setprecision(1) << sample.level
			<< ", quality=" << std::setprecision(0) << sample.quality
			<< ") for " << REATTACK_STREAK_TICKS << " ticks after a spike; the native"
			<< " onset edge never latched." << std::endl);
		return expectedMidi;
	}

	struct LiveNote
	{
		uintptr_t note = 0;
		uintptr_t record = 0;
		uint32_t mask = 0;
		float recordTime = 0.0f;
		float eventTime = 0.0f;
		float windowEntry = 0.0f;
		float windowExit = 0.0f;
		uint8_t stateC0 = 0;
		uint8_t stateC1 = 0;
		uint8_t stateC2 = 0;
		uint8_t stateC3 = 0;
	};

	bool IsBeatVfxPromptActive(uintptr_t fork)
	{
		uint8_t active = 0;
		uintptr_t entity = 0;
		uint8_t request = 0;
		return TryRead(fork + BEAT_VFX_ACTIVE, active)
			&& TryRead(fork + BEAT_VFX_ENTITY, entity)
			&& TryRead(fork + BEAT_VFX_PROMPT_REQUEST, request)
			&& (active != 0 || entity != 0 || request != 0);
	}

	bool ValidateBeatVfxFork(uintptr_t fork)
	{
		uintptr_t vtable = 0;
		return fork != 0 && TryRead(fork, vtable) && vtable == BEAT_VFX_VTABLE;
	}

	bool ArmSelectedNativePrompt(const LiveNote& selected)
	{
		uintptr_t wrapper = 0;
		uintptr_t controller = 0;
		uintptr_t controllerVtable = 0;
		if (!TryRead(selected.note + NOTE_VFX_WRAPPER, wrapper) || wrapper == 0
			|| !TryRead(wrapper, controller) || controller == 0
			|| !TryRead(controller, controllerVtable))
		{
			return false;
		}

		if (controllerVtable == NOTE_VFX_VTABLE)
		{
			uintptr_t armSlot = 0;
			uintptr_t clearSlot = 0;
			uintptr_t promptFork = 0;
			if (!TryRead(controllerVtable + 0x20, armSlot) || armSlot != ARM_NOTE_VFX_PROMPT
				|| !TryRead(controllerVtable + 0x24, clearSlot) || clearSlot != CLEAR_NOTE_VFX_PROMPT
				|| !TryRead(controller + NOTE_VFX_PROMPT_FORK, promptFork)
				|| !ValidateBeatVfxFork(promptFork))
			{
				return false;
			}
			if (IsBeatVfxPromptActive(promptFork)) return true;

			reinterpret_cast<ThiscallVoidFn>(armSlot)(reinterpret_cast<void*>(controller), nullptr);
			uint8_t request = 0;
			return TryRead(promptFork + BEAT_VFX_PROMPT_REQUEST, request) && request == 1;
		}

		if (controllerVtable != SPECIALIZED_NOTE_VFX_VTABLE) return false;

		uintptr_t armBcSlot = 0;
		uintptr_t armC0Slot = 0;
		uintptr_t clearSlot = 0;
		uintptr_t bcFork = 0;
		uintptr_t c0Fork = 0;
		uint8_t promptState = 0;
		uint8_t suppressPrompt = 0;
		if (!TryRead(controllerVtable + 0x14, armBcSlot) || armBcSlot != ARM_SPECIALIZED_PROMPT_BC
			|| !TryRead(controllerVtable + 0x18, armC0Slot) || armC0Slot != ARM_SPECIALIZED_PROMPT_C0
			|| !TryRead(controllerVtable + 0x1C, clearSlot) || clearSlot != CLEAR_SPECIALIZED_PROMPT
			|| !TryRead(controller + SPECIALIZED_PROMPT_BC_FORK, bcFork)
			|| !TryRead(controller + SPECIALIZED_PROMPT_C0_FORK, c0Fork)
			|| !ValidateBeatVfxFork(bcFork) || !ValidateBeatVfxFork(c0Fork)
			|| !TryRead(selected.note + NOTE_SPECIALIZED_PROMPT_STATE, promptState)
			|| !TryRead(selected.note + NOTE_SPECIALIZED_PROMPT_SUPPRESS, suppressPrompt))
		{
			return false;
		}

		const bool useBcFork = promptState != 0 && suppressPrompt == 0;
		const uintptr_t selectedFork = useBcFork ? bcFork : c0Fork;
		const uintptr_t otherFork = useBcFork ? c0Fork : bcFork;
		uint8_t otherRequest = 0;
		if (IsBeatVfxPromptActive(selectedFork)
			&& TryRead(otherFork + BEAT_VFX_PROMPT_REQUEST, otherRequest) && otherRequest == 0)
		{
			return true;
		}

		const uintptr_t armSlot = useBcFork ? armBcSlot : armC0Slot;
		reinterpret_cast<ThiscallVoidFn>(armSlot)(reinterpret_cast<void*>(controller), nullptr);
		uint8_t selectedRequest = 0;
		return TryRead(selectedFork + BEAT_VFX_PROMPT_REQUEST, selectedRequest) && selectedRequest == 1
			&& TryRead(otherFork + BEAT_VFX_PROMPT_REQUEST, otherRequest) && otherRequest == 0;
	}

	bool ReadLiveNote(uintptr_t noteAddress, LiveNote& note)
	{
		note = {};
		note.note = noteAddress;
		if (!TryRead(noteAddress + NOTE_RECORD, note.record) || note.record == 0) return false;
		if (!TryRead(noteAddress + NOTE_EVENT_TIME, note.eventTime)) return false;
		if (!TryRead(noteAddress + NOTE_WINDOW_ENTRY, note.windowEntry)) return false;
		if (!TryRead(noteAddress + NOTE_WINDOW_EXIT, note.windowExit)) return false;
		uint32_t packedStates = 0;
		if (!TryRead(noteAddress + NOTE_STATE_C0, packedStates)) return false;
		note.stateC0 = static_cast<uint8_t>(packedStates & 0xFF);
		note.stateC1 = static_cast<uint8_t>((packedStates >> 8) & 0xFF);
		note.stateC2 = static_cast<uint8_t>((packedStates >> 16) & 0xFF);
		note.stateC3 = static_cast<uint8_t>((packedStates >> 24) & 0xFF);
		if (!TryRead(note.record + RECORD_MASK, note.mask)) return false;
		if (!TryRead(note.record + RECORD_TIME, note.recordTime)) return false;
		return true;
	}

	void ObserveBendDecision(uintptr_t noteAddress, bool originalResult)
	{
		LiveNote note;
		if (!ReadLiveNote(noteAddress, note))
		{
			return;
		}
		float observedBendAmount = 0.0f;
		if (!TryRead(note.record + RECORD_BEND_AMOUNT, observedBendAmount)
			|| !std::isfinite(observedBendAmount)
			|| observedBendAmount < BEND_AMOUNT_MIN_SEMITONES)
		{
			return;
		}

		const uint32_t packedStates = static_cast<uint32_t>(note.stateC0)
			| (static_cast<uint32_t>(note.stateC1) << 8)
			| (static_cast<uint32_t>(note.stateC2) << 16)
			| (static_cast<uint32_t>(note.stateC3) << 24);
		const int loudestMidi = QueryNativeLoudestPlayedNote();
		auto observation = std::find_if(
			bendDecisionObservations.begin(),
			bendDecisionObservations.begin() + bendDecisionObservationCount,
			[&note](const BendDecisionObservation& candidate)
			{
				return candidate.record == note.record;
			});
		if (observation == bendDecisionObservations.begin() + bendDecisionObservationCount)
		{
			if (bendDecisionObservationCount == bendDecisionObservations.size()) return;
			observation = bendDecisionObservations.begin() + bendDecisionObservationCount;
			++bendDecisionObservationCount;
			observation->record = note.record;
		}
		if (observation->hasObservation
			&& observation->packedStates == packedStates
			&& observation->loudestMidi == loudestMidi
			&& observation->originalResult == originalResult)
		{
			return;
		}

		std::array<uint8_t, 0x48> recordBytes = {};
		const bool hasRecordBytes = TryRead(note.record, recordBytes);
		std::ostringstream rawRecord;
		if (hasRecordBytes)
		{
			rawRecord << std::hex << std::setfill('0');
			for (const auto value : recordBytes)
			{
				rawRecord << std::setw(2) << static_cast<unsigned int>(value);
			}
		}

		uint32_t flags = 0;
		TryRead(note.record + RECORD_FLAGS, flags);
		LOG_INFO("(NBN BEND DIAGNOSTIC) original-decision"
			<< " updateTime=" << std::fixed << std::setprecision(6) << observedScoringUpdateTime
			<< " note=0x" << std::hex << note.note
			<< " record=0x" << note.record
			<< " mask=0x" << note.mask
			<< " flags=0x" << flags
			<< " states=0x" << packedStates << std::dec
			<< " authored=" << std::fixed << std::setprecision(6) << note.recordTime
			<< " event=" << note.eventTime
			<< " window=" << note.windowEntry << ".." << note.windowExit
			<< " loudestMidi=" << loudestMidi
			<< " originalResult=" << std::boolalpha << originalResult
			<< " nbnEnabled=" << ResearchProbeRuntime::IsNoteByNoteEnabled()
			<< " recordBytes=" << (hasRecordBytes ? rawRecord.str() : "unavailable")
			<< "." << std::endl);

		observation->packedStates = packedStates;
		observation->loudestMidi = loudestMidi;
		observation->originalResult = originalResult;
		observation->hasObservation = true;
	}

	bool ForEachLiveNote(void* owner, const std::function<bool(const LiveNote&)>& visitor)
	{
		uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		uintptr_t begin = 0;
		uintptr_t end = 0;
		if (!TryRead(ownerAddress + OWNER_NOTES_BEGIN, begin)) return false;
		if (!TryRead(ownerAddress + OWNER_NOTES_END, end)) return false;
		if (begin == 0 || end < begin || end - begin > 0x4000) return false;
		for (uintptr_t slot = begin; slot < end; slot += sizeof(uintptr_t))
		{
			uintptr_t noteAddress = 0;
			if (!TryRead(slot, noteAddress) || noteAddress == 0) continue;
			LiveNote note;
			if (!ReadLiveNote(noteAddress, note)) continue;
			if (!visitor(note)) return true;
		}
		return true;
	}


	int NextSingleNoteMidi()
	{
		if (nextNoteMidiRecord == selectedRecord) return nextNoteMidi;
		nextNoteMidiRecord = selectedRecord;
		nextNoteMidi = -1;
		void* owner = trackedOwner;
		if (owner == nullptr) return -1;
		LiveNote next;
		bool found = false;
		ForEachLiveNote(owner, [&](const LiveNote& note)
		{
			if (note.recordTime > selectedRecordTime + 0.01f && (!found || note.recordTime < next.recordTime))
			{
				next = note;
				found = true;
			}
			return true;
		});
		int32_t chordId = -1;
		uint8_t nextString = 0;
		uint8_t nextFret = 0;
		int16_t tuningOffset = 0;
		if (!found || !TryRead(next.record + RECORD_CHORD_ID, chordId) || chordId != -1
			|| !TryRead(next.record + RECORD_STRING, nextString) || !TryRead(next.record + RECORD_FRET, nextFret)
			|| nextString > 5 || !TryRead(TUNING_OFFSETS + static_cast<uintptr_t>(nextString) * 2, tuningOffset))
		{
			return -1;
		}
		int midi = -1;
		if (NoteByNote::TryResolvePlayedNotePitch(nextString, nextFret, tuningOffset,
			ResearchProbeRuntime::GetInputOnsetShiftSemitones(), midi, IsBassArrangement()))
		{
			nextNoteMidi = midi;
		}
		return nextNoteMidi;
	}

	bool TryInferFromFollowingPick()
	{
		if (selectedChordId != -1 || expectedMidi < 0) return false;
		const int next = NextSingleNoteMidi();
		if (next < 0 || next == expectedMidi || next == previousExpectedMidi) return false;
		auto qualifies = [&](const NoteByNote::PickedAttack& attack)
		{
			return attack.minimumSample > lastTakenPickSample
				&& (attack.confirmedMidi < 0 || attack.confirmedMidi == previousExpectedMidi);
		};
		for (unsigned later = 0; later < pickedAttacks.Count(); ++later)
		{
			const auto* following = pickedAttacks.At(later);
			if (following == nullptr || following->rejected || following->confirmedMidi != next) continue;
			for (unsigned earlier = 0; earlier < later; ++earlier)
			{
				const auto* attack = pickedAttacks.At(earlier);
				if (attack == nullptr || !qualifies(*attack)) continue;
				acceptedPick = *attack;
				acceptedPick.confirmedMidi = expectedMidi;
				pickedAttacks.RemoveFirstCount(earlier + 1);
				LOG_INFO("(NBN PICK BUFFER) Inferred attack=" << acceptedPick.time << " as the target midi="
					<< expectedMidi << ": a later attack=" << following->time << " confirmed the NEXT note ("
					<< next << "), so this one was played but drowned by the ringing note." << std::endl);
				return true;
			}
			if (hasLastUnresolvedAttack && qualifies(lastUnresolvedAttack)
				&& lastUnresolvedAttack.minimumSample < following->minimumSample)
			{
				acceptedPick = lastUnresolvedAttack;
				acceptedPick.confirmedMidi = expectedMidi;   // see above
				hasLastUnresolvedAttack = false;
				LOG_INFO("(NBN PICK BUFFER) Inferred dropped attack=" << acceptedPick.time << " as the target midi="
					<< expectedMidi << ": a later attack=" << following->time << " confirmed the NEXT note ("
					<< next << "), so this one was played but drowned by the ringing note." << std::endl);
				return true;
			}
		}
		return false;
	}

	bool FindSelectedNote(void* owner, LiveNote& found)
	{
		bool wasFound = false;
		ForEachLiveNote(owner, [&](const LiveNote& note)
		{
			if (note.record != selectedRecord) return true;
			found = note;
			wasFound = true;
			return false;
		});
		return wasFound;
	}

	void* ResolvePlayerSong(void* owner)
	{
		uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		uintptr_t collection = 0;
		if (!TryRead(ownerAddress + OWNER_COMPONENTS, collection) || collection == 0) return nullptr;
		uintptr_t begin = 0;
		uintptr_t end = 0;
		if (!TryRead(collection + 0x4, begin) || !TryRead(collection + 0x8, end)) return nullptr;
		if (begin == 0 || end < begin || end - begin > 0x800) return nullptr;
		void* resolved = nullptr;
		size_t matchCount = 0;
		for (uintptr_t entry = begin; entry + 8 <= end; entry += 8)
		{
			uintptr_t component = 0;
			if (!TryRead(entry + 0x4, component) || component == 0) continue;
			uintptr_t vtable = 0;
			if (!TryRead(component, vtable)) continue;
			if (vtable != PLAYER_SONG_VTABLE) continue;
			resolved = reinterpret_cast<void*>(component);
			++matchCount;
		}
		return matchCount == 1 ? resolved : nullptr;
	}

	void EmitCandidateEvent(void* owner, float updateTime, const LiveNote& note,
		ResearchProtocol::ExpectedAttackEventKind kind)
	{
		ResearchProtocol::ExpectedAttackEvent event;
		event.kind = kind;
		event.ownerAddress = reinterpret_cast<uintptr_t>(owner);
		event.epoch = epochIndex;
		event.updateTime = updateTime;
		event.isAfterUpdate = 1;
		event.isNotePresent = 1;
		event.note.nativeNoteAddress = note.note;
		event.note.recordAddress = note.record;
		event.note.noteMask = note.mask;
		event.note.authoredTime = note.recordTime;
		event.note.nativeEventTime = note.eventTime;
		event.note.rangeA4 = note.windowEntry;
		event.note.rangeA8 = note.windowExit;
		event.note.stateC0 = note.stateC0;
		event.note.stateC1 = note.stateC1;
		event.note.stateC2 = note.stateC2;
		event.note.stateC3 = note.stateC3;
		uint8_t stringIndex = 0;
		uint8_t fret = 0;
		uint32_t flags = 0;
		uint32_t hash = 0;
		int32_t chordId = -1;
		int32_t chordNotesId = -1;
		int32_t phraseIteration = -1;
		if (TryRead(note.record + RECORD_STRING, stringIndex)) event.note.stringIndex = stringIndex;
		if (TryRead(note.record + RECORD_FRET, fret)) event.note.fret = fret;
		if (TryRead(note.record + RECORD_FLAGS, flags)) event.note.noteFlags = flags;
		if (TryRead(note.record + RECORD_HASH, hash)) event.note.noteHash = hash;
		if (TryRead(note.record + RECORD_CHORD_ID, chordId)) event.note.chordId = chordId;
		if (TryRead(note.record + RECORD_CHORD_NOTES_ID, chordNotesId)) event.note.chordNotesId = chordNotesId;
		if (TryRead(note.record + RECORD_PHRASE_ITERATION, phraseIteration)) event.note.phraseIterationId = phraseIteration;
		if ((event.note.noteMask & NOTE_MASK_BEND) != 0)
		{
			float semitones = 0.0f;
			if (TryRead(note.record + RECORD_BEND_AMOUNT, semitones)
				&& std::isfinite(semitones)
				&& semitones >= BEND_AMOUNT_MIN_SEMITONES
				&& semitones <= BEND_AMOUNT_MAX_SEMITONES)
			{
				event.note.bendSemitones = static_cast<int32_t>(std::lround(semitones));
			}
			else
			{
				event.note.bendSemitones = -1;
			}
		}
		ResearchProbeRuntime::PublishExpectedAttackEvent(event);
	}
	constexpr uint32_t NOTE_MASK_CHORD_PANEL = 0x80000000u;
	constexpr uint32_t NOTE_MASK_HIGH_DENSITY = 0x00200000u;
	uintptr_t promotedRepeatRecord = 0;
	uint32_t promotedRepeatOriginalMask = 0;
	uint32_t promotedRepeatHash = 0;
	float promotedRepeatTime = 0.0f;

	void RestorePromotedRepeatChord(bool mayWrite);

	bool PromoteRepeatChordForRedraw(uintptr_t record)
	{
		if (record == 0 || promotedRepeatRecord == record) return promotedRepeatRecord == record;
		RestorePromotedRepeatChord(true);
		uint32_t mask = 0;
		uint32_t hash = 0;
		float time = 0.0f;
		if (!TryRead(record + RECORD_MASK, mask) || !TryRead(record + RECORD_HASH, hash)
			|| !TryRead(record + RECORD_TIME, time))
		{
			return false;
		}
		if ((mask & NOTE_MASK_CHORD_PANEL) != 0) return false;
		const uint32_t promoted = (mask | NOTE_MASK_CHORD_PANEL) & ~NOTE_MASK_HIGH_DENSITY;
		if (!TryWriteGameUint32(record + RECORD_MASK, promoted)) return false;
		promotedRepeatRecord = record;
		promotedRepeatOriginalMask = mask;
		promotedRepeatHash = hash;
		promotedRepeatTime = time;
		LOG_INFO("(NBN FLOW) Repeat-strum chord record 0x" << std::hex << record << " promoted for the redraw:"
			<< " mask 0x" << mask << " -> 0x" << promoted << std::dec << "." << std::endl);
		return true;
	}

	void RestorePromotedRepeatChord(bool mayWrite)
	{
		if (promotedRepeatRecord == 0) return;
		const uintptr_t record = promotedRepeatRecord;
		promotedRepeatRecord = 0;
		if (!mayWrite)
		{
			LOG_INFO("(NBN FLOW) Promoted repeat-strum record 0x" << std::hex << record << std::dec
				<< " dropped without a restore; its owner is not provably alive." << std::endl);
			return;
		}
		const uint32_t promoted = (promotedRepeatOriginalMask | NOTE_MASK_CHORD_PANEL) & ~NOTE_MASK_HIGH_DENSITY;
		uint32_t mask = 0;
		uint32_t hash = 0;
		float time = 0.0f;
		if (!TryRead(record + RECORD_MASK, mask) || !TryRead(record + RECORD_HASH, hash)
			|| !TryRead(record + RECORD_TIME, time) || mask != promoted || hash != promotedRepeatHash
			|| time != promotedRepeatTime)
		{
			LOG_INFO("(NBN FLOW) Promoted repeat-strum record 0x" << std::hex << record << std::dec
				<< " no longer matches; its mask is left alone." << std::endl);
			return;
		}
		TryWriteGameUint32(record + RECORD_MASK, promotedRepeatOriginalMask);
	}

	void ClearSelection()
	{
		RestorePromotedRepeatChord(true);
		mlRescueRecord = 0;
		enhancedRescueFeedback = {};
		acceptedPickRecord = 0;
		armedExpectedMidi = -1;
		armedBufferCommitRecord = 0;
		gatePhase = GatePhase::Idle;
		selectedRecord = 0;
		selectedRecordTime = 0.0f;
		selectedString = -1;
		selectedFret = -1;
		selectedChordId = -1;
		selectedChordNotesId = -1;
		selectedHoldTime = 0.0f;
		selectedCompensation = 0.0;
		expectedMidi = -1;
		previousExpectedMidi = -1;
		previousWasBend = false;
		visualGroupCount = 0;
		lastVisualGroupRecord = 0;
		isBendTarget = false;
		bendAcceptMidi = -1;
		bendVisualizationSnapshot = {};
		bendVisualReached = false;
		bendVisualReachedRecord = 0;
		bendVisualReachedEpoch = 0;
		isBendChildTarget = false;
		isLegatoTarget = false;
		isHammerOnTarget = false;
		legatoConfirmTickCount = 0;
		hasLegatoPitchDeparted = false;
		legatoRunCount = 0;
		for (auto& isBend : legatoRunIsBend) isBend = false;
		legatoRunIndex = 0;
		isConfirmingLegatoRun = false;
		isBendRunConfirmation = false;
		wasBendTrackerPitchLogged = false;
		isRawBendAcceptArmed = false;
		rawBendAcceptStreak = 0;
		ndBendSightingStreak = 0;
		reattackWindowTicks = 0;
		reattackStreak = 0;
		spikeRecencyTicks = 0;
		pendingPrimedOnset = -1;
		hasPickPitchDeparted = false;
		pickPitchConfirmTicks = 0;
		isHoldSuppressed = false;
		holdTickCount = 0;
		postReleaseTickCount = 0;
		commitTickCount = 0;
		inputReleaseTickCount = 0;
		wasCommitOverrideLogged = false;
		heldPlayerSong = nullptr;
		renderFramesWhileHeld = 0;
		denseSuccessor = {};
		isFlowRedrawRebuild = false;
	}
	double CurrentSongSpeed()
	{
		if (flowSpeedPassRequestedPercent > 0.0f) return flowSpeedPassRequestedPercent / 100.0;
		const float player = ResearchProbeRuntime::GetPlayerSpeedRealPercent();
		if (player >= 1.0f && player <= 400.0f) return player / 100.0;
		return hasMeasuredSongSpeed ? measuredSongSpeed : 1.0;
	}
	struct TransportSample { std::chrono::steady_clock::time_point at; float time; };
	std::array<TransportSample, 64> transportSamples{};
	size_t transportSampleCount = 0;
	size_t transportSampleNext = 0;

	void PushTransportSample(float updateTime)
	{
		const auto now = std::chrono::steady_clock::now();
		if (transportSampleCount != 0)
		{
			const auto& last = transportSamples[(transportSampleNext + transportSamples.size() - 1) % transportSamples.size()];
			if (updateTime < last.time || updateTime - last.time > 0.5f
				|| now - last.at > std::chrono::milliseconds(300)) transportSampleCount = 0;
			else if (updateTime == last.time) return;
		}
		transportSamples[transportSampleNext] = { now, updateTime };
		transportSampleNext = (transportSampleNext + 1) % transportSamples.size();
		if (transportSampleCount < transportSamples.size()) ++transportSampleCount;
	}
	double lastInstantSongSpeed = -1.0;

	bool TryInstantSongSpeed(double& speed)
	{
		if (transportSampleCount < 4) return false;
		const size_t size = transportSamples.size();
		const auto& newest = transportSamples[(transportSampleNext + size - 1) % size];
		for (size_t back = 1; back < transportSampleCount; ++back)
		{
			const auto& older = transportSamples[(transportSampleNext + size - 1 - back) % size];
			const double real = std::chrono::duration<double>(newest.at - older.at).count();
			if (real < 0.25 && back + 1 < transportSampleCount) continue;
			if (real < 0.15) return false;
			const double rate = static_cast<double>(newest.time - older.time) / real;
			if (rate < 0.005 || rate > 4.5) return false;
			speed = rate;
			lastInstantSongSpeed = rate;
			return true;
		}
		return false;
	}
	float HoldCompensationSeconds()
	{
		double speed = 0.0;
		if (flowSpeedPassRequestedPercent > 0.0f || ResearchProbeRuntime::GetPlayerSpeedRealPercent() > 0.0f)
		{
			const double r = (std::max)(0.01, (std::min)(1.0, CurrentSongSpeed()));
			return static_cast<float>(selectedCompensation * (2.0 * r - 1.0));
		}
		if (!TryInstantSongSpeed(speed))
			speed = lastInstantSongSpeed > 0.0 ? lastInstantSongSpeed
				: hasMeasuredSongSpeed ? measuredSongSpeed : 1.0;
		speed = (std::max)(0.01, (std::min)(1.0, speed));
		return static_cast<float>(selectedCompensation * (2.0 * speed - 1.0));
	}

	void SampleSongSpeed(float updateTime)
	{
		const auto now = std::chrono::steady_clock::now();
		if (gatePhase != GatePhase::Armed && gatePhase != GatePhase::Idle)
		{
			songSpeedSampleTime = -1.0f;
			transportSampleCount = 0;
			return;
		}
		PushTransportSample(updateTime);
		if (gatePhase == GatePhase::Armed && selectedRecord != 0 && updateTime < selectedHoldTime)
			selectedHoldTime = selectedRecordTime - HoldCompensationSeconds();
		if (songSpeedSampleTime < 0.0f)
		{
			songSpeedSampleTime = updateTime;
			songSpeedSampleAt = now;
			return;
		}
		const double realSeconds = std::chrono::duration<double>(now - songSpeedSampleAt).count();
		if (realSeconds < 0.3) return;
		const double songSeconds = static_cast<double>(updateTime - songSpeedSampleTime);
		songSpeedSampleTime = updateTime;
		songSpeedSampleAt = now;
		if (realSeconds > 1.5 || songSeconds <= 0.0) return;   // stalled frame or jump: skip
		const double ratio = songSeconds / realSeconds;
		if (ratio < 0.005 || ratio > 4.5) return;
		const double previous = measuredSongSpeed;
		measuredSongSpeed = hasMeasuredSongSpeed ? previous * 0.8 + ratio * 0.2 : ratio;
		hasMeasuredSongSpeed = true;
		if (gatePhase == GatePhase::Armed && selectedRecord != 0)
			selectedHoldTime = selectedRecordTime - HoldCompensationSeconds();
		if (std::fabs(measuredSongSpeed - previous) > 0.05)
		{
			LOG_INFO("(NBN SPEED) Measured song speed " << std::fixed << std::setprecision(0)
				<< measuredSongSpeed * 100.0 << "% from the transport; using " << CurrentSongSpeed() * 100.0
				<< "% (" << (flowSpeedPassRequestedPercent > 0.0f ? "flow request"
					: ResearchProbeRuntime::GetPlayerSpeedRealPercent() > 0.0f ? "player slider" : "measured")
				<< ")." << std::defaultfloat << std::endl);
		}
	}

	bool OwnsNativeHold()
	{
		return gatePhase == GatePhase::WaitingForInputRelease
			|| gatePhase == GatePhase::Holding
			|| gatePhase == GatePhase::CommitBeforeRelease
			|| gatePhase == GatePhase::DenseRebuildPending
			|| gatePhase == GatePhase::DenseRecommitAfterRebuild
			|| gatePhase == GatePhase::DensePlayerSongStartPending;
	}
	constexpr uintptr_t OWNER_FROZEN_ON_TAG = 0x5E3;
	volatile bool isNativeFreezeFlagEnabled = false;
	constexpr uintptr_t NATIVE_SCHEDULER_GLOBAL = 0x0135F5AC;
	constexpr uintptr_t OWNER_FREEZE_TAG_STRING = 0x5C8;
	constexpr uintptr_t OWNER_FREEZE_TAG_BEGIN = 0x5D8;
	constexpr uintptr_t OWNER_FREEZE_TAG_END = 0x5DC;
	constexpr uintptr_t OWNER_RESUME_FROM_TAG = 0x5E4;
	constexpr uintptr_t OWNER_FREEZE_START_TIME = 0x618;
	constexpr uintptr_t CHORD_DISPLAY_GLOBAL = 0x0135F54C;
	constexpr uintptr_t CHORD_DISPLAY_GLOBAL_HOP = 0x10;
	constexpr uintptr_t CHORD_DISPLAY_WRAPPER_SLOT = 0x50;
	constexpr uintptr_t OWNER_CHORD_DISPLAY_COMPONENT = 0x78;
	constexpr uintptr_t CHORD_DISPLAY_TEMPLATE_BEGIN = 0x94;
	constexpr uintptr_t CHORD_DISPLAY_TEMPLATE_END = 0x98;
	constexpr uintptr_t CHORD_DISPLAY_SLOT = 0x1F4;
	constexpr uintptr_t CHORD_DISPLAY_VISIBLE = 0x1F0;
	volatile bool isNativeChordPanelEnabled = false;
	uintptr_t shownChordPanelComponent = 0;
	volatile bool isScheduleShiftEnabled = false;
	uint64_t scheduleShiftCount = 0;
	std::chrono::steady_clock::time_point transportFrozenAt{};
	bool hasTransportFrozenAt = false;
	constexpr uintptr_t NATIVE_SCHEDULE_SHIFT_FN = 0x57EB00;
	constexpr int NATIVE_SCHEDULE_SHIFT_ENTRY = 4;
	constexpr int NATIVE_SCHEDULE_SHIFT_OP_ADD = 3;

	void CallNativeScheduleShift(double elapsedSeconds)
	{
		uintptr_t scheduler = 0;
		if (!TryRead(NATIVE_SCHEDULER_GLOBAL, scheduler) || scheduler == 0)
		{
			LOG_INFO("(NBN LAS SHIFT) Scheduler global empty; shift skipped." << std::endl);
			return;
		}
		if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0
			|| elapsedSeconds > 120.0)
		{
			LOG_INFO("(NBN LAS SHIFT) Elapsed span " << elapsedSeconds
				<< "s out of bounds; shift skipped." << std::endl);
			return;
		}
		const uintptr_t fn = NATIVE_SCHEDULE_SHIFT_FN;
		__asm {
			push edi
			mov eax, 4
			xor edi, edi
			sub esp, 8
			fld qword ptr elapsedSeconds
			fstp qword ptr [esp]
			push 3
			push scheduler
			mov ecx, fn
			call ecx
			pop edi
		}
		++scheduleShiftCount;
		LOG_INFO("(NBN LAS SHIFT) Native schedule shift applied: entry "
			<< NATIVE_SCHEDULE_SHIFT_ENTRY << " += " << std::fixed
			<< std::setprecision(3) << elapsedSeconds
			<< "s (call #" << scheduleShiftCount << ")." << std::endl);
	}
	constexpr uintptr_t NATIVE_CHORD_MATCHER_FN = 0x4E6E90;
	constexpr uintptr_t MATCHER_ARRANGEMENT_SLOT = 0x10;
	constexpr uintptr_t MATCHER_DET_SLOT = 0x08;
	constexpr uintptr_t MATCHER_STATE_SLOT = 0x04;
	constexpr uintptr_t MATCHER_GATE_A_OFFSET = 0xDD8;
	constexpr uintptr_t MATCHER_GATE_B_OFFSET = 0xD38;
	constexpr uintptr_t MATCHER_GATE_A_THRESHOLD = 0x1224418; // double global
	constexpr uintptr_t MATCHER_GATE_B_THRESHOLD = 0x12243A0; // double global

	int CallNativeChordMatcher(const int* midis, int count, int frameOffset)
	{
		if (midis == nullptr || count <= 0 || count > 6 || frameOffset < 0) return -1;
		uintptr_t root = 0, arrangement = 0, det = 0, state = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return -1;
		if (!TryRead(root + MATCHER_ARRANGEMENT_SLOT, arrangement) || arrangement == 0) return -1;
		if (!TryRead(arrangement + MATCHER_DET_SLOT, det) || det == 0) return -1;
		if (!TryRead(det + MATCHER_STATE_SLOT, state) || state == 0) return -1;
		float gateA = 0.0f, gateB = 0.0f;
		double thrA = 0.0, thrB = 0.0;
		if (!TryRead(state + MATCHER_GATE_A_OFFSET, gateA)
			|| !TryRead(state + MATCHER_GATE_B_OFFSET, gateB)
			|| !TryRead(MATCHER_GATE_A_THRESHOLD, thrA)
			|| !TryRead(MATCHER_GATE_B_THRESHOLD, thrB))
		{
			return -1;
		}
		if (!std::isfinite(gateA) || !std::isfinite(gateB)
			|| static_cast<double>(gateA) < thrA
			|| static_cast<double>(gateB) < thrB)
		{
			return -2; // input too quiet - the game's own precondition failed; matcher not run
		}
		int arr[6] = { 0, 0, 0, 0, 0, 0 };
		for (int i = 0; i < count && i < 6; ++i) arr[i] = midis[i];
		const uintptr_t fn = NATIVE_CHORD_MATCHER_FN;
		int* arrPtr = arr;
		int cnt = count;
		int frameArg = frameOffset;
		uintptr_t detArg = det;
		unsigned char resultAl = 0;
		__asm {
			push 0
			push frameArg
			push cnt
			push arrPtr
			push detArg
			mov ecx, fn
			call ecx
			mov resultAl, al
		}
		return (resultAl & 1) ? 1 : 0;
	}
	constexpr uintptr_t NATIVE_SINGLE_NOTE_MATCHER_FN = 0x4E7B30;
	int CallNativeSingleNoteMatcher(int expectedMidi, int frameOffset)
	{
		if (expectedMidi < 0 || frameOffset < 0) return -1;
		uintptr_t root = 0, arrangement = 0, det = 0, state = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return -1;
		if (!TryRead(root + MATCHER_ARRANGEMENT_SLOT, arrangement) || arrangement == 0) return -1;
		if (!TryRead(arrangement + MATCHER_DET_SLOT, det) || det == 0) return -1;
		if (!TryRead(det + MATCHER_STATE_SLOT, state) || state == 0) return -1;
		int expectedBuf[6] = { expectedMidi, -1, -1, -1, -1, -1 };
		const uintptr_t fn = NATIVE_SINGLE_NOTE_MATCHER_FN;
		int* bufPtr = expectedBuf;
		uintptr_t detArg = det;
		int flagArg = 0;
		int frameArg = frameOffset;
		unsigned char resultAl = 0;
		__asm {
			push frameArg
			push flagArg
			push bufPtr
			push detArg
			mov ecx, fn
			call ecx
			mov resultAl, al
		}
		return (resultAl & 1) ? 1 : 0;
	}
	bool TryReadCurrentRingTime(double& outTs)
	{
		uintptr_t root = 0, arrangement = 0, det = 0, state = 0, ring = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return false;
		if (!TryRead(root + MATCHER_ARRANGEMENT_SLOT, arrangement) || arrangement == 0) return false;
		if (!TryRead(arrangement + MATCHER_DET_SLOT, det) || det == 0) return false;
		if (!TryRead(det + MATCHER_STATE_SLOT, state) || state == 0) return false;
		if (!TryRead(state + DETECTOR_RING_BUFFER, ring) || ring == 0) return false;
		int32_t writeCursor = 0, capacity = 0;
		if (!TryRead(state + DETECTOR_RING_INDEX, writeCursor)
			|| !TryRead(state + DETECTOR_RING_CAPACITY, capacity)
			|| capacity <= 0 || capacity > DETECTOR_RING_MAX_CAPACITY
			|| writeCursor < 0 || writeCursor >= capacity)
		{
			return false;
		}
		return TryRead(ring + static_cast<uintptr_t>(writeCursor) * DETECTOR_RING_STRIDE
			+ RING_FRAME_TIMESTAMP, outTs) && std::isfinite(outTs);
	}


	bool NativeChordMatchesAttack(const int* tones, int count, uint64_t attackSample)
	{
		if (!hasHoldLatchRingTime) return false;
		uintptr_t root = 0, arrangement = 0, det = 0, state = 0, ring = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0
			|| !TryRead(root + MATCHER_ARRANGEMENT_SLOT, arrangement) || arrangement == 0
			|| !TryRead(arrangement + MATCHER_DET_SLOT, det) || det == 0
			|| !TryRead(det + MATCHER_STATE_SLOT, state) || state == 0
			|| !TryRead(state + DETECTOR_RING_BUFFER, ring) || ring == 0) return false;
		int32_t cursor = 0, capacity = 0, validCount = 0;
		if (!TryRead(state + DETECTOR_RING_INDEX, cursor)
			|| !TryRead(state + DETECTOR_RING_CAPACITY, capacity)
			|| !TryRead(state + DETECTOR_RING_VALID_COUNT, validCount)
			|| capacity <= 0 || capacity > DETECTOR_RING_MAX_CAPACITY
			|| cursor < 0 || cursor >= capacity || validCount <= 0) return false;
		if (validCount > capacity) validCount = capacity;
		auto readTimestamp = [&](int offset, double& timestamp)
		{
			int index = cursor - offset;
			if (index < 0) index += capacity;
			return TryRead(ring + static_cast<uintptr_t>(index) * DETECTOR_RING_STRIDE
				+ RING_FRAME_TIMESTAMP, timestamp);
		};
		double newestTime = 0.0;
		if (!readTimestamp(0, newestTime)) return false;
		int framesMatched = 0;
		int matcherResult = -1;
		int matchedOffset = -1;
		const bool accepted = NoteByNote::MatchChordAttackFrames(newestTime, holdLatchRingTime,
			attackSample, pickScanSample, pickSampleRate, validCount, readTimestamp,
			[&](int offset)
			{
				++framesMatched;
				matcherResult = CallNativeChordMatcher(tones, count, offset);
				if (matcherResult == 1) matchedOffset = offset;
				return matcherResult == 1;
			});
		static uint64_t loggedAttackSample = 0;
		static uint64_t loggedScanSample = 0;
		const bool periodicScan = loggedAttackSample != attackSample || pickScanSample < loggedScanSample
			|| pickScanSample - loggedScanSample >= pickSampleRate / 10;
		if (accepted || (periodicScan && framesMatched > 0))
		{
			loggedAttackSample = attackSample;
			loggedScanSample = pickScanSample;
			LOG_INFO("(NBN CHORD SCAN) attack=" << attackSample << " scan=" << pickScanSample
				<< " capture=" << latestRawAudioSample << " rate=" << pickSampleRate
				<< " ringNow=" << std::setprecision(12) << newestTime << " latch=" << holdLatchRingTime
				<< " frames=" << framesMatched << " valid=" << validCount << " result=" << matcherResult
				<< " matchedOffset=" << matchedOffset << " record=0x" << std::hex << selectedRecord
				<< std::dec << std::setprecision(6) << std::endl);
		}
		return accepted;
	}

	bool RawCloseChordMatchesAttack(const int* tones, int count, uint64_t attackSample)
	{
		if (!NoteByNote::SupportsCloseChord(tones, count) || !chordAttacks.HasFreshAttack(pickScanSample, pickSampleRate)) return false;
		static uint64_t measuredAttack = 0;
		static uintptr_t measuredRecord = 0;
		if (measuredAttack == attackSample && measuredRecord == selectedRecord) return false;
		RawPitchVerifier::AudioSnapshot snapshot;
		if (!RawPitchVerifier::CaptureSnapshot(snapshot) || snapshot.sampleRate != pickSampleRate) return false;
		const uint32_t requiredSamples = snapshot.sampleRate / 4;
		const uint64_t windowEnd = attackSample + requiredSamples;
		if (snapshot.endSampleIndex < windowEnd || snapshot.endSampleIndex < snapshot.sampleCount) return false;
		const uint64_t snapshotStart = snapshot.endSampleIndex - snapshot.sampleCount;
		if (snapshotStart > attackSample) return false;
		measuredAttack = attackSample;
		measuredRecord = selectedRecord;
		const uint32_t offset = static_cast<uint32_t>(attackSample - snapshotStart);
		const bool confirmed = NoteByNote::ConfirmCloseChord(snapshot.samples + offset,
			requiredSamples, snapshot.sampleRate, tones, count);
		LOG_INFO("(NBN RAW CLOSE CHORD) confirmed=" << confirmed << " tones=" << tones[0] << ',' << tones[1]
			<< " samples=" << attackSample << ".." << windowEnd << " record=0x" << std::hex
			<< selectedRecord << std::dec << std::endl);
		return confirmed;
	}
	int StrumChordPresenceForAttack(const int* playedTones, int count, uint64_t attackSample)
	{
		static uint64_t measuredAttack = 0;
		static uintptr_t measuredRecord = 0;
		static int measuredVerdict = -1;
		if (attackSample == 0 || count < 2) return -1;
		if (measuredAttack == attackSample && measuredRecord == selectedRecord) return measuredVerdict;
		RawPitchVerifier::AudioSnapshot snapshot;
		if (!RawPitchVerifier::CaptureSnapshot(snapshot) || snapshot.sampleRate != pickSampleRate) return -1;
		const uint32_t span = NoteByNote::GetStrumPresenceSampleSpan(snapshot.sampleRate);
		if (snapshot.endSampleIndex < attackSample + span || snapshot.endSampleIndex < snapshot.sampleCount) return -1;
		const uint64_t snapshotStart = snapshot.endSampleIndex - snapshot.sampleCount;
		measuredAttack = attackSample;
		measuredRecord = selectedRecord;
		measuredVerdict = -1;
		if (snapshotStart > attackSample)
		{
			LOG_INFO("(NBN STRUM PRESENCE SHADOW) attack=" << attackSample
				<< " unavailable: the raw ring no longer holds the strum." << std::endl);
			return -1;
		}
		NoteByNote::StrumChordPresence presence;
		const uint32_t offset = static_cast<uint32_t>(attackSample - snapshotStart);
		if (!NoteByNote::MeasureStrumChordPresence(snapshot.samples + offset, snapshot.sampleCount - offset,
			snapshot.sampleRate, playedTones, count, presence)) return -1;
		measuredVerdict = presence.confirmed ? 1 : 0;
		std::ostringstream tones;
		for (int i = 0; i < presence.toneCount; ++i)
		{
			const auto& tone = presence.tones[i];
			tones << ' ' << tone.midi << "/h" << tone.harmonic << ':' << (tone.present ? 'Y' : 'n')
				<< '(' << std::scientific << std::setprecision(1) << tone.power << " vs " << tone.neighbour << ')';
		}
		LOG_INFO("(NBN STRUM PRESENCE SHADOW) attack=" << attackSample << " present=" << presence.presentCount
			<< '/' << presence.toneCount << " need=" << presence.requiredCount
			<< " confirmed=" << (presence.confirmed ? 1 : 0) << " energy=" << std::scientific << std::setprecision(1)
			<< presence.energy << std::defaultfloat << " |" << tones.str() << " record=0x" << std::hex
			<< selectedRecord << std::dec << std::endl);
		return measuredVerdict;
	}
	struct PendingStrumLedger
	{
		uint64_t attack = 0;
		uintptr_t record = 0;
		int tones[6] = {};
		int count = 0;
		unsigned readsDone = 0;
		bool accepted = false;
		bool strummedByLate = false;
		bool wrongAtEarly = false;
		bool earlyMeasured = false;
	};
	PendingStrumLedger pendingStrumLedger;
	uint64_t finishedStrumLedgerAttack = 0;   // one ledger per strum: the hold re-offers it every tick
	int finishedStrumLedgerVerdict = -1;

	void FlushStrumLedgerSummary(const char* ending)
	{
		auto& ledger = pendingStrumLedger;
		if (ledger.attack == 0) return;
		finishedStrumLedgerAttack = ledger.attack;
		finishedStrumLedgerVerdict = ledger.strummedByLate ? 1 : ((ledger.readsDone & 4u) ? 0 : -1);
		const bool veto = !ledger.strummedByLate;
		LOG_INFO("(NBN STRUM LEDGER SUMMARY) attack=" << ledger.attack << " record=0x" << std::hex << ledger.record
			<< std::dec << " reads=" << ledger.readsDone << " strummedBy120=" << (ledger.strummedByLate ? 1 : 0)
			<< " wrongAt65=" << (ledger.wrongAtEarly ? 1 : (ledger.earlyMeasured ? 0 : -1))
			<< " current=" << (ledger.accepted ? "accepted" : "not-accepted-yet")
			<< " stage1=" << (veto ? "VETO" : "pass") << " stage2=" << (veto ? "refuse" : "ACCEPT")
			<< (ledger.accepted && veto ? " <- stage 1 would have refused this accept" : "")
			<< (!ledger.accepted && !veto ? " <- strummed but the current rules did not accept" : "")
			<< " (" << ending << ")" << std::endl);
		ledger = {};
	}

	void RegisterStrumLedgerAttack(uint64_t attack, const int* tones, int count)
	{
		if (attack == 0 || count < 2 || count > 6) return;
		if (pendingStrumLedger.attack == attack && pendingStrumLedger.record == selectedRecord) return;
		if (attack == finishedStrumLedgerAttack) return;
		FlushStrumLedgerSummary("superseded by a newer strum");
		pendingStrumLedger.attack = attack;
		pendingStrumLedger.record = selectedRecord;
		pendingStrumLedger.count = count;
		for (int i = 0; i < count; ++i) pendingStrumLedger.tones[i] = tones[i];
	}
	int GetStrumLedgerVerdict(uint64_t attack)
	{
		if (attack != 0 && attack == finishedStrumLedgerAttack) return finishedStrumLedgerVerdict;
		if (attack == 0 || pendingStrumLedger.attack != attack) return -1;
		if (pendingStrumLedger.strummedByLate) return 1;
		return (pendingStrumLedger.readsDone & 4u) ? 0 : -1;
	}

	void MarkStrumLedgerAccepted(uint64_t attack)
	{
		if (pendingStrumLedger.attack == attack) pendingStrumLedger.accepted = true;
	}

	void ProcessPendingStrumLedger()
	{
		auto& ledger = pendingStrumLedger;
		if (ledger.attack == 0) return;
		RawPitchVerifier::AudioSnapshot snapshot;
		if (!RawPitchVerifier::CaptureSnapshot(snapshot) || snapshot.sampleRate == 0
			|| snapshot.endSampleIndex < snapshot.sampleCount) return;
		const uint64_t snapshotStart = snapshot.endSampleIndex - snapshot.sampleCount;
		if (ledger.attack < snapshotStart + NoteByNote::GetLedgerPreSamples(snapshot.sampleRate))
		{
			FlushStrumLedgerSummary("the raw ring no longer holds the strum");
			return;
		}
		const NoteByNote::LedgerRead reads[] = {
			NoteByNote::LedgerRead::Early, NoteByNote::LedgerRead::Standard, NoteByNote::LedgerRead::Late };
		for (int r = 0; r < 3; ++r)
		{
			const unsigned bit = 1u << r;
			if (ledger.readsDone & bit) continue;
			if (ledger.attack + NoteByNote::GetLedgerPostSamples(reads[r], snapshot.sampleRate) > snapshot.endSampleIndex)
				break;
			ledger.readsDone |= bit;
			NoteByNote::LedgerReading reading;
			if (!NoteByNote::MeasureStrumLedger(snapshot.samples, snapshot.sampleCount,
				static_cast<uint32_t>(ledger.attack - snapshotStart), snapshot.sampleRate,
				ledger.tones, ledger.count, reads[r], reading)) continue;
			if (reads[r] == NoteByNote::LedgerRead::Early)
			{
				ledger.earlyMeasured = true;
				ledger.wrongAtEarly = reading.wrongStrum;
			}
			if (reading.strummed) ledger.strummedByLate = true;
			std::ostringstream tones;
			tones << std::scientific << std::setprecision(1);
			for (int i = 0; i < reading.toneCount; ++i)
			{
				const auto& tone = reading.tones[i];
				tones << ' ' << tone.midi << "/h" << tone.harmonic << ':' << (tone.struck ? 'Y' : 'n')
					<< (tone.wrongNeighbour ? "!" : "") << "(b" << tone.baseline << " p" << tone.post
					<< " r" << tone.rise << " nr" << tone.neighbourRise << ')';
			}
			LOG_INFO("(NBN STRUM LEDGER) attack=" << ledger.attack << " read=" << NoteByNote::GetLedgerReadName(reads[r])
				<< " struck=" << reading.struckCount << '/' << reading.toneCount << " need=" << reading.requiredCount
				<< " strummed=" << (reading.strummed ? 1 : 0) << " wrong=" << (reading.wrongStrum ? 1 : 0)
				<< " energy=" << std::scientific << std::setprecision(1) << reading.energy << std::defaultfloat
				<< " |" << tones.str() << std::endl);
		}
		if (ledger.readsDone == 7u) FlushStrumLedgerSummary("all reads done");
	}

	bool RawUnisonMatchesAttack(int midi, uint64_t attackSample)
	{
		ResearchProtocol::RawNoteConfirmation evidence;
		const double frequency = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
		if (!ResearchProbeRuntime::QueryRawNoteConfirmation(frequency, evidence, attackSample)
			|| !evidence.confirmed) return false;
		LOG_INFO("(NBN RAW UNISON) Confirmed pitch=" << midi << " samples=" << attackSample
			<< ".." << evidence.endSampleIndex << " record=0x" << std::hex << selectedRecord
			<< std::dec << std::endl);
		return true;
	}
	bool RawPowerChordMatchesAttack(const int* tones, int count, uint64_t attackSample)
	{
		int probes[6] = {};
		const int probeCount = NoteByNote::SelectPowerChordProbeTones(tones, count, probes);
		if (probeCount == 0 || attackSample == 0 || pickSampleRate == 0) return false;
		static uint64_t measuredAttack = 0;
		static uintptr_t measuredRecord = 0;
		static bool measuredResult = false;
		if (measuredAttack == attackSample && measuredRecord == selectedRecord) return measuredResult;
		uint32_t needed = 0;
		for (int i = 0; i < probeCount; ++i)
		{
			const uint32_t window = NoteByNote::PickConfirmationWindowSamples(probes[i], pickSampleRate);
			if (window > needed) needed = window;
		}
		if (latestRawAudioSample < attackSample || latestRawAudioSample - attackSample < needed) return false;
		bool confirmed = true;
		float weakestChange = 1e9f;
		for (int i = 0; i < probeCount && confirmed; ++i)
		{
			ResearchProtocol::RawNoteConfirmation evidence;
			const double frequency = 440.0 * std::pow(2.0, (probes[i] - 69.0) / 12.0);
			confirmed = ResearchProbeRuntime::QueryRawNoteConfirmation(frequency, evidence, attackSample)
				&& evidence.confirmed && evidence.attackChange >= 0.2f;
			if (evidence.attackChange < weakestChange) weakestChange = evidence.attackChange;
		}
		measuredAttack = attackSample;
		measuredRecord = selectedRecord;
		measuredResult = confirmed;
		LOG_INFO("(NBN RAW POWER CHORD) confirmed=" << confirmed << " probe=" << probes[0]
			<< (probeCount > 1 ? "+" : "") << " change=" << weakestChange << " attack=" << attackSample
			<< " record=0x" << std::hex << selectedRecord << std::dec << std::endl);
		return confirmed;
	}
	int32_t lastCapturedOnsetSeq = -1;
	void CaptureOnsetSpectrum(int expected)
	{
		uintptr_t root = 0, arrangement = 0, det = 0, state = 0, ring = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return;
		if (!TryRead(root + MATCHER_ARRANGEMENT_SLOT, arrangement) || arrangement == 0) return;
		if (!TryRead(arrangement + MATCHER_DET_SLOT, det) || det == 0) return;
		if (!TryRead(det + MATCHER_STATE_SLOT, state) || state == 0) return;
		if (!TryRead(state + DETECTOR_RING_BUFFER, ring) || ring == 0) return;
		int32_t cursor = 0, capacity = 0;
		if (!TryRead(state + DETECTOR_RING_INDEX, cursor)
			|| !TryRead(state + DETECTOR_RING_CAPACITY, capacity)
			|| capacity <= 0 || capacity > DETECTOR_RING_MAX_CAPACITY
			|| cursor < 0 || cursor >= capacity)
		{
			return;
		}
		const uintptr_t frame = ring + static_cast<uintptr_t>(cursor) * DETECTOR_RING_STRIDE;
		uint8_t onsetFlag = 0;
		if (!TryRead(frame + RING_FRAME_ONSET_FLAG, onsetFlag) || onsetFlag == 0) return;
		int32_t seq = -1;
		TryRead(frame + RING_FRAME_SEQUENCE, seq);
		if (seq == lastCapturedOnsetSeq) return; // one capture per distinct onset frame
		lastCapturedOnsetSeq = seq;

		float level = 0.0f;
		TryRead(frame + RING_FRAME_LEVEL, level);
		int32_t count = 0;
		TryRead(frame + RING_FRAME_PAIR_COUNT, count);
		if (count < 0) count = 0;
		if (count > 48) count = 48;
		struct PeakEnergy { int midi; float energy; };
		PeakEnergy peaks[48];
		int found = 0;
		for (int32_t i = 0; i < count; ++i)
		{
			int32_t midi = -1;
			float energy = 0.0f;
			if (TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8, midi)
				&& TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8 + 4, energy)
				&& midi >= 0 && midi < 128 && std::isfinite(energy) && energy > 0.0f)
			{
				peaks[found].midi = midi;
				peaks[found].energy = energy;
				++found;
			}
		}
		for (int a = 0; a < found; ++a)
		{
			for (int b = a + 1; b < found; ++b)
			{
				if (peaks[b].energy > peaks[a].energy)
				{
					PeakEnergy tmp = peaks[a];
					peaks[a] = peaks[b];
					peaks[b] = tmp;
				}
			}
		}
		std::ostringstream ss;
		const int top = found < 14 ? found : 14;
		for (int i = 0; i < top; ++i)
		{
			if (i) ss << ' ';
			ss << peaks[i].midi << ':' << std::fixed << std::setprecision(1) << peaks[i].energy;
		}
		const int ndLoudest = QueryNativeLoudestPlayedNote();
		LOG_INFO("(NBN CAPTURE) exp=" << expected << " ndLoudest=" << ndLoudest
			<< " domPeak=" << (found > 0 ? peaks[0].midi : -1) << " lvl=" << std::fixed << std::setprecision(1)
			<< level << " peaks=[" << ss.str() << "]" << std::endl);
	}
	void LogTier0ShadowEvidence(int expectedMidi, const char* context)
	{
		if (expectedMidi < 0) return;
		ResearchProtocol::RawToneEvidence evidence;
		const double frequency =
			440.0 * std::pow(2.0, (static_cast<double>(expectedMidi) - 69.0) / 12.0);
		if (!ResearchProbeRuntime::QueryRawToneEvidence(frequency, 0.15f, evidence)) return;
		LOG_INFO("(NBN TIER0) " << context << " exp=" << expectedMidi
			<< std::fixed << std::setprecision(1) << " f=" << frequency
			<< std::setprecision(4)
			<< " tgt=" << evidence.targetPower
			<< " -1=" << evidence.minusOnePower
			<< " +1=" << evidence.plusOnePower
			<< " -2=" << evidence.minusTwoPower
			<< " +2=" << evidence.plusTwoPower
			<< " rms=" << evidence.totalRms
			<< " win=" << evidence.windowSampleCount << "@" << evidence.sampleRate
			<< std::endl);
	}
	void LogTier1ShadowPitch(int expectedMidi)
	{
		if (expectedMidi < 0) return;
		if (!ResearchProbeRuntime::IsMlPitchServiceAlive()) return;
		float mlMidi = 0.0f;
		float mlConfidence = 0.0f;
		double mlAgeSeconds = 0.0;
		if (!ResearchProbeRuntime::QueryMlPitch(mlMidi, mlConfidence, mlAgeSeconds)) return;
		LOG_INFO("(NBN TIER1) shadow exp=" << expectedMidi
			<< std::fixed << std::setprecision(2)
			<< " ml=" << mlMidi
			<< " conf=" << mlConfidence
			<< std::setprecision(3) << " age=" << mlAgeSeconds
			<< std::endl);
	}
	void AppendDetectionCompareCsv(int technique, int expectedMidi, int nativeMidi,
		bool nativeMatch, bool mlHadOpinion, int mlMidi, bool mlMatch, int code)
	{
		(void)technique; (void)expectedMidi; (void)nativeMidi; (void)nativeMatch;
		(void)mlHadOpinion; (void)mlMidi; (void)mlMatch; (void)code;
	}
	void SampleDetectionComparison()
	{
		if (expectedMidi < 0) return;
		const DetectionTechnique technique = CurrentDetectionTechnique();

		const bool haveChordTones = technique == DetectionTechnique::Chord
			&& researchChordToneCount > 0 && researchChordTonesRecord == selectedRecord;
		auto pitchIsTarget = [&](int midi) -> bool
		{
			if (midi < 0) return false;
			if (haveChordTones)
			{
				for (int i = 0; i < researchChordToneCount; ++i)
					if (researchChordTones[i] == midi) return true;
				return false;
			}
			if (midi == expectedMidi) return true;
			if (technique == DetectionTechnique::Bend && bendAcceptMidi >= 0 && midi == bendAcceptMidi)
				return true;
			return false;
		};

		const int nativeMidi = QueryNativeLoudestPlayedNote();
		const bool nativeMatch = pitchIsTarget(nativeMidi);

		bool mlHadOpinion = false;
		bool mlMatch = false;
		int mlMidiInt = -1;
		if (ResearchProbeRuntime::IsMlPitchServiceAlive())
		{
			float mlMidi = 0.0f, mlConfidence = 0.0f;
			double mlAgeSeconds = 0.0;
			if (ResearchProbeRuntime::QueryMlPitch(mlMidi, mlConfidence, mlAgeSeconds)
				&& mlConfidence >= 0.30f && mlAgeSeconds <= 0.75)
			{
				mlHadOpinion = true;
				mlMidiInt = static_cast<int>(std::lround(mlMidi));
				mlMatch = pitchIsTarget(mlMidiInt);
			}
		}

		const int code = !mlHadOpinion ? 2 : (nativeMatch == mlMatch ? 1 : 0);
		auto& c = g_detectionComparison;
		if (mlHadOpinion) { if (code == 1) ++c.agree; else ++c.disagree; }
		c.lastTechnique = static_cast<int>(technique);
		c.lastNativeMatch = nativeMatch;
		c.lastMlMatch = mlMatch;
		c.lastMlHadOpinion = mlHadOpinion;
		c.lastValid = true;
		if (c.historyCount < DETECTION_COMPARE_HISTORY)
		{
			c.history[c.historyCount++] = static_cast<uint8_t>(code);
		}
		else
		{
			for (uint32_t i = 1; i < DETECTION_COMPARE_HISTORY; ++i) c.history[i - 1] = c.history[i];
			c.history[DETECTION_COMPARE_HISTORY - 1] = static_cast<uint8_t>(code);
		}
		AppendDetectionCompareCsv(static_cast<int>(technique), expectedMidi, nativeMidi,
			nativeMatch, mlHadOpinion, mlMidiInt, mlMatch, code);
	}
	volatile bool g_tier0Enforcement = true;
	constexpr double TIER0_TARGET_DETUNE_RATIO = 1.0145453349375237; // 2^(25/1200)
	float Tier0WindowSecondsForFrequency(double frequencyHz)
	{
		if (frequencyHz < 200.0) return 0.30f;    // midi < ~55: lobe ~3.3 Hz
		if (frequencyHz < 330.0) return 0.20f;    // midi < ~64: lobe ~5 Hz
		return 0.15f;
	}

	bool QueryTier0EvidenceWideTarget(int expectedMidi, ResearchProtocol::RawToneEvidence& evidence)
	{
		const double frequency =
			440.0 * std::pow(2.0, (static_cast<double>(expectedMidi) - 69.0) / 12.0);
		const float windowSeconds = Tier0WindowSecondsForFrequency(frequency);
		if (!ResearchProbeRuntime::QueryRawToneEvidence(frequency, windowSeconds, evidence))
		{
			return false;
		}
		ResearchProtocol::RawToneEvidence detuned;
		if (ResearchProbeRuntime::QueryRawToneEvidence(
				frequency * TIER0_TARGET_DETUNE_RATIO, windowSeconds, detuned)
			&& detuned.targetPower > evidence.targetPower)
		{
			evidence.targetPower = detuned.targetPower;
		}
		if (ResearchProbeRuntime::QueryRawToneEvidence(
				frequency / TIER0_TARGET_DETUNE_RATIO, windowSeconds, detuned)
			&& detuned.targetPower > evidence.targetPower)
		{
			evidence.targetPower = detuned.targetPower;
		}
		return true;
	}
	volatile bool g_sharpCorridorLog = true;
	void LogCentsComb(int expectedMidi)
	{
		const double expectedFrequency = 440.0 * std::pow(2.0, (expectedMidi - 69.0) / 12.0);
		ResearchProtocol::RawToneComb comb;
		if (!ResearchProbeRuntime::QueryRawToneComb(expectedFrequency, comb))
		{
			LOG_INFO("(NBN TIER0 COMB) exp=" << expectedMidi << " snapshot=unavailable" << std::endl);
			return;
		}
		constexpr int WINDOW_MS[] = { 50, 100, 150 };
		for (int window = 0; window < 3; ++window)
		{
			int peakBin = 0;
			float corridorMax = 0.0f;
			float outsideMax = 0.0f;
			std::ostringstream bins;
			bins << std::scientific << std::setprecision(6);
			for (int bin = 0; bin < 17; ++bin)
			{
				const float power = comb.powers[window][bin];
				if (power > comb.powers[window][peakBin]) peakBin = bin;
				const float cents = -50.0f + bin * 12.5f;
				if (cents >= 25.0f && cents <= 80.0f)
				{
					if (power > corridorMax) corridorMax = power;
				}
				else if (power > outsideMax)
				{
					outsideMax = power;
				}
				if (bin != 0) bins << ',';
				bins << power;
			}
			LOG_INFO("(NBN TIER0 COMB) exp=" << expectedMidi
				<< " endSample=" << comb.endSampleIndex << " rate=" << comb.sampleRate
				<< " windowMs=" << WINDOW_MS[window] << " samples=" << comb.sampleCounts[window]
				<< " peakBinCents=" << (-50.0f + peakBin * 12.5f)
				<< std::scientific << std::setprecision(6)
				<< " rms=" << comb.rms[window] << " peakPow=" << comb.powers[window][peakBin]
				<< " corridorMax=" << corridorMax << " outsideMax=" << outsideMax
				<< " bins=" << bins.str() << std::defaultfloat << std::endl);
		}
	}
	bool QueryTier0WideProbePower(double centerHz, float& outPower)
	{
		const float windowSeconds = Tier0WindowSecondsForFrequency(centerHz);
		ResearchProtocol::RawToneEvidence probe;
		if (!ResearchProbeRuntime::QueryRawToneEvidence(centerHz, windowSeconds, probe)) return false;
		outPower = probe.targetPower;
		if (ResearchProbeRuntime::QueryRawToneEvidence(
				centerHz * TIER0_TARGET_DETUNE_RATIO, windowSeconds, probe)
			&& probe.targetPower > outPower)
		{
			outPower = probe.targetPower;
		}
		if (ResearchProbeRuntime::QueryRawToneEvidence(
				centerHz / TIER0_TARGET_DETUNE_RATIO, windowSeconds, probe)
			&& probe.targetPower > outPower)
		{
			outPower = probe.targetPower;
		}
		return true;
	}
	bool TryEstimateRawBendPitch(double baseMidi, double targetMidi,
		float& outMidi, float& outConfidence)
	{
		outMidi = -1.0f;
		outConfidence = 0.0f;
		if (!std::isfinite(baseMidi) || !std::isfinite(targetMidi) || targetMidi <= baseMidi)
			return false;
		static std::chrono::steady_clock::time_point lastAt{};
		static double lastTarget = -1.0;
		static float lastMidi = -1.0f;
		static float lastConf = 0.0f;
		static bool lastValid = false;
		const auto now = std::chrono::steady_clock::now();
		if (targetMidi == lastTarget
			&& std::chrono::duration<double>(now - lastAt).count() < 0.040)
		{
			outMidi = lastMidi;
			outConfidence = lastConf;
			return lastValid;
		}
		lastAt = now;
		lastTarget = targetMidi;
		lastValid = false;
		const double center = std::floor((baseMidi + targetMidi) * 0.5 + 0.5);
		const double freq = 440.0 * std::pow(2.0, (center - 69.0) / 12.0);
		const float windowSeconds = Tier0WindowSecondsForFrequency(freq) * 0.5f;
		ResearchProtocol::RawToneEvidence e;
		if (!ResearchProbeRuntime::QueryRawToneEvidence(freq, windowSeconds, e)) return false;
		if (!std::isfinite(e.totalRms) || e.totalRms < 0.015f) return false;

		const double p[5] = { e.minusTwoPower, e.minusOnePower, e.targetPower,
			e.plusOnePower, e.plusTwoPower };
		const double m[5] = { center - 2.0, center - 1.0, center, center + 1.0, center + 2.0 };
		int peak = 0;
		double mean = 0.0;
		for (int i = 0; i < 5; ++i)
		{
			if (!std::isfinite(p[i])) return false;
			mean += p[i];
			if (p[i] > p[peak]) peak = i;
		}
		mean /= 5.0;
		if (!(p[peak] > 0.0) || mean <= 0.0) return false;

		double refined = m[peak];
		if (peak > 0 && peak < 4 && p[peak - 1] > 0.0 && p[peak + 1] > 0.0)
		{
			const double y0 = std::log(p[peak - 1]);
			const double y1 = std::log(p[peak]);
			const double y2 = std::log(p[peak + 1]);
			const double denom = y0 - 2.0 * y1 + y2;
			if (denom < 0.0)
			{
				const double delta = 0.5 * (y0 - y2) / denom; // +-0.5 semitone about the peak bin
				if (delta > -1.0 && delta < 1.0) refined = m[peak] + delta;
			}
		}
		if (refined < baseMidi - 0.5) refined = baseMidi - 0.5;
		if (refined > targetMidi + 1.0) refined = targetMidi + 1.0;

		const double contrast = p[peak] / mean;
		double confidence = (contrast - 1.5) * 40.0;
		if (confidence < 0.0) confidence = 0.0;
		if (confidence > 100.0) confidence = 100.0;

		lastMidi = static_cast<float>(refined);
		lastConf = static_cast<float>(confidence);
		lastValid = true;
		outMidi = lastMidi;
		outConfidence = lastConf;
		return true;
	}

	void RefreshBendVisualizationSnapshot()
	{
		BendVisualizationSnapshot snapshot;
		if (!(isBendTarget || isBendRunConfirmation) || expectedMidi < 0)
		{
			bendVisualizationSnapshot = snapshot;
			bendVisualReached = false;
			bendVisualReachedRecord = 0;
			bendVisualReachedEpoch = 0;
			return;
		}

		snapshot.baseMidi = expectedMidi;
		snapshot.targetMidi = bendAcceptMidi >= 0
			? bendAcceptMidi
			: (isConfirmingLegatoRun && legatoRunIndex < legatoRunCount
				&& legatoRunIsBend[legatoRunIndex]
				? legatoRunMidi[legatoRunIndex]
				: -1);
		const int32_t wanted = snapshot.targetMidi >= 0 ? snapshot.targetMidi : expectedMidi;
		float trackerPitch = 0.0f;
		float estimateMidi = -1.0f;
		float estimateConfidence = 0.0f;
		if (TryGetSoundingPitchNear(static_cast<float>(wanted),
				static_cast<float>(MAX_BEND_SEMITONES) + 1.5f, trackerPitch))
		{
			snapshot.soundingMidi = trackerPitch;
			snapshot.soundingQuality = 100.0f;
		}
		else if (snapshot.targetMidi > expectedMidi
			&& TryEstimateRawBendPitch(static_cast<double>(expectedMidi),
				static_cast<double>(snapshot.targetMidi), estimateMidi, estimateConfidence)
			&& estimateConfidence >= 20.0f)
		{
			snapshot.soundingMidi = estimateMidi;
			snapshot.soundingQuality = estimateConfidence;
		}
		else if (hasLastStateGateSample
			&& lastStateGateSample.currentNote >= 0
			&& std::isfinite(lastStateGateSample.quality)
			&& lastStateGateSample.quality >= DETECTOR_RAW_BEND_QUALITY_FLOOR)
		{
			snapshot.soundingMidi = static_cast<float>(lastStateGateSample.currentNote);
			snapshot.soundingQuality = lastStateGateSample.quality;
		}

		bendVisualizationSnapshot = snapshot;
	}
	bool Tier0ConfirmsExpected(int expectedMidi)
	{
		if (expectedMidi < 0) return false;
		ResearchProtocol::RawToneEvidence evidence;
		const double frequency =
			440.0 * std::pow(2.0, (static_cast<double>(expectedMidi) - 69.0) / 12.0);
		if (!QueryTier0EvidenceWideTarget(expectedMidi, evidence)) return false;
		if (evidence.targetPower < 1e-5f) return false;
		float worstNeighbour = evidence.minusOnePower;
		if (evidence.plusOnePower > worstNeighbour) worstNeighbour = evidence.plusOnePower;
		if (evidence.minusTwoPower > worstNeighbour) worstNeighbour = evidence.minusTwoPower;
		if (evidence.plusTwoPower > worstNeighbour) worstNeighbour = evidence.plusTwoPower;
		if (evidence.targetPower < worstNeighbour * 5.0f) return false;
		auto wideTargetPower = [](double centerHz, float& outPower) -> bool
		{
			return QueryTier0WideProbePower(centerHz, outPower);
		};
		ResearchProtocol::RawToneEvidence subOctave = {};
		ResearchProtocol::RawToneEvidence subTwelfth = {};
		float fifthAbovePower = 0.0f;
		if (!wideTargetPower(frequency * 0.5, subOctave.targetPower)
			|| !wideTargetPower(frequency / 3.0, subTwelfth.targetPower)
			|| !wideTargetPower(frequency * 1.5, fifthAbovePower))
		{
			return false;
		}
		if (subOctave.targetPower >= evidence.targetPower * 0.4f
			|| subTwelfth.targetPower >= evidence.targetPower * 0.4f)
		{
			return false;                  // a lower note's harmonic, not the note itself
		}
		if (fifthAbovePower >= evidence.targetPower * 0.1f && fifthAbovePower >= 1e-5f)
		{
			static ULONGLONG lastPhantomLogTick = 0;
			const ULONGLONG nowTick = GetTickCount64();
			if (nowTick - lastPhantomLogTick >= 250)
			{
				lastPhantomLogTick = nowTick;
				LOG_INFO("(NBN TIER0) RESCUE-REFUSED exp=" << expectedMidi
					<< std::fixed << std::setprecision(6)
					<< " tgt=" << evidence.targetPower
					<< " fifthAbove=" << fifthAbovePower
					<< " - sub-octave note betrayed by its 3rd harmonic at 1.5f."
					<< std::endl);
			}
			return false;
		}

		LOG_INFO("(NBN TIER0) RESCUE exp=" << expectedMidi
			<< std::fixed << std::setprecision(6)
			<< " tgt=" << evidence.targetPower
			<< " worstNeighbour=" << worstNeighbour
			<< " subOct=" << subOctave.targetPower
			<< " subTwelfth=" << subTwelfth.targetPower
			<< " fifthAbove=" << fifthAbovePower
			<< " rms=" << std::setprecision(4) << evidence.totalRms
			<< " - raw fundamental confirmed despite the bin verdict." << std::endl);
		return true;
	}
	volatile bool isChordTier0RescueEnabled = false;

	bool Tier0ConfirmsChord(const int* tones, int count)
	{
		if (tones == nullptr || count < 2) return false;   // singles use Tier0ConfirmsExpected
		const int n = count < 6 ? count : 6;
		double toneHz[6] = { 0 };
		float tonePower[6] = { 0 };
		for (int i = 0; i < n; ++i)
		{
			if (tones[i] < 0) return false;
			toneHz[i] = 440.0 * std::pow(2.0, (static_cast<double>(tones[i]) - 69.0) / 12.0);
			if (!QueryTier0WideProbePower(toneHz[i], tonePower[i])) return false;
			if (tonePower[i] < 1e-4f) return false;         // every tone must carry real energy
		}
		const double semi = std::pow(2.0, 1.0 / 12.0);
		for (int i = 0; i < n; ++i)
		{
			for (int side = 0; side < 2; ++side)
			{
				const int nbMidi = tones[i] + (side == 0 ? 1 : -1);
				bool nearOtherTone = false;
				for (int j = 0; j < n; ++j)
					if (j != i && std::abs(nbMidi - tones[j]) <= 1) { nearOtherTone = true; break; }
				if (nearOtherTone) continue;
				float nbPower = 0.0f;
				if (!QueryTier0WideProbePower(side == 0 ? toneHz[i] * semi : toneHz[i] / semi, nbPower))
					continue;
				if (tonePower[i] < nbPower * 2.0f) return false;   // a neighbour rivals the tone
			}
		}
		return true;   // every expected tone present and dominant
	}
	constexpr uintptr_t NATIVE_STARTAT_CORE = 0x4749C0;

	struct GameTagString
	{
		char inlineBuffer[0x10];
		char* endPointer;
		void* inlineMarker;
	};
	static_assert(sizeof(GameTagString) == 0x18, "tag string layout is 0x18 bytes");

	void CallNativeStartAtNow(void* owner)
	{
		const uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		uintptr_t ownerVtable = 0;
		if (TryRead(ownerAddress, ownerVtable) && ownerVtable == LAS_OWNER_VTABLE)
		{
			LOG_INFO("(NBN NATIVE SEEK) REFUSED StartAt core on the LAS owner 0x"
				<< std::hex << ownerAddress << std::dec
				<< ": 0x4749C0 writes the frozen-object field owner+0x5E3, which is"
				<< " out of bounds on the 0x5D0-byte LAS object (heap smash). The"
				<< " coordinated rebuild carries the resume." << std::endl);
			return;
		}
		GameTagString emptyTag = {};
		emptyTag.endPointer = emptyTag.inlineBuffer;
		emptyTag.inlineMarker = &emptyTag.endPointer;
		GameTagString* tagPtr = &emptyTag;
		const uintptr_t fn = NATIVE_STARTAT_CORE;
		LOG_INFO("(NBN NATIVE SEEK) Calling the StartAt core with an empty tag"
			<< " (resume + seek-to-now + speed reset) on owner=0x" << std::hex
			<< ownerAddress << std::dec << "." << std::endl);
		__asm {
			push esi
			push edi
			mov eax, ownerAddress
			push tagPtr
			mov ecx, fn
			call ecx
			pop edi
			pop esi
		}
		LOG_INFO("(NBN NATIVE SEEK) StartAt core returned." << std::endl);
	}
	volatile bool isNativeReleaseEnabled = true;
	constexpr uintptr_t OWNER_PRESENTATION_COMPONENT = 0x348;
	constexpr uintptr_t PRESENTATION_ACTIVE_FLAG = 0x9C;
	bool musicStoppedByHold = false;
	float musicStoppedEpoch = 0.0f;
	void LogNativeFreezeGroundTruth(void* owner, const char* moment)
	{
		const auto ownerAddress = reinterpret_cast<uintptr_t>(owner);
		uintptr_t chainRoot = 0, chainHop = 0, resolved = 0, resolvedVtable = 0;
		const bool chainAlive = TryRead(CHORD_DISPLAY_GLOBAL, chainRoot) && chainRoot != 0
			&& TryRead(chainRoot + CHORD_DISPLAY_GLOBAL_HOP, chainHop) && chainHop != 0
			&& TryRead(chainHop + CHORD_DISPLAY_WRAPPER_SLOT, resolved) && resolved != 0;
		if (chainAlive) TryRead(resolved, resolvedVtable);
		uintptr_t scheduler = 0;
		TryRead(NATIVE_SCHEDULER_GLOBAL, scheduler);
		uintptr_t tagBegin = 0, tagEnd = 0;
		uint8_t resumeFlag = 0xFF;
		double freezeStart = 0.0;
		TryRead(ownerAddress + OWNER_FREEZE_TAG_BEGIN, tagBegin);
		TryRead(ownerAddress + OWNER_FREEZE_TAG_END, tagEnd);
		TryRead(ownerAddress + OWNER_RESUME_FROM_TAG, resumeFlag);
		TryRead(ownerAddress + OWNER_FREEZE_START_TIME, freezeStart);
		LOG_INFO("(NBN NATIVE GROUND) moment=" << moment
			<< " geChain=" << (chainAlive ? "alive" : "dead")
			<< " resolved=0x" << std::hex << resolved
			<< " resolvedVtable=0x" << resolvedVtable
			<< " (owner=0x" << ownerAddress
			<< " ownerVtable=0x" << LAS_OWNER_VTABLE << ")"
			<< " scheduler=0x" << scheduler << std::dec
			<< " tagLen=" << (tagEnd >= tagBegin ? tagEnd - tagBegin : 0)
			<< " resumeFlag=" << static_cast<int>(resumeFlag)
			<< " freezeStart=" << std::fixed << std::setprecision(3) << freezeStart
			<< std::endl);
	}
	bool TryWriteGameByte(uintptr_t address, uint8_t value) noexcept;

	bool TryWriteGameBytes(uintptr_t destination, const void* source, size_t length) noexcept
	{
		__try
		{
			memcpy(reinterpret_cast<void*>(destination), source, length);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void ShowNativeChordPanel(void* owner, int chordId)
	{
		(void)owner; // the scoring owner's +0x78 is NOT the chord display; see above.
		if (!isNativeChordPanelEnabled || chordId < 0) return;
		uintptr_t globalHead = 0;
		uintptr_t hop = 0;
		uintptr_t wrapper = 0;
		uintptr_t component = 0;
		uintptr_t base = 0;
		uintptr_t end = 0;
		if (!TryRead(CHORD_DISPLAY_GLOBAL, globalHead) || globalHead == 0
			|| !TryRead(globalHead + CHORD_DISPLAY_GLOBAL_HOP, hop) || hop == 0
			|| !TryRead(hop + CHORD_DISPLAY_WRAPPER_SLOT, wrapper) || wrapper == 0
			|| !TryRead(wrapper + OWNER_CHORD_DISPLAY_COMPONENT, component)
			|| component == 0
			|| !TryRead(component + CHORD_DISPLAY_TEMPLATE_BEGIN, base) || base == 0
			|| !TryRead(component + CHORD_DISPLAY_TEMPLATE_END, end)
			|| end <= base
			|| static_cast<uintptr_t>(chordId) >= (end - base) / sizeof(ChordTemplateView))
		{
			LOG_INFO("(NBN LAS CHORD PANEL) Chord display component unavailable for chordId="
				<< chordId << "; panel not driven." << std::endl);
			return;
		}
		ChordTemplateView view = {};
		if (!TryRead(base + static_cast<uintptr_t>(chordId) * sizeof(ChordTemplateView), view))
		{
			return;
		}
		const bool wroteTemplate = TryWriteGameBytes(
			component + CHORD_DISPLAY_SLOT, &view, sizeof(view));
		const bool wroteVisible = TryWriteGameByte(component + CHORD_DISPLAY_VISIBLE, 1);
		if (wroteTemplate && wroteVisible) shownChordPanelComponent = component;
		char safeName[sizeof(view.name) + 1] = {};
		memcpy(safeName, view.name, sizeof(view.name));
		LOG_INFO("(NBN LAS CHORD PANEL) Native chord display driven for chordId=" << chordId
			<< " name=" << (safeName[0] != '\0' ? safeName : "?")
			<< " template=" << wroteTemplate << " visible=" << wroteVisible
			<< "." << std::endl);
	}

	void HideNativeChordPanel()
	{
		if (shownChordPanelComponent == 0) return;
		uintptr_t globalHead = 0;
		uintptr_t hop = 0;
		uintptr_t wrapper = 0;
		uintptr_t component = 0;
		const bool stillLive =
			TryRead(CHORD_DISPLAY_GLOBAL, globalHead) && globalHead != 0
			&& TryRead(globalHead + CHORD_DISPLAY_GLOBAL_HOP, hop) && hop != 0
			&& TryRead(hop + CHORD_DISPLAY_WRAPPER_SLOT, wrapper) && wrapper != 0
			&& TryRead(wrapper + OWNER_CHORD_DISPLAY_COMPONENT, component)
			&& component == shownChordPanelComponent;
		if (stillLive) TryWriteGameByte(shownChordPanelComponent + CHORD_DISPLAY_VISIBLE, 0);
		shownChordPanelComponent = 0;
		LOG_INFO("(NBN LAS CHORD PANEL) Native chord display hidden (live=" << stillLive
			<< ")." << std::endl);
	}

	bool TryWriteGameByte(uintptr_t address, uint8_t value) noexcept
	{
		__try
		{
			*reinterpret_cast<volatile uint8_t*>(address) = value;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
	void* freezeFlagSetOwner = nullptr;

	void SetNativeFreezeFlag(void* owner, bool frozen)
	{
		if (owner == nullptr) return;
		if (frozen && !isNativeFreezeFlagEnabled) return;
		if (!frozen && owner != freezeFlagSetOwner) return;
		if (frozen)
		{
			uintptr_t tagBegin = 0, tagEnd = 0;
			uint8_t resumeFlag = 0xFF;
			const bool readable =
				TryRead(reinterpret_cast<uintptr_t>(owner) + OWNER_FREEZE_TAG_BEGIN, tagBegin)
				&& TryRead(reinterpret_cast<uintptr_t>(owner) + OWNER_FREEZE_TAG_END, tagEnd)
				&& TryRead(reinterpret_cast<uintptr_t>(owner) + OWNER_RESUME_FROM_TAG, resumeFlag);
			constexpr uintptr_t TAG_LENGTH_SANITY_LIMIT = 64u * 1024u * 1024u;
			const uintptr_t tagLength = tagEnd >= tagBegin ? tagEnd - tagBegin : 0;
			const bool tagSane = readable
				&& tagBegin != 0
				&& tagEnd > tagBegin
				&& tagLength <= TAG_LENGTH_SANITY_LIMIT
				&& (resumeFlag == 0 || resumeFlag == 1);
			if (!tagSane)
			{
				LOG_INFO("(NBN LAS FREEZE FLAG) freeze-family fields read uninitialized"
					<< " (tagBegin=0x" << std::hex << tagBegin
					<< " tagEnd=0x" << tagEnd << std::dec
					<< " resumeFlag=" << static_cast<int>(resumeFlag)
					<< "); no write performed." << std::endl);
				return;
			}
			LOG_INFO("(NBN LAS FREEZE FLAG) tag sane at write time: tagLen=" << tagLength
				<< " resumeFlag=" << static_cast<int>(resumeFlag) << "." << std::endl);
		}
		const bool didWrite = TryWriteGameByte(
			reinterpret_cast<uintptr_t>(owner) + OWNER_FROZEN_ON_TAG, frozen ? 1 : 0);
		freezeFlagSetOwner = (frozen && didWrite) ? owner : nullptr;
		LOG_INFO("(NBN LAS FREEZE FLAG) owner+0x5E3 <- " << (frozen ? 1 : 0)
			<< " written=" << didWrite << "." << std::endl);
	}
	uintptr_t sanitizedFreezeOwners[8] = {};
	size_t sanitizedFreezeOwnerNext = 0;

	void SanitizeOwnerFreezeStateOnce(void* owner)
	{
		const auto base = reinterpret_cast<uintptr_t>(owner);
		if (base == 0) return;
		for (const auto known : sanitizedFreezeOwners)
		{
			if (known == base) return;
		}
		sanitizedFreezeOwners[sanitizedFreezeOwnerNext] = base;
		sanitizedFreezeOwnerNext = (sanitizedFreezeOwnerNext + 1)
			% (sizeof(sanitizedFreezeOwners) / sizeof(sanitizedFreezeOwners[0]));

		uintptr_t tagBegin = 0, tagEnd = 0;
		TryRead(base + OWNER_FREEZE_TAG_BEGIN, tagBegin);
		TryRead(base + OWNER_FREEZE_TAG_END, tagEnd);
		const uintptr_t tagBase = base + OWNER_FREEZE_TAG_STRING;
		const uint32_t bufferAddress = static_cast<uint32_t>(tagBase);
		const uint32_t endFieldAddress = static_cast<uint32_t>(base + OWNER_FREEZE_TAG_BEGIN);
		const double zeroTime = 0.0;
		const bool wroteBegin = TryWriteGameBytes(
			base + OWNER_FREEZE_TAG_BEGIN, &bufferAddress, sizeof(bufferAddress));
		const bool wroteEnd = TryWriteGameBytes(
			base + OWNER_FREEZE_TAG_END, &endFieldAddress, sizeof(endFieldAddress));
		const bool wroteNul = TryWriteGameByte(tagBase, 0);
		const bool wroteFrozenSong = TryWriteGameByte(base + 0x5E2, 0);
		const bool wroteResume = TryWriteGameByte(base + OWNER_RESUME_FROM_TAG, 0);
		const bool wroteFrozen = TryWriteGameByte(base + OWNER_FROZEN_ON_TAG, 0);
		const bool wroteStart = TryWriteGameBytes(
			base + OWNER_FREEZE_START_TIME, &zeroTime, sizeof(zeroTime));
		LOG_INFO("(NBN LAS FREEZE INIT) Owner 0x" << std::hex << base
			<< " freeze state initialized to the valid empty form at bootstrap"
			<< " (was tagBegin=0x" << tagBegin << " tagEnd=0x" << tagEnd << std::dec
			<< "); wrote begin/end/nul/frozenSong/resume/frozen/start="
			<< wroteBegin << wroteEnd << wroteNul << wroteFrozenSong
			<< wroteResume << wroteFrozen << wroteStart << "." << std::endl);
	}

	void PerformOwnedReleaseAtEpoch(void* owner, float releaseEpoch, const char* reason)
	{
		musicStoppedByHold = false;
		LogNativeFreezeGroundTruth(owner, "release");
		SetNativeFreezeFlag(owner, false);
		if (isScheduleShiftEnabled && hasTransportFrozenAt)
		{
			const double frozenSeconds = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - transportFrozenAt).count();
			CallNativeScheduleShift(frozenSeconds);
		}
		hasTransportFrozenAt = false;
		HideNativeChordPanel();
		if (isNativeReleaseEnabled)
		{
			LogNativeFreezeGroundTruth(owner, "native-release-before");
			LOG_INFO("(NBN NATIVE RELEASE) Running the game's own resume (StartAt core,"
				<< " empty tag) instead of the coordinated PlayerSong restart at epoch="
				<< std::fixed << std::setprecision(6) << releaseEpoch
				<< " heldEpoch=" << heldEpoch << " (" << reason << ")." << std::endl);
			reinterpret_cast<MarkNotesFn>(MARK_ACTIVE_NOTES)(nullptr, owner);
			CallNativeStartAtNow(owner);
			reinterpret_cast<ThiscallFloatFn>(COORDINATED_REBUILD)(owner, nullptr, releaseEpoch);
			uintptr_t component = 0;
			uint8_t presentationActive = 0xFF;
			if (TryRead(reinterpret_cast<uintptr_t>(owner) + OWNER_PRESENTATION_COMPONENT, component)
				&& component != 0)
			{
				TryRead(component + PRESENTATION_ACTIVE_FLAG, presentationActive);
			}
			LOG_INFO("(NBN NATIVE RELEASE) Hybrid release done: StartAt core + coordinated"
				<< " restart; presentationActive=" << static_cast<int>(presentationActive)
				<< "." << std::endl);
			LogNativeFreezeGroundTruth(owner, "native-release-after");
		}
		else
		{
			LOG_INFO("(NBN LAS RELEASE) Marking active native scoring notes and requesting the coordinated"
				<< " PlayerSong restart at epoch=" << std::fixed << std::setprecision(6) << releaseEpoch
				<< " (" << reason << ")." << std::endl);
			reinterpret_cast<MarkNotesFn>(MARK_ACTIVE_NOTES)(nullptr, owner);
			reinterpret_cast<ThiscallFloatFn>(COORDINATED_REBUILD)(owner, nullptr, releaseEpoch);
		}

		uintptr_t playerSong = reinterpret_cast<uintptr_t>(heldPlayerSong);
		uint32_t pendingState = 0;
		uint8_t pendingFlag = 0;
		uint8_t running = 0;
		uint8_t stopped = 0;
		TryRead(playerSong + PLAYER_SONG_PENDING_STATE, pendingState);
		TryRead(playerSong + PLAYER_SONG_PENDING_FLAG, pendingFlag);
		TryRead(playerSong + PLAYER_SONG_RUNNING, running);
		TryRead(playerSong + PLAYER_SONG_STOPPED, stopped);
		float clockPrimary = 0.0f;
		TryRead(reinterpret_cast<uintptr_t>(owner) + OWNER_CLOCK_PRIMARY, clockPrimary);
		LOG_INFO("(NBN LAS RELEASE) Post-release PlayerSong pendingState=" << pendingState
			<< " pendingFlag=" << static_cast<int>(pendingFlag)
			<< " running=" << static_cast<int>(running)
			<< " stopped=" << static_cast<int>(stopped)
			<< " ownerClock=" << std::fixed << std::setprecision(6) << clockPrimary
			<< "." << std::endl);
	}

	void PerformOwnedRelease(void* owner, const char* reason)
	{
		PerformOwnedReleaseAtEpoch(owner, heldEpoch, reason);
	}
	void FaultWithoutRelease(const std::string& reason)
	{
		void* owner = trackedOwner;
		LOG_ERROR("(NBN LAS FAULT) Recovering in place instead of disabling NBN ("
			<< reason << "); re-bootstrapping for the current section." << std::endl);
		ResetBootstrap(reason.c_str(), owner, true);
		trackedOwner = owner;
	}
	void ResetBootstrap(const char* reason, void* liveOwner = nullptr, bool releaseStoppedMusic = false)
	{
		if (releaseStoppedMusic && musicStoppedByHold && liveOwner != nullptr)
		{
			LOG_ERROR("(NBN LAS LIFECYCLE) Resuming the music the hold stopped before resetting (" << reason << ")." << std::endl);
			PerformOwnedReleaseAtEpoch(liveOwner, musicStoppedEpoch, reason);
		}
		musicStoppedByHold = false;   // whatever happened, a reset leaves no hold that owns a stopped song
		if (OwnsNativeHold())
		{
			LOG_ERROR("(NBN LAS LIFECYCLE) Bootstrap reset while a native hold was owned (" << reason
				<< "); the hold is abandoned without further native calls because its owner is no longer current."
				<< std::endl);
			if (liveOwner != nullptr && liveOwner == freezeFlagSetOwner)
			{
				SetNativeFreezeFlag(liveOwner, false);
			}
			else if (freezeFlagSetOwner != nullptr)
			{
				LOG_ERROR("(NBN LAS FREEZE FLAG) Tracking for a set owner+0x5E3 dropped without a"
					<< " clear write; the owner is not provably alive." << std::endl);
				freezeFlagSetOwner = nullptr;
			}
		}
		RestorePromotedRepeatChord(liveOwner != nullptr);
		ClearSelection();
		previousSelectedString = -1;
		trackedOwner = nullptr;
		hasLastUpdateTime = false;
		isEpochConfirmed = false;
		pickedAttacks.Clear();
		chordAttacks.Clear();
		pickScanSample = 0;
		latestRawAudioSample = 0;
		rawAttackStreamAvailable = false;
		chordHoldNeedsCaptureBoundary = false;
		pickSampleRate = 0;
		confirmedViaGrid = false;
		hasNativeSectionRangeFailureLogged = false;
		gridLatchRefusals = 0;
		hasGridLatchRefusalLogged = false;
		rollbackIdentityMisses = 0;
		hasRollbackIdentityMissLogged = false;
		hasPendingBoundary = false;
		pendingBoundaryBeforeRollback = 0.0f;
		pendingBoundaryAfterRollback = 0.0f;
		greyCutoff = 0.0f;
		sectionEndBoundary = 0.0f;
		hasSectionEndBoundary = false;
		epochIndex = 0;
		consumedRecords.clear();
	}
	void LogPhraseIterationSpread(void* owner, int32_t selectedIteration, float selectedTime)
	{
		struct Span
		{
			int32_t iteration = -1;
			uint32_t count = 0;
			float earliest = 0.0f;
			float latest = 0.0f;
		};
		std::vector<Span> spans;
		uint32_t unreadable = 0;

		ForEachLiveNote(owner, [&](const LiveNote& note)
		{
			if (note.record == 0) return true;
			int32_t iteration = -1;
			if (!TryRead(note.record + RECORD_PHRASE_ITERATION, iteration))
			{
				++unreadable;
				return true;
			}
			for (auto& span : spans)
			{
				if (span.iteration != iteration) continue;
				++span.count;
				if (note.recordTime < span.earliest) span.earliest = note.recordTime;
				if (note.recordTime > span.latest) span.latest = note.recordTime;
				return true;
			}
			Span span;
			span.iteration = iteration;
			span.count = 1;
			span.earliest = note.recordTime;
			span.latest = note.recordTime;
			spans.push_back(span);
			return true;
		});

		if (spans.empty()) return;
		std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b)
		{
			return a.earliest < b.earliest;
		});

		std::ostringstream summary;
		for (size_t index = 0; index < spans.size(); ++index)
		{
			if (index != 0) summary << "  ";
			summary << "it" << spans[index].iteration
				<< "=" << spans[index].count << "n"
				<< "[" << std::fixed << std::setprecision(3) << spans[index].earliest
				<< ".." << spans[index].latest << "]";
		}
		LOG_INFO("(NBN LAS PHRASE) selected=it" << selectedIteration
			<< "@" << std::fixed << std::setprecision(3) << selectedTime
			<< " liveIterations=" << spans.size()
			<< (unreadable != 0 ? " unreadable=" + std::to_string(unreadable) : "")
			<< " | " << summary.str() << std::endl);
	}


	bool FindEarliestEligibleNote(void* owner, LiveNote& earliest, int legatoReferenceString)
	{
		(void)legatoReferenceString;
		bool hasEarliest = false;
		ForEachLiveNote(owner, [&](const LiveNote& note)
		{
			if ((note.mask & NOTE_MASK_CHILD) != 0
				&& ((note.mask & NOTE_MASK_BEND) == 0 || isFlowUntilMissEnabled))
			{
				return true;
			}
			if (note.stateC0 != 0 && note.stateC1 != 0) return true;
			if (note.recordTime < greyCutoff - GREY_EPSILON) return true;
			if (hasSectionEndBoundary
				&& note.recordTime >= sectionEndBoundary - GREY_EPSILON)
			{
				return true;
			}
			if (consumedRecords.count(note.record) != 0) return true;
			if (note.record == lastCommittedChordRecord
				&& lastCommittedChordRecord != 0
				&& std::chrono::duration<double>(std::chrono::steady_clock::now()
					- lastCommittedChordAt).count() < COMMITTED_CHORD_RESELECT_GUARD_SECONDS)
			{
				return true;
			}
			if (!hasEarliest || note.recordTime < earliest.recordTime
				|| (note.recordTime == earliest.recordTime && note.record < earliest.record))
			{
				earliest = note;
				hasEarliest = true;
			}
			return true;
		});
		return hasEarliest;
	}

	void UpdateSelection(void* owner, float updateTime)
	{
		LiveNote earliest;
		bool hasEarliest = FindEarliestEligibleNote(owner, earliest, previousSelectedString);

		if (!hasEarliest)
		{
			if (selectedRecord != 0)
			{
				LOG_INFO("(NBN LAS SELECT) No eligible unresolved native record remains in the live vector."
					<< std::endl);
				ClearSelection();
			}
			return;
		}

		if (earliest.record == selectedRecord) return;

		selectedRecord = earliest.record;
		selectedRecordTime = earliest.recordTime;
		uint8_t stringIndex = 0;
		uint8_t fret = 0;
		int32_t chordId = -1;
		int32_t chordNotesId = -1;
		selectedString = TryRead(earliest.record + RECORD_STRING, stringIndex) ? stringIndex : -1;
		selectedFret = TryRead(earliest.record + RECORD_FRET, fret) ? fret : -1;
		selectedChordId = TryRead(earliest.record + RECORD_CHORD_ID, chordId) ? chordId : -1;
		selectedChordNotesId = TryRead(earliest.record + RECORD_CHORD_NOTES_ID, chordNotesId)
			? chordNotesId
			: -1;
		if (!TryRead(static_cast<uintptr_t>(ENGINE_COMPENSATION), selectedCompensation)
			|| selectedCompensation < 0.04 || selectedCompensation > 0.07)
		{
			FaultWithoutRelease("The native 0.053 engine compensation constant could not be validated");
			return;
		}
		selectedHoldTime = selectedRecordTime - HoldCompensationSeconds();
		expectedMidi = -1;
		armedExpectedMidi = -1;
		armedBufferCommitRecord = 0;
		wasArmedBufferCommitLogged = false;
		isHoldSuppressed = false;
		gatePhase = GatePhase::Armed;
		int32_t selectedPhraseIteration = -1;
		TryRead(selectedRecord + RECORD_PHRASE_ITERATION, selectedPhraseIteration);
		LOG_INFO("(NBN LAS SELECT) epoch=" << epochIndex
			<< " record=0x" << std::hex << selectedRecord
			<< " mask=0x" << earliest.mask << std::dec
			<< " time=" << std::fixed << std::setprecision(6) << selectedRecordTime
			<< " string=" << selectedString << " fret=" << selectedFret
			<< " phraseIteration=" << selectedPhraseIteration
			<< " windowEntry=" << earliest.windowEntry
			<< " windowExit=" << earliest.windowExit
			<< " lateWindow=" << (earliest.windowExit - selectedRecordTime)
			<< " holdTime=" << selectedHoldTime
			<< " updateTime=" << updateTime << "." << std::endl);
		LogPhraseIterationSpread(owner, selectedPhraseIteration, selectedRecordTime);
		EmitCandidateEvent(owner, updateTime, earliest,
			ResearchProtocol::ExpectedAttackEventKind::CandidateChanged);
	}

	bool AreOwnerClocksAtEpoch(void* owner, float epoch)
	{
		uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		float clocks[5] = {};
		bool clocksMatch = TryRead(ownerAddress + OWNER_CLOCK_PRIMARY, clocks[0])
			&& TryRead(ownerAddress + OWNER_CLOCK_SECONDARY, clocks[1])
			&& TryRead(ownerAddress + OWNER_CLOCK_RENDER, clocks[2])
			&& TryRead(ownerAddress + OWNER_CLOCK_EPOCH_LOW, clocks[3])
			&& TryRead(ownerAddress + OWNER_CLOCK_EPOCH_HIGH, clocks[4]);
		for (float clock : clocks)
		{
			if (clock != epoch) clocksMatch = false;
		}
		return clocksMatch;
	}

	bool SetAndVerifyHeldEpoch(void* owner, float epoch)
	{
		reinterpret_cast<ThiscallFloatFn>(SET_FIVE_CLOCKS)(owner, nullptr, epoch);
		return AreOwnerClocksAtEpoch(owner, epoch);
	}

	bool EstablishHold(void* owner, float updateTime, const LiveNote& selected)
	{
		uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		uintptr_t ownerVtable = 0;
		if (!TryRead(ownerAddress, ownerVtable) || ownerVtable != LAS_OWNER_VTABLE)
		{
			FaultWithoutRelease("The scoring owner does not expose the GamePlaysongLAS vtable required for the hold");
			return false;
		}
		uintptr_t slotSetClocks = 0;
		uintptr_t slotRebuild = 0;
		uintptr_t slotDecision = 0;
		if (!TryRead(ownerVtable + 0x50, slotSetClocks) || slotSetClocks != SET_FIVE_CLOCKS
			|| !TryRead(ownerVtable + 0x54, slotRebuild) || slotRebuild != COORDINATED_REBUILD
			|| !TryRead(ownerVtable + 0xF0, slotDecision) || slotDecision != NATIVE_HIT_DECISION)
		{
			FaultWithoutRelease("The live GamePlaysongLAS vtable slots do not match the proven +0x50/+0x54/+0xF0 methods");
			return false;
		}

		void* playerSong = ResolvePlayerSong(owner);
		if (playerSong == nullptr)
		{
			FaultWithoutRelease("Exactly one live GameComponentPlayerSong could not be resolved from the owner's component collection");
			return false;
		}
		uintptr_t playerSongAddress = reinterpret_cast<uintptr_t>(playerSong);
		uintptr_t playerSongVtable = 0;
		uintptr_t slotStop = 0;
		if (!TryRead(playerSongAddress, playerSongVtable) || playerSongVtable != PLAYER_SONG_VTABLE
			|| !TryRead(playerSongVtable + 0x24, slotStop) || slotStop != STOP_TMUSIC)
		{
			FaultWithoutRelease("The resolved PlayerSong vtable or its Stop_TMusic slot does not match the proven layout");
			return false;
		}
		uint32_t playerSongMode = 0;
		if (!TryRead(playerSongAddress + PLAYER_SONG_MODE, playerSongMode) || playerSongMode != 1)
		{
			FaultWithoutRelease("PlayerSong is not in the mode-1 music state required by Stop_TMusic");
			return false;
		}
		RefreshArrangementInstrument();
		int16_t tuningOffset = 0;
		if (selectedChordId != -1)
		{
			expectedMidi = -1;
			isBendTarget = false;
			isBendChildTarget = false;
			bendAcceptMidi = -1;
			isLegatoTarget = false;
			isHammerOnTarget = false;
			legatoConfirmTickCount = 0;
			hasLegatoPitchDeparted = false;
			legatoRunCount = 0;
			isConfirmingLegatoRun = false;
			chordDecisionEvalCount = 0;
			hasChordDecisionLogAnchor = false;
			chordSoundingPeakAttack = 0;
			chordSoundingPeak = 0;
		}
		else
		{
			if (selectedString < 0 || selectedString > 5 || selectedFret < 0)
			{
				FaultWithoutRelease("The selected record's string/fret identity is outside the supported guitar range");
				return false;
			}
			const int inputShift = ResearchProbeRuntime::GetInputOnsetShiftSemitones();
			if (TryRead(TUNING_OFFSETS + static_cast<uintptr_t>(selectedString) * 2, tuningOffset)
				&& tuningOffset >= -12 && tuningOffset <= 12)
			{
				if (!NoteByNote::TryResolvePlayedNotePitch(selectedString, selectedFret, tuningOffset, inputShift, expectedMidi,
					IsBassArrangement()))
				{
					FaultWithoutRelease("The selected note's played pitch is invalid");
					return false;
				}
			}
			else
			{
				FaultWithoutRelease("The authored per-string tuning offset could not be read");
				return false;
			}
			selectedNativeTone = -1;
			{
				uintptr_t singleTemplate = 0;
				ChordTemplateView singleView = {};
				if (TryRead(selected.note + 0x30, singleTemplate) && singleTemplate != 0
					&& TryRead(singleTemplate, singleView)
					&& singleView.notes[selectedString] >= 0)
				{
					selectedNativeTone = singleView.notes[selectedString];
				}
			}
			isBendTarget = (selected.mask & NOTE_MASK_BEND) != 0;
			isBendChildTarget = isBendTarget && (selected.mask & NOTE_MASK_CHILD) != 0;
			ResolveBendAcceptance(selectedRecord);
			isLegatoTarget = NoteByNote::UsesNoPickLegatoAcceptance(selected.mask)
				&& NoteByNote::LegatoContinuesPreviousString(previousSelectedString, selectedString);
			isHammerOnTarget = NoteByNote::IsHammerOn(selected.mask);
			legatoConfirmTickCount = 0;
			hasLegatoPitchDeparted = false;
			BuildLegatoRun(owner, selectedRecordTime, selectedString);
		}

		if (updateTime < selectedHoldTime)
		{
			FaultWithoutRelease("The late-hold boundary is too far from the current authoritative scoring time");
			return false;
		}
		const float maxOvershoot = isFlowUntilMissEnabled ? FLOW_HOLD_BOUNDARY_MAX_OVERSHOOT : HOLD_BOUNDARY_MAX_OVERSHOOT;
		if (updateTime - selectedHoldTime > maxOvershoot)
		{
			isHoldSuppressed = true;
			LOG_ERROR("(NBN LAS HOLD) Hold boundary overshot by "
				<< std::fixed << std::setprecision(3) << (updateTime - selectedHoldTime)
				<< "s (max " << maxOvershoot << ") for record=0x"
				<< std::hex << selectedRecord << std::dec
				<< "; the record plays through natively instead of faulting."
				<< std::endl);
			return false;
		}
		float epoch = selectedHoldTime;

		uint8_t childFlag = 0;
		TryRead(ownerAddress + OWNER_CHILD_FLAG, childFlag);
		uint8_t runningBefore = 0;
		uint8_t stoppedBefore = 0;
		TryRead(playerSongAddress + PLAYER_SONG_RUNNING, runningBefore);
		TryRead(playerSongAddress + PLAYER_SONG_STOPPED, stoppedBefore);

		LOG_INFO("(NBN LAS HOLD) Requesting the lesson-derived hold: record=0x" << std::hex << selectedRecord
			<< " playerSong=0x" << playerSongAddress << std::dec
			<< " string=" << selectedString << " fret=" << selectedFret
			<< " updateTime=" << std::fixed << std::setprecision(6) << updateTime
			<< " targetHoldTime=" << selectedHoldTime
			<< " heldEpoch=" << epoch
			<< " compensation=" << selectedCompensation
			<< " childFlag=" << static_cast<int>(childFlag)
			<< " runningBefore=" << static_cast<int>(runningBefore)
			<< " stoppedBefore=" << static_cast<int>(stoppedBefore)
			<< "." << std::endl);
		if (!ArmSelectedNativePrompt(selected))
		{
			if (selectedChordId != -1)
			{
				uintptr_t wrapper = 0;
				uintptr_t controller = 0;
				uintptr_t vtable = 0;
				TryRead(selected.note + NOTE_VFX_WRAPPER, wrapper);
				if (wrapper != 0) TryRead(wrapper, controller);
				if (controller != 0) TryRead(controller, vtable);
				LOG_INFO("(NBN LAS CHORD) The single-note prompt fork does not arm for a"
					<< " chord record; holding without the freeze-note presentation."
					<< " wrapper=0x" << std::hex << wrapper
					<< " controller=0x" << controller
					<< " vtable=0x" << vtable << std::dec << std::endl);
			}
			else
			{
				FaultWithoutRelease("The selected native NoteVfx controller could not arm its validated prompt fork");
				return false;
			}
		}
		if (selectedChordId != -1)
		{
			LOG_INFO("(NBN LAS CHORD) Skipping Play_FreezeNoteTrack for the chord hold"
				<< " so the chord panel survives on the unswept track." << std::endl);
			ShowNativeChordPanel(owner, selectedChordId);
		}
		else
		{
			PlayFreezeNoteTrack();
		}
		reinterpret_cast<ThiscallVoidFn>(STOP_TMUSIC)(playerSong, nullptr);
		musicStoppedByHold = true;
		musicStoppedEpoch = epoch;

		uint8_t runningAfter = 0xFF;
		uint8_t stoppedAfter = 0xFF;
		TryRead(playerSongAddress + PLAYER_SONG_RUNNING, runningAfter);
		TryRead(playerSongAddress + PLAYER_SONG_STOPPED, stoppedAfter);
		if (runningAfter != 0 || stoppedAfter != 1)
		{
			FaultWithoutRelease(
				"Stop_TMusic did not commit the proven +0xD9=0/+0xDA=1 latched stop state");
			return false;
		}

		if (!SetAndVerifyHeldEpoch(owner, epoch))
		{
			heldEpoch = epoch;
			heldPlayerSong = playerSong;
			PerformOwnedRelease(owner, "the five-clock hold readback failed, so the stopped PlayerSong is released");
			FaultWithoutRelease("The owner's five clocks did not all commit to the held epoch");
			return false;
		}
		heldEpoch = epoch;
		heldPlayerSong = playerSong;
		holdTickCount = 0;
		renderFramesWhileHeld = 0;
		gatePhase = GatePhase::Holding;
		inputReleaseTickCount = 0;
		holdProgressAnchor = std::chrono::steady_clock::now();
		mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
		mlBendConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
		enhancedLegatoConfirmation.Reset(latestRawAudioSample);
		{
			const ULONGLONG nowTick = GetTickCount64();
			LOG_INFO("(NBN FLOW TIMING) hold established "
				<< (g_lastAttackSpikeTick != 0
					? static_cast<long long>(nowTick - g_lastAttackSpikeTick) : -1LL)
				<< " ms after the last attack spike (-1 = none seen this session)"
				<< " recordTime=" << std::fixed << std::setprecision(3) << selectedRecordTime
				<< " heldEpoch=" << heldEpoch << "." << std::endl);
			if (g_lastAttackSpikeTick != 0
				&& nowTick - g_lastAttackSpikeTick <= 700)
			{
				reattackWindowTicks = REATTACK_WINDOW_TICKS;
				reattackStreak = 0;
				spikeRecencyTicks = REATTACK_WINDOW_TICKS;
				LOG_INFO("(NBN LAS INPUT) Hold established "
					<< (nowTick - g_lastAttackSpikeTick)
					<< "ms after an attack spike: opening the raw-confirmation window so"
					<< " a pick played through the transition is judged on its sustain"
					<< " instead of discarded." << std::endl);
			}
		}
		hasHoldProgressAnchor = true;
		lastStuckWarnHeldSeconds = 0.0;
		transportFrozenAt = holdProgressAnchor;
		hasTransportFrozenAt = true;
		sawSpikeDuringHold = false;
		sawLevelSpikeDuringHold = false;
		hasHoldLatchRingTime = TryReadCurrentRingTime(holdLatchRingTime);
		hasNdSoundingStreak = false;
		reattackWindowTicks = 0;
		reattackStreak = 0;
		spikeRecencyTicks = 0;
		ResetChordOnsetEvidenceAnchor();
		int primedOnset = QueryNativeOnsetNote();
		StashPrimedOnset(primedOnset);

		SetNativeFreezeFlag(owner, true);
			LogNativeFreezeGroundTruth(owner, "hold-established");
		LOG_INFO("(NBN LAS HOLD) Established. All five owner clocks hold " << std::fixed
			<< std::setprecision(6) << epoch
			<< "; expectedMidi=" << expectedMidi
			<< " (string " << selectedString << " open "
			<< (expectedMidi >= 0 ? expectedMidi - selectedFret : -1)
			<< " fret " << selectedFret << "), primedOnset=" << primedOnset
			<< "; authoredTemplateTone=" << selectedNativeTone
			<< " inputShift=" << ResearchProbeRuntime::GetInputOnsetShiftSemitones()
			<< ". Scoring and the native onset detector continue at the frozen time."
			<< std::endl);
		lastUpdateTime = epoch;
		EmitCandidateEvent(owner, updateTime, selected,
			ResearchProtocol::ExpectedAttackEventKind::HoldEstablished);
		return true;
	}

	DenseChainResult TryAdvanceDenseChain(void* owner, float /*updateTime*/)
	{
		LiveNote next;
		if (!FindEarliestEligibleNote(owner, next, selectedString))
		{
			return DenseChainResult::NotRequired;
		}

		int32_t nextChordId = -1;
		if (!TryRead(next.record + RECORD_CHORD_ID, nextChordId))
		{
			PerformOwnedRelease(owner, "the dense successor's chord identity was unreadable");
			FaultWithoutRelease("The dense successor's chord identity was unreadable");
			return DenseChainResult::Faulted;
		}
		const bool isChordSuccessor = nextChordId != -1;
		if (isChordSuccessor && !areChordHoldsEnabled) return DenseChainResult::NotRequired;
		if (isChordSuccessor && !areRepeatStrumHoldsEnabled
			&& (next.mask & 0x80000000u) == 0)
		{
			return DenseChainResult::NotRequired;
		}

		float nextHoldTime = next.recordTime - HoldCompensationSeconds();
		float spacing = nextHoldTime - heldEpoch;
		const float denseWindow = isChordSuccessor
			? CHORD_DENSE_WINDOW_SECONDS
			: PLAYER_SONG_RESTART_SECONDS;
		if (spacing <= HELD_TIME_EPSILON || spacing > denseWindow)
		{
			return DenseChainResult::NotRequired;
		}

		uint8_t nextString = 0;
		uint8_t nextFret = 0;
		if (!TryRead(next.record + RECORD_STRING, nextString)
			|| !TryRead(next.record + RECORD_FRET, nextFret)
			|| (!isChordSuccessor && nextString > 5))
		{
			PerformOwnedRelease(owner, "the dense successor's string/fret identity was invalid");
			FaultWithoutRelease("The dense successor's string/fret identity was invalid");
			return DenseChainResult::Faulted;
		}
		int denseNoteMidi = -1;
		if (!isChordSuccessor)
		{
			const int inputShift = ResearchProbeRuntime::GetInputOnsetShiftSemitones();
			int16_t tuningOffset = 0;
			if (TryRead(TUNING_OFFSETS + static_cast<uintptr_t>(nextString) * 2, tuningOffset)
				&& tuningOffset >= -12 && tuningOffset <= 12)
			{
				if (!NoteByNote::TryResolvePlayedNotePitch(nextString, nextFret, tuningOffset, inputShift, denseNoteMidi,
					IsBassArrangement()))
				{
					FaultWithoutRelease("The dense successor's played pitch is invalid");
					return DenseChainResult::Faulted;
				}
			}
			else
			{
				PerformOwnedRelease(owner, "the dense successor's tuning offset was invalid");
				FaultWithoutRelease("The dense successor's tuning offset was invalid");
				return DenseChainResult::Faulted;
			}
		}

		denseSuccessor.record = next.record;
		denseSuccessor.note = next.note;
		denseSuccessor.recordTime = next.recordTime;
		denseSuccessor.holdTime = nextHoldTime;
		denseSuccessor.stringIndex = nextString;
		denseSuccessor.fret = nextFret;
		denseSuccessor.chordId = nextChordId;
		int32_t nextChordNotesId = -1;
		if (!TryRead(next.record + RECORD_CHORD_NOTES_ID, nextChordNotesId))
		{
			PerformOwnedRelease(owner, "the dense successor's chord-notes identity was unreadable");
			FaultWithoutRelease("The dense successor's chord-notes identity was unreadable");
			return DenseChainResult::Faulted;
		}
		denseSuccessor.chordNotesId = nextChordNotesId;
		denseSuccessor.expectedMidi = denseNoteMidi;
		denseSuccessor.isBend = (next.mask & NOTE_MASK_BEND) != 0;
		denseSuccessor.isBendChild = denseSuccessor.isBend
			&& (next.mask & NOTE_MASK_CHILD) != 0;
		denseSuccessor.isLegato = NoteByNote::UsesNoPickLegatoAcceptance(next.mask);
		denseSuccessor.isHammerOn = NoteByNote::IsHammerOn(next.mask);
		if (isChordSuccessor && (next.mask & NOTE_MASK_CHORD_PANEL) == 0)
		{
			PromoteRepeatChordForRedraw(next.record);
		}

		PerformOwnedReleaseAtEpoch(owner, nextHoldTime,
			"the committed dense note is advancing through a coordinated PlayerSong/fretboard rebuild");
		if (!AreOwnerClocksAtEpoch(owner, nextHoldTime))
		{
			heldEpoch = nextHoldTime;
			PerformOwnedRelease(owner, "the dense coordinated rebuild failed its five-clock readback");
			FaultWithoutRelease("The dense coordinated rebuild did not set all five owner clocks to the successor epoch");
			return DenseChainResult::Faulted;
		}

		heldEpoch = nextHoldTime;
		holdTickCount = 0;
		postReleaseTickCount = 0;
		commitTickCount = 0;
		wasCommitOverrideLogged = false;
		renderFramesWhileHeld = 0;
		gatePhase = GatePhase::DenseRebuildPending;
		inputReleaseTickCount = 0;
		denseRebuildQueuedAt = std::chrono::steady_clock::now();   // phase-1 flow instrumentation
		hasDenseRebuildTiming = true;

		int currentMidi = QueryNativeLoudestPlayedNote();
		LOG_INFO("(NBN LAS CHAIN) Queued the dense successor through the coordinated PlayerSong/fretboard rebuild:"
			<< " record=0x" << std::hex << denseSuccessor.record << std::dec
			<< " string=" << denseSuccessor.stringIndex << " fret=" << denseSuccessor.fret
			<< " authoredTime=" << std::fixed << std::setprecision(6) << denseSuccessor.recordTime
			<< " requestedEpoch=" << denseSuccessor.holdTime
			<< " spacing=" << spacing
			<< " restartBoundary=" << PLAYER_SONG_RESTART_SECONDS
			<< " expectedMidi=" << denseSuccessor.expectedMidi
			<< " currentMidi=" << currentMidi
			<< ". Hit decisions remain blocked until the native play packet refreshes the visible fretboard target"
			<< " and Stop_TMusic is re-latched at the same epoch."
			<< std::endl);
		return DenseChainResult::Advanced;
	}
	void ResolveBendAcceptance(uintptr_t record)
	{
		bendAcceptMidi = -1;
		if (!isBendTarget || record == 0 || expectedMidi < 0) return;

		float semitones = 0.0f;
		if (!TryRead(record + RECORD_BEND_AMOUNT, semitones)
			|| !std::isfinite(semitones)
			|| semitones < BEND_AMOUNT_MIN_SEMITONES
			|| semitones > BEND_AMOUNT_MAX_SEMITONES)
		{
			LOG_INFO("(NBN LAS BEND) No usable bend amount at record+0x40 (read "
				<< std::fixed << std::setprecision(3) << semitones
				<< "); falling back to accepting expected+1 through expected+"
				<< MAX_BEND_SEMITONES << "." << std::endl);
			return;
		}

		bendAcceptMidi = expectedMidi + static_cast<int>(std::lround(semitones));
		LOG_INFO("(NBN LAS BEND) Chart bend amount " << std::fixed << std::setprecision(3)
			<< semitones << " semitones, so this bend is accepted only at MIDI "
			<< bendAcceptMidi << " rather than anywhere from " << (expectedMidi + 1)
			<< " to " << (expectedMidi + MAX_BEND_SEMITONES)
			<< ". Crossing the window mid-bend no longer completes it." << std::endl);
	}

	bool IsAcceptedOnset(int onset)
	{
		if (onset < 0 || expectedMidi < 0) return false;
		if (!isBendTarget && onset != expectedMidi) return false;
		if (IsPlainPickedTarget())
		{
			return acceptedPickRecord == selectedRecord && acceptedPick.confirmedMidi == expectedMidi;
		}
		if (onset == expectedMidi) return true;
		if (!isBendTarget) return false;
		if (bendAcceptMidi >= 0) return onset == bendAcceptMidi;
		return onset > expectedMidi && onset <= expectedMidi + MAX_BEND_SEMITONES;
	}
	bool MatchesPickPitch(int sounding)
	{
		if (sounding < 0 || expectedMidi < 0) return false;
		if (sounding == expectedMidi) return true;
		if (!isBendTarget || sounding < expectedMidi) return false;
		const int top = bendAcceptMidi >= 0
			? bendAcceptMidi
			: expectedMidi + MAX_BEND_SEMITONES;
		return sounding <= top;
	}
	void StashPrimedOnset(int primedOnset)
	{
		pendingPrimedOnset = -1;
		if (primedOnset < 0) return;
		const bool mayBeCarriedBend = previousWasBend
			&& primedOnset >= previousExpectedMidi - MAX_BEND_SEMITONES
			&& primedOnset <= previousExpectedMidi;
		if (primedOnset == previousExpectedMidi || mayBeCarriedBend) return;
		if (!IsAcceptedOnset(primedOnset)) return;
		pendingPrimedOnset = primedOnset;
		LOG_INFO("(NBN LAS INPUT) Primed onset " << primedOnset
			<< " matches the new target and cannot be the previous note ("
			<< previousExpectedMidi << "); keeping the played-ahead pick for the"
			<< " first holding tick instead of discarding it." << std::endl);
	}
	void BuildVisualGroup(void* owner, uintptr_t groupRecord, float groupTime, int groupString)
	{
		(void)owner; (void)groupRecord; (void)groupTime; (void)groupString;
		visualGroupCount = 0;
	}
	void BeginBendConfirmation()
	{
		if (legatoRunCount < MAX_LEGATO_RUN)
		{
			for (uint32_t index = legatoRunCount; index > 0; --index)
			{
				legatoRunMidi[index] = legatoRunMidi[index - 1];
				legatoRunIsBend[index] = legatoRunIsBend[index - 1];
			}
			++legatoRunCount;
		}
		else
		{
			for (uint32_t index = legatoRunCount - 1; index > 0; --index)
			{
				legatoRunMidi[index] = legatoRunMidi[index - 1];
				legatoRunIsBend[index] = legatoRunIsBend[index - 1];
			}
		}
		legatoRunMidi[0] = bendAcceptMidi;
		legatoRunIsBend[0] = true;
		isConfirmingLegatoRun = true;
		isBendRunConfirmation = true;
		wasBendTrackerPitchLogged = false;
		isRawBendAcceptArmed = false;
		rawBendAcceptStreak = 0;
		rawEstimateBendStreak = 0;
		ndBendSightingStreak = 0;
		hasTrackerSampleAnchor = false;
		legatoRunIndex = 0;
		legatoConfirmTickCount = 0;
		holdProgressAnchor = std::chrono::steady_clock::now();
		mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
		mlBendConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
		enhancedLegatoConfirmation.Reset(latestRawAudioSample);
	}
	bool StartBendFromWaitAccept(int onset, const char* how)
	{
		if (!isBendTarget || isBendChildTarget || bendAcceptMidi < 0) return false;
		BeginBendConfirmation();
		if (onset >= 0 && onset < bendAcceptMidi) isRawBendAcceptArmed = true;
		gatePhase = GatePhase::Holding;
		inputReleaseTickCount = 0;
		LOG_INFO("(NBN LAS BEND) " << how << " " << onset << " accepted as the pick during the"
			<< " input-release wait; now holding until the bend reaches " << bendAcceptMidi
			<< " instead of committing the unbent note." << std::endl);
		return true;
	}

	void BuildLegatoRun(void* owner, float runTime, int runString)
	{
		(void)owner; (void)runTime; (void)runString;
		legatoRunCount = 0;
		for (auto& isBend : legatoRunIsBend) isBend = false;
		legatoRunIndex = 0;
		isConfirmingLegatoRun = false;
		isBendRunConfirmation = false;
		wasBendTrackerPitchLogged = false;
		isRawBendAcceptArmed = false;
		rawBendAcceptStreak = 0;
		ndBendSightingStreak = 0;
		hasBendApproachBeenObserved = false;
	}

	void AdoptDenseSuccessor()
	{
		if (promotedRepeatRecord != denseSuccessor.record) RestorePromotedRepeatChord(true);
		previousExpectedMidi = (isBendTarget && bendAcceptMidi >= 0)
			? bendAcceptMidi
			: expectedMidi;
		previousSelectedString = selectedString;
		previousWasBend = isBendTarget;
		isBendTarget = denseSuccessor.isBend;
		isBendChildTarget = denseSuccessor.isBendChild;
		isLegatoTarget = denseSuccessor.isLegato
			&& NoteByNote::LegatoContinuesPreviousString(previousSelectedString, denseSuccessor.stringIndex);
		isHammerOnTarget = denseSuccessor.isHammerOn;
		legatoConfirmTickCount = 0;
		hasLegatoPitchDeparted = false;
		selectedRecord = denseSuccessor.record;
		selectedRecordTime = denseSuccessor.recordTime;
		selectedString = denseSuccessor.stringIndex;
		selectedFret = denseSuccessor.fret;
		selectedChordId = denseSuccessor.chordId;
		selectedChordNotesId = denseSuccessor.chordNotesId;
		selectedHoldTime = denseSuccessor.holdTime;
		expectedMidi = denseSuccessor.expectedMidi;
		ResolveBendAcceptance(selectedRecord);
		isHoldSuppressed = false;
		holdTickCount = 0;
		postReleaseTickCount = 0;
		commitTickCount = 0;
		wasCommitOverrideLogged = false;
		renderFramesWhileHeld = 0;
		inputReleaseTickCount = 0;
		sawSpikeDuringHold = false;
		sawLevelSpikeDuringHold = false;
		hasHoldLatchRingTime = TryReadCurrentRingTime(holdLatchRingTime);
		selectedNativeTone = -1;
		hasNdSoundingStreak = false;
		reattackWindowTicks = 0;
		reattackStreak = 0;
		spikeRecencyTicks = 0;
		if (selectedChordId == -1 && !isLegatoTarget)
		{
			const ULONGLONG nowTick = GetTickCount64();
			if (g_lastAttackSpikeTick != 0
				&& nowTick - g_lastAttackSpikeTick <= 700)
			{
				reattackWindowTicks = REATTACK_WINDOW_TICKS;
				spikeRecencyTicks = REATTACK_WINDOW_TICKS;
				LOG_INFO("(NBN LAS INPUT) Dense successor adopted "
					<< (nowTick - g_lastAttackSpikeTick)
					<< "ms after an attack spike: raw-confirmation window opened for a"
					<< " pick played through the transition." << std::endl);
			}
		}
		{
			const ULONGLONG nowTick = GetTickCount64();
			g_flowAdoptTick = nowTick;
		}
		ResetChordOnsetEvidenceAnchor();
		gatePhase = GatePhase::DensePlayerSongStartPending;
	}

	bool RelatchDenseSuccessor(void* owner, float updateTime, const LiveNote& selected)
	{
		uintptr_t playerSongAddress = reinterpret_cast<uintptr_t>(heldPlayerSong);
		uintptr_t playerSongVtable = 0;
		uintptr_t slotStop = 0;
		if (playerSongAddress == 0
			|| !TryRead(playerSongAddress, playerSongVtable) || playerSongVtable != PLAYER_SONG_VTABLE
			|| !TryRead(playerSongVtable + 0x24, slotStop) || slotStop != STOP_TMUSIC)
		{
			FaultWithoutRelease("The dense rebuild completed without the validated PlayerSong Stop_TMusic boundary");
			return false;
		}
		if (std::fabs(updateTime - selectedHoldTime) > HOLD_BOUNDARY_MAX_OVERSHOOT)
		{
			PerformOwnedRelease(owner, "the dense rebuild resumed too far from its requested successor epoch");
			FaultWithoutRelease("The dense rebuild did not publish the successor close enough to its requested epoch");
			return false;
		}

		if (!ArmSelectedNativePrompt(selected))
		{
			if (selectedChordId != -1)
			{
				LOG_INFO("(NBN LAS CHORD) The single-note prompt fork does not arm for a"
					<< " chord record; the dense relatch holds without the freeze-note"
					<< " presentation." << std::endl);
			}
			else
			{
				PerformOwnedRelease(owner, "the dense successor's native NoteVfx prompt could not be armed");
				FaultWithoutRelease("The dense successor did not expose a validated native prompt fork");
				return false;
			}
		}
		if (selectedChordId != -1)
		{
			LOG_INFO("(NBN LAS CHORD) Skipping Play_FreezeNoteTrack for the dense chord"
				<< " relatch so the chord panel survives on the unswept track." << std::endl);
			ShowNativeChordPanel(owner, selectedChordId);
		}
		else
		{
			PlayFreezeNoteTrack();
		}
		reinterpret_cast<ThiscallVoidFn>(STOP_TMUSIC)(heldPlayerSong, nullptr);
		musicStoppedByHold = true;
		musicStoppedEpoch = selectedHoldTime;
		uint8_t runningAfter = 0xFF;
		uint8_t stoppedAfter = 0xFF;
		TryRead(playerSongAddress + PLAYER_SONG_RUNNING, runningAfter);
		TryRead(playerSongAddress + PLAYER_SONG_STOPPED, stoppedAfter);
		if (runningAfter != 0 || stoppedAfter != 1)
		{
			FaultWithoutRelease(
				"The dense rebuild's Stop_TMusic relatch did not commit +0xD9=0/+0xDA=1");
			return false;
		}
		if (!SetAndVerifyHeldEpoch(owner, selectedHoldTime))
		{
			heldEpoch = selectedHoldTime;
			PerformOwnedRelease(owner, "the dense rebuild relatch failed its five-clock readback");
			FaultWithoutRelease("The dense rebuild could not re-pin all five clocks to the successor epoch");
			return false;
		}

		heldEpoch = selectedHoldTime;
		holdTickCount = 0;
		renderFramesWhileHeld = 0;
		gatePhase = GatePhase::WaitingForInputRelease;
		inputReleaseTickCount = 0;
		bendWaitPitchFloor = -1;
		bendWaitOnTargetTicks = 0;
		transportFrozenAt = std::chrono::steady_clock::now();
		hasTransportFrozenAt = true;
		reattackWindowTicks = 0;
		reattackStreak = 0;
		spikeRecencyTicks = 0;
		if (selectedChordId == -1 && !isLegatoTarget && !isBendChildTarget
			&& expectedMidi != previousExpectedMidi)
		{
			const ULONGLONG nowSpikeTick = GetTickCount64();
			if (g_lastAttackSpikeTick != 0
				&& nowSpikeTick - g_lastAttackSpikeTick <= 700)
			{
				reattackWindowTicks = REATTACK_WINDOW_TICKS;
				spikeRecencyTicks = REATTACK_WINDOW_TICKS;
				LOG_INFO("(NBN LAS INPUT) Dense relatch kept the raw-confirmation window"
					<< " open (attack spike " << (nowSpikeTick - g_lastAttackSpikeTick)
					<< "ms ago): a pick played through the transition is judged on its"
					<< " sustain." << std::endl);
			}
		}
		{
			const ULONGLONG nowFlowTick = GetTickCount64();
			g_flowArmedTick = nowFlowTick;
		}
		sawSpikeDuringHold = false;
		sawLevelSpikeDuringHold = false;
		lastStuckWarnHeldSeconds = 0.0;
		ResetChordOnsetEvidenceAnchor();
		SetNativeFreezeFlag(owner, true);
		holdProgressAnchor = std::chrono::steady_clock::now();
		mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
		mlBendConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
		enhancedLegatoConfirmation.Reset(latestRawAudioSample);
		hasHoldProgressAnchor = true;
		lastStuckWarnHeldSeconds = 0.0;
		int primedOnset = QueryNativeOnsetNote();
		int currentMidi = QueryNativeLoudestPlayedNote();
		BuildLegatoRun(owner, selectedRecordTime, selectedString);
		if (isBendChildTarget && bendAcceptMidi >= 0)
		{
			gatePhase = GatePhase::Holding;
			inputReleaseTickCount = 0;
		}
		else if (selectedChordId != -1)
		{
			gatePhase = GatePhase::Holding;
			inputReleaseTickCount = 0;
		}
		LOG_INFO("(NBN LAS CHAIN) PlayerSong/fretboard rebuild completed and Stop_TMusic was re-latched:"
			<< " record=0x" << std::hex << selectedRecord << std::dec
			<< " string=" << selectedString << " fret=" << selectedFret
			<< " updateTime=" << std::fixed << std::setprecision(6) << updateTime
			<< " heldEpoch=" << heldEpoch
			<< " expectedMidi=" << expectedMidi
			<< " primedOnset=" << primedOnset
			<< " currentMidi=" << currentMidi
			<< ". The successor will arm after the detector confirms input release."
			<< std::endl);
		if (hasDenseRebuildTiming)   // phase-1 flow instrumentation: the heavy per-note cost
		{
			const double rebuildMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - denseRebuildQueuedAt).count();
			hasDenseRebuildTiming = false;
			LOG_INFO("(NBN FLOW TIMING) dense rebuild span " << std::fixed << std::setprecision(1)
				<< rebuildMs << " ms (Queued -> completed; this is the per-note teardown+restart"
				<< " that gaps audio and blocks the highway in a dense run)." << std::endl);
		}
		EmitCandidateEvent(owner, updateTime, selected,
			ResearchProtocol::ExpectedAttackEventKind::CandidateChanged);
		EmitCandidateEvent(owner, heldEpoch, selected,
			ResearchProtocol::ExpectedAttackEventKind::HoldEstablished);
		denseSuccessor = {};
		isFlowRedrawRebuild = false;
		return true;
	}

	void RefreshDetectionFeedbackMl()
	{
		const auto now = GetTickCount64();
		if (detectionFeedback.stringIndex < 0 || detectionFeedback.mlRole == NoteByNote::DetectorRole::Confirmed
			|| NoteByNote::GetDetectorColor(NoteByNote::DetectorRole::Confirmed, detectionFeedback.tick, now)
				== 0xFFFFFFFF) return;
		ResearchProtocol::MlNoteEvidence evidence;
		if (ResearchProbeRuntime::QueryMlNoteEvidence(detectionFeedback.targetMidi, 0.5f,
			feedbackMinimumMlSample, evidence)
			&& evidence.verdict == ResearchProtocol::MlNoteVerdict::Confirmed)
		{
			NoteByNote::CreditConfirmedMlFeedback(detectionFeedback, evidence.analyzedSampleIndex,
				feedbackMinimumMlSample, feedbackMaximumMlSample, now);
		}
	}
	void FillMissingChordLabel(NoteByNote::DetectionFeedback& feedback)
	{
		if (selectedChordId < 0 || feedback.chordLabel[0] != '\0') return;
		uintptr_t note = 0;
		LiveNote selected;
		if (trackedOwner != nullptr && FindSelectedNote(trackedOwner, selected))
			note = selected.note;
		const bool hasTones = researchChordTonesRecord == selectedRecord;
		NoteByNoteNativeScoring::TryDescribeChordLabel(note, hasTones ? researchChordTones : nullptr,
			hasTones ? researchChordToneCount : 0, feedback.chordLabel, sizeof(feedback.chordLabel));
	}

	void PublishCommittedDetectionFeedback()
	{
		NoteByNote::DetectionFeedback nextFeedback;
		feedbackMinimumMlSample = mlConfirmationState.GetMinimumSampleIndex();
		feedbackMaximumMlSample = ResearchProbeRuntime::GetMlAudioSampleIndex();
		if (IsPlainPickedTarget() && acceptedPickRecord == selectedRecord)
		{
			nextFeedback = acceptedPick.feedback;
			feedbackMinimumMlSample = acceptedPick.minimumMlSample;
		}
		else if (selectedChordId >= 0 && chordAcceptFeedback.targetRecord == selectedRecord)
		{
			nextFeedback = chordAcceptFeedback;
		}
		else
		{
			const bool mlRescued = mlRescueRecord == selectedRecord;
			nextFeedback.nativeMidi = QueryNativeLoudestPlayedNote();
			nextFeedback.nativeRole = mlRescued && !isBendTarget
				? NoteByNote::DetectorRole::Unused : NoteByNote::DetectorRole::Confirmed;
			if (enhancedRescueFeedback.targetRecord == selectedRecord)
			{
				nextFeedback = enhancedRescueFeedback;
				nextFeedback.nativeRole = nextFeedback.nativeMidi < 0 ? NoteByNote::DetectorRole::Unused
					: NoteByNote::NativeCorroboratesPick(nextFeedback.nativeMidi, nextFeedback.enhancedMidi)
						? NoteByNote::DetectorRole::Confirmed : NoteByNote::DetectorRole::Rejected;
			}
			if (mlRescued)
			{
				const int confirmedTarget = isBendTarget && bendAcceptMidi >= 0 ? bendAcceptMidi : expectedMidi;
				nextFeedback.mlRole = mlRescueMidi == confirmedTarget
					? NoteByNote::DetectorRole::Confirmed : NoteByNote::DetectorRole::Partial;
				nextFeedback.mlMidi = mlRescueMidi;
				if (mlRescueMidi == confirmedTarget
					&& !NoteByNote::NativeCorroboratesPick(nextFeedback.nativeMidi, confirmedTarget))
					nextFeedback.nativeRole = NoteByNote::DetectorRole::Unused;
			}
		}
		nextFeedback.targetRecord = selectedRecord;
		nextFeedback.tick = GetTickCount64();
		nextFeedback.targetMidi = isBendTarget && bendAcceptMidi >= 0 ? bendAcceptMidi : expectedMidi;
		nextFeedback.stringIndex = selectedChordId == -1 ? selectedString : -1;
		nextFeedback.fret = selectedChordId == -1 ? selectedFret : -1;
		FillMissingChordLabel(nextFeedback);
		NoteByNote::PublishDetectionFeedback(detectionFeedback, nextFeedback);
		RefreshDetectionFeedbackMl();
	}
	void PublishFlowCommitFeedback(bool buffered)
	{
		NoteByNote::DetectionFeedback nextFeedback;
		if (buffered)
		{
			nextFeedback = acceptedPick.feedback;
		}
		else
		{
			nextFeedback.nativeMidi = QueryNativeLoudestPlayedNote();
			nextFeedback.nativeRole = NoteByNote::DetectorRole::Confirmed;
		}
		nextFeedback.targetRecord = selectedRecord;
		nextFeedback.tick = GetTickCount64();
		nextFeedback.targetMidi = armedExpectedMidi;
		nextFeedback.stringIndex = selectedChordId == -1 ? selectedString : -1;
		nextFeedback.fret = selectedChordId == -1 ? selectedFret : -1;
		FillMissingChordLabel(nextFeedback);
		NoteByNote::PublishDetectionFeedback(detectionFeedback, nextFeedback);
	}

	void CompleteHeldCommit(void* owner, float updateTime, const LiveNote& selected, const char* reason)
	{
		RecordSameTimeGroupCommit();
		PublishCommittedDetectionFeedback();
		mlRescueRecord = 0;
		enhancedRescueFeedback = {};
		successorStrumSample = IsPlainPickedTarget() && acceptedPickRecord == selectedRecord
			&& acceptedPick.strumBelongsToSuccessor ? acceptedPick.minimumSample : 0;
		if (successorStrumSample == 0 && IsPlainPickedTarget() && acceptedPickRecord == selectedRecord)
		{
			for (unsigned index = 0; index < pickedAttacks.Count(); ++index)
			{
				const auto* waiting = pickedAttacks.At(index);
				if (waiting != nullptr && !waiting->rejected
					&& waiting->minimumSample > acceptedPick.minimumSample)
				{
					successorStrumSample = waiting->minimumSample;
					break;
				}
			}
		}
		successorStrumRecord = 0;
		if (IsPlainPickedTarget())
		{
			if (acceptedPickRecord == selectedRecord)
				LOG_INFO("(NBN PICK BUFFER) Committed record=" << selectedRecord
					<< " attack=" << acceptedPick.time << " sample=" << acceptedPick.confirmedSample
					<< " remaining=" << pickedAttacks.Count()
					<< " successorStrum=" << successorStrumSample << std::endl);
			else
				LOG_INFO("(NBN PICK BUFFER) Committed record=" << selectedRecord
					<< " without a buffered pick (legato or native path) remaining=" << pickedAttacks.Count()
					<< " successorStrum=" << successorStrumSample << std::endl);
		}
		acceptedPickRecord = 0;
		consumedRecords.insert(selectedRecord);
		previousSelectedString = selectedString;
		previousExpectedMidi = (isBendTarget && bendAcceptMidi >= 0)
			? bendAcceptMidi
			: expectedMidi;
		previousWasBend = isBendTarget;
		LOG_INFO("(NBN LAS WHY) " << (reason != nullptr ? reason : "unspecified")
			<< " | string=" << selectedString << " fret=" << selectedFret
			<< " ticks=" << holdTickCount << std::endl);
		LOG_INFO("(NBN LAS COMMIT) Secured the selected native hit before transport release: record=0x"
			<< std::hex << selectedRecord << std::dec
			<< " heldTicks=" << holdTickCount << "." << std::endl);
		{   // phase-1 flow instrumentation: real-time gap between consecutive commits = cadence
			const auto nowWall = std::chrono::steady_clock::now();
			if (hasLastCommitWallClock)
			{
				const double gapMs = std::chrono::duration<double, std::milli>(
					nowWall - lastCommitWallClock).count();
				LOG_INFO("(NBN FLOW TIMING) commit-to-commit " << std::fixed << std::setprecision(1)
					<< gapMs << " ms (" << (gapMs > 0.0 ? 1000.0 / gapMs : 0.0)
					<< " notes/sec cadence)." << std::endl);
			}
			lastCommitWallClock = nowWall;
			hasLastCommitWallClock = true;
		}
		if (!isFlowUntilMissEnabled)
		{
			DenseChainResult chainResult = TryAdvanceDenseChain(owner, updateTime);
			if (chainResult != DenseChainResult::NotRequired) return;
		}

		PerformOwnedRelease(owner, reason);
		gatePhase = GatePhase::PostRelease;
		postReleaseTickCount = 0;
	}
	constexpr float FLOW_PENDING_PICK_MIN_SECONDS = 0.16f;
	constexpr float FLOW_PENDING_PICK_MAX_SECONDS = 0.26f;
	constexpr float FLOW_NEXT_NOTE_MARGIN_SECONDS = 0.073f;   // compensation (0.053) + 0.02

	float FlowPendingAllowanceForGap(float gap)
	{
		return FlowSpeedCap::AllowanceForGap(gap);   // shared with the Settings-screen cap
	}

	uintptr_t flowAllowanceRecord = 0;
	float flowAllowanceSeconds = FLOW_PENDING_PICK_MIN_SECONDS;
	float FlowPendingAllowance(void* owner)
	{
		if (flowAllowanceRecord == selectedRecord) return flowAllowanceSeconds;
		flowAllowanceRecord = selectedRecord;
		float next = -1.0f;
		ForEachLiveNote(owner, [&](const LiveNote& note)
		{
			if (note.recordTime > selectedRecordTime + 0.01f && (next < 0.0f || note.recordTime < next))
				next = note.recordTime;
			return true;
		});
		flowAllowanceSeconds = FlowPendingAllowanceForGap(next > 0.0f ? next - selectedRecordTime : -1.0f);
		return flowAllowanceSeconds;
	}
	uintptr_t flowPendingLoggedRecord = 0;
	constexpr double FLOW_CAPTURE_WAIT_SECONDS = 0.14;
	uintptr_t flowBoundaryRecord = 0;
	std::chrono::steady_clock::time_point flowBoundaryReachedAt{};

	bool IsWaitingForFlowCapture()
	{
		const auto now = std::chrono::steady_clock::now();
		if (flowBoundaryRecord != selectedRecord)
		{
			flowBoundaryRecord = selectedRecord;
			flowBoundaryReachedAt = now;
		}
		return std::chrono::duration<double>(now - flowBoundaryReachedAt).count() < FLOW_CAPTURE_WAIT_SECONDS;
	}
	constexpr float FLOW_WINDOW_EXIT_MARGIN_SECONDS = 0.03f;
	uintptr_t flowExtensionLoggedRecord = 0;
	constexpr float FLOW_CHORD_MAX_WAIT_SECONDS = 0.20f;
	bool HasPendingFlowStrum()
	{
		return pickSampleRate != 0 && chordAttacks.HasFreshAttack(latestRawAudioSample, pickSampleRate);
	}
	constexpr double FLOW_RECENT_CAPTURE_SECONDS = 0.15;
	bool HasPendingFlowAttack()
	{
		for (unsigned index = 0; index < pickedAttacks.Count(); ++index)
		{
			const auto* attack = pickedAttacks.At(index);
			if (attack == nullptr || attack->rejected || attack->sampleRate == 0) continue;
			const double window = attack->confirmedMidi >= 0 ? FLOW_RECENT_CAPTURE_SECONDS : 0.25;
			if (latestRawAudioSample < attack->minimumSample
				+ static_cast<uint64_t>(window * attack->sampleRate)) return true;
		}
		return false;
	}
	void TickArmedFlowBuffer(const LiveNote& selected)
	{
		if (selectedChordId != -1 || armedBufferCommitRecord == selectedRecord) return;
		if ((selected.mask & NOTE_MASK_BEND) != 0 && !isFlowUntilMissEnabled) return;
		if (NoteByNote::UsesNoPickLegatoAcceptance(selected.mask)
			&& NoteByNote::LegatoContinuesPreviousString(previousSelectedString, selectedString)) return;
		if (armedExpectedMidi < 0)
		{
			RefreshArrangementInstrument();
			int16_t tuningOffset = 0;
			int midi = -1;
			if (selectedString < 0 || selectedString > 5 || selectedFret < 0
				|| !TryRead(TUNING_OFFSETS + static_cast<uintptr_t>(selectedString) * 2, tuningOffset)
				|| tuningOffset < -12 || tuningOffset > 12
				|| !NoteByNote::TryResolvePlayedNotePitch(selectedString, selectedFret, tuningOffset,
					ResearchProbeRuntime::GetInputOnsetShiftSemitones(), midi, IsBassArrangement())) return;
			armedExpectedMidi = midi;
		}
		NoteByNote::PickedAttack taken;
		if (!pickedAttacks.Take(armedExpectedMidi, taken, KeepsMismatchedPicks())) return;
		acceptedPick = taken;
		armedBufferCommitRecord = selectedRecord;
		LOG_INFO("(NBN FLOW) Buffered attack=" << taken.time << " midi=" << taken.confirmedMidi
			<< " commits record=0x" << std::hex << selectedRecord << std::dec
			<< " while Armed; no hold." << std::endl);
	}
	constexpr float FLOW_REDRAW_MIN_STEP_SECONDS = 0.03f;
	void BeginFlowRedrawRebuild(void* owner, const LiveNote& selected, float stepBack)
	{
		denseSuccessor = {};
		isFlowRedrawRebuild = false;
		denseSuccessor.record = selectedRecord;
		denseSuccessor.note = selected.note;
		denseSuccessor.recordTime = selectedRecordTime;
		denseSuccessor.holdTime = selectedHoldTime;
		denseSuccessor.stringIndex = selectedString;
		denseSuccessor.fret = selectedFret;
		denseSuccessor.chordId = selectedChordId;
		denseSuccessor.chordNotesId = selectedChordNotesId;
		denseSuccessor.expectedMidi = expectedMidi;
		denseSuccessor.isBend = isBendTarget;
		denseSuccessor.isBendChild = isBendChildTarget;
		denseSuccessor.isLegato = isLegatoTarget;
		denseSuccessor.isHammerOn = isHammerOnTarget;
		const float rebuildEpoch = (std::min)(selectedHoldTime, selectedRecordTime - 0.002f);
		PerformOwnedReleaseAtEpoch(owner, rebuildEpoch,
			"flow freeze-back redraws the highway at the missed note through the coordinated rebuild");
		if (!AreOwnerClocksAtEpoch(owner, rebuildEpoch))
		{
			heldEpoch = rebuildEpoch;
			PerformOwnedRelease(owner, "the flow redraw rebuild failed its five-clock readback");
			FaultWithoutRelease("The flow redraw rebuild did not set all five owner clocks to the held note");
			return;
		}
		heldEpoch = rebuildEpoch;
		holdTickCount = 0;
		postReleaseTickCount = 0;
		commitTickCount = 0;
		wasCommitOverrideLogged = false;
		renderFramesWhileHeld = 0;
		inputReleaseTickCount = 0;
		isFlowRedrawRebuild = true;
		gatePhase = GatePhase::DenseRebuildPending;
		denseRebuildQueuedAt = std::chrono::steady_clock::now();
		hasDenseRebuildTiming = true;
		LOG_INFO("(NBN FLOW) Freeze-back of " << std::fixed << std::setprecision(3) << stepBack
			<< " s: redrawing the highway at the missed note (record=0x" << std::hex << selectedRecord
			<< std::dec << ") through the coordinated rebuild." << std::defaultfloat << std::endl);
	}

	void AdoptFlowRedraw()
	{
		const int savedPreviousExpectedMidi = previousExpectedMidi;
		const int savedPreviousSelectedString = previousSelectedString;
		const bool savedPreviousWasBend = previousWasBend;
		const bool savedIsLegatoTarget = isLegatoTarget;
		AdoptDenseSuccessor();
		previousExpectedMidi = savedPreviousExpectedMidi;
		previousSelectedString = savedPreviousSelectedString;
		previousWasBend = savedPreviousWasBend;
		isLegatoTarget = savedIsLegatoTarget;
		isFlowRedrawRebuild = false;
	}
	float flowSpeedCapPercent = 100.0f;
	float flowSpeedCapSectionStart = -1.0f;
	float flowSpeedCapSectionEnd = -1.0f;
	uint64_t flowSpeedPassKey = ~0ull;
	float flowSpeedPassSectionStart = -1.0f;
	float flowSpeedPassCap = 100.0f;
	std::chrono::steady_clock::time_point flowSpeedPassStartedAt{};
	constexpr std::chrono::milliseconds FLOW_SPEED_APPLY_WINDOW{ 1500 };
	int flowSpeedPassRequests = 0;
	std::chrono::steady_clock::time_point flowSpeedLastRequestAt{};
	std::chrono::steady_clock::time_point flowSpeedCapComputedAt{};
	std::chrono::steady_clock::time_point flowSpeedCheckedAt{};

	float ComputeFlowSpeedCap(void* owner, float& typicalGapOut, size_t& noteCountOut)
	{
		std::vector<float> times;
		ForEachLiveNote(owner, [&](const LiveNote& note)
		{
			if (note.recordTime >= greyCutoff - GREY_EPSILON && note.recordTime < sectionEndBoundary)
			{
				times.push_back(note.recordTime);
			}
			return true;
		});
		std::sort(times.begin(), times.end());
		float lowest = 100.0f;
		typicalGapOut = 0.0f;
		noteCountOut = times.size();
		bool anySection = false;
		std::vector<float> slice;
		for (const TimelineSection& section : timelineSections)
		{
			const float from = (std::max)(section.start, greyCutoff);
			const float to = (std::min)(section.end, sectionEndBoundary);
			if (!(to > from)) continue;
			anySection = true;
			slice.assign(std::lower_bound(times.begin(), times.end(), from - GREY_EPSILON),
				std::lower_bound(times.begin(), times.end(), to));
			float typicalGap = 0.0f;
			const size_t sliceCount = slice.size();
			const float cap = FlowSpeedCap::CapFromNoteTimes(slice.data(), slice.size(), IsBassArrangement(), &typicalGap);
			if (cap < lowest)
			{
				lowest = cap;
				typicalGapOut = typicalGap;
				noteCountOut = sliceCount;
			}
		}
		if (!anySection)
			return FlowSpeedCap::CapFromNoteTimes(times.data(), times.size(), IsBassArrangement(), &typicalGapOut);
		return lowest;
	}

	void TickFlowSpeedCap(void* owner)
	{
		if (!isFlowUntilMissEnabled) flowSpeedPassRequestedPercent = -1.0f;
		if (!isFlowUntilMissEnabled || !isEpochConfirmed || !hasSectionEndBoundary
			|| !ResearchProbeRuntime::IsNoteByNoteEnabled())
		{
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now - flowSpeedCheckedAt < std::chrono::milliseconds(250)) return;
		flowSpeedCheckedAt = now;

		if (greyCutoff != flowSpeedCapSectionStart || sectionEndBoundary != flowSpeedCapSectionEnd)
		{
			flowSpeedCapSectionStart = greyCutoff;
			flowSpeedCapSectionEnd = sectionEndBoundary;
			flowSpeedCapPercent = 100.0f;
			flowSpeedCapComputedAt = {};
		}
		if (now - flowSpeedCapComputedAt >= std::chrono::seconds(2)
			|| now - flowSpeedPassStartedAt <= FLOW_SPEED_APPLY_WINDOW)
		{
			flowSpeedCapComputedAt = now;
			float typicalGap = 0.0f;
			size_t noteCount = 0;
			const float cap = ComputeFlowSpeedCap(owner, typicalGap, noteCount);
			if (cap < flowSpeedCapPercent)
			{
				flowSpeedCapPercent = cap;
				LOG_INFO("(NBN FLOW SPEED) Section [" << std::fixed << std::setprecision(3)
					<< greyCutoff << ".." << sectionEndBoundary << "] cap=" << std::setprecision(0)
					<< cap << "% (densest section: typical tight gap " << std::setprecision(3) << typicalGap
					<< " s over " << noteCount << " notes, " << (IsBassArrangement() ? "bass" : "guitar")
					<< ")." << std::defaultfloat << std::endl);
			}
		}
		const bool hasPlayerSpeed = ResearchProbeRuntime::GetPlayerSpeedRealPercent() > 0.0f;
		if (!hasPlayerSpeed && !hasMeasuredSongSpeed) return;
		const uint64_t passKey = sectionLatchCounter * 100000ull + epochIndex;
		if (passKey != flowSpeedPassKey || greyCutoff != flowSpeedPassSectionStart)
		{
			flowSpeedPassKey = passKey;
			flowSpeedPassSectionStart = greyCutoff;
			flowSpeedPassStartedAt = now;
			flowSpeedPassCap = flowSpeedCapPercent;
			flowSpeedPassRequests = 0;
			flowSpeedPassRequestedPercent = -1.0f;   // the game re-sends the player's speed at a pass start
		}
		const float speed = flowSpeedPassRequestedPercent > 0.0f
			? (hasMeasuredSongSpeed && measuredSongSpeed * 100.0 > flowSpeedPassRequestedPercent * 1.25
				? static_cast<float>(measuredSongSpeed * 100.0) : flowSpeedPassRequestedPercent)
			: static_cast<float>(CurrentSongSpeed() * 100.0);
		if (now - flowSpeedPassStartedAt <= FLOW_SPEED_APPLY_WINDOW)
			flowSpeedPassCap = (std::min)(flowSpeedPassCap, flowSpeedCapPercent);
		if (now - flowSpeedPassStartedAt <= FLOW_SPEED_APPLY_WINDOW && flowSpeedPassCap < 99.5f
			&& speed > flowSpeedPassCap + 0.5f
			&& flowSpeedPassRequests < 2
			&& (flowSpeedPassRequests == 0 || now - flowSpeedLastRequestAt >= std::chrono::milliseconds(700)))
		{
			++flowSpeedPassRequests;
			flowSpeedLastRequestAt = now;
			ResearchProbeRuntime::SetSongSpeedPercent(flowSpeedPassCap);
			flowSpeedPassRequestedPercent = flowSpeedPassCap;
			LOG_INFO("(NBN FLOW SPEED) Speed " << std::fixed << std::setprecision(0) << speed
				<< "% lowered to " << flowSpeedPassCap << "% for this pass (flow cap from the section's"
				<< " note spacing)." << std::defaultfloat << std::endl);
		}
	}
	void SyncFlowModeFromHost()
	{
		bool hostFlow = isFlowUntilMissEnabled;
		if (!ResearchProbeRuntime::GetHostFlowModeEnabled(hostFlow) || hostFlow == isFlowUntilMissEnabled) return;
		isFlowUntilMissEnabled = hostFlow;
		LOG_INFO("(NBN FLOW) FLOW MODE switched " << (hostFlow ? "On" : "Off")
			<< " from the menu/overlay; the controller follows." << std::endl);
	}
	bool HandleNoteNavigation(void* owner, int direction)
	{
		if (direction < 0)
		{
			LOG_INFO("(NBN SKIP) LEFT ARROW pressed; going back to the previous note is not built yet." << std::endl);
			return false;
		}
		if (gatePhase != GatePhase::Holding && gatePhase != GatePhase::WaitingForInputRelease)
		{
			LOG_INFO("(NBN SKIP) RIGHT ARROW pressed with no note frozen (phase " << DescribeGatePhase(gatePhase)
				<< "); ignored." << std::endl);
			return false;
		}
		LOG_INFO("(NBN SKIP) RIGHT ARROW: skipping the frozen " << (selectedChordId != -1 ? "chord" : "note")
			<< " record=0x" << std::hex << selectedRecord << std::dec << " (string " << selectedString
			<< " fret " << selectedFret << " chordId " << selectedChordId << ") without scoring it." << std::endl);
		consumedRecords.insert(selectedRecord);
		PerformOwnedRelease(owner, "RIGHT ARROW skipped the frozen note");
		ClearSelection();
		return true;
	}

	void HandleAfterUpdate(void* owner, float updateTime)
	{
		const int noteNavigation = ResearchProbeRuntime::ConsumeNoteNavigation();
		SyncFlowModeFromHost();
		SampleSongSpeed(updateTime);
		RetargetBufferedAttacks();
		TickPickedAttackStream();
		ProcessPendingStrumLedger();
		TrackSameTimeGroup();
		TickFlowSpeedCap(owner);
		if (noteNavigation != 0 && HandleNoteNavigation(owner, noteNavigation)) return;
		if (gatePhase == GatePhase::Idle) return;

		LiveNote selected;
		bool isPresent = FindSelectedNote(owner, selected);

		switch (gatePhase)
		{
			case GatePhase::Armed:
			{
				if (!isPresent)
				{
					FaultWithoutRelease("The selected native record disappeared from the live vector before its hold boundary");
					return;
				}
				if (isFlowUntilMissEnabled) TickArmedFlowBuffer(selected);
				if (selected.stateC0 != 0 && selected.stateC1 != 0)
				{
					const bool requiresTargetOwnedFreshAttack =
						NoteByNote::ShouldBlockPreHoldHitDecision(
							isFlowUntilMissEnabled, isHoldSuppressed);
					const bool hasTargetOwnedFreshAttack = requiresTargetOwnedFreshAttack
						&& IsPlainPickedTarget() && TakeBufferedPick();
					if (NoteByNote::ShouldRejectNaturalPreHoldCommit(
						isFlowUntilMissEnabled, isHoldSuppressed, hasTargetOwnedFreshAttack))
					{
						LOG_ERROR("(NBN LAS COMMIT) The selected record committed while its pre-hold"
							<< " hit decision was blocked: record=0x" << std::hex << selectedRecord
							<< std::dec << " states=" << static_cast<int>(selected.stateC0)
							<< static_cast<int>(selected.stateC1) << static_cast<int>(selected.stateC2)
							<< static_cast<int>(selected.stateC3) << "." << std::endl);
						FaultWithoutRelease("A selected record bypassed the pre-hold hit-decision gate");
						return;
					}
					consumedRecords.insert(selectedRecord);
					LOG_INFO("(NBN LAS COMMIT) Natural commit before any hold: record=0x" << std::hex
						<< selectedRecord << std::dec << " states=" << static_cast<int>(selected.stateC0)
						<< static_cast<int>(selected.stateC1) << static_cast<int>(selected.stateC2)
						<< static_cast<int>(selected.stateC3)
						<< (armedBufferCommitRecord == selectedRecord ? " (buffered attack)" : " (native decision)")
						<< "." << std::endl);
					if (isFlowUntilMissEnabled) PublishFlowCommitFeedback(armedBufferCommitRecord == selectedRecord);
					const int flowCommittedMidi = armedExpectedMidi;
					const int flowCommittedString = selectedString;
					if (flowCommittedMidi >= 0 && armedBufferCommitRecord != selectedRecord)
					{
						NoteByNote::PickedAttack discardedForNative;
						pickedAttacks.Take(flowCommittedMidi, discardedForNative, KeepsMismatchedPicks());
					}
					{
						const auto nowWall = std::chrono::steady_clock::now();
						const double flowGapMs = std::chrono::duration_cast<std::chrono::microseconds>(
							nowWall - lastCommitWallClock).count() / 1000.0;
						if (hasLastCommitWallClock)
							LOG_INFO("(NBN FLOW TIMING) commit-to-commit " << std::fixed << std::setprecision(1)
								<< flowGapMs << " ms (flow, no hold)." << std::defaultfloat << std::endl);
						lastCommitWallClock = nowWall;
						hasLastCommitWallClock = true;
					}
					if (selectedChordId != -1)
					{
						const uint64_t strum = chordAttacks.GetAttackSample();
						if (strum != 0)
						{
							chordAttacks.BeginHold(strum);
							lastChordStrumSample = strum;
						}
						lastCommittedChordRecord = selectedRecord;
						lastCommittedChordId = selectedChordId;
						lastCommittedChordAt = std::chrono::steady_clock::now();
					}
					ClearSelection();
					if (flowCommittedMidi >= 0)
					{
						previousExpectedMidi = flowCommittedMidi;
						previousSelectedString = flowCommittedString;
					}
					return;
				}
				const bool stateC3MeansExpiry = !isFlowUntilMissEnabled || updateTime >= selected.windowExit;
				if (selected.stateC3 != 0 && stateC3MeansExpiry)
				{
					if (isHoldSuppressed)
					{
						consumedRecords.insert(selectedRecord);
						LOG_INFO("(NBN LAS COMMIT) Hold-suppressed record expired natively as a miss: record=0x"
							<< std::hex << selectedRecord << std::dec << "." << std::endl);
						ClearSelection();
						return;
					}
					if (selectedChordId != -1 && areChordHoldsEnabled)
					{
						LOG_INFO("(NBN LAS CHORD) Chord record 0x" << std::hex
							<< selectedRecord << std::dec << " reads stateC3="
							<< static_cast<int>(selected.stateC3)
							<< " pre-hold; chord state semantics are unmapped, so this is"
							<< " not treated as expiry." << std::endl);
					}
					else
					{
						consumedRecords.insert(selectedRecord);
						LOG_INFO("(NBN LAS COMMIT) Selected single-note record expired before its"
							<< " hold could be established (dense-run overshoot); accepting the"
							<< " native miss and advancing rather than faulting: record=0x"
							<< std::hex << selectedRecord << std::dec
							<< " expectedMidi=" << expectedMidi
							<< " holdTime=" << std::fixed << std::setprecision(6) << selectedHoldTime
							<< " updateTime=" << updateTime << "." << std::endl);
						ClearSelection();
						return;
					}
				}
				if (!isHoldSuppressed && updateTime >= selectedHoldTime)
				{
					if (selectedChordId != -1 && !areChordHoldsEnabled)
					{
						isHoldSuppressed = true;
						LOG_INFO("(NBN LAS HOLD) Chord record 0x" << std::hex << selectedRecord
							<< std::dec << " (chordId " << selectedChordId
							<< ") is not held (chord holds disabled); native play continues."
							<< std::endl);
						return;
					}
					if (selectedChordId != -1 && (selected.mask & 0x80000000u) == 0
						&& !areRepeatStrumHoldsEnabled)
					{
						isHoldSuppressed = true;
						LOG_INFO("(NBN LAS CHORD) Repeat-strum chord record 0x" << std::hex
							<< selectedRecord << std::dec << " (chordId " << selectedChordId
							<< ", mask 0x" << std::hex << selected.mask << std::dec
							<< ") plays through natively; only full chords hold."
							<< std::endl);
						return;
					}
					if (isFlowUntilMissEnabled && selectedChordId == -1
						&& updateTime < selectedRecordTime + FlowPendingAllowance(owner)
						&& (!hasSectionEndBoundary || updateTime < sectionEndBoundary - GREY_EPSILON)
						/* no captured-pick condition: see above */)
					{
						if (flowPendingLoggedRecord != selectedRecord)
						{
							flowPendingLoggedRecord = selectedRecord;
							LOG_INFO("(NBN FLOW) Boundary reached with a pick still being confirmed; holding"
								<< " off the freeze (record=0x" << std::hex << selectedRecord << std::dec
								<< ")." << std::endl);
						}
						return;
					}
					if (isFlowUntilMissEnabled && selectedChordId != -1
						&& updateTime < selectedRecordTime + (std::min)(FlowPendingAllowance(owner), FLOW_CHORD_MAX_WAIT_SECONDS)
						&& (!hasSectionEndBoundary || updateTime < sectionEndBoundary - GREY_EPSILON)
						/* no strum condition: see above */)
					{
						if (flowPendingLoggedRecord != selectedRecord)
						{
							flowPendingLoggedRecord = selectedRecord;
							LOG_INFO("(NBN FLOW) Chord boundary reached with a strum being judged by the game;"
								<< " holding off the freeze (record=0x" << std::hex << selectedRecord << std::dec
								<< " chordId=" << selectedChordId << ")." << std::endl);
						}
						return;
					}
					if (isFlowUntilMissEnabled
						&& updateTime < selected.windowExit - FLOW_WINDOW_EXIT_MARGIN_SECONDS
						&& (!hasSectionEndBoundary || updateTime < sectionEndBoundary - GREY_EPSILON)
						&& (selectedChordId != -1 ? HasPendingFlowStrum()
							: (armedBufferCommitRecord == selectedRecord || HasPendingFlowAttack())))
					{
						if (flowExtensionLoggedRecord != selectedRecord)
						{
							flowExtensionLoggedRecord = selectedRecord;
							LOG_INFO("(NBN FLOW) Past the flow window with a " << (selectedChordId != -1 ? "strum" : "pick")
								<< " still being confirmed; keeping it flowing until the native window closes (record=0x"
								<< std::hex << selectedRecord << std::dec << ")." << std::endl);
						}
						return;
					}
					if (isFlowUntilMissEnabled && updateTime - selectedHoldTime > 0.03f)
					{
						LOG_INFO("(NBN FLOW) Pending pick did not confirm; freezing back at the note's boundary "
							<< std::fixed << std::setprecision(3) << selectedHoldTime << " (transport "
							<< updateTime << ")." << std::defaultfloat << std::endl);
					}
					const float flowStepBack = updateTime - selectedHoldTime;
					const bool isChordFreeze = selectedChordId != -1;
					const bool isRepeatStrum = isChordFreeze && (selected.mask & NOTE_MASK_CHORD_PANEL) == 0;
					if (EstablishHold(owner, updateTime, selected)
						&& (isChordFreeze
							|| (isFlowUntilMissEnabled && flowStepBack > FLOW_REDRAW_MIN_STEP_SECONDS)))
					{
						if (isRepeatStrum) PromoteRepeatChordForRedraw(selectedRecord);
						BeginFlowRedrawRebuild(owner, selected, flowStepBack);
					}
				}
				return;
			}
			case GatePhase::DenseRebuildPending:
			{
				++postReleaseTickCount;
				if (isFlowRedrawRebuild)
				{
					if (!isPresent || postReleaseTickCount >= 6) AdoptFlowRedraw();
					return;
				}
				if (isPresent && selected.stateC0 == 0 && selected.stateC1 == 0
					&& selected.stateC2 == 0 && selected.stateC3 == 0)
				{
					LOG_INFO("(NBN LAS COMMIT) The dense rebuild reset the committed previous state;"
						<< " forcing exactly one recommit for record=0x" << std::hex
						<< selectedRecord << std::dec << "." << std::endl);
					gatePhase = GatePhase::DenseRecommitAfterRebuild;
					commitTickCount = 0;
					wasCommitOverrideLogged = false;
					return;
				}
				if (isPresent && (selected.stateC0 == 0 || selected.stateC1 == 0))
				{
					PerformOwnedRelease(owner, "the dense rebuild left the previous record partially committed");
					FaultWithoutRelease("The dense rebuild produced a partial previous-record scoring state");
					return;
				}
				if (!isPresent || postReleaseTickCount >= 6)
				{
					AdoptDenseSuccessor();
				}
				return;
			}
			case GatePhase::DenseRecommitAfterRebuild:
			{
				++commitTickCount;
				if (!isPresent)
				{
					LOG_INFO("(NBN LAS COMMIT) The dense rebuild removed the already-committed previous record;"
						<< " no recommit identity remains." << std::endl);
					AdoptDenseSuccessor();
					return;
				}
				if (selected.stateC0 != 0 && selected.stateC1 != 0)
				{
					LOG_INFO("(NBN LAS COMMIT) Dense rebuild recommit completed for record=0x" << std::hex
						<< selectedRecord << std::dec << "." << std::endl);
					AdoptDenseSuccessor();
					return;
				}
				if (commitTickCount >= 120)
				{
					PerformOwnedRelease(owner, "the dense rebuild recommit did not complete within its bounded window");
					FaultWithoutRelease("The dense rebuild could not restore the committed previous record");
				}
				return;
			}
			case GatePhase::DensePlayerSongStartPending:
			{
				++postReleaseTickCount;
				uintptr_t playerSongAddress = reinterpret_cast<uintptr_t>(heldPlayerSong);
				uint8_t running = 0xFF;
				uint8_t stopped = 0xFF;
				uint32_t pendingState = 0;
				uint8_t pendingFlag = 0;
				if (playerSongAddress == 0
					|| !TryRead(playerSongAddress + PLAYER_SONG_RUNNING, running)
					|| !TryRead(playerSongAddress + PLAYER_SONG_STOPPED, stopped)
					|| !TryRead(playerSongAddress + PLAYER_SONG_PENDING_STATE, pendingState)
					|| !TryRead(playerSongAddress + PLAYER_SONG_PENDING_FLAG, pendingFlag))
				{
					PerformOwnedRelease(owner, "the dense rebuild's PlayerSong state became unreadable");
					FaultWithoutRelease("The dense rebuild could not observe the PlayerSong play packet");
					return;
				}
				if (stopped != 0)
				{
					if (postReleaseTickCount % 60 == 0)
					{
						LOG_INFO("(NBN LAS CHAIN) Waiting for the coordinated dense play packet:"
							<< " ticks=" << postReleaseTickCount
							<< " pendingState=" << pendingState
							<< " pendingFlag=" << static_cast<int>(pendingFlag)
							<< " running=" << static_cast<int>(running)
							<< " stopped=" << static_cast<int>(stopped) << "." << std::endl);
					}
					if (postReleaseTickCount >= DENSE_REBUILD_TIMEOUT_TICKS)
					{
						PerformOwnedRelease(owner, "the dense coordinated play packet did not clear Stop_TMusic");
						FaultWithoutRelease("The dense PlayerSong/fretboard rebuild timed out");
					}
					return;
				}
				if (!isPresent)
				{
					PerformOwnedRelease(owner, "the dense successor was absent after the PlayerSong play packet completed");
					FaultWithoutRelease("The dense successor did not survive the coordinated rebuild");
					return;
				}
				const bool successorStateDirty = selectedChordId != -1
					? (selected.stateC0 != 0 || selected.stateC1 != 0)
					: (selected.stateC0 != 0 || selected.stateC1 != 0
						|| selected.stateC2 != 0 || selected.stateC3 != 0);
				if (successorStateDirty)
				{
					PerformOwnedRelease(owner, "the dense successor changed scoring state before its visible target was armed");
					FaultWithoutRelease("The dense successor was not unresolved after the coordinated rebuild");
					return;
				}
				RelatchDenseSuccessor(owner, updateTime, selected);
				return;
			}
			case GatePhase::WaitingForInputRelease:
			{
				if (!isPresent)
				{
					PerformOwnedRelease(owner,
						"the dense successor disappeared while waiting for input release");
					FaultWithoutRelease("The dense successor disappeared while waiting for input release");
					return;
				}
				if (selected.stateC0 != 0 || selected.stateC1 != 0)
				{
					PerformOwnedRelease(owner,
						"the dense successor committed while its native hit decision was blocked");
					FaultWithoutRelease("The dense successor committed before a new input was armed");
					return;
				}

				if (IsPlainPickedTarget())
				{
					if (TakeBufferedPick()
						|| (isLegatoTarget && isHammerOnTarget && !isBendTarget && !isConfirmingLegatoRun
							&& ConfirmHeldLegatoPitch())
						|| ConfirmSameTimeSibling())
					{
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
					}
					return;
				}
				{
					const int reattack = TickSpikeReattackAcceptance(
						!MatchesPickPitch(previousExpectedMidi));
					if (reattack != -1 && IsAcceptedOnset(reattack))
					{
						if (StartBendFromWaitAccept(reattack, "Raw re-attack")) return;
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
						LOG_INFO("(NBN LAS INPUT) Accepted raw re-attack " << reattack
							<< " during the input-release wait: the onset edge carried a"
							<< " transient pitch, but the settled raw pitch matches the"
							<< " successor and not the previous note ("
							<< previousExpectedMidi << ")." << std::endl);
						return;
					}
				}

				int discardedOnset = QueryNativeOnsetNote();
				if (discardedOnset != -1)
				{
					const bool mayBeCarriedBend = previousWasBend
						&& discardedOnset >= previousExpectedMidi - MAX_BEND_SEMITONES
						&& discardedOnset <= previousExpectedMidi;
					if (IsAcceptedOnset(discardedOnset)
						&& discardedOnset != previousExpectedMidi
						&& !mayBeCarriedBend)
					{
						if (StartBendFromWaitAccept(discardedOnset, "Onset")) return;
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
						LOG_INFO("(NBN LAS INPUT) Accepted onset " << discardedOnset
							<< " during the input-release wait: it matches the successor and not"
							<< " the previous note (" << previousExpectedMidi
							<< "), so it is fresh playing rather than carry-over." << std::endl);
						return;
					}
					if (IsAcceptedOnset(discardedOnset) && spikeRecencyTicks > 0)
					{
						if (StartBendFromWaitAccept(discardedOnset, "Same-pitch onset")) return;
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
						LOG_INFO("(NBN LAS INPUT) Accepted same-pitch onset "
							<< discardedOnset << " during the input-release wait: a level"
							<< " spike accompanied it, and a decaying ring cannot spike,"
							<< " so it is a fresh pick even though the pitch matches the"
							<< " previous note (" << previousExpectedMidi << ")."
							<< std::endl);
						return;
					}

					LOG_INFO("(NBN LAS INPUT) Discarded carried onset " << discardedOnset
						<< " while waiting for the previous note to be released (expected "
						<< expectedMidi << ", previous " << previousExpectedMidi << ")."
						<< std::endl);
					if (discardedOnset != previousExpectedMidi && !mayBeCarriedBend)
					{
						reattackWindowTicks = REATTACK_WINDOW_TICKS;
						reattackStreak = 0;
					}
				}
				if (isBendTarget && !isBendChildTarget && bendAcceptMidi >= 0)
				{
					const int rawBend = QueryNativeLoudestPlayedNote();
					const bool mayBeCarriedBend = previousWasBend
						&& rawBend >= previousExpectedMidi - MAX_BEND_SEMITONES
						&& rawBend <= previousExpectedMidi;
					if (rawBend >= 0 && rawBend != previousExpectedMidi && !mayBeCarriedBend)
					{
						if (bendWaitPitchFloor < 0 || rawBend < bendWaitPitchFloor)
							bendWaitPitchFloor = rawBend;
						const bool rose = rawBend > bendWaitPitchFloor;
						const bool atTarget = rawBend >= bendAcceptMidi;
						bendWaitOnTargetTicks = atTarget ? bendWaitOnTargetTicks + 1 : 0;
						if (atTarget && ((rose && bendWaitOnTargetTicks >= BEND_ACCEPT_MIN_HOLD_TICKS)
							|| bendWaitOnTargetTicks >= BEND_WAIT_ON_TARGET_TICKS))
						{
							gatePhase = GatePhase::Holding;
							inputReleaseTickCount = 0;
							LOG_INFO("(NBN LAS INPUT) Bend confirmed by raw pitch during the"
								<< " input-release wait: sounding " << rawBend
								<< " reached the bent target " << bendAcceptMidi << " (floor "
								<< bendWaitPitchFloor << ", " << (rose ? "rose" : "steady")
								<< "), distinguishable from the previous note ("
								<< previousExpectedMidi << "), so it is the player's bend rather"
								<< " than carry-over." << std::endl);
							return;
						}
					}
					else
					{
						bendWaitOnTargetTicks = 0;
					}
				}
				if (spikeRecencyTicks > 0)
				{
					int primedOnset = QueryNativeOnsetNote();
					StashPrimedOnset(primedOnset);
					gatePhase = GatePhase::Holding;
					inputReleaseTickCount = 0;
					LOG_INFO("(NBN LAS INPUT) Level spike during the input-release wait; the"
						<< " follower is armed on the fresh attack even though its onset is masked"
						<< " by the previous note's ring (a decaying ring cannot spike). primedOnset="
						<< primedOnset << "." << std::endl);
					return;
				}

				bool isInputPresent = false;
				static bool hasReleaseReadFailureLogged = false;
				if (!TryReadDetectorInputPresence(isInputPresent))
				{
					if (!hasReleaseReadFailureLogged)
					{
						hasReleaseReadFailureLogged = true;
						LOG_ERROR("(NBN LAS INPUT) The detector ring is unreadable while waiting"
							<< " for input release; preserving the wait until current input state"
							<< " can be determined." << std::endl);
					}
					inputReleaseTickCount = 0;
					return;
				}
				hasReleaseReadFailureLogged = false;

				if (isInputPresent)
				{
					inputReleaseTickCount = 0;
					return;
				}

				++inputReleaseTickCount;
				if (inputReleaseTickCount < INPUT_RELEASE_CONFIRMATION_TICKS) return;

				int primedOnset = QueryNativeOnsetNote();
				StashPrimedOnset(primedOnset);
				gatePhase = GatePhase::Holding;
				LOG_INFO("(NBN LAS INPUT) Input release confirmed across "
					<< INPUT_RELEASE_CONFIRMATION_TICKS
					<< " detector polls; dense successor is now armed, primedOnset="
					<< primedOnset << "." << std::endl);
				return;
			}
			case GatePhase::Holding:
			{
				if (!isPresent)
				{
					FaultWithoutRelease("The selected native record disappeared from the live vector during the hold; the hold is abandoned without release because its scoring identity is gone");
					return;
				}
				if (selectedChordId != -1 && holdTickCount != 0 && holdTickCount % 60 == 0)
				{
					EmitCandidateEvent(owner, updateTime, selected,
						ResearchProtocol::ExpectedAttackEventKind::CandidateChanged);
				}
				if (selected.stateC0 != 0 || selected.stateC1 != 0)
				{
					PerformOwnedRelease(owner,
						"the held record committed while its native hit decision was blocked");
					FaultWithoutRelease("The held record committed before an exact onset was accepted");
					return;
				}
				if (hasHoldProgressAnchor)
				{
					const double heldSeconds = std::chrono::duration<double>(
						std::chrono::steady_clock::now() - holdProgressAnchor).count();
					const double safetyBudget = selectedChordId != -1
						? CHORD_HOLD_SAFETY_RELEASE_SECONDS
						: HOLD_SAFETY_RELEASE_SECONDS;
					if (heldSeconds >= safetyBudget && !isSafetyReleaseEnabled)
					{
						if (heldSeconds - lastStuckWarnHeldSeconds >= safetyBudget)
						{
							lastStuckWarnHeldSeconds = heldSeconds;
							LogDetectorGates("stuck-hold", true);
							LOG_ERROR("(NBN LAS SAFETY) The hold on record=0x" << std::hex
								<< selectedRecord << std::dec << " (string " << selectedString
								<< " fret " << selectedFret << ", expected MIDI " << expectedMidi
								<< ") has made no progress for " << std::fixed
								<< std::setprecision(1) << heldSeconds << "s. The safety release"
								<< " is disabled, so it stays held; the refusing state above is"
								<< " the evidence to read. N releases it." << std::endl);
						}
					}
					else if (heldSeconds >= safetyBudget)
					{
						LogDetectorGates("safety-release", true);
						LOG_ERROR("(NBN LAS SAFETY) The hold on record=0x" << std::hex
							<< selectedRecord << std::dec << " (string " << selectedString
							<< " fret " << selectedFret << ", expected MIDI " << expectedMidi
							<< ") made no progress for " << std::fixed << std::setprecision(1)
							<< heldSeconds << "s across " << holdTickCount
							<< " ticks on this note, so it cannot be satisfied. Releasing the"
							<< " transport without accepting the note rather than leaving the"
							<< " game frozen." << std::endl);
						consumedRecords.insert(selectedRecord);
						PerformOwnedRelease(owner,
							"the hold made no progress and was released by the safety timeout");
						ClearSelection();
						return;
					}
				}
				if (selectedChordId != -1)
				{
					LogDetectorGates("chord-holding", false);
					return;
				}

				if (isLegatoTarget && !isBendTarget && !isConfirmingLegatoRun && ConfirmHeldLegatoPitch())
				{
					gatePhase = GatePhase::CommitBeforeRelease;
					commitTickCount = 0;
					wasCommitOverrideLogged = false;
					return;
				}
				if (ConfirmSameTimeSibling())
				{
					gatePhase = GatePhase::CommitBeforeRelease;
					commitTickCount = 0;
					wasCommitOverrideLogged = false;
					return;
				}

				const bool mlBendConfirmed = isBendTarget && bendAcceptMidi >= 0 && MlConfirmsBendTarget();
				if (mlBendConfirmed && !isConfirmingLegatoRun)
				{
					BeginBendConfirmation();
				}

				if (isBendChildTarget && bendAcceptMidi >= 0 && !isConfirmingLegatoRun)
				{
					BeginBendConfirmation();
					LOG_INFO("(NBN LAS BEND) Bend child target, so no pick is required: this"
						<< " record continues the bend its parent started. Holding until the"
						<< " tracker reads " << bendAcceptMidi
						<< " (string " << selectedString << " fret " << selectedFret << ")."
						<< (legatoRunCount > 1 ? " The legato continuation(s) follow it." : "")
						<< std::endl);
					return;
				}
				{
					CaptureOnsetSpectrum(expectedMidi);
					int onset = -1;
					if (IsPlainPickedTarget())
					{
						if (!TakeBufferedPick()) return;
						onset = expectedMidi;
					}
					else if (!isConfirmingLegatoRun)
					{
						onset = QueryNativeOnsetNote();
						if (onset == -1)
						{
							int sounding = QueryNativeLoudestPlayedNote();
							if (sounding >= 0) sounding += NativeFrameOffset();   // played frame, like the onset
							if (sounding != -1 && !MatchesPickPitch(sounding))
							{
								hasPickPitchDeparted = true;
								pickPitchConfirmTicks = 0;
							}
							else if (sounding != -1 && MatchesPickPitch(sounding)
								&& hasPickPitchDeparted)
							{
								if (++pickPitchConfirmTicks >= LEGATO_CONFIRMATION_TICKS)
								{
									onset = expectedMidi;
									LOG_INFO("(NBN LAS REATTACK) Legato/bend accepted from"
										<< " departure-then-lock: the gated query reported a"
										<< " different pitch during this hold and then held "
										<< expectedMidi << " for " << pickPitchConfirmTicks
										<< " polls, the native legato-sustain rule (0x4E9670,"
										<< " pending a faithful port)." << std::endl);
								}
							}
							else if (sounding == -1)
							{
								pickPitchConfirmTicks = 0;
							}
						}
						if (onset == -1 && isBendTarget && bendAcceptMidi >= 0
							&& g_tier0Enforcement && Tier0ConfirmsExpected(bendAcceptMidi))
						{
							if (++bendRescueStreak >= BEND_RESCUE_STREAK)
							{
								bendRescueStreak = 0;
								onset = expectedMidi;
								enhancedRescueFeedback.targetRecord = selectedRecord;
								enhancedRescueFeedback.enhancedMidi = bendAcceptMidi;
								enhancedRescueFeedback.nativeMidi = QueryNativeLoudestPlayedNote();
								enhancedRescueFeedback.enhancedRole = NoteByNote::DetectorRole::Confirmed;
								LOG_INFO("(NBN LAS BEND RESCUE) Bend accepted: tier-0 confirms the bend"
									<< " settled at its target " << bendAcceptMidi << " (expected "
									<< expectedMidi << ") for " << BEND_RESCUE_STREAK
									<< " ticks where the detector read wobbled." << std::endl);
							}
						}
						else if (isBendTarget)
						{
							bendRescueStreak = 0;
						}
					}
					pendingPrimedOnset = -1;
					if (onset != -1)
					{
						LOG_INFO("(NBN LAS INPUT) Native onset " << onset << " while holding; expected "
						<< expectedMidi << (isBendTarget ? " (bend, accepts up to +3)" : "")
						<< " ndStreak=" << std::fixed << std::setprecision(3)
						<< CurrentNdStreakSeconds() << "s." << std::endl);
						if (expectedMidi >= 0 && onset != expectedMidi
							&& onset >= expectedMidi - 2 && onset <= expectedMidi + 2)
						{
							char frame[192];
							if (TryDescribeCurrentAnalysisFrame(frame, sizeof(frame)))
							{
								LOG_INFO("(NBN LAS NEAR MISS) onset=" << onset << " expected="
									<< expectedMidi << " | frame " << frame << std::endl);
							}
						}
						if (onset >= 0 && hasLastBendCommit && !sawSpikeDuringHold
							&& std::chrono::duration<double>(
								std::chrono::steady_clock::now() - lastBendCommitAt).count()
								< BEND_RELEASE_GUARD_SECONDS)
						{
							LOG_INFO("(NBN LAS BEND) Follower onset " << onset << " rejected: within "
								<< BEND_RELEASE_GUARD_SECONDS << "s of a bend commit and no attack"
								<< " spike - the decaying release ring, not a pluck (#52 cascade"
								<< " guard)." << std::endl);
							onset = -1;
						}
						if (IsAcceptedOnset(onset) || (isBendTarget && MatchesPickPitch(onset)))
						{
							if (bendAcceptMidi >= 0)
							{
								BeginBendConfirmation();
								if (onset < bendAcceptMidi)
								{
									isRawBendAcceptArmed = true;
								}
								LOG_INFO("(NBN LAS BEND) Pick accepted at " << onset
									<< (onset != expectedMidi
										? " (in the bend band above the base; the gesture"
										  " must still be proven)"
										: "")
									<< "; now holding until the bend reaches "
									<< bendAcceptMidi
									<< ". The bent pitch never arrives as an onset, so it is"
									<< " confirmed from the continuous pitch tracker."
									<< (legatoRunCount > 1
										? " The legato continuation(s) follow it."
										: "")
									<< std::endl);
								return;
							}
							if (legatoRunCount != 0)
							{
								isConfirmingLegatoRun = true;
								legatoRunIndex = 0;
								legatoConfirmTickCount = 0;
								LOG_INFO("(NBN LAS LEGATO) Pick accepted; holding for "
									<< legatoRunCount << " legato continuation(s) before the run"
									<< " commits. Next expected pitch " << legatoRunMidi[0]
									<< "." << std::endl);
								return;
							}
							gatePhase = GatePhase::CommitBeforeRelease;
							commitTickCount = 0;
							wasCommitOverrideLogged = false;
							LOG_INFO("(NBN LAS COMMIT) Matching onset accepted; transport remains held until"
								<< " the selected native record commits." << std::endl);
							return;
						}
						else
						{
							reattackWindowTicks = REATTACK_WINDOW_TICKS;
							reattackStreak = 0;
						}
					}
					if (isConfirmingLegatoRun && legatoRunIndex < legatoRunCount)
					{
						const int expectedRunMidi = legatoRunMidi[legatoRunIndex];
						const int currentMidi = QueryNativeLoudestPlayedNote();
						const bool isBendElement = legatoRunIsBend[legatoRunIndex];
						bool hasReachedRunPitch = false;
						bool reachedByTracker = false;
						if (isBendChildTarget)
						{
							hasBendApproachBeenObserved = true;
						}
						if (isBendElement)
						{
							const float wanted = static_cast<float>(expectedRunMidi);
							float sounding = 0.0f;
							if (TryGetSoundingPitchNear(
									wanted, BEND_OVERBEND_ALLOWANCE_SEMITONES, sounding))
							{
								hasReachedRunPitch =
									sounding >= (wanted - BEND_UNDERBEND_SEMITONES)
									&& sounding < (wanted + BEND_OVERBEND_ALLOWANCE_SEMITONES);
								reachedByTracker = hasReachedRunPitch;
								if (hasReachedRunPitch && !wasBendTrackerPitchLogged)
								{
									wasBendTrackerPitchLogged = true;
									LOG_INFO("(NBN LAS BEND) Tracked pitch " << std::fixed
										<< std::setprecision(3) << sounding
										<< " reached the target " << expectedRunMidi
										<< " within the widened bend band ("
										<< BEND_UNDERBEND_SEMITONES << " under, "
										<< BEND_OVERBEND_ALLOWANCE_SEMITONES << " over)."
										<< std::endl);
								}
							}
							else
							{
								if (currentMidi >= 0 && currentMidi < expectedRunMidi)
								{
									hasBendApproachBeenObserved = true;
								}
								hasReachedRunPitch = hasBendApproachBeenObserved
									&& (currentMidi >= expectedRunMidi);
								if (!hasReachedRunPitch)
								{
									DetectorGateSample raw;
									if (TryReadDetectorGates(raw))
									{
										const float rawPitch =
											static_cast<float>(raw.currentNote);
										const bool credible = raw.currentNote >= 0
											&& std::isfinite(raw.quality)
											&& raw.quality >= DETECTOR_RAW_BEND_QUALITY_FLOOR
											&& rawPitch >= (wanted - BEND_UNDERBEND_SEMITONES)
											&& rawPitch < (wanted + BEND_OVERBEND_ALLOWANCE_SEMITONES);
										if (!credible)
										{
											isRawBendAcceptArmed = true;
											ndBendSightingStreak = 0;
											float streakResetBelow = wanted - BEND_UNDERBEND_SEMITONES;
											if (bendAcceptMidi > expectedMidi && expectedMidi >= 0)
											{
												const float baseRelative = static_cast<float>(expectedMidi) + BEND_UNDERBEND_SEMITONES;
												if (baseRelative < streakResetBelow) streakResetBelow = baseRelative;
											}
											const bool belowTarget = rawPitch >= 0.0f && rawPitch < streakResetBelow;
											if (belowTarget)
											{
												rawBendAcceptStreak = 0;
											}
										}
										else
										{
											isRawBendAcceptArmed = true;
											++rawBendAcceptStreak;
											const bool inBendReleaseGuard = hasLastBendCommit
												&& !sawSpikeDuringHold
												&& std::chrono::duration<double>(
													std::chrono::steady_clock::now()
														- lastBendCommitAt).count()
													< BEND_RELEASE_GUARD_SECONDS;
											const bool strongGreen =
												raw.quality >= DETECTOR_RAW_BEND_STRONG_QUALITY
												&& !inBendReleaseGuard
												&& rawBendAcceptStreak >= BEND_ACCEPT_MIN_HOLD_TICKS;
											if (strongGreen
												|| rawBendAcceptStreak >= DETECTOR_RAW_BEND_STREAK_TICKS)
											{
												hasReachedRunPitch = true;
												LOG_INFO("(NBN LAS BEND) Raw detector pitch "
													<< raw.currentNote << " (quality "
													<< std::fixed << std::setprecision(0)
													<< raw.quality << ") "
													<< (strongGreen
														? "is a strong green; accepted immediately"
														: "held the target for a streak")
													<< " for target " << expectedRunMidi
													<< "; accepted from the ungated field because"
													<< " the tracker is unregistered while frozen"
													<< " and the gated queries are blind during a"
													<< " bend." << std::endl);
											}
										}
									}
								}
								if (!hasReachedRunPitch)
								{
									for (int midi = expectedRunMidi - 2; midi < expectedRunMidi; ++midi)
									{
										if (midi >= 0 && ReadNdSoundingStrength(midi) >= 0.0f)
										{
											hasBendApproachBeenObserved = true;
											break;
										}
									}
									bool targetSighted = false;
									const int overAllowance =
										static_cast<int>(BEND_OVERBEND_ALLOWANCE_SEMITONES);
									for (int midi = expectedRunMidi;
										midi <= expectedRunMidi + overAllowance; ++midi)
									{
										if (ReadNdSoundingStrength(midi) >= 0.0f)
										{
											targetSighted = true;
											break;
										}
									}
									if (targetSighted && hasBendApproachBeenObserved)
									{
										if (++ndBendSightingStreak >= 2)
										{
											hasReachedRunPitch = true;
											LOG_INFO("(NBN LAS BEND) Native sounding table"
												<< " reached the bend target " << expectedRunMidi
												<< " (approach observed, " << ndBendSightingStreak
												<< " sighting ticks)." << std::endl);
										}
									}
									else if (!targetSighted)
									{
										ndBendSightingStreak = 0;
									}
								}
								if (!hasReachedRunPitch)
								{
									float estimateMidi = -1.0f;
									float estimateConfidence = 0.0f;
									if (TryEstimateRawBendPitch(static_cast<double>(expectedMidi),
											static_cast<double>(expectedRunMidi), estimateMidi, estimateConfidence)
										&& estimateConfidence >= 40.0f)
									{
										if (estimateMidi < wanted - BEND_UNDERBEND_SEMITONES)
										{
											hasBendApproachBeenObserved = true;
											rawEstimateBendStreak = 0;
										}
										else if (estimateMidi < wanted + BEND_OVERBEND_ALLOWANCE_SEMITONES
											&& hasBendApproachBeenObserved)
										{
											if (++rawEstimateBendStreak >= 2)
											{
												hasReachedRunPitch = true;
												reachedByTracker = true;   // same estimator the veto uses; do not veto it
												LOG_INFO("(NBN LAS BEND) Raw-audio estimate " << std::fixed
													<< std::setprecision(2) << estimateMidi << " (confidence "
													<< std::setprecision(0) << estimateConfidence
													<< ") held the target " << expectedRunMidi
													<< " for 2 ticks; native pitch=" << currentMidi
													<< " (the player-frame reach; native may read a"
													<< " semitone low in Speaker Mode)." << std::endl);
											}
										}
										else
										{
											rawEstimateBendStreak = 0;
										}
									}
								}
								const auto now = std::chrono::steady_clock::now();
								if (!hasTrackerSampleAnchor
									|| std::chrono::duration<double>(
										now - trackerSampleAnchor).count() >= 1.0)
								{
									trackerSampleAnchor = now;
									hasTrackerSampleAnchor = true;
									LogMotionTrackers("bend-fallback", expectedRunMidi);
								}
							}
						}
						else
						{
							hasReachedRunPitch = (currentMidi == expectedRunMidi);
						}
						bool enhancedCheckedBend = false;
						float enhancedBendMidi = -1.0f;
						if (isBendElement && hasReachedRunPitch && !reachedByTracker)
						{
							float vetoEstMidi = -1.0f;
							float vetoEstConfidence = 0.0f;
							if (TryEstimateRawBendPitch(
									static_cast<double>(expectedMidi),
									static_cast<double>(expectedRunMidi),
									vetoEstMidi, vetoEstConfidence)
								&& vetoEstConfidence >= 40.0f)
							{
								enhancedCheckedBend = true;
								enhancedBendMidi = vetoEstMidi;
								if (vetoEstMidi < static_cast<float>(expectedRunMidi)
									- BEND_UNDERBEND_SEMITONES - BEND_VETO_ESTIMATOR_SLACK_SEMITONES)
								{
									hasReachedRunPitch = false;
									static uintptr_t loggedVetoRecord = 0;
									static uint64_t loggedVetoHoldTick = 0;
									if (loggedVetoRecord != selectedRecord || holdTickCount > loggedVetoHoldTick + 30)
										LOG_INFO("(NBN LAS BEND VETO) Native reached " << expectedRunMidi
											<< " (native pitch=" << currentMidi << ") but the raw-audio estimate reads "
											<< std::fixed << std::setprecision(2) << vetoEstMidi << " (confidence "
											<< std::setprecision(0) << vetoEstConfidence << "), below the band; not yet"
											<< " bent." << std::endl);
									loggedVetoRecord = selectedRecord;
									loggedVetoHoldTick = holdTickCount;
								}
							}
						}
						if (isBendElement && mlBendConfirmed && expectedRunMidi == bendAcceptMidi)
						{
							hasReachedRunPitch = true;
							enhancedCheckedBend = false;
							enhancedRescueFeedback = {};
							mlRescueRecord = selectedRecord;
							mlRescueMidi = expectedRunMidi;
							LOG_INFO("(NBN LAS ML BEND) Two distinct post-target predictions confirmed bend pitch "
								<< expectedRunMidi << "; native pitch=" << currentMidi
								<< " child=" << isBendChildTarget << "." << std::endl);
						}
						const uint32_t requiredPolls = isBendElement
								? 1u
								: LEGATO_CONFIRMATION_TICKS;
						if (hasReachedRunPitch)
						{
							if (isBendElement)
							{
								bendVisualReached = true;
								bendVisualReachedRecord = selectedRecord;
								bendVisualReachedEpoch = epochIndex;
							}
							++legatoConfirmTickCount;
							if (legatoConfirmTickCount >= requiredPolls)
							{
								if (enhancedCheckedBend)
								{
									enhancedRescueFeedback.targetRecord = selectedRecord;
									enhancedRescueFeedback.enhancedMidi = static_cast<int>(enhancedBendMidi + 0.5f);
									enhancedRescueFeedback.nativeMidi = currentMidi;
									enhancedRescueFeedback.enhancedRole = NoteByNote::DetectorRole::Partial;
								}
								legatoConfirmTickCount = 0;
								++legatoRunIndex;
								if (isBendElement)
								{
									TryWriteDedupeGlobal(expectedRunMidi);
									lastBendCommitAt = std::chrono::steady_clock::now();
									hasLastBendCommit = true;
								}
								isRawBendAcceptArmed = false;
								rawBendAcceptStreak = 0;
								ndBendSightingStreak = 0;
								hasBendApproachBeenObserved = false;
								holdProgressAnchor = std::chrono::steady_clock::now();
								mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
								mlBendConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
								enhancedLegatoConfirmation.Reset(latestRawAudioSample);
								if (legatoRunIndex >= legatoRunCount)
								{
									isConfirmingLegatoRun = false;
									gatePhase = GatePhase::CommitBeforeRelease;
									commitTickCount = 0;
									wasCommitOverrideLogged = false;
									LOG_INFO("(NBN LAS LEGATO) Run complete; all " << legatoRunCount
										<< " continuation(s) played. The gesture commits as one note."
										<< std::endl);
									consumedRecords.insert(selectedRecord);
									lastCommittedChordRecord = selectedRecord;
									lastCommittedChordId = -1;
									lastCommittedChordAt = std::chrono::steady_clock::now();
									pickAttackFloorSample = chordAttacks.GetConsumedThroughSample();
									lastChordStrumSample = 0;
								}
								else
								{
									LOG_INFO("(NBN LAS LEGATO) Continuation " << legatoRunIndex
										<< " of " << legatoRunCount << " confirmed at pitch "
										<< expectedRunMidi << "; next expected "
										<< legatoRunMidi[legatoRunIndex] << "." << std::endl);
								}
							}
						}
						else
						{
							legatoConfirmTickCount = 0;
						}
						return;
					}
					if (isLegatoTarget && expectedMidi != previousExpectedMidi)
					{
						const int currentPitch = QueryNativeLoudestPlayedNote();
						if (currentPitch != expectedMidi)
						{
							hasLegatoPitchDeparted = true;
							legatoConfirmTickCount = 0;
						}
						else if (hasLegatoPitchDeparted)
						{
							++legatoConfirmTickCount;
							if (legatoConfirmTickCount >= LEGATO_CONFIRMATION_TICKS)
							{
								gatePhase = GatePhase::CommitBeforeRelease;
								commitTickCount = 0;
								wasCommitOverrideLogged = false;
								legatoConfirmTickCount = 0;
								LOG_INFO("(NBN LAS COMMIT) Legato pitch " << expectedMidi
									<< " confirmed from the detector's current-pitch query across "
									<< LEGATO_CONFIRMATION_TICKS
									<< " polls, after first departing from it; this no-pick"
									<< " technique produces no attack for the onset query to"
									<< " latch." << std::endl);
							}
						}
					}
				}
				return;
			}
			case GatePhase::CommitBeforeRelease:
			{
				++commitTickCount;
				if (!isPresent)
				{
					PerformOwnedRelease(owner,
						"the selected record disappeared before its held commit completed");
					FaultWithoutRelease("The selected record disappeared before its held commit completed");
					return;
				}
				if (selected.stateC0 != 0 && selected.stateC1 != 0)
				{
					CompleteHeldCommit(owner, updateTime, selected,
						"the selected onset was committed before transport release");
					return;
				}
				if (commitTickCount >= 120)
				{
					PerformOwnedRelease(owner,
						"the held pre-release commit did not complete within its bounded window");
					FaultWithoutRelease("The held pre-release commit did not complete within its bounded window");
				}
				return;
			}
			case GatePhase::PostRelease:
			{
				++postReleaseTickCount;
				if (isPresent && selected.stateC0 == 0 && selected.stateC1 == 0
					&& selected.stateC2 == 0 && selected.stateC3 == 0)
				{
					LOG_INFO("(NBN LAS COMMIT) The release rebuild reset the committed selected state;"
						<< " forcing exactly one native recommit for record=0x" << std::hex
						<< selectedRecord << std::dec << "." << std::endl);
					gatePhase = GatePhase::RecommitAfterRelease;
					commitTickCount = 0;
					wasCommitOverrideLogged = false;
					return;
				}
				if (postReleaseTickCount >= 6)
				{
					ClearSelection();
				}
				return;
			}
			case GatePhase::RecommitAfterRelease:
			{
				++commitTickCount;
				if (!isPresent)
				{
					LOG_INFO("(NBN LAS COMMIT) The already-committed selected record left the live vector"
						<< " after release; no post-release identity remains to recommit." << std::endl);
					ClearSelection();
					return;
				}
				if (selected.stateC0 != 0 && selected.stateC1 != 0)
				{
					LOG_INFO("(NBN LAS COMMIT) Forced recommit completed for record=0x" << std::hex
						<< selectedRecord << std::dec << "." << std::endl);
					ClearSelection();
					return;
				}
				if (commitTickCount >= 120)
				{
					FaultWithoutRelease("The forced single recommit did not complete within the bounded window");
				}
				return;
			}
			default:
				return;
		}
	}

	void __stdcall ScoringUpdateDetour(void* owner, float updateTime)
	{
		std::lock_guard<std::recursive_mutex> lock(controllerMutex);
		observedScoringUpdateTime = updateTime;
		bool isEnabled = ResearchProbeRuntime::IsNoteByNoteEnabled();

		if (isReleaseRequested)
		{
			isReleaseRequested = false;
			if (OwnsNativeHold()
				&& owner == trackedOwner)
			{
				PerformOwnedRelease(owner, "Note by Note was stopped while a native hold was owned");
			}
			ClearSelection();
		}

		if (!isEnabled)
		{
			bendVisualizationSnapshot = {};
			if (trackedOwner != nullptr) ResetBootstrap("Note by Note is disabled", owner, true);
			RefreshBendVisualizationSnapshot();
			originalScoringUpdate(owner, updateTime);
			return;
		}
		if (reArmRequested)
		{
			reArmRequested = false;
			ResetBootstrap("Note by Note was re-enabled; re-arming for the current section");
			trackedOwner = owner;
		}

		if (owner != trackedOwner)
		{
			ResetBootstrap("the scoring owner changed");
			trackedOwner = owner;
		}
		if (isEpochConfirmed && timelineOwner == owner && !timelineSections.empty())
		{
			uintptr_t liveContainer = 0;
			uintptr_t liveBegin = 0;
			uintptr_t liveEnd = 0;
			if (TryRead(reinterpret_cast<uintptr_t>(owner) + LAS_PHRASE_SECTION_CONTAINER, liveContainer)
				&& liveContainer != 0
				&& TryRead(liveContainer + PHRASE_SECTION_VECTOR_BEGIN, liveBegin)
				&& TryRead(liveContainer + PHRASE_SECTION_VECTOR_END, liveEnd)
				&& (liveContainer != timelineContainer
					|| liveBegin != timelineBegin
					|| liveEnd != timelineEnd))
			{
				LOG_INFO("(NBN LAS TIMELINE) The authored section grid changed under the confirmed"
					<< " controller (song reload); re-arming for the new song." << std::endl);
				ResetBootstrap("the song's authored section grid changed");
				trackedOwner = owner;
			}
		}
		if (!isEpochConfirmed)
		{
			TryLatchSectionFromGrid(owner);
		}
		else if (HasSelectionMovedToNewSection(owner))
		{
			const float previousStart = greyCutoff;
			LOG_INFO("(NBN LAS BOOTSTRAP) Riff Repeater section selection changed while enabled;"
				<< " auto-following to the newly selected section (no toggle needed)."
				<< std::endl);
			ResetBootstrap("the Riff Repeater section selection changed", owner);
			trackedOwner = owner;
			TryLatchSectionFromGrid(owner, previousStart);
		}
		if (isEpochConfirmed && (++armDiagTicks % 90u) == 0u)
		{
			LOG_INFO("(NBN LAS ARMDIAG) via=" << (confirmedViaGrid ? "grid" : "fallback")
				<< " grid=[" << std::fixed << std::setprecision(3) << greyCutoff << ".."
				<< sectionEndBoundary << "] transport=" << updateTime
				<< " inSection=" << ((updateTime >= greyCutoff && updateTime < sectionEndBoundary) ? 1 : 0)
				<< " timelineSections=" << timelineSections.size()
				<< " epoch=" << epochIndex << "." << std::endl);
		}

		float nativeSectionStart = 0.0f;
		float nativeSectionEnd = 0.0f;
		const bool hasNativeRange = TryReadNativeSectionRange(owner, nativeSectionStart, nativeSectionEnd);
		if (!hasNativeRange && !isEpochConfirmed)
		{
			if (!hasNativeSectionRangeFailureLogged)
			{
				hasNativeSectionRangeFailureLogged = true;
				LOG_ERROR("(NBN LAS BOOTSTRAP) The GamePlaysongLAS Riff Repeater bounds"
					<< " were unreadable or their mirrored fields disagreed; Note by Note"
					<< " remains inert until Rocksmith publishes a valid native range. "
					<< DescribeNativeSectionRange(owner) << std::endl);
			}
			originalScoringUpdate(owner, updateTime);
			return;
		}
		if (hasNativeRange)
		{
			hasNativeSectionRangeFailureLogged = false;
		}
		(void)nativeSectionStart;
		if (hasLastUpdateTime && updateTime < lastUpdateTime - ROLLBACK_THRESHOLD)
		{
			if (OwnsNativeHold())
			{
				LOG_ERROR("(NBN LAS LIFECYCLE) External rollback while a native hold was"
					<< " owned; abandoning the hold without release and re-bootstrapping"
					<< " in place." << std::endl);
				SetNativeFreezeFlag(owner, false);
				ResetBootstrap("an external rollback occurred while a native hold was owned");
				trackedOwner = owner;
			}
			else if (!isEpochConfirmed)
			{
				if (!hasPendingBoundary)
				{
					hasPendingBoundary = true;
					pendingBoundaryBeforeRollback = lastUpdateTime;
					pendingBoundaryAfterRollback = updateTime;
					LOG_INFO("(NBN LAS BOOTSTRAP) Native rollback "
						<< std::fixed << std::setprecision(6) << lastUpdateTime
						<< " -> " << updateTime << "; resolving the section boundary by live"
						<< " authored/native identity across both rollback endpoints."
						<< std::endl);
				}
			}
			else
			{
				++epochIndex;
				ClearSelection();
				consumedRecords.clear();
				TryWriteDedupeGlobal(-1);
				LOG_INFO("(NBN LAS BOOTSTRAP) Native epoch restart -> epoch " << epochIndex
					<< " at updateTime=" << std::fixed << std::setprecision(6) << updateTime
					<< "." << std::endl);
			}
		}
		lastUpdateTime = updateTime;
		hasLastUpdateTime = true;
		if (!isEpochConfirmed && hasPendingBoundary)
		{
			LiveNote beforeRollbackBoundary;
			LiveNote afterRollbackBoundary;
			bool hasBeforeRollbackBoundary = false;
			bool hasAfterRollbackBoundary = false;
			ForEachLiveNote(owner, [&](const LiveNote& note)
			{
				if (std::fabs(note.eventTime - pendingBoundaryBeforeRollback) <= BOUNDARY_EPSILON
					&& std::fabs(note.recordTime - pendingBoundaryBeforeRollback) <= BOUNDARY_EPSILON)
				{
					beforeRollbackBoundary = note;
					hasBeforeRollbackBoundary = true;
				}
				if (std::fabs(note.eventTime - pendingBoundaryAfterRollback) <= BOUNDARY_EPSILON
					&& std::fabs(note.recordTime - pendingBoundaryAfterRollback) <= BOUNDARY_EPSILON)
				{
					afterRollbackBoundary = note;
					hasAfterRollbackBoundary = true;
				}
				return true;
			});

			if (hasBeforeRollbackBoundary == hasAfterRollbackBoundary && !hasRollbackIdentityMissLogged
				&& ++rollbackIdentityMisses >= 180)
			{
				hasRollbackIdentityMissLogged = true;
				LOG_ERROR("(NBN LAS BOOTSTRAP) Rollback " << std::fixed << std::setprecision(3)
					<< pendingBoundaryBeforeRollback << " -> " << pendingBoundaryAfterRollback << ": "
					<< (hasBeforeRollbackBoundary ? "BOTH endpoints" : "NEITHER endpoint")
					<< " matched a live note's authored and native time, so the section boundary cannot be"
					<< " resolved this way; Note by Note stays inert until the grid latch succeeds." << std::endl);
			}
			if (hasBeforeRollbackBoundary != hasAfterRollbackBoundary)
			{
				const bool usesBeforeRollbackEndpoint = hasBeforeRollbackBoundary;
				const LiveNote& boundaryNote = usesBeforeRollbackEndpoint
					? beforeRollbackBoundary
					: afterRollbackBoundary;
				greyCutoff = usesBeforeRollbackEndpoint
					? pendingBoundaryBeforeRollback
					: pendingBoundaryAfterRollback;
				sectionEndBoundary = nativeSectionEnd;
				hasSectionEndBoundary = true;
				isEpochConfirmed = true;
				confirmedViaGrid = false;
				epochIndex = 1;
				consumedRecords.clear();
				LOG_INFO("(NBN LAS BOOTSTRAP) Section boundary confirmed by record=0x"
					<< std::hex << boundaryNote.record << std::dec << " at greyCutoff="
					<< std::fixed << std::setprecision(6) << greyCutoff << " using the rollback's "
					<< (usesBeforeRollbackEndpoint ? "pre-jump" : "post-jump")
					<< " endpoint; end latched at " << sectionEndBoundary
					<< "; epoch 1 begins." << std::endl);

				if (!usesBeforeRollbackEndpoint)
				{
					LOG_INFO("(NBN LAS BOOTSTRAP) The post-jump endpoint is the native"
						<< " section-initialization update; its first scoring pass is"
						<< " suppressed." << std::endl);
					RefreshBendVisualizationSnapshot();
					return;
				}
			}
		}

		if (isEpochConfirmed)
		{
			if (gatePhase == GatePhase::Idle || gatePhase == GatePhase::Armed)
			{
				UpdateSelection(owner, updateTime);
				if (gatePhase == GatePhase::Armed) LogDetectorGates("armed", false);
			}
			else if (OwnsNativeHold())
			{
				++holdTickCount;

				{
					const bool usesSuccessor = denseSuccessor.record != 0;
					const uintptr_t visual = usesSuccessor ? denseSuccessor.record : selectedRecord;
					if (visual != lastVisualGroupRecord)
					{
						lastVisualGroupRecord = visual;
						BuildVisualGroup(owner, visual,
							usesSuccessor ? denseSuccessor.recordTime : selectedRecordTime,
							usesSuccessor ? denseSuccessor.stringIndex : selectedString);
					}
				}
				if (holdTickCount == 1)
				{
					holdTickRateAnchor = std::chrono::steady_clock::now();
					holdTickRateAnchorTick = 0;
					hasDetectorSampleAnchor = false;
					hasLastSampledRingIndex = false;
				}
				LogDetectorGates(DescribeGatePhase(gatePhase), false);
				if (gatePhase != GatePhase::DensePlayerSongStartPending
					&& std::fabs(updateTime - heldEpoch) > HELD_TIME_EPSILON)
				{
					std::ostringstream reason;
					reason << "The authoritative scoring time advanced to " << std::fixed
						<< std::setprecision(6) << updateTime << " while the hold owned epoch "
						<< heldEpoch << "; the stop latch failed, so no release seek is issued";
					FaultWithoutRelease(reason.str());
				}
				else if (holdTickCount % 240 == 0)
				{
					const auto now = std::chrono::steady_clock::now();
					const double elapsedSeconds =
						std::chrono::duration<double>(now - holdTickRateAnchor).count();
					const double ticksPerSecond = elapsedSeconds > 0.0
						? static_cast<double>(holdTickCount - holdTickRateAnchorTick) / elapsedSeconds
						: 0.0;
					LOG_INFO("(NBN LAS HOLD) Steady at " << std::fixed << std::setprecision(6)
						<< heldEpoch << " after " << holdTickCount << " scoring ticks at "
						<< std::setprecision(1) << ticksPerSecond << " ticks/sec"
						<< (ticksPerSecond < 55.0
							? " (below the usual 60; the scoring update is frame-coupled)"
							: "")
						<< "." << std::endl);
					holdTickRateAnchor = now;
					holdTickRateAnchorTick = holdTickCount;
				}
			}
		}

		RefreshBendVisualizationSnapshot();
		originalScoringUpdate(owner, updateTime);

		if (ResearchProbeRuntime::IsNoteByNoteEnabled() && isEpochConfirmed)
		{
			HandleAfterUpdate(owner, updateTime);
		}
	}
	bool EvaluateHeldChordDecision(void* owner, void* unusedEdx, void* note)
	{
		const auto noteAddress = reinterpret_cast<uintptr_t>(note);
		float authoredTime = 0.0f;
		float startDelta = 0.0f;
		float endDelta = 0.0f;
		double detectorClock = 0.0;
		const bool hasWindow = TryRead(noteAddress + NOTE_EVENT_TIME, authoredTime)
			&& TryRead(noteAddress + NOTE_DETECT_WINDOW_START_DELTA, startDelta)
			&& TryRead(noteAddress + NOTE_DETECT_WINDOW_END_DELTA, endDelta)
			&& std::isfinite(authoredTime)
			&& std::isfinite(startDelta)
			&& std::isfinite(endDelta)
			&& TryReadDetectorClock(detectorClock)
			&& std::isfinite(detectorClock);
		if (!hasWindow)
		{
			return originalHitDecision(owner, unusedEdx, note);
		}

		const float windowStart = authoredTime + startDelta;
		const float windowEnd = authoredTime + endDelta;
		const auto clock = static_cast<float>(detectorClock);
		const bool isClockInWindow = clock >= windowStart && clock <= windowEnd;

		bool didSlide = false;
		bool result = false;
		if (isChordWindowSlideEnabled)
		{
			const float negativeStart = -(authoredTime + 1.0f);   // windowStart == -1.0
			didSlide = TryWriteGameFloat(
				noteAddress + NOTE_DETECT_WINDOW_START_DELTA, negativeStart);
			result = originalHitDecision(owner, unusedEdx, note);
			TryWriteGameFloat(noteAddress + NOTE_DETECT_WINDOW_START_DELTA, startDelta);
			if (didSlide) ++chordWindowSlideCount;
		}
		else
		{
			result = originalHitDecision(owner, unusedEdx, note);
		}
		const auto now = std::chrono::steady_clock::now();
		if ((result && didSlide)
			|| (verboseTrace
				&& (!hasChordWindowLogAnchor
					|| std::chrono::duration<double>(now - chordWindowLogAnchor).count() >= 1.0)))
		{
			chordWindowLogAnchor = now;
			hasChordWindowLogAnchor = true;
			char frameSummary[160] = "unreadable";
			TryDescribeCurrentAnalysisFrame(frameSummary, sizeof(frameSummary));
			LOG_INFO("(NBN LAS CHORD WINDOW) clock=" << std::fixed << std::setprecision(3)
				<< detectorClock
				<< " window=[" << windowStart << "," << windowEnd << "]"
				<< " heldEpoch=" << heldEpoch
				<< (isClockInWindow ? " in-window" : " OUT-OF-WINDOW")
				<< (didSlide ? " slid" : (isChordWindowSlideEnabled ? "" : " slide-off"))
				<< " result=" << std::boolalpha << result
				<< " slides=" << chordWindowSlideCount
				<< " | frame " << frameSummary << "." << std::endl);
		}
		return result;
	}

	bool __fastcall HitDecisionDetour(void* owner, void* unusedEdx, void* note)
	{
		std::lock_guard<std::recursive_mutex> lock(controllerMutex);

		if (!isEpochConfirmed || selectedRecord == 0 || owner != trackedOwner
			|| !ResearchProbeRuntime::IsNoteByNoteEnabled())
		{
			const bool originalResult = originalHitDecision(owner, unusedEdx, note);
			ObserveBendDecision(reinterpret_cast<uintptr_t>(note), originalResult);
			return originalResult;
		}

		uintptr_t record = 0;
		float recordTime = 0.0f;
		if (!TryRead(reinterpret_cast<uintptr_t>(note) + NOTE_RECORD, record) || record == 0
			|| !TryRead(record + RECORD_TIME, recordTime))
		{
			return originalHitDecision(owner, unusedEdx, note);
		}

		if (recordTime < greyCutoff - GREY_EPSILON)
		{
			return originalHitDecision(owner, unusedEdx, note);
		}

		if (record == selectedRecord)
		{
			if (gatePhase == GatePhase::Armed && isFlowUntilMissEnabled
				&& armedBufferCommitRecord == selectedRecord)
			{
				if (!wasArmedBufferCommitLogged)
				{
					wasArmedBufferCommitLogged = true;
					LOG_INFO("(NBN FLOW) Forcing the Armed hit decision for the buffered attack." << std::endl);
				}
				return true;
			}
			if (gatePhase == GatePhase::Armed
				&& NoteByNote::ShouldBlockPreHoldHitDecision(
					isFlowUntilMissEnabled, isHoldSuppressed))
			{
				return false;
			}
			if (gatePhase == GatePhase::WaitingForInputRelease
				|| gatePhase == GatePhase::Holding
				|| gatePhase == GatePhase::DenseRebuildPending
				|| gatePhase == GatePhase::DensePlayerSongStartPending)
			{
				if (selectedChordId != -1
					&& (gatePhase == GatePhase::Holding
						|| gatePhase == GatePhase::WaitingForInputRelease)
					&& areChordHoldsEnabled)
				{
					const bool naturalResult = EvaluateHeldChordDecision(owner, unusedEdx, note);
					++chordDecisionEvalCount;
					int matcherTones[6] = { 0, 0, 0, 0, 0, 0 };
					int playedTones[6] = { 0, 0, 0, 0, 0, 0 };
					int playedMidiByString[6] = { -1, -1, -1, -1, -1, -1 };
					int matcherToneCount = 0;
					{
						const int matcherInputShift = ResearchProbeRuntime::GetInputOnsetShiftSemitones();
						uintptr_t chordTemplate = 0;
						ChordTemplateView matcherView = {};
						if (TryRead(reinterpret_cast<uintptr_t>(note) + 0x30, chordTemplate)
							&& chordTemplate != 0 && TryRead(chordTemplate, matcherView))
						{
							for (int i = 0; i < 6; ++i)
							{
								if (matcherView.frets[i] < 0x1A && matcherView.notes[i] >= 0)
								{
									int16_t tuningOffset = 0;
									int playedMidi = -1;
									if (!TryRead(TUNING_OFFSETS + static_cast<uintptr_t>(i) * 2, tuningOffset)
										|| !NoteByNote::TryResolvePlayedNotePitch(i, matcherView.frets[i],
											tuningOffset, matcherInputShift, playedMidi, IsBassArrangement()))
									{
										FaultWithoutRelease("The held chord's played pitch could not be resolved");
										return false;
									}
									playedTones[matcherToneCount] = playedMidi;
									playedMidiByString[i] = playedMidi;
									matcherTones[matcherToneCount++] = matcherView.notes[i];
								}
							}
						}
						if (matcherToneCount > 0)
						{
							researchChordToneCount = matcherToneCount;
							for (int i = 0; i < matcherToneCount; ++i)
								researchChordTones[i] = playedTones[i];
							researchChordTonesRecord = selectedRecord;
						}
					}
					const bool portRan = matcherToneCount > 0;
					const bool freshAttack = rawAttackStreamAvailable
						&& chordAttacks.HasFreshAttack(pickScanSample, pickSampleRate);
					const uint64_t chordAttackSample = chordAttacks.GetAttackSample();
					const uint64_t chordAttackOriginSample = chordAttacks.GetAgeOriginSample();
					uint32_t chordNoteMask = 0;
					if (!TryRead(record + RECORD_MASK, chordNoteMask))
					{
						LOG_ERROR("(NBN LAS CHORD) Cannot read held chord technique mask." << std::endl);
						return false;
					}
					const bool fretHandMuted = NoteByNote::ChordAttackGate::IsFretHandMuted(chordNoteMask);
					int unisonPitch = -1;
					const bool isUnison = NoteByNote::TryGetUnisonPitch(playedTones, matcherToneCount, unisonPitch);
					int soundingTargetTones = 0;
					for (int i = 0; i < matcherToneCount; ++i)
					{
						if (ReadNdSoundingStrength(matcherTones[i]) >= 0.0f) ++soundingTargetTones;
					}
					const double attackAgeMs = pickSampleRate != 0 && pickScanSample >= chordAttackOriginSample
						? static_cast<double>(pickScanSample - chordAttackOriginSample) * 1000.0 / pickSampleRate
						: 1e9;
					if (chordAttackSample != chordSoundingPeakAttack)
					{
						chordSoundingPeakAttack = chordAttackSample;
						chordSoundingPeak = 0;
					}
					if (freshAttack && soundingTargetTones > chordSoundingPeak)
						chordSoundingPeak = soundingTargetTones;
					const int requiredSounding = NoteByNote::GetRequiredChordSoundingToneCount(matcherToneCount);
					const int strumPresence = freshAttack && !fretHandMuted && !isUnison && matcherToneCount >= 2
						? StrumChordPresenceForAttack(playedTones, matcherToneCount, chordAttackSample) : -1;
					if (freshAttack && !fretHandMuted && !isUnison && matcherToneCount >= 2)
						RegisterStrumLedgerAttack(chordAttackSample, playedTones, matcherToneCount);
					const bool nativeMatcherMatches = !isUnison && !naturalResult
						&& NativeChordMatchesAttack(matcherTones, matcherToneCount, chordAttackOriginSample);
					const bool corroborated = NoteByNote::IsChordSoundingCorroborated(matcherToneCount,
						chordSoundingPeak, naturalResult, attackAgeMs,
						strumPresence == 1 || IsBassArrangement(), nativeMatcherMatches);
					const bool rawUnisonMatches = isUnison
						&& RawUnisonMatchesAttack(unisonPitch, chordAttackSample);
					const bool closeDyadMatches = !isUnison && !nativeMatcherMatches
						&& RawCloseChordMatchesAttack(playedTones, matcherToneCount, chordAttackSample);
					ResearchProtocol::MlChordEvidence mlChordEvidence;
					const bool mlStringsMatch = !isUnison && !nativeMatcherMatches
						&& !closeDyadMatches && freshAttack
						&& ResearchProbeRuntime::QueryMlChordEvidence(
							playedMidiByString, 0.5f, chordAttackSample, mlChordEvidence)
						&& mlChordEvidence.sampleRate == pickSampleRate
						&& mlChordEvidence.verdict == ResearchProtocol::MlNoteVerdict::Confirmed;
					const bool tier0Matches = !isUnison && !naturalResult && !nativeMatcherMatches
						&& !closeDyadMatches && !mlStringsMatch && isChordTier0RescueEnabled
						&& Tier0ConfirmsChord(playedTones, matcherToneCount);
					if (freshAttack && (naturalResult || nativeMatcherMatches))
						chordNativeAgreedAttack = chordAttackSample;
					const bool nativeAgreedOnStrum = freshAttack && chordAttackSample != 0
						&& chordNativeAgreedAttack == chordAttackSample;
					const bool powerChordRawMatches = !isUnison && !closeDyadMatches && nativeAgreedOnStrum
						&& NoteByNote::IsPowerChordShape(playedTones, matcherToneCount)
						&& RawPowerChordMatchesAttack(playedTones, matcherToneCount, chordAttackSample);
					NoteByNote::ChordPitchDecisionInput chordDecision;
					chordDecision.toneCount = matcherToneCount;
					chordDecision.isFretHandMuted = fretHandMuted;
					chordDecision.didBuildTarget = portRan;
					chordDecision.hasFreshAttack = freshAttack;
					chordDecision.isCorroborated = corroborated;
					chordDecision.isUnison = isUnison;
					chordDecision.rawUnisonMatches = rawUnisonMatches;
					chordDecision.naturalMatches = naturalResult;
					chordDecision.nativeMatcherMatches = nativeMatcherMatches;
					chordDecision.closeDyadMatches = closeDyadMatches;
					chordDecision.mlStringsMatch = mlStringsMatch;
					chordDecision.isTier0Enabled = isChordTier0RescueEnabled;
					chordDecision.tier0Matches = tier0Matches;
					chordDecision.powerChordRawMatches = powerChordRawMatches;
					chordDecision.nativeAgreedOnStrum = nativeAgreedOnStrum;
					const auto pitchConfirmation = NoteByNote::EvaluateChordPitchDecision(chordDecision);
					const bool legacyPitchesMatch = pitchConfirmation != NoteByNote::ChordPitchConfirmation::None;
					bool ledgerDecided = false;
					bool pitchesMatch = legacyPitchesMatch;
					if (NoteByNote::STRUM_LEDGER_DECIDES && !IsBassArrangement()
						&& freshAttack && !fretHandMuted && !isUnison && matcherToneCount >= 2)
					{
						ProcessPendingStrumLedger();
						const int ledgerVerdict = GetStrumLedgerVerdict(chordAttackSample);
						const bool witness = legacyPitchesMatch;
						if (ledgerVerdict == 1) pitchesMatch = witness;
						else if (ledgerVerdict == 0) pitchesMatch = false;
						else pitchesMatch = matcherToneCount >= 3 && naturalResult && legacyPitchesMatch
							&& chordSoundingPeak >= matcherToneCount;
						ledgerDecided = ledgerVerdict == 1 && witness;
						static uint64_t loggedLedgerDecision = 0;
						if (ledgerVerdict != -1 && loggedLedgerDecision != chordAttackSample)
						{
							loggedLedgerDecision = chordAttackSample;
							LOG_INFO("(NBN STRUM LEDGER DECISION) attack=" << chordAttackSample
								<< " ledger=" << (ledgerVerdict == 1 ? "strummed" : "not-strummed")
								<< " witness=" << (witness ? 1 : 0) << " (vote=" << (naturalResult ? 1 : 0)
								<< " matcher=" << (nativeMatcherMatches ? 1 : 0) << " ml=" << (mlStringsMatch ? 1 : 0)
								<< " table=" << chordSoundingPeak << '/' << matcherToneCount << ") legacy="
								<< (legacyPitchesMatch ? "accept" : "refuse") << " -> "
								<< (pitchesMatch ? "ACCEPT" : "refuse") << " age=" << std::fixed << std::setprecision(0)
								<< attackAgeMs << "ms" << std::defaultfloat << std::endl);
						}
					}
					const bool isRestrumOfSameChord = lastCommittedChordId >= 0
						&& lastCommittedChordId == selectedChordId && lastCommittedChordRecord != selectedRecord;
					const bool dyadFullyNative = matcherToneCount == 2 && !isUnison && chordSoundingPeak >= 2
						&& nativeAgreedOnStrum && (naturalResult || nativeMatcherMatches);
					if (freshAttack && !fretHandMuted && legacyPitchesMatch && !pitchesMatch
						&& (((isRestrumOfSameChord || matcherToneCount >= 3)
							&& naturalResult && chordSoundingPeak >= (matcherToneCount + 1) / 2)
							|| dyadFullyNative))
					{
						pitchesMatch = true;
						static uint64_t loggedRestrumAttack = 0;
						if (loggedRestrumAttack != chordAttackSample)
						{
							loggedRestrumAttack = chordAttackSample;
							LOG_INFO("(NBN LAS CHORD) " << (isRestrumOfSameChord ? "Re-strum of the same chord"
								: dyadFullyNative ? "Dyad fully confirmed by the game" : "Strummed chord")
								<< " accepted on the strum + game vote: attack=" << chordAttackSample
								<< " table=" << chordSoundingPeak << '/' << matcherToneCount
								<< " chordId=" << selectedChordId << std::endl);
						}
					}
					if (pitchConfirmation == NoteByNote::ChordPitchConfirmation::MlStrings)
					{
						LOG_INFO("(NBN CHORD ML STRINGS) Accepted fresh attack=" << chordAttackSample
							<< " analyzed=" << mlChordEvidence.analyzedSampleIndex
							<< " requiredMask=0x" << std::hex
							<< static_cast<int>(mlChordEvidence.requiredStringMask)
							<< " matchedMask=0x" << static_cast<int>(mlChordEvidence.matchedStringMask)
							<< std::dec << " age=" << std::fixed << std::setprecision(3)
							<< mlChordEvidence.ageSeconds << "s record=0x" << std::hex
							<< selectedRecord << std::dec << std::endl);
					}
					{
						static uint64_t lateLoggedAttack = 0;
						if (freshAttack && nativeMatcherMatches && lateLoggedAttack != chordAttackSample
							&& chordSoundingPeak >= requiredSounding && attackAgeMs > 100.0 && attackAgeMs <= 200.0)
						{
							lateLoggedAttack = chordAttackSample;
							LOG_INFO("(NBN STRUM PRESENCE LATE) native matcher hit at " << std::fixed << std::setprecision(0)
								<< attackAgeMs << " ms, peak " << chordSoundingPeak << '/' << matcherToneCount
								<< "; " << (corroborated ? "ACCEPTED (tones present at the strum)"
									: strumPresence == 0 ? "refused (tones not present at the strum)"
									: "refused (presence not measured)")
								<< " attack=" << chordAttackSample << std::endl);
						}
					}
					if (!fretHandMuted && portRan && freshAttack && !corroborated)
					{
						static ULONGLONG lastUncorroboratedLogTick = 0;
						const ULONGLONG nowTick = GetTickCount64();
						if (nowTick - lastUncorroboratedLogTick >= 500)
						{
							lastUncorroboratedLogTick = nowTick;
							LOG_INFO("(NBN LAS CHORD) Strum refused: native vote " << (naturalResult ? "yes" : "no")
								<< ", peak " << chordSoundingPeak << " of " << matcherToneCount << " tones sounding since the strum (need "
								<< requiredSounding << "), strum " << std::fixed << std::setprecision(0) << attackAgeMs
								<< " ms old." << std::endl);
						}
					}
					const bool attackMatchesTarget = fretHandMuted ? sawSpikeDuringHold : pitchesMatch;
					const bool nativeHit = freshAttack && chordAttacks.TryConsumeForNote(
						attackMatchesTarget, pickScanSample, pickSampleRate);
					if (nativeHit && fretHandMuted)
						LOG_INFO("(NBN FRET MUTE) Fresh attack accepted without pitched-chord matching; sample="
							<< chordAttackSample << " record=0x" << std::hex << record << std::dec << std::endl);
					if (verboseTrace && matcherToneCount > 0)
					{
						const auto obsNow = std::chrono::steady_clock::now();
						const bool dueThrottled = !hasMatcherObserveAnchor
							|| std::chrono::duration<double>(
								obsNow - matcherObserveAnchor).count() >= 0.5;
						if (nativeHit || sawSpikeDuringHold || dueThrottled)
						{
							matcherObserveAnchor = obsNow;
							hasMatcherObserveAnchor = true;
							const int soundingSeen = soundingTargetTones;
							LOG_INFO("(NBN NATIVE HIT) hit=" << (nativeHit ? "YES" : "no")
								<< " tones=" << matcherToneCount
								<< " sounding=" << soundingSeen << "/" << matcherToneCount
								<< " portRan=" << (portRan ? "y" : "n")
								<< " nativeVote=" << (naturalResult ? "true" : "false")
								<< " freshStrum=" << (freshAttack ? "y" : "n")
								<< "." << std::endl);
						}
					}
					if (nativeHit)
					{
						MarkStrumLedgerAccepted(chordAttackSample);
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
						holdProgressAnchor = std::chrono::steady_clock::now();
						mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
						mlBendConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
						enhancedLegatoConfirmation.Reset(latestRawAudioSample);
						consumedRecords.insert(selectedRecord);
						lastCommittedChordRecord = selectedRecord;
						lastCommittedChordId = selectedChordId;
						lastCommittedChordAt = std::chrono::steady_clock::now();
						pickAttackFloorSample = chordAttacks.GetConsumedThroughSample();
						lastChordStrumSample = chordAttackSample;
						{
							using NoteByNote::ChordPitchConfirmation;
							using NoteByNote::DetectorRole;
							chordAcceptFeedback = {};
							chordAcceptFeedback.targetRecord = selectedRecord;
							chordAcceptFeedback.nativeMidi = QueryNativeLoudestPlayedNote();
							chordAcceptFeedback.nativeRole = naturalResult || nativeMatcherMatches
								|| nativeAgreedOnStrum
								|| pitchConfirmation == ChordPitchConfirmation::Natural
								|| pitchConfirmation == ChordPitchConfirmation::NativeMatcher
								? DetectorRole::Confirmed : DetectorRole::Unused;
							chordAcceptFeedback.enhancedRole = fretHandMuted || ledgerDecided
								|| pitchConfirmation == ChordPitchConfirmation::Unison
								|| pitchConfirmation == ChordPitchConfirmation::CloseDyad
								|| pitchConfirmation == ChordPitchConfirmation::Tier0
								|| pitchConfirmation == ChordPitchConfirmation::PowerChord
								? DetectorRole::Confirmed : DetectorRole::Unused;
							chordAcceptFeedback.mlRole = pitchConfirmation == ChordPitchConfirmation::MlStrings
								? DetectorRole::Confirmed : DetectorRole::Unused;
							NoteByNoteNativeScoring::TryDescribeChordLabel(reinterpret_cast<uintptr_t>(note),
								researchChordTonesRecord == selectedRecord ? researchChordTones : nullptr,
								researchChordTonesRecord == selectedRecord ? researchChordToneCount : 0,
								chordAcceptFeedback.chordLabel, sizeof(chordAcceptFeedback.chordLabel));
						}
						char chordIdentity[64] = "?";
						NoteByNoteNativeScoring::TryDescribeChordTarget(
							reinterpret_cast<uintptr_t>(note), chordIdentity, sizeof(chordIdentity));
						LOG_INFO("(NBN LAS CHORD) A raw attack and attack-window chord confirmation accepted the held"
							<< " chord record=0x" << std::hex << selectedRecord << std::dec
							<< " (chordId " << selectedChordId << ", " << chordIdentity
							<< ") - consumed raw strum sample=" << chordAttackSample
							<< (chordAttackOriginSample != chordAttackSample ? " (inherited from the previous note)" : "")
							<< " through=" << chordAttacks.GetConsumedThroughSample()
							<< " after " << chordDecisionEvalCount
							<< " evaluation(s); committing." << std::endl);
						LOG_INFO("(NBN CHORD TIER0 SHADOW) tier0=" << (Tier0ConfirmsChord(playedTones, matcherToneCount) ? "yes" : "no")
							<< " peak=" << chordSoundingPeak << "/" << matcherToneCount
							<< " vote=" << (naturalResult ? "yes" : "no")
							<< " age=" << std::fixed << std::setprecision(0) << attackAgeMs << "ms"
							<< " strumPresence=" << strumPresence
							<< " chord=" << chordIdentity << std::endl);
						chordDecisionEvalCount = 0;
						QueryNativeOnsetNote();
						return true;
					}
					const auto now = std::chrono::steady_clock::now();
					if (!hasChordDecisionLogAnchor
						|| std::chrono::duration<double>(
							now - chordDecisionLogAnchor).count() >= 1.0)
					{
						chordDecisionLogAnchor = now;
						hasChordDecisionLogAnchor = true;
						char chordIdentity[64] = "?";
						NoteByNoteNativeScoring::TryDescribeChordTarget(
							reinterpret_cast<uintptr_t>(note), chordIdentity, sizeof(chordIdentity));
						{
							std::ostringstream sounding;
							for (int index = 0; index < matcherToneCount; ++index)
							{
								const int midi = matcherTones[index];
								const float strength = ReadNdSoundingStrength(midi);
								const float raw = ReadNdRawSoundingStrength(midi);
								if (index != 0) sounding << ' ';
								sounding << midi << ':' << (strength >= 0.0f ? "YES" : "no");
								if (std::isfinite(raw)) sounding << "(raw " << raw << ')';
								else sounding << "(absent)";
							}
							LOG_INFO("(NBN ND CHORD) sounding-table per native template tone: " << sounding.str() << std::endl);
						}
						LOG_INFO("(NBN LAS CHORD) Held chord decision evaluated false ("
							<< chordDecisionEvalCount << " evaluation(s) so far)"
							<< " record=0x" << std::hex << selectedRecord << std::dec
							<< " chordId=" << selectedChordId
							<< " chord=" << chordIdentity
							<< " chordNotesId=" << selectedChordNotesId
							<< " time=" << std::fixed << std::setprecision(3)
							<< selectedRecordTime
							<< "; the native evaluator IS running against the frozen"
							<< " transport." << std::endl);
					}
					return false;
				}
				return false;
			}
			if (gatePhase == GatePhase::CommitBeforeRelease
				|| gatePhase == GatePhase::RecommitAfterRelease
				|| gatePhase == GatePhase::DenseRecommitAfterRebuild)
			{
				bool originalResult = originalHitDecision(owner, unusedEdx, note);
				if (!wasCommitOverrideLogged)
				{
					wasCommitOverrideLogged = true;
					const char* commitName = "post-release recommit";
					if (gatePhase == GatePhase::CommitBeforeRelease)
					{
						commitName = "pre-release commit";
					}
					else if (gatePhase == GatePhase::DenseRecommitAfterRebuild)
					{
						commitName = "dense-rebuild recommit";
					}
					LOG_INFO("(NBN LAS INPUT) Forcing the selected native "
						<< commitName
						<< "; the original decision returned "
						<< std::boolalpha << originalResult << "." << std::endl);
				}
				return true;
			}
			return originalHitDecision(owner, unusedEdx, note);
		}
		return false;
	}
}

void NoteByNoteScoringCore::Initialize()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	if (isInitialized) return;

	isInitialized = true;
	LOG_INFO("(NBN LAS LIFECYCLE) Reloadable Note by Note controller initialized."
		<< " The lesson-derived hold (Stop_TMusic latch + five-clock epoch) and coordinated"
		<< " PlayerSong-packet release are armed but inert until Note by Note is enabled."
		<< std::endl);
	if (!ResearchProbeRuntime::IsRawSnapshotBridgeAvailable())
		LOG_ERROR("(NBN LAS LIFECYCLE) Raw snapshot bridge unavailable: chord, dyad, unison and"
			<< " fret-hand-mute attacks cannot confirm in this controller." << std::endl);
}

bool NoteByNoteScoringCore::IsAvailable()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return isInitialized;
}

void NoteByNoteNativeScoring::SetChordHoldsEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	areChordHoldsEnabled = enabled;
	LOG_INFO("(NBN LAS CHORD) Chord holds " << (enabled ? "enabled" : "disabled")
		<< (enabled
			? ": held chords release on the game's own hit decision."
			: ": chords play through natively while successors stay gated.")
		<< std::endl);
}

void NoteByNoteNativeScoring::SetFreezePromptSoundEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isFreezePromptSoundEnabled = enabled;
	LOG_INFO("(NBN LAS PROMPT) " << FREEZE_NOTE_TRACK_EVENT << " before each hold "
		<< (enabled ? "enabled (the per-note percussion cue is back)"
			: "disabled (default: the per-note percussion cue is silenced)")
		<< std::endl);
}

void NoteByNoteNativeScoring::SetFlowUntilMissEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isFlowUntilMissEnabled = enabled;
	LOG_INFO("(NBN FLOW) Flow-until-miss "
		<< (enabled
			? "ENABLED: the hold boundary sits late (recordTime + grace) so on-time notes"
			  " commit naturally and the transport plays straight through; freeze only on a"
			  " real miss. Dense sections keep their audio."
			: "disabled: the pre-flow freeze-per-note boundary (recordTime - compensation)"
			  " is restored; every note freezes and restarts the transport.")
		<< std::endl);
}

bool NoteByNoteNativeScoring::GetFlowUntilMissEnabled()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return isFlowUntilMissEnabled;
}

void NoteByNoteNativeScoring::SetFlowLateGraceSeconds(float seconds)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	if (seconds < 0.0f) seconds = 0.0f;
	if (seconds > 0.29f) seconds = 0.29f;
	flowLateGraceSeconds = seconds;
	LOG_INFO("(NBN FLOW) Flow late-grace set to " << std::fixed << std::setprecision(3)
		<< flowLateGraceSeconds << "s (how far past a note's time the transport flows"
		<< " before the boundary freezes on it)." << std::endl);
}

float NoteByNoteNativeScoring::GetFlowLateGraceSeconds()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return flowLateGraceSeconds;
}

bool NoteByNoteNativeScoring::GetChordHoldsEnabled()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return areChordHoldsEnabled;
}

void NoteByNoteNativeScoring::SetVerboseTrace(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	verboseTrace = enabled;
	LOG_INFO("(NBN TRACE) Verbose trace " << (enabled ? "ENABLED" : "disabled")
		<< (enabled
			? ": per-tick DETECT/BEND/CHORD-WINDOW/eval-false logs are on for diagnosis."
			: ": high-frequency logs are off; event logs still print. Higher FPS.")
		<< std::endl);
}

bool NoteByNoteNativeScoring::GetVerboseTrace()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return verboseTrace;
}

namespace
{
	const char* DescribeStrategy(NoteByNoteNativeScoring::DetectionStrategy strategy)
	{
		switch (strategy)
		{
			case NoteByNoteNativeScoring::DetectionStrategy::NativeOnly:
				return "NativeOnly (ML shadows only)";
			case NoteByNoteNativeScoring::DetectionStrategy::MlOnly:
				return "MlOnly (ML co-sign + strict veto; native shadows)";
			default:
				return "Blend (native + tier-0 + ML: mutual rescue, ML veto only when confident and not octave-off)";
		}
	}
}

void NoteByNoteNativeScoring::SetChordDetectionStrategy(DetectionStrategy strategy)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	g_chordDetectionStrategy = static_cast<int>(strategy);
	LOG_INFO("(NBN STRATEGY) Chord detection = " << DescribeStrategy(strategy)
		<< " (no ML-primary chord decider yet; ML stays shadow/rescue)" << std::endl);
}

void NoteByNoteNativeScoring::SetNoteDetectionStrategy(DetectionStrategy strategy)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	g_noteDetectionStrategy = static_cast<int>(strategy);
	LOG_INFO("(NBN STRATEGY) Single-note detection = " << DescribeStrategy(strategy) << std::endl);
}

void NoteByNoteNativeScoring::SetBendDetectionStrategy(DetectionStrategy strategy)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	g_bendDetectionStrategy = static_cast<int>(strategy);
	LOG_INFO("(NBN STRATEGY) Bend detection = " << DescribeStrategy(strategy)
		<< " (no ML-primary bend decider yet; ML stays shadow/rescue)" << std::endl);
}

bool NoteByNoteNativeScoring::TryDescribeSelectedChordTarget(uintptr_t record, char* buffer, size_t bufferLength)
{
	if (buffer == nullptr || bufferLength == 0) return false;
	buffer[0] = '\0';
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	if (record == 0 || record != selectedRecord || selectedChordId < 0 || trackedOwner == nullptr) return false;
	LiveNote selected;
	if (!FindSelectedNote(trackedOwner, selected)) return false;
	return TryDescribeChordTarget(selected.note, buffer, bufferLength);
}

namespace
{
	bool TryReadChordTemplateView(uintptr_t noteAddress, ChordTemplateView& view)
	{
		uintptr_t templateAddress = 0;
		return TryRead(noteAddress + NOTE_CHORD_RECORD, templateAddress)
			&& templateAddress != 0
			&& TryRead(templateAddress, view);
	}

	void FormatChordFingering(const ChordTemplateView& view, char* buffer, size_t bufferLength)
	{
		char frets[24] = {};
		size_t at = 0;
		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			const uint8_t fret = view.frets[stringIndex];
			at += std::snprintf(frets + at, sizeof(frets) - at, "%s%s",
				stringIndex == 0 ? "" : "/",
				fret < 0x1A ? std::to_string(fret).c_str() : "x");
			if (at >= sizeof(frets)) break;
		}
		std::snprintf(buffer, bufferLength, "[%s]", frets);
	}
}

bool NoteByNoteNativeScoring::TryDescribeChordTarget(
	uintptr_t noteAddress,
	char* buffer,
	size_t bufferLength)
{
	if (buffer == nullptr || bufferLength == 0) return false;
	buffer[0] = '\0';

	ChordTemplateView view = {};
	if (!TryReadChordTemplateView(noteAddress, view)) return false;
	char name[sizeof(view.name) + 1] = {};
	for (size_t i = 0; i < sizeof(view.name) && view.name[i] != '\0'; ++i)
	{
		if (view.name[i] < 0x20 || view.name[i] > 0x7E) { name[0] = '\0'; break; }
		name[i] = view.name[i];
	}

	char fingering[24] = {};
	FormatChordFingering(view, fingering, sizeof(fingering));

	if (name[0] != '\0')
	{
		std::snprintf(buffer, bufferLength, "%s %s", name, fingering);
	}
	else
	{
		std::snprintf(buffer, bufferLength, "%s", fingering);
	}
	return true;
}

bool NoteByNoteNativeScoring::TryDescribeChordLabel(uintptr_t noteAddress, const int* tones, int toneCount,
	char* buffer, size_t bufferLength)
{
	if (buffer == nullptr || bufferLength == 0) return false;
	buffer[0] = '\0';
	ChordTemplateView view = {};
	if (TryReadChordTemplateView(noteAddress, view))
	{
		char name[sizeof(view.name) + 1] = {};
		for (size_t i = 0; i < sizeof(view.name) && view.name[i] != '\0'; ++i)
		{
			if (view.name[i] < 0x20 || view.name[i] > 0x7E) { name[0] = '\0'; break; }
			name[i] = view.name[i];
		}
		const char* at = name;
		while (*at == ' ') ++at;
		if (*at >= 'A' && *at <= 'G')
		{
			char root[3] = { *at, '\0', '\0' };
			if (at[1] == '#' || at[1] == 'b') root[1] = at[1];
			std::snprintf(buffer, bufferLength, "%s", root);
			return true;
		}
	}
	if (tones == nullptr || toneCount <= 0) return false;
	int lowest = -1;
	for (int i = 0; i < (std::min)(toneCount, 6); ++i)
		if (tones[i] >= 0 && tones[i] <= 127 && (lowest < 0 || tones[i] < lowest)) lowest = tones[i];
	if (lowest < 0) return false;
	std::snprintf(buffer, bufferLength, "%s", NoteByNote::FormatPitch(lowest).c_str());
	return true;
}

bool NoteByNoteNativeScoring::IsBassArrangementActive()
{
	return IsBassArrangement();
}

bool NoteByNoteNativeScoring::TryDescribeSelectedChordFingering(
	uintptr_t record,
	char* buffer,
	size_t bufferLength,
	int& lowestPlayedString)
{
	lowestPlayedString = -1;
	if (buffer == nullptr || bufferLength == 0) return false;
	buffer[0] = '\0';
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	if (record == 0 || record != selectedRecord || selectedChordId < 0 || trackedOwner == nullptr) return false;
	LiveNote selected;
	if (!FindSelectedNote(trackedOwner, selected)) return false;

	ChordTemplateView view = {};
	if (!TryReadChordTemplateView(selected.note, view)) return false;
	FormatChordFingering(view, buffer, bufferLength);
	for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
	{
		if (view.frets[stringIndex] >= 0x1A) continue;
		lowestPlayedString = stringIndex;
		break;
	}
	return true;
}

void NoteByNoteNativeScoring::SetNativeChordPanelEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isNativeChordPanelEnabled = enabled;
	if (!enabled) HideNativeChordPanel();
	LOG_INFO("(NBN LAS CHORD PANEL) Native chord display driving "
		<< (enabled ? "enabled" : "disabled") << "." << std::endl);
}

void NoteByNoteNativeScoring::SetScheduleShiftEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isScheduleShiftEnabled = enabled;
	LOG_INFO("(NBN LAS SHIFT) Native schedule-shift-at-release "
		<< (enabled ? "enabled: each release compensates the engine for the frozen span (entry 4, op add)."
			: "disabled.")
		<< std::endl);
}

void NoteByNoteNativeScoring::SetNdAcceptEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isNdAcceptEnabled = enabled;
	hasNdSoundingStreak = false;
	LOG_INFO("(NBN ND) Native sounding-table acceptance "
		<< (enabled ? "enabled: expected pitch above the native threshold for 0.18s accepts."
			: "disabled: acceptance falls back to the heuristic stack alone.")
		<< std::endl);
}

void NoteByNoteNativeScoring::SetNativeReleaseEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isNativeReleaseEnabled = enabled;
	LOG_INFO("(NBN NATIVE RELEASE) Native release "
		<< (enabled ? "enabled (hybrid): owned releases run the StartAt core (unfreeze +"
			" seek-to-now + speed reset) and then the coordinated PlayerSong restart"
			" for the music."
			: "disabled: owned releases use the coordinated PlayerSong restart alone.")
		<< std::endl);
}

void NoteByNoteNativeScoring::SetSafetyReleaseEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isSafetyReleaseEnabled = enabled;
	LOG_INFO("(NBN LAS SAFETY) Timed safety release "
		<< (enabled ? "enabled: a no-progress hold releases after its budget."
			: "disabled: a no-progress hold stays held and logs its refusing state; N releases it.")
		<< std::endl);
}

bool NoteByNoteNativeScoring::GetNativeChordPanelEnabled()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return isNativeChordPanelEnabled;
}

void NoteByNoteNativeScoring::SetNativeFreezeFlagEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isNativeFreezeFlagEnabled = enabled;
	LOG_INFO("(NBN LAS FREEZE FLAG) Native frozen-on-tag flag writes "
		<< (enabled ? "enabled" : "disabled")
		<< ": owner+0x5E3 " << (enabled ? "announces every owned hold" : "is left native")
		<< "." << std::endl);
}

bool NoteByNoteNativeScoring::GetNativeFreezeFlagEnabled()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return isNativeFreezeFlagEnabled;
}

void NoteByNoteNativeScoring::SetRepeatStrumHoldsEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	areRepeatStrumHoldsEnabled = enabled;
	LOG_INFO("(NBN LAS CHORD) Repeat-strum holds " << (enabled ? "enabled" : "disabled")
		<< (enabled
			? ": bare-0x2 repeat records hold and release like full chords."
			: ": bare-0x2 repeat records play through natively; skipped strums count as missed.")
		<< std::endl);
}

bool NoteByNoteNativeScoring::GetRepeatStrumHoldsEnabled()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return areRepeatStrumHoldsEnabled;
}

void NoteByNoteNativeScoring::SetChordWindowSlideEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isChordWindowSlideEnabled = enabled;
	LOG_INFO("(NBN LAS CHORD WINDOW) Window slide " << (enabled ? "enabled" : "disabled")
		<< (enabled
			? ": a held chord whose detection window the detector clock has left is"
			  " evaluated with the window slid onto the clock, authored width preserved."
			: ": held chords are evaluated against their authored windows only, so"
			  " out-of-window refusals are measured rather than repaired.")
		<< std::endl);
}

void NoteByNoteNativeScoring::SetChordTier0RescueEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isChordTier0RescueEnabled = enabled;
	LOG_INFO("(NBN CHORD TIER0) Chord tier-0 rescue " << (enabled ? "ENABLED" : "disabled")
		<< (enabled
			? ": a fresh-strummed chord the native scan + ND both miss is accepted when every"
			  " expected tone is energy-confirmed (per-tone raw dominance)."
			: ": chords rely on the native scan + ND only.")
		<< std::endl);
}

bool NoteByNoteNativeScoring::GetChordTier0RescueEnabled()
{
	return isChordTier0RescueEnabled;
}

bool NoteByNoteNativeScoring::GetChordWindowSlideEnabled()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return isChordWindowSlideEnabled;
}

void NoteByNoteNativeScoring::Shutdown()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	ClearSelection();
	ResetBootstrap("the reloadable controller is shutting down");
	originalScoringUpdate = nullptr;
	originalHitDecision = nullptr;
	isInitialized = false;
}

void NoteByNoteNativeScoring::ProcessScoringUpdate(
	void* owner,
	float updateTime,
	ResearchProtocol::ScoringUpdate original)
{
	if (original == nullptr) return;
	originalScoringUpdate = reinterpret_cast<ScoringUpdateFn>(original);
	ScoringUpdateDetour(owner, updateTime);
}

bool NoteByNoteNativeScoring::ProcessHitDecision(
	void* owner,
	void* unusedEdx,
	void* note,
	ResearchProtocol::HitDecision original)
{
	if (original == nullptr) return false;
	originalHitDecision = reinterpret_cast<HitDecisionFn>(original);
	return HitDecisionDetour(owner, unusedEdx, note);
}

ResearchProtocol::NoteByNoteState NoteByNoteNativeScoring::GetResearchState()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	ResearchProtocol::NoteByNoteState state;
	state.isInitialized = isInitialized ? 1 : 0;
	state.isEpochConfirmed = isEpochConfirmed ? 1 : 0;
	state.ownsNativeHold = OwnsNativeHold() ? 1 : 0;
	state.gatePhase = static_cast<ResearchProtocol::GatePhase>(gatePhase);
	state.trackedOwner = reinterpret_cast<uintptr_t>(trackedOwner);
	state.selectedRecord = selectedRecord;
	state.epoch = epochIndex;
	state.holdTickCount = holdTickCount;
	state.visualGroupCount = visualGroupCount;
	for (uint32_t i = 0; i < visualGroupCount
		&& i < ResearchProtocol::NoteByNoteState::MaxVisualGroup; ++i)
	{
		state.visualGroupRecords[i] = visualGroupRecords[i];
		state.visualGroupStrings[i] = visualGroupStrings[i];
		state.visualGroupFrets[i] = visualGroupFrets[i];
	}
	state.lastUpdateTime = lastUpdateTime;
	state.selectedRecordTime = selectedRecordTime;
	state.selectedHoldTime = selectedHoldTime;
	state.heldEpoch = heldEpoch;
	state.selectedString = selectedString;
	state.selectedFret = selectedFret;
	state.selectedChordId = selectedChordId;
	state.selectedChordNotesId = selectedChordNotesId;
	state.expectedMidi = JudgeMidi();
	RefreshDetectionFeedbackMl();
	state.detectionFeedback = detectionFeedback;
	if (selectedChordId >= 0 && researchChordToneCount > 0
		&& researchChordTonesRecord == selectedRecord)
	{
		state.expectedChordToneCount = (std::min)(static_cast<uint32_t>(researchChordToneCount),
			ResearchProtocol::NoteByNoteState::MaxChordTones);
		for (uint32_t i = 0; i < state.expectedChordToneCount; ++i)
			state.expectedChordTones[i] = researchChordTones[i];
	}
	state.isBendTarget = isBendTarget ? 1 : 0;
	state.bendAcceptMidi = bendAcceptMidi;

	if (denseSuccessor.record != 0)
	{
		state.visualRecord = denseSuccessor.record;
		state.visualString = denseSuccessor.stringIndex;
		state.visualFret = denseSuccessor.fret;
		state.visualChordId = denseSuccessor.chordId;
		state.visualChordNotesId = denseSuccessor.chordNotesId;
	}
	else
	{
		state.visualRecord = selectedRecord;
		state.visualString = selectedString;
		state.visualFret = selectedFret;
		state.visualChordId = selectedChordId;
		state.visualChordNotesId = selectedChordNotesId;
	}
	{
		static std::chrono::steady_clock::time_point bendFeedLogAnchor;
		static bool hasBendFeedLogAnchor = false;
		const auto now = std::chrono::steady_clock::now();
		if (verboseTrace && NbnInputPresent()
			&& (!hasBendFeedLogAnchor
				|| std::chrono::duration<double>(now - bendFeedLogAnchor).count() >= 2.0))
		{
			bendFeedLogAnchor = now;
			hasBendFeedLogAnchor = true;
			LOG_INFO("(NBN BEND FEED) isBendTarget=" << isBendTarget
				<< " isBendRunConfirmation=" << isBendRunConfirmation
				<< " expectedMidi=" << expectedMidi
				<< " bendAcceptMidi=" << bendAcceptMidi
				<< " phase=" << static_cast<int>(gatePhase) << std::endl);
		}
	}
	if (hasLastStateGateSample
		&& std::isfinite(lastStateGateSample.level)
		&& std::chrono::duration<double>(
			std::chrono::steady_clock::now() - lastStateGateSampleAt).count() < 2.0)
	{
		state.detectorLevelDb = lastStateGateSample.level;
		state.detectorQuality = std::isfinite(lastStateGateSample.quality)
			? lastStateGateSample.quality
			: 0.0f;
		state.detectorLoudestMidi = lastStateGateSample.currentNote;
		state.detectorSampleValid = 1;
		state.detectorPassesLevel = lastStateGateSample.passesLevel ? 1 : 0;
		strncpy_s(state.holdRefusal, DescribeDetectorRefusal(lastStateGateSample), _TRUNCATE);
	}
	strncpy_s(state.holdPhase, DescribeGatePhase(gatePhase), _TRUNCATE);
	state.bendBaseMidi = bendVisualizationSnapshot.baseMidi;
	state.bendTargetMidi = bendVisualizationSnapshot.targetMidi;
	state.soundingMidi = bendVisualizationSnapshot.soundingMidi;
	state.soundingQuality = bendVisualizationSnapshot.soundingQuality;
	state.bendReachedTarget = bendVisualReached
		&& selectedRecord == bendVisualReachedRecord
		&& epochIndex == bendVisualReachedEpoch ? 1 : 0;
	state.chordAuthorityIsMl = g_chordDetectionStrategy != static_cast<int>(DetectionStrategy::NativeOnly) ? 1 : 0;
	state.noteAuthorityIsMl = g_noteDetectionStrategy != static_cast<int>(DetectionStrategy::NativeOnly) ? 1 : 0;
	state.bendAuthorityIsMl = g_bendDetectionStrategy != static_cast<int>(DetectionStrategy::NativeOnly) ? 1 : 0;
	{
		const DetectionComparison& c = g_detectionComparison;
		state.compareAgreeCount = c.agree;
		state.compareDisagreeCount = c.disagree;
		state.compareLastTechnique = c.lastTechnique;
		state.compareLastNativeMatch = c.lastNativeMatch ? 1 : 0;
		state.compareLastMlMatch = c.lastMlMatch ? 1 : 0;
		state.compareLastMlHadOpinion = c.lastMlHadOpinion ? 1 : 0;
		state.compareLastValid = c.lastValid ? 1 : 0;
		state.compareHistoryCount = (std::min)(c.historyCount,
			ResearchProtocol::NoteByNoteState::CompareHistoryLength);
		for (uint32_t i = 0; i < state.compareHistoryCount; ++i)
			state.compareHistory[i] = c.history[i];
	}
	return state;
}

void NoteByNoteScoringCore::ObserveRenderedAttack(
	const NoteByNoteProbe::NativeRenderedAttack& attack)
{
	std::unique_lock<std::recursive_mutex> lock(controllerMutex, std::try_to_lock);
	if (!lock.owns_lock()) return;
	if (!OwnsNativeHold()) return;

	++renderFramesWhileHeld;
	if (renderFramesWhileHeld <= 3 || renderFramesWhileHeld % 60 == 0)
	{
		std::ostringstream notes;
		for (size_t index = 0; index < attack.notes.size(); ++index)
		{
			if (index != 0) notes << ',';
			notes << attack.notes[index].stringIndex << ':' << attack.notes[index].fret;
		}
		LOG_INFO("(NBN LAS RENDER) heldFrame=" << renderFramesWhileHeld
			<< " songTime=" << std::fixed << std::setprecision(6) << attack.songTime
			<< " incomingFront=" << notes.str()
			<< " x=" << attack.longitudinalPosition << "." << std::endl);
	}
}

void NoteByNoteScoringCore::Stop()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	detectionFeedback = {};
	mlRescueRecord = 0;
	enhancedRescueFeedback = {};
	if (OwnsNativeHold())
	{
		isReleaseRequested = true;
		LOG_INFO("(NBN LAS STOP) A native hold is owned; its coordinated release is queued to the"
			<< " next scoring tick, which continues to run while the game is held." << std::endl);
		return;
	}
	ClearSelection();
}

void NoteByNoteScoringCore::RequestReArm()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	reArmRequested = true;
	LOG_INFO("(NBN LAS BOOTSTRAP) Re-arm requested on enable; the next scoring tick"
		<< " re-bootstraps for the current section." << std::endl);
}
