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

// Every line this TU logs is mirrored, flushed per line, into a file next to the
// loaded probe, because the console scrollback is flooded by per-frame diagnostics
// within seconds. Timestamps are seconds since this DLL loaded, so a reload starts a
// new epoch (the file is appended, never truncated).
namespace
{
	void AppendNbnTraceLine(const std::string& text)
	{
#if defined(RSMODS_PUBLIC_RELEASE)
		// Master (what users get) writes no trace: nothing reads it, it would grow without bound, and
		// from xinput1_3.dll at the game root the path below lands one folder above the game.
		(void)text;
		return;
#endif
		static std::mutex traceMutex;
		std::lock_guard<std::mutex> lock(traceMutex);
		static const auto traceEpoch = std::chrono::steady_clock::now();
		static std::ofstream trace = []()
		{
			// The probe loads from <game>\RSModsResearch\Loaded\, so the trace sits
			// in <game>\RSModsResearch\ regardless of the process working directory.
			char modulePath[MAX_PATH] = {};
			HMODULE module = nullptr;
			std::string path = "RSModsResearch\\nbn-trace.log";
			if (GetModuleHandleExA(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
						| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(&AppendNbnTraceLine), &module)
				&& GetModuleFileNameA(module, modulePath, MAX_PATH) != 0)
			{
				std::string full(modulePath);
				const auto lastSlash = full.find_last_of('\\');
				if (lastSlash != std::string::npos)
				{
					const auto parentSlash = full.find_last_of('\\', lastSlash - 1);
					if (parentSlash != std::string::npos)
					{
						path = full.substr(0, parentSlash + 1) + "nbn-trace.log";
					}
				}
			}
			return std::ofstream(path, std::ios::app);
		}();
		if (!trace.is_open()) return;
		const double seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - traceEpoch).count();
		trace << std::fixed << std::setprecision(3) << seconds << ' ' << text;
		if (text.empty() || text.back() != '\n') trace << '\n';
		trace.flush();
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

// Authoritative Learn A Song Note by Note controller.
//
// Applies the Guided Experience lesson hold to the GamePlaysongLAS owner:
//
// - The lesson tick and the LAS tick are near-identical and neither contains a freeze
//   gate. The lesson hold works because FreezeOnTag core 0x474A80 performs exactly two
//   operations against shared state: PlayerSong Stop_TMusic (vtable +0x24, 0x44C130)
//   and the owner's five-clock setter (vtable +0x50) at the frozen epoch.
// - Stop_TMusic writes the 16-bit value 0x100 at PlayerSong +0xD9: it clears the
//   publication gate +0xD9 AND sets +0xDA. The PlayerSong component tick 0x44C7F0,
//   the only native re-publisher of +0xD9, exits immediately while +0xDA is nonzero.
//   That is why Stop latches the hold and Pause_TMusic does not.
// - While held, the tick keeps running at the frozen time: slot +0x94 re-publishes the
//   held owner clock to the render-facing timer each frame and native scoring 0x7E2880
//   keeps evaluating the selected note's own hit decision 0x7E2640. Input detection
//   therefore stays live during the hold, as in the shipped lesson.
// - Release mirrors ResumeFromTag minus the lesson-only tag seek: mark active scoring
//   notes through 0x7E8920, then invoke coordinated slot +0x54 at the requested epoch. That
//   slot rebuilds the timeline bounds, calls slot +0x50, and sends PlayerSong the same
//   native play packet (0x44BED0) that every Riff Repeater loop restart uses to restart
//   music. The lesson-only speed restore 0x472A20 is intentionally not called because
//   this controller never modifies the native rate or the +0x348 component flag.
// - Dense successors rebuild directly at the next held epoch and wait for that play packet
//   to refresh PlayerSong's transferred fretboard target. Only then is Stop_TMusic
//   re-latched at the same epoch. Advancing the five owner clocks alone moves incoming
//   note geometry but leaves the stopped PlayerSong target on the committed prior note.
// - The hold is late-only: native scoring gets the full approach and can commit an
//   on-time note without transport intervention. Only an unresolved note that reaches
//   record time minus the engine compensation is stopped, and the current authoritative
//   update time becomes the held epoch so establishing the hold does not seek backwards.

namespace
{
	// Native addresses (non-ASLR executable).
	constexpr uintptr_t LAS_OWNER_VTABLE = 0x11D1430;      // GamePlaysongLAS
	constexpr uintptr_t PLAYER_SONG_VTABLE = 0x119F668;    // GameComponentPlayerSong
	constexpr uintptr_t NATIVE_HIT_DECISION = 0x7E2640;    // owner vtable +0xF0
	constexpr uintptr_t STOP_TMUSIC = 0x44C130;            // PlayerSong vtable +0x24
	constexpr uintptr_t SET_FIVE_CLOCKS = 0x7DE490;        // owner vtable +0x50
	constexpr uintptr_t COORDINATED_REBUILD = 0x7E87F0;    // owner vtable +0x54
	constexpr uintptr_t MARK_ACTIVE_NOTES = 0x7E8920;      // owner in EDX, plain ret
	constexpr uintptr_t PLAY_SOUND_CSTRING = 0x7CE8D0;     // event name in ESI
	constexpr uintptr_t ENGINE_COMPENSATION = 0x1224A20;   // double, 0.053 engine constant
	// GamePlaysongLAS stores Rocksmith's selected Riff Repeater bounds twice. Native
	// range writers 0x7E87F0/0x7E9130 populate both pairs from the selected phrases.
	// These move: slot +0x54 (COORDINATED_REBUILD) recomputes them as
	// [min(heldEpoch, ref), max(heldEpoch, ref)] on every owned restart, so +0x3C0
	// (start) drifts to the held epoch while +0x3C8 (end=max=ref) stays pinned to the
	// section's authored end. The stable source is the phrase-section grid below.
	constexpr uintptr_t LAS_SECTION_START = 0x3C0;
	constexpr uintptr_t LAS_SECTION_START_MIRROR = 0x3C4;
	constexpr uintptr_t LAS_SECTION_END = 0x3C8;
	constexpr uintptr_t LAS_SECTION_END_MIRROR = 0x3CC;

	// The stable authored phrase-section grid: Rocksmith's own Riff Repeater section
	// list, loaded once per song and never rewritten during play (unlike +0x3C0/+0x3C8).
	// owner+0x78 -> a container whose std::vector spans [+0xF4 begin, +0xF8 end), stride
	// 0x58; each entry carries authored start at +0x24 and end at +0x28 in absolute
	// seconds, as a half-open chain (end[i] == start[i+1]). The owner+0x78 root matches
	// the _DAT_0135F54C global chain (not the lesson-mode chord-display chain).
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

	// Rocksmith's own un-windowed onset detector, the shipped lesson's wait-for-note
	// query (NDGetOnsetNote core). The windowed per-note decision cannot release a long
	// hold because onset timestamps live on the input-stream clock, which keeps
	// advancing while the song clock is frozen.
	//
	// It dedupes on the returned value against a single global, 0x012F6920:
	// `if (note == last) return -1; last = note;`. The note itself is read fresh from the
	// analysis ring's current frame on every call, and is only reported when that frame's
	// pitch equals the previous frame's (the "settled fundamental" requirement). Two
	// consequences: a pitch that is already the last reported value is invisible until
	// some different value passes through, and the call is not read-only, because it
	// writes that global. Do not call it for diagnostics.
	//
	// Nothing else in the game touches 0x012F6920, and the only native caller of this core
	// is the Lua thunk, so during Learn a Song the controller owns the dedupe state outright.
	constexpr uintptr_t ONSET_NOTE_QUERY = 0x48DC40;
	// NDGetLoudestPlayedNote core, arrangement index in EAX.
	//
	// It returns detector+0x5F4 when two amplitude gates pass, and -1 otherwise. -1 is
	// therefore **not** "no valid note is sounding": it is "the engine's gates refused", and
	// detector+0x5F4 can still hold a valid note when that happens. The onset query is gated
	// on exactly the same two comparisons, which is why both queries go quiet together.
	constexpr uintptr_t LOUDEST_PLAYED_NOTE_QUERY = 0x48E5F0;
	// Per-string tuning offsets (int16[6]) used by the NDGetMidi core 0x48DC00,
	// combined with the standard guitar string bases it hardcodes.
	constexpr uintptr_t TUNING_OFFSETS = 0x1199D2C;

	// Rocksmith's continuous pitch trackers, the input behind the shipped bend lessons.
	//
	// Resolved through Technique_MotionNote_IsLocated (0x00404E10) and _GetLocation
	// (0x00404E70). They report a fractional MIDI pitch, which is why they matter here:
	// the loudest-played-note query is an integer, so a bend sitting at 67.9 reads as a
	// whole semitone away from its target and only registers once it overshoots. The idle
	// value is the -1.0f sentinel the native failure path loads. They are not lesson-only
	// machinery and work in ordinary Learn a Song play, so nothing has to be registered,
	// frozen or resumed to read them.
	//
	// Read-only. Nothing here calls into the game or writes to its memory.
	constexpr uintptr_t MOTION_NOTE_SINGLETON = 0x0135F57C;
	constexpr uintptr_t MOTION_NOTE_SINGLETON_STEP = 0x10;
	constexpr uintptr_t MOTION_NOTE_CONTAINER_STEP = 0x04;
	constexpr uintptr_t MOTION_NOTE_ARRAY_POINTER = 0x1284;
	constexpr uintptr_t MOTION_NOTE_ARRAY_COUNT = 0x1288;
	constexpr uintptr_t MOTION_NOTE_RECORD_STRIDE = 0x50;
	constexpr uintptr_t MOTION_NOTE_RECORD_PITCH = 0x28;
	constexpr uintptr_t MOTION_NOTE_RECORD_ACTIVE = 0x3C;
	constexpr uintptr_t MOTION_NOTE_RECORD_LOCATED = 0x3D;
	// The count is read rather than assumed; this only bounds the loop against a corrupt read.
	constexpr uint32_t MOTION_NOTE_MAX_RECORDS = 64;
	// Ubisoft's acceptance band is defined in eseratingfunctionbends.lua: sUnderbend and
	// sVariance default to 0.5 each and the target counts as hit when
	// bend_pitch_to_hit - sUnderbend <= pitch < + sVariance.
	// Tighter than the base game on purpose: 0.5 under a 2-semitone bend accepts a
	// ~1.5-semitone half-bend. 0.3 still forgives the top wobble (a real bend rests
	// 0-0.2 under) but rejects a bend that never reached pitch.
	constexpr float BEND_UNDERBEND_SEMITONES = 0.3f;
	constexpr float BEND_VARIANCE_SEMITONES = 0.5f;
	// Overbend allowance. The lesson's symmetric 0.5 band is a teaching band where
	// overbending is an error. In song play a player who bends past the target has
	// reached it, so overshoot up to this bound counts as reached (both in the tracker
	// search radius and the accept ceiling); a genuinely different note two or more
	// semitones up stays excluded.
	constexpr float BEND_OVERBEND_ALLOWANCE_SEMITONES = 1.5f;
	// Error allowance for the raw-tap fractional veto. The veto refuses an integer-path
	// reach when the raw estimate reads below target - BEND_UNDERBEND_SEMITONES; the
	// estimate of a still-moving pitch trails the string slightly, so without this
	// allowance a bend resting ~0.2-0.3 under (a real, reached bend) is vetoed until
	// overbent. A genuine half-way bend still sits far below target - 0.45 and stays refused.
	constexpr float BEND_VETO_ESTIMATOR_SLACK_SEMITONES = 0.15f;

	// Rocksmith's note-detection engine.
	//
	// Lets a stalled hold say which condition refused it. Both native input queries are
	// refused by the same two amplitude gates before either looks at a pitch, and neither
	// reports which one failed: they both return -1. Without this, nothing distinguishes
	// "the player is not playing", "the signal is below the engine's noise floor", "the
	// pitch has not settled across two analysis frames" and "this value was already consumed".
	//
	// The whole chain is plain memory reachable from the same root as the motion-note
	// trackers, so it is read through the guarded TryRead and costs no native call. The
	// onset query is not a read-only observation (it writes the dedupe global 0x012F6920),
	// so an extra diagnostic call to it would consume the edge the controller is waiting
	// for. Nothing below calls it.
	//
	// The trackers live at arrangement + 0x04 and the detection engine at + 0x08, two
	// fields of one arrangement object.
	constexpr uintptr_t DETECTION_ROOT = 0x0135F57C;
	constexpr uintptr_t DETECTION_ARRANGEMENT_GUITAR = 0x10;   // Player 1, guitar and bass; +0x14 = player 2
	constexpr uintptr_t DETECTION_ENGINE = 0x08;
	constexpr uintptr_t DETECTION_DETECTOR = 0x04;
	constexpr uintptr_t DETECTOR_CURRENT_NOTE = 0x5F4;         // what 0x48E5F0 returns
	// The double the window leaves (0x4E5DB0 and 0x4E5B80) gate every note-window query
	// on: windowStart <= clock <= windowEnd or refuse without reading input. Not the
	// transport clock.
	constexpr uintptr_t DETECTOR_ANALYSIS_CLOCK = 0xD08;
	constexpr uintptr_t DETECTOR_GATE_QUALITY = 0xD38;
	constexpr uintptr_t DETECTOR_RING_BUFFER = 0xDB8;
	constexpr uintptr_t DETECTOR_RING_INDEX = 0xDBC;
	constexpr uintptr_t DETECTOR_RING_CAPACITY = 0xDC0;
	// Live count of valid ring frames (state+0xDC8) and each frame's own capture timestamp
	// (frame+0x730, double seconds). The native onset-window scan (FUN_004E6A70/004E4B60)
	// bounds its walk by these live timestamps, not by the frozen detector clock, which is
	// why it keeps working while the transport is frozen.
	constexpr uintptr_t DETECTOR_RING_VALID_COUNT = 0xDC8;
	constexpr uintptr_t RING_FRAME_TIMESTAMP = 0x730;
	constexpr uintptr_t DETECTOR_GATE_LEVEL = 0xDD8;
	constexpr uintptr_t DETECTOR_PITCH_MODE = 0x11EC;
	constexpr uintptr_t DETECTOR_RING_STRIDE = 0x7D0;
	constexpr uintptr_t RING_FRAME_PITCH_MODE_TWO = 0x0C;
	constexpr uintptr_t RING_FRAME_PITCH_DEFAULT = 0x14;
	constexpr uintptr_t RING_FRAME_SEQUENCE = 0x748;
	// Private to 0x48DC40: the only two instructions that touch it are its own compare and
	// store, so during Learn a Song the controller is its sole owner.
	constexpr uintptr_t ONSET_DEDUPE_GLOBAL = 0x012F6920;
	// The gate thresholds are read from the game rather than hardcoded, so the log shows
	// what the engine is actually comparing against. These defaults are used only if the
	// read fails.
	constexpr uintptr_t DETECTOR_GATE_LEVEL_THRESHOLD = 0x01224418;
	constexpr uintptr_t DETECTOR_GATE_QUALITY_THRESHOLD = 0x012243A0;
	constexpr double DETECTOR_GATE_LEVEL_FALLBACK = -55.0;
	constexpr double DETECTOR_GATE_QUALITY_FALLBACK = 50.0;
	// Only bounds the ring indexing against a corrupt read; the capacity is read, not assumed.
	constexpr int32_t DETECTOR_RING_MAX_CAPACITY = 4096;
	// The native "is this note being played" seam, the lesson engine's own rating
	// primitive (NDIsNoteBeingPlayed -> 0x48DE10 -> 0x4E9590 -> 0x4E6380).
	// The detector keeps a current-sounding table: count at +0x6A4, entries at
	// +0x604 with stride 8 {int32 midi, float strength}; the native predicate
	// is "expected midi present with strength above the float threshold at
	// 0x01199DD4" (5.0). [arr+0x04] == [[arr+0x08]+0x04] is the same detector
	// resolved elsewhere, and dead-input garbage (MIDI 88-92) sits at strength ~2.3,
	// below the bar. The scan is replicated instead of calling the Lua-adjacent
	// wrapper. The duration gate is the lesson recipe's ~0.18s sustained-sounding
	// requirement.
	constexpr uintptr_t DETECTOR_SOUNDING_TABLE = 0x604;
	constexpr uintptr_t DETECTOR_SOUNDING_COUNT = 0x6A4;
	constexpr uintptr_t ND_STRENGTH_THRESHOLD_GLOBAL = 0x01199DD4;
	constexpr float ND_STRENGTH_THRESHOLD_FALLBACK = 5.0f;
	constexpr double ND_SOUNDING_HOLD_SECONDS = 0.18;
	constexpr int32_t ND_SOUNDING_MAX_ENTRIES = 64;
	// Enabled by default; the toggle is an emergency revert (nd-accept-off).
	volatile bool isNdAcceptEnabled = true;

	// A stalled hold reports its detector state at this cadence. Wall-clock rather than
	// tick-counted, so the cadence itself is not frame-coupled.
	constexpr double DETECTOR_SAMPLE_INTERVAL_SECONDS = 1.0;
	// A pick attack jumps past this between two consecutive scoring ticks; the natural
	// decay moves ~1 dB/s, so even this low threshold is two orders of magnitude above
	// the tick-to-tick decay. Kept low because a re-pick over a string already ringing
	// at -20 dB adds only a few dB.
	constexpr float DETECTOR_SPIKE_JUMP_DB = 2.5f;
	// Post-spike burst length: at 30-60 ticks/s this records roughly a quarter second
	// of the attack transient.
	constexpr int32_t DETECTOR_SPIKE_BURST_TICKS = 12;
	// Bend-release cascade guard: for this long after a bend commits, a follower must show
	// attack energy (a spike) to accept an onset. A bend release rings the string down through
	// every follower's pitch and the onset detector fires on the loud decaying ring, skipping
	// the next notes with no pluck. A decaying release never spikes the meter; a real repluck
	// does. Covers the release decay; a genuinely fast repluck still spikes and passes.
	constexpr double BEND_RELEASE_GUARD_SECONDS = 0.4;
	// Onset-frame attack evidence for chord holds (see the cursor state further down).
	// The level floor rejects dead-input garbage onset blips (spikes at -80..-88 dB with
	// loudest reading MIDI 86-95); real strum frames sit at -52 dB or louder.
	// The frame minimum keeps the previous target's committing strum (whose attack
	// spans a couple of ring frames around the relatch) from counting as evidence
	// for this hold: ~10 frames is ~100 ms at ~95 ring frames/s.
	constexpr float ONSET_EVIDENCE_LEVEL_FLOOR_DB = -65.0f;
	constexpr uint32_t ONSET_EVIDENCE_MIN_FRAMES = 10;
	// Plain picks use the raw-audio attack stream.
	// Other techniques retain their four-frame comparison. An onset flag without
	// a measurable rise is insufficient: the detector also flags ringing strings.
	constexpr int32_t ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES = 4;
	constexpr float ONSET_EVIDENCE_RISE_DB = 1.5f;
	// Bound per-tick scan work when the cursor falls far behind (a hitch between
	// scoring ticks); the newest frames are the ones trusted.
	constexpr int32_t ONSET_SCAN_MAX_FRAMES_PER_TICK = 120;
	// Raw-detector bend acceptance. While the transport is frozen the motion-tracker
	// container never holds an active record (registration is transport-driven), and
	// the gated queries return -1 for a bending pitch because a continuously moving
	// pitch rarely reaches the 50-point quality gate. The ungated current-note field
	// reads the bend target at moderate quality (~37) when the bend arrives, while the
	// quality-0 noise floor reads arbitrary pitches. The floor sits between those two
	// regimes, and the streak requires the target to persist across consecutive ticks
	// so one noise sample cannot accept.
	constexpr float DETECTOR_RAW_BEND_QUALITY_FLOOR = 15.0f;
	constexpr int32_t DETECTOR_RAW_BEND_STREAK_TICKS = 3;
	// A credible raw read this strong is accepted on the spot rather than waiting for the
	// streak, because a 1-semitone half bend only flashes its target briefly at high quality
	// and the streak rarely sustains through the wobble. The bend-release guard still
	// excludes a decaying ring, so this cannot cascade.
	constexpr float DETECTOR_RAW_BEND_STRONG_QUALITY = 70.0f;
	// Minimum consecutive on-target ticks before either instant-accept path (the strong-green
	// Holding accept and the rising input-release accept) may commit a bend. A single momentary
	// read inside the target band (detector noise, a harmonic, a brief overshoot-and-back on a
	// partial bend) does not count. Small, so a genuine full bend still feels immediate while a
	// flash-through is rejected. The >= DETECTOR_RAW_BEND_STREAK_TICKS fallback is unchanged.
	constexpr int32_t BEND_ACCEPT_MIN_HOLD_TICKS = 2;
	// Spike re-attack acceptance. A pick can produce a clear level spike and many
	// consecutive high-quality ticks with the raw current-note reading exactly the
	// expected pitch while the native onset never fires (its internal frame comparison
	// depends on the ring pitch fields, which can read unrelated values through the
	// attack). So the pick is accepted from what is visible: a level spike opens a short
	// window, and inside it the raw pitch must equal the expected note with both native
	// gates passing on consecutive ticks. A decaying ring alone can never commit, because
	// it never jumps.
	constexpr int32_t REATTACK_WINDOW_TICKS = 15;
	constexpr int32_t REATTACK_STREAK_TICKS = 3;

	// ML rescue master switch. Veto-only (false) breaks legitimate low or held notes
	// (e.g. low E fret 1) that rely on ML stuck-rescue to latch.
	volatile bool g_mlRescueEnabled = true;

	// ML can confirm a held note from distinct post-target audio observations.
	volatile bool isMlStuckRescueEnabled = true;
	constexpr float ML_STUCK_RESCUE_CONF = 0.45f;
	// Bends want a longer held streak and tier-0 only (no ML): the companion's 1.2s / ~5 Hz
	// window smears a dynamic bend and reads the target early, while tier-0's 5x-dominance
	// guard does not confirm until the pitch settles at the target. The longer streak
	// rejects a fast glide-through the target.
	constexpr int32_t BEND_RESCUE_STREAK = 4;

	// Owner offsets (GamePlaysongLAS).
	constexpr uintptr_t OWNER_CLOCK_PRIMARY = 0x3B4;
	constexpr uintptr_t OWNER_CLOCK_SECONDARY = 0x3B8;
	constexpr uintptr_t OWNER_CLOCK_RENDER = 0x3D0;
	constexpr uintptr_t OWNER_CLOCK_EPOCH_LOW = 0x3D4;
	constexpr uintptr_t OWNER_CLOCK_EPOCH_HIGH = 0x3D8;
	constexpr uintptr_t OWNER_COMPONENTS = 0x18;
	constexpr uintptr_t OWNER_CHILD_FLAG = 0x395;
	constexpr uintptr_t OWNER_NOTES_BEGIN = 0x41C;
	constexpr uintptr_t OWNER_NOTES_END = 0x420;

	// Native note object offsets.
	constexpr uintptr_t NOTE_RECORD = 0x2C;
	constexpr uintptr_t NOTE_EVENT_TIME = 0x38;
	// The native hit decision packs these two floats into its detection query: every
	// window leaf receives [eventTime + startDelta, eventTime + endDelta] and refuses
	// outright unless the detector clock lies inside that span, before consulting any input.
	constexpr uintptr_t NOTE_DETECT_WINDOW_END_DELTA = 0x94;
	constexpr uintptr_t NOTE_DETECT_WINDOW_START_DELTA = 0x98;
	// The chord template the evaluator judges the note against (query slot [7]).
	constexpr uintptr_t NOTE_CHORD_RECORD = 0x30;

	// SNG chord template, as the evaluator walks it: QueryNoteWindow's chord branch
	// reads six per-string frets at +0x04 with values < 0x1A playable, and the noteway
	// indexes the template array with a 0x48 stride (including the trailing 32-byte name).
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

	// NoteVfx controller and BeatVfx fork offsets.
	constexpr uintptr_t NOTE_VFX_PROMPT_FORK = 0xA4;
	constexpr uintptr_t SPECIALIZED_PROMPT_BC_FORK = 0xBC;
	constexpr uintptr_t SPECIALIZED_PROMPT_C0_FORK = 0xC0;
	constexpr uintptr_t BEAT_VFX_ACTIVE = 0x04;
	constexpr uintptr_t BEAT_VFX_ENTITY = 0x10;
	constexpr uintptr_t BEAT_VFX_PROMPT_REQUEST = 0x25;

	// SNG record offsets (standard Rocksmith 2014 note record layout).
	// SNG note-mask bits: an open low E carries mask 0x802004 (SINGLE | SUSTAIN | OPEN),
	// and Rocksmith's presentability predicate at 0x7A5CE0 tests 0x2000 and then reads
	// record+0x3C as the sustain length, which only makes sense if 0x2000 is SUSTAIN.
	constexpr uint32_t NOTE_MASK_HAMMERON = NoteByNote::NOTE_MASK_HAMMER_ON;
	constexpr uint32_t NOTE_MASK_PULLOFF = NoteByNote::NOTE_MASK_PULL_OFF;
	constexpr uint32_t NOTE_MASK_BEND = 0x00001000;
	// A tapped note (right- or left-hand tap) is fretted without a pick, so it retains
	// no-pick legato acceptance: confirmed by pitch rather than waiting for a pick onset
	// that never arrives. Tapped notes carry mask 0x904000 (SINGLE | RIGHTHAND | TAP).
	constexpr uint32_t NOTE_MASK_TAP = NoteByNote::NOTE_MASK_TAP;

	// A pull-off or tap is played without a pick attack, so the edge-detected
	// onset query frequently reports nothing at all for it: the string is already
	// sounding and only the fretted pitch changes. The current-pitch query does see
	// it. Requiring a settled reading avoids accepting the momentary pitches crossed
	// on the way to the target.
	constexpr uint32_t LEGATO_CONFIRMATION_TICKS = 2;
	// A hold that cannot be satisfied releases itself rather than locking the game.
	//
	// Backstop for unsatisfiable holds (for example a note beyond the loop end on the first
	// pass, before any turnover has revealed the boundary, or a note carrying record+0x04 = 1),
	// including causes not yet identified. The note is not accepted: the transport is released
	// and the song continues. Generous on purpose: a player stopping to read the highway or
	// look away is normal use, not a stuck hold.
	constexpr double HOLD_SAFETY_RELEASE_SECONDS = 60.0;
	// Chords get a shorter safety budget. A refused chord hold is freed by this
	// timeout alone (strumming never resets the progress anchor, only legato-run
	// progress does), and a stalled chord reads as the game hanging.
	constexpr double CHORD_HOLD_SAFETY_RELEASE_SECONDS = 15.0;
	// Double-strum guard: a chord committed at the section start restarts PlayerSong
	// at an epoch the bootstrap reads as a section change, which clears consumedRecords
	// and re-selects the just-played chord, forcing a second strum of an already-accepted
	// chord. The last committed chord survives that reset for this many wall-clock
	// seconds; a real loop replay arrives later than this and re-targets the chord normally.
	constexpr double COMMITTED_CHORD_RESELECT_GUARD_SECONDS = 2.0;

	// Rocksmith grades a bend by whether the player reaches the target pitch, so the
	// bent pitch is the correct thing to accept. Requiring the unbent fundamental is
	// wrong, and at a degraded tick rate it is close to unpassable: the unbent pitch
	// exists for only a few tens of milliseconds before the bend takes it away.
	constexpr int MAX_BEND_SEMITONES = 3;

	constexpr uintptr_t RECORD_MASK = 0x00;
	constexpr uintptr_t RECORD_FLAGS = 0x04;
	constexpr uintptr_t RECORD_HASH = 0x08;
	constexpr uintptr_t RECORD_TIME = 0x0C;
	constexpr uintptr_t RECORD_STRING = 0x10;
	constexpr uintptr_t RECORD_FRET = 0x11;
	constexpr uintptr_t RECORD_CHORD_ID = 0x14;
	// The bend amount in semitones (2.0 for a whole-step bend). The adjacent fields look
	// like a bend curve (a time at +0x44 inside the note's sustain window and the value
	// repeated at +0x48) rather than a single scalar.
	//
	// Without it, bend acceptance is a range (expected+1 through expected+3) that fires
	// the moment the pitch crosses into it, completing mid-bend, and can consume the
	// following note when that note's pitch lies inside the range.
	constexpr uintptr_t RECORD_BEND_AMOUNT = 0x40;
	// The value is trusted within a sane musical range and the range check is used outside
	// it, so an unexpected chart degrades to range acceptance rather than unplayable bends.
	constexpr float BEND_AMOUNT_MIN_SEMITONES = 0.5f;
	constexpr float BEND_AMOUNT_MAX_SEMITONES = 4.0f;
	constexpr uintptr_t RECORD_CHORD_NOTES_ID = 0x18;
	constexpr uintptr_t RECORD_PHRASE_ITERATION = 0x20;

	constexpr uint32_t NOTE_MASK_IGNORE = 0x00040000;
	constexpr uint32_t NOTE_MASK_CHILD = 0x10000000;

	// PlayerSong offsets.
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
	// Flow waits up to FLOW_PENDING_PICK_MAX_SECONDS past the note itself (0.26 + the 0.053
	// compensation behind the hold boundary), so its freeze-back needs more room than freeze mode.
	constexpr float FLOW_HOLD_BOUNDARY_MAX_OVERSHOOT = 0.34f;
	constexpr float PLAYER_SONG_RESTART_SECONDS = 0.27f;
	// Chord successors take the dense rebuild out to a wider spacing. Typical chord
	// chains sit just over the single-note boundary, so every chord would otherwise go
	// release -> restart -> re-freeze, re-running the visual churn around the strum
	// line. Jumping straight to the next chord skips only the strum tail between two
	// holds, which is the right trade for note-by-note practice.
	constexpr float CHORD_DENSE_WINDOW_SECONDS = 0.60f;
	constexpr uint32_t INPUT_RELEASE_CONFIRMATION_TICKS = 3;
	// A bend target whose bent pitch is distinguishable from the previous note may sit dead
	// steady on target during the input-release wait (a fast pick-and-bend arrives already
	// bent, with no rise left to observe); accept it after this many on-target polls even
	// without an observed rise. Small, because the same-pitch bar has already excluded the
	// previous note's ring.
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

	// Bootstrap state. The section start is confirmed by the delayed-identity
	// rollback-boundary bootstrap: a natural loop turnover jumps the transport clock
	// backward; the endpoint that a live note record matches by both authored time and
	// native event time is the true loop start, and greyCutoff is written from it exactly
	// once per section. It is never re-read from the owner's +0x3C0 field, which the game
	// moves to each restart epoch (that would drift greyCutoff forward and grey the
	// section's own early notes). The section end is latched once from +0x3C8 at
	// confirmation, before any owned restart moves it, and likewise never re-read.
	// Rollbacks after confirmation only advance the epoch and clear consumedRecords; they
	// never touch either boundary.
	void* trackedOwner = nullptr;
	float lastUpdateTime = 0.0f;
	bool hasLastUpdateTime = false;
	bool isEpochConfirmed = false;
	// Diagnostics: which path confirmed the current section (grid latch vs delayed-identity
	// fallback), surfaced in status so arming can be verified without racing the log.
	bool confirmedViaGrid = false;
	// Throttle for the periodic arming-state diagnostic.
	uint32_t armDiagTicks = 0;
	bool hasNativeSectionRangeFailureLogged = false;
	// Consecutive ticks the grid latch refused, and whether that refusal was reported. The latch
	// retries silently every tick (its early refusals are usually transient), so without this a
	// song whose loop never snaps to the grid would leave NBN inert with nothing in the log.
	uint32_t gridLatchRefusals = 0;
	// Why EnsureTimelineForOwner last refused, for the grid-latch refusal line.
	std::string gridReadRejectReason;
	bool hasGridLatchRefusalLogged = false;
	uint32_t rollbackIdentityMisses = 0;
	bool hasRollbackIdentityMissLogged = false;
	// The two endpoints of the first backward jump, held until a live record resolves one of
	// them to the section boundary by the authored+native identity condition.
	bool hasPendingBoundary = false;
	float pendingBoundaryBeforeRollback = 0.0f;
	float pendingBoundaryAfterRollback = 0.0f;
	float greyCutoff = 0.0f;
	// The upper selected-section boundary, latched once from +0x3C8 at confirmation.
	float sectionEndBoundary = 0.0f;
	bool hasSectionEndBoundary = false;

	// The cached authored section timeline for the current song. Built once per owner from
	// the stable phrase-section grid and used to latch the loop bounds directly, so arming
	// does not wait for a transport rollback or depend on the moving +0x3C0/+0x3C8 mirror.
	// Immutable for the song.
	struct TimelineSection { float start; float end; };
	std::vector<TimelineSection> timelineSections;
	void* timelineOwner = nullptr;
	// The grid identity the cache was built from. The owner+0x78 container (and its vector
	// begin/end) is repopulated when a new song loads, sometimes reusing the same owner
	// pointer, so keying the cache on the owner alone would leak the previous song's sections
	// into the next song (wrong sections, and out-of-range reads). Re-validating these each
	// call rebuilds the timeline per song lifecycle.
	uintptr_t timelineContainer = 0;
	uintptr_t timelineBegin = 0;
	uintptr_t timelineEnd = 0;
	// When the current hold began, for the safety release. Reset whenever the player makes
	// progress within a legato run, so a long but advancing run is never cut short.
	std::chrono::steady_clock::time_point holdProgressAnchor{};
	bool hasHoldProgressAnchor = false;
	uint64_t epochIndex = 0;
	// Every section latch (start, resume, re-select): the flow speed cap's "new pass" signal, since a
	// resume re-latches the same section at epoch 1 again.
	uint64_t sectionLatchCounter = 0;

	// Selection and hold state.
	GatePhase gatePhase = GatePhase::Idle;
	uintptr_t selectedRecord = 0;
	float selectedRecordTime = 0.0f;
	int selectedString = -1;
	int selectedFret = -1;
	int selectedChordId = -1;
	int selectedChordNotesId = -1;
	float selectedHoldTime = 0.0f;
	double selectedCompensation = 0.0;
	// The song speed as measured from the transport itself (song seconds per real second while the
	// note is Armed and the transport runs freely). 1.0 until measured.
	double measuredSongSpeed = 1.0;
	bool hasMeasuredSongSpeed = false;
	// The speed flow itself requested this pass (GE_SetRRSpeed, real percent), -1 for none. The game
	// re-sends the player's own speed at every pass start, so this is reset with the pass.
	float flowSpeedPassRequestedPercent = -1.0f;
	float songSpeedSampleTime = -1.0f;
	std::chrono::steady_clock::time_point songSpeedSampleAt{};
	int expectedMidi = -1;
	// The chord target's tones as the native matcher last evaluated them (physical MIDI
	// frame, from note+0x30). Tagged with the record they were read for so the research
	// state never reports a previous chord's tones for the current target. Read only.
	int researchChordTones[6] = {};
	int researchChordToneCount = 0;
	uintptr_t researchChordTonesRecord = 0;
	// The pitch of the note that was just committed. A carried onset necessarily has
	// this pitch, which is what makes it distinguishable from fresh playing.
	int previousExpectedMidi = -1;
	// The string of the note just committed. A no-pick pull-off or tap can only be
	// sounded by a string that is already ringing, so legato handling only applies
	// when the successor is on this same string.
	int previousSelectedString = -1;
	// The raw-audio sample a chord commit consumed through. A single-note successor's
	// "attack" at or before this is the chord's own strum re-flagged by the HFC detector,
	// not a fresh pick, so it is ignored.
	uint64_t pickAttackFloorSample = 0;
	// Sequence inference state (TryInferFromFollowingPick).
	NoteByNote::PickedAttack lastUnresolvedAttack;
	bool hasLastUnresolvedAttack = false;
	uint64_t lastTakenPickSample = 0;
	uintptr_t nextNoteMidiRecord = 0;
	int nextNoteMidi = -1;
	// Same-time singles: a chart can write a two-hand tap pair as two SINGLE records at one
	// time on different strings (mask TAP|RIGHTHAND|SINGLE), not as a chord. Each is its own
	// hold, so after the first commits the second would freeze again at the same moment and
	// demand a fresh attack whose energy already went to the first note. A later member of
	// such a group (a "sibling") is accepted when the game's sounding table shows its pitch
	// rose since the group's first member became the target. See TrackSameTimeGroup /
	// ConfirmSameTimeSibling.
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
	// The strum that committed the last chord (0 after a legato run). A masked single-note
	// successor ignores attacks inside that strum's tail (OctaveCollisionGate.hpp).
	uint64_t lastChordStrumSample = 0;
	// The strum the just-committed single note confirmed off but did not own
	// (StrumHandoff.hpp, set in CompleteHeldCommit). Bound to the first chord hold that claims
	// it so the dense successor's second hold reset re-inherits the same strum; dropped when it
	// is too old for any chord to consume or another record comes along.
	uint64_t successorStrumSample = 0;
	uintptr_t successorStrumRecord = 0;
	// Whether the note just committed was a bend. A bend sweeps through a range of
	// pitches, so a carried onset from it does not necessarily carry that note's
	// nominal pitch and cannot be told apart by pitch alone.
	bool previousWasBend = false;
	// Set when the selected record carries NOTE_MASK_BEND, which widens acceptance
	// upward to the pitches the bend itself produces.
	bool isBendTarget = false;
	// The exact pitch a bend must reach, or -1 when the chart's bend amount was unreadable
	// or implausible, in which case acceptance falls back to the old tolerance range.
	int bendAcceptMidi = -1;
	// Set when the selected record is a bend child, that is a continuation of a bend whose
	// parent has already been played and accepted.
	//
	// Such a record cannot be satisfied by a pick. Rocksmith authors a bend as a parent and a
	// child on the same string and fret a fraction of a second apart, the player performs one
	// continuous gesture, and by the time the child is selected the string is already bent and
	// ringing at the target pitch with nothing left to pluck. Requiring an onset at the unbent
	// pitch would run the hold to the safety release.
	//
	// The child is still a target rather than skipped, because skipping bend children makes a
	// bend accept itself the moment its parent is played. So what satisfies it changes instead:
	// the continuous pitch tracker can see the bend being held, and a bend produces no new pick
	// attack anyway, which is the same constraint as the retained no-pick techniques.
	bool isBendChildTarget = false;
	// Defined beside the acceptance test it feeds, but needed by hold establishment above.
	void ResolveBendAcceptance(uintptr_t record);
	// Forward declaration: FaultWithoutRelease recovers by re-bootstrapping, but ResetBootstrap is
	// defined below it.
	void ResetBootstrap(const char* reason, void* liveOwner, bool releaseStoppedMusic);
	// Defined beside the legato run it builds on, but needed by the hold tick above it.
	void BeginBendConfirmation();
	void StashPrimedOnset(int primedOnset);
	bool MatchesPickPitch(int sounding);
	// Defined beside the tracker read it explains, but needed by the hold tick above it.
	void LogMotionTrackers(const char* context, int wantedMidi);
	// Throttles the tracker report to once a second while a bend is being confirmed.
	std::chrono::steady_clock::time_point trackerSampleAnchor{};
	bool hasTrackerSampleAnchor = false;
	// The target's legato continuation, published so the renderer can keep it visible.
	uint32_t visualGroupCount = 0;
	uintptr_t visualGroupRecords[ResearchProtocol::NoteByNoteState::MaxVisualGroup] = {};
	// The same gesture as string/fret pairs, for the neck-diagram gate, which receives
	// coordinates rather than a note pointer.
	int32_t visualGroupStrings[ResearchProtocol::NoteByNoteState::MaxVisualGroup] = {};
	int32_t visualGroupFrets[ResearchProtocol::NoteByNoteState::MaxVisualGroup] = {};
	uintptr_t lastVisualGroupRecord = 0;
	// Set for hammer-on, pull-off and tap records, which are accepted from the detector's
	// current pitch because they produce no pick attack for the onset query to latch.
	bool isLegatoTarget = false;
	// A hammer-on is a legato target that ALSO stays eligible for the pick buffer, so a
	// hammer-on that happens to be picked still commits (see IsPlainPickedTarget).
	bool isHammerOnTarget = false;
	uint32_t legatoConfirmTickCount = 0;
	// True once the current-pitch query has reported something other than the target's
	// pitch since this target became active. Legato acceptance requires it: the pitch
	// must arrive after the target exists, so a string still ringing at the right pitch
	// (a bend release sweeping down, a lingering previous note) cannot commit a note
	// the player has not articulated.
	bool hasLegatoPitchDeparted = false;
	// A legato run is played as one gesture, so it is accepted as one note.
	//
	// Skipping the continuations as targets would let the pick alone complete the gesture:
	// the timeline would advance past the whole run and the continuation would collapse
	// visually into the following note, which may be on another string.
	//
	// The run is held as a unit. The first note needs its pick attack, and each
	// continuation is then confirmed by pitch, because a no-pick continuation produces no
	// pick attack for the onset query to latch. Nothing is committed until the whole run
	// has been played, so an incomplete or wrong run simply does not advance.
	constexpr uint32_t MAX_LEGATO_RUN = 8;
	int legatoRunMidi[MAX_LEGATO_RUN] = {};
	// Which elements of the run are a bend rather than a fretted legato note. The two need
	// different tests: a fretted note lands on a discrete fret and its pitch is exact, a
	// bend sweeps continuously and has to be judged against a band. Without this, a bend
	// target that also begins a legato run would have its bend requirement dropped.
	bool legatoRunIsBend[MAX_LEGATO_RUN] = {};
	uint32_t legatoRunCount = 0;
	uint32_t legatoRunIndex = 0;
	bool isConfirmingLegatoRun = false;

	// ---- Detection strategy per technique ---------------------------------------------
	// Note by Note ships as one tuned engine. Blend (default) fuses native + tier-0 + ML:
	// each can rescue the other where it is weak, and neither vetoes the other unless it is
	// confidently right. NativeOnly / MlOnly force a single engine for testing and isolation.
	// The swap arrives over the probe-command channel (no GUI).
	using DetectionStrategy = NoteByNoteNativeScoring::DetectionStrategy;
	enum class DetectionTechnique { Single = 0, Chord = 1, Bend = 2 };
	volatile int g_chordDetectionStrategy = static_cast<int>(DetectionStrategy::Blend);
	// Single notes default to NativeOnly (ML shadow): the tier-0 raw verifier
	// (QueryRawNoteConfirmation) stays the unconditional gate and native the frame, but ML
	// neither vetoes nor rescues a single-note accept. ML still displays in the HUD via the
	// live reader. Compile-time default so a DLL reload cannot silently put ML back in the
	// note path. Chords and bends keep Blend.
	volatile int g_noteDetectionStrategy = static_cast<int>(DetectionStrategy::NativeOnly);
	volatile int g_bendDetectionStrategy = static_cast<int>(DetectionStrategy::Blend);

	// Blend only lets ML VETO a native accept when it is confidently seeing a genuinely
	// different note. This bar sits above the 0.5 accept bar so an unsure or borderline ML
	// read never blocks a correct native read; an octave-equivalent read never vetoes at all.
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

	// ML may rescue (accept-only, fills a native/tier-0 miss) in Blend and MlOnly, never in
	// NativeOnly. Native still detects; the rescue only adds accepts, it never blocks.
	// Gated on g_mlRescueEnabled.
	bool MlMayRescueNow() { return g_mlRescueEnabled && CurrentTechniqueStrategy() != DetectionStrategy::NativeOnly; }

	// Live native-vs-ML agreement sampler + per-session CSV. Defined further down, next to the
	// shadow loggers; forward-declared here so the hold-phase sampler can call it.
	void SampleDetectionComparison();

	// Corner-HUD snapshot, updated by SampleDetectionComparison and read by GetResearchState.
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
	// -------------------------------------------------------------------------------------
	// True when the run being confirmed is a bend rather than fretted legato notes.
	//
	// The two need different comparisons. A hammer-on or pull-off lands on a discrete fret,
	// so its pitch is exact. A bend sweeps continuously and wobbles around the top, so
	// requiring the detector to report exactly the bent pitch on consecutive polls makes
	// bends very hard to trigger. Reaching the target pitch is the musically correct test,
	// and overbending slightly still counts.
	bool isBendRunConfirmation = false;
	// Keeps the tracked-pitch line to one per bend. The confirmation runs every scoring
	// tick, so logging unconditionally would bury the surrounding hold diagnostics.
	bool wasBendTrackerPitchLogged = false;
	// Raw-detector bend acceptance state. Armed only once the detector has been seen
	// off-target since the confirmation began, so a clean leftover ring at the target
	// pitch (a same-pitch predecessor) cannot complete a bend that was never bent.
	bool isRawBendAcceptArmed = false;
	int32_t rawBendAcceptStreak = 0;
	// Consecutive ticks the raw-audio bend estimate (TryEstimateRawBendPitch) sat inside the
	// target band; the player-frame bend reach (see the run confirmation).
	int32_t rawEstimateBendStreak = 0;
	// ND sounding-table bend evidence. While frozen the motion trackers are unregistered
	// and the gated integer query needs settled quality, so a bend resting just under
	// target would stall until held dead-on. The sounding table lists the current pitch
	// as integer MIDI with an engine strength: a below-target sighting arms the approach
	// (the sweep passing through), a target-or-over sighting while armed reaches, on two
	// consecutive ticks so one stray entry cannot complete a bend.
	int32_t ndBendSightingStreak = 0;
	// The same off-target-first rule for the plain integer fallback
	// (currentMidi >= target): with the whole bend band routed into confirmation, a
	// carried in-band onset reaches this flow with the ring still sounding at the accept
	// pitch, and an unarmed >= comparison would confirm the bend on the first poll. The
	// gesture must be observed approaching from below first.
	bool hasBendApproachBeenObserved = false;
	// Defined below, next to the visual grouping it mirrors, but needed by hold
	// establishment above it.
	void BuildLegatoRun(void* owner, float runTime, int runString);
	bool isHoldSuppressed = false;
	// Chord holds. A strum has no single expected pitch, so a held chord is accepted by
	// evaluating the game's own hit decision instead of blocking it. Bridge-toggleable
	// (chord-holds-on/off) to fall back to play-through behavior live.
	volatile bool areChordHoldsEnabled = true;
	// Flow-until-miss. Moves the hold boundary from before the strike line
	// (selectedRecordTime - the ~0.053 engine compensation) to selectedRecordTime +
	// flowLateGraceSeconds, so an on-time note commits naturally while Armed (the "Natural
	// commit before any hold" path, transport never stops). Dense sections play straight
	// through instead of being sliced into per-note audio fragments, and the freeze fires
	// only on a real miss. probe_flow_on/off toggles it live; the overlay's Note by Note page
	// switches it off ("Keep playing while I keep up").
	volatile bool isFlowUntilMissEnabled = true;
	// How far past a note's record time the transport keeps flowing before the boundary
	// freezes on it (seconds). Bounded above by the native detect window's exit
	// (recordTime + ~0.3): the frozen clock must still sit inside the note's window or the
	// native hit decision treats it as dead, so this stays comfortably under that. Kept short
	// because the grace is song time, so a lowered speed stretches it further.
	// Bridge-settable (probe_flow_grace <sec>).
	volatile float flowLateGraceSeconds = 0.08f;
	// Flow phase 3: under flow the pick buffer commits notes too, not only the game's own
	// natural decision. While Armed (transport running, no hold) the target pitch is resolved
	// early (armedExpectedMidi) so buffered attacks are judged against it; a confirmed one
	// makes the hit decision return true for the selected record (armedBufferCommitRecord),
	// which commits it through the ordinary "Natural commit before any hold" path: no freeze,
	// no release, no rebuild.
	int armedExpectedMidi = -1;
	uintptr_t armedBufferCommitRecord = 0;
	bool wasArmedBufferCommitLogged = false;
	// Verbose trace toggle. The per-tick trace logs (LAS DETECT, BEND FEED, CHORD WINDOW,
	// the chord eval-false + sounding-table dump, and the native-matcher observe call) are
	// on by default in development builds; `probe_verbose_off` turns them off live over the
	// probe-command channel for higher FPS. Event logs (commits, accepts, holds, onset
	// evidence) are never gated by this, only the high-frequency output.
#if defined(RSMODS_PUBLIC_RELEASE)
	volatile bool verboseTrace = false;   // Master has no probe command to turn it off, and no trace file
#else
	volatile bool verboseTrace = true;
#endif
	// Safety-release toggle, default off: the timed release reads as an accept from the
	// player's seat and masks every stuck-hold refusal it fires on. Off, a stuck hold stays
	// held and logs its refusing state every budget interval; N (Stop) releases it.
	// safety-release-on|off.
	volatile bool isSafetyReleaseEnabled = false;
	// Seconds mark of the last stuck-hold warning, so the disabled path logs once
	// per budget interval instead of every tick. Reset when a hold (re)latches.
	double lastStuckWarnHeldSeconds = 0.0;
	uint64_t chordDecisionEvalCount = 0;
	// Peak count of the held chord's tones seen in the sounding table since the current strum
	// (chordSoundingPeakAttack). A single tick's read is noisy (strings ring in and out), so the
	// "how much of the chord was actually strummed" question is answered by the peak over the
	// attack window, not by the read at the accept tick.
	uint64_t chordSoundingPeakAttack = 0;
	// The strum on which the game's vote or chord matcher last agreed (power-chord path).
	uint64_t chordNativeAgreedAttack = 0;
	int chordSoundingPeak = 0;
	std::chrono::steady_clock::time_point chordDecisionLogAnchor;
	bool hasChordDecisionLogAnchor = false;
	// Throttle for the observe-first native-matcher log.
	std::chrono::steady_clock::time_point matcherObserveAnchor;
	bool hasMatcherObserveAnchor = false;
	// Carry-over-ring freshness for the matcher accept: a repeated chord's previous strum
	// keeps ringing, so the spectral matcher reads the full chord even when the player picks
	// only one string of it. The matcher must have fallen below its YES at least once since
	// this target latched before a YES may commit, so a fresh strum, not a continuous ring,
	// is the only way to arm acceptance.
	uintptr_t matcherFreshnessRecord = 0;
	bool matcherFreshnessArmed = false;
	// Chord window slide. The hit decision refuses a held chord without consulting any input
	// once the detector clock (detector +0xD08) has left the note's authored-time window
	// (note +0x38 plus the +0x98/+0x94 deltas). While a hold owns the transport the song
	// clock is frozen but the detector clock is not, so a passed window refuses forever.
	// The slide translates the window onto the clock for exactly one original-decision call,
	// authored width preserved, deltas restored immediately after. Bridge-toggleable
	// (chord-window-on/off); the (NBN LAS CHORD WINDOW) log reports the divergence.
	volatile bool isChordWindowSlideEnabled = true;
	uint64_t chordWindowSlideCount = 0;
	std::chrono::steady_clock::time_point chordWindowLogAnchor;
	bool hasChordWindowLogAnchor = false;
	// Bare-0x2 repeat strums hold like full chords. Playing them through lets the running
	// transport overshoot the next chord's hold boundary when two play-through records occur
	// in a row. The spike gate and the chord safety budget bound a stalling repeat; toggle
	// back with repeat-holds-off.
	volatile bool areRepeatStrumHoldsEnabled = true;
	// Double-strum guard state; see COMMITTED_CHORD_RESELECT_GUARD_SECONDS.
	uintptr_t lastCommittedChordRecord = 0;
	// The chord id of that commit (-1 after a legato run): a re-strum of the same chord is judged
	// on the strum, not on its tones rising (see the re-strum rule in the chord decision).
	int lastCommittedChordId = -1;
	std::chrono::steady_clock::time_point lastCommittedChordAt{};
	// Attack evidence for chord acceptance: set by the spike detector or the ring
	// onset-flag scan below while holding, cleared at hold establishment. Without it
	// the ringing previous strum satisfies the evaluator when the next target is the
	// same chord (ring-through auto-advance instead of waiting for a fresh strum).
	bool sawSpikeDuringHold = false;
	// A stricter fresh-attack signal set only by the level-spike gate (a genuine loud
	// attack), never by the analysis-ring onset flag. The onset-flag half of
	// sawSpikeDuringHold fires on a still-ringing chord's own onset stamps, which can
	// cascade a whole repeat-strum section off one pick. A real chord strum always spikes
	// the level meter; a decaying ring never does, so the matcher accept gates on this.
	// Reset every hold and every dense successor with sawSpikeDuringHold.
	bool sawLevelSpikeDuringHold = false;
	// The analysis-ring timestamp captured when the current target latched. This is the
	// window origin for the native onset-window scan port (the value the native code takes
	// from the now-frozen detector clock). Frames newer than this are "since this note
	// latched"; the onset+match must occur in that span. Reset per hold and per dense
	// successor so each note requires its own fresh attack.
	double holdLatchRingTime = 0.0;
	bool hasHoldLatchRingTime = false;
	// The selected single note's authored pitch in the detector's tuning frame (read from
	// its own template, exactly as the chord path reads view.notes), so the native
	// onset+harmonic scan can judge single notes the same way it judges chords: no
	// display-frame onset shift, transpose-correct in every tuning. -1 = unavailable
	// (dense successor / unreadable template) -> the single note falls back to the legacy
	// onset path. Bends/legato keep the legacy pitch-tracking path regardless.
	int selectedNativeTone = -1;
	// When a bend last committed, so a follower can require attack energy for the release
	// decay window (see BEND_RELEASE_GUARD_SECONDS).
	std::chrono::steady_clock::time_point lastBendCommitAt{};
	bool hasLastBendCommit = false;
	// Onset-frame attack evidence cursor. The level-spike gate alone cannot see a re-strum
	// layered on a still-loud ring: fresh strums can move the meter less than the 2.5 dB
	// jump, so every true evaluator decision would be discarded as ring. The analysis ring
	// stamps an onset flag on attack frames themselves, which separates a fresh attack from
	// a ring at any absolute level. Frames are walked from this cursor so none are missed
	// between frame-coupled scoring ticks (~30/s against ~95 ring frames/s); only frames
	// written at least ONSET_EVIDENCE_MIN_FRAMES after the hold latched count, so the
	// committing strum of the previous target cannot bleed evidence into this hold.
	int32_t onsetScanRingIndex = -1;
	uint32_t onsetScanFramesSinceLatch = 0;
	bool hasOnsetScanAnchor = false;
	NoteByNote::PickedAttackQueue pickedAttacks;
	NoteByNote::PickedAttack acceptedPick;
	uintptr_t acceptedPickRecord = 0;
	NoteByNote::DetectionFeedback detectionFeedback;
	// Which detector(s) passed the chord just accepted, published at its commit.
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
	// Rocksmith's scoring update is frame-coupled, so the tick rate is the frame rate and
	// worth reporting. It is a measurement only: the detector does not starve at low tick
	// rates, because the reported note is read fresh from the analysis ring on every call
	// and nothing about a pluck expires between polls.
	std::chrono::steady_clock::time_point holdTickRateAnchor;
	uint64_t holdTickRateAnchorTick = 0;
	// Detector sampling state. The ring write index is kept so a stalled hold can say
	// whether the engine's analysis ring is still advancing, which no single sample shows.
	std::chrono::steady_clock::time_point detectorSampleAnchor;
	bool hasDetectorSampleAnchor = false;
	int32_t lastSampledRingIndex = -1;
	bool hasLastSampledRingIndex = false;
	// Level-spike capture. A re-pick attack is a sharp upward level jump against a
	// ~1 dB/s decay, so comparing consecutive scoring ticks catches every pick even
	// when the once-a-second interval logger sleeps through it. A spike opens a short
	// burst of per-tick compact samples so the attack transient's quality/pitch/dedupe
	// evolution is on record.
	float lastTickDetectorLevel = 0.0f;
	bool hasLastTickDetectorLevel = false;
	int32_t spikeBurstTicksRemaining = 0;
	// Spike re-attack acceptance state (see REATTACK_WINDOW_TICKS). Deliberately its
	// own level tracker rather than sharing the logger's, so the logger stays a pure
	// observer.
	float reattackLastLevel = 0.0f;
	bool hasReattackLastLevel = false;
	int32_t reattackWindowTicks = 0;
	int32_t reattackStreak = 0;
	Research::MlConfirmationState mlConfirmationState;
	Research::MlConfirmationState mlBendConfirmationState;
	NoteByNote::HeldPitchConfirmation enhancedLegatoConfirmation;
	int32_t bendRescueStreak = 0;      // consecutive ticks tier-0/ML confirm the bend reached its target

	// Flow instrumentation: measures the per-note cost that makes fast play choppy, the
	// coordinated PlayerSong/fretboard rebuild span (heavy, restarts the audio) and the
	// wall-clock gap between consecutive commits.
	std::chrono::steady_clock::time_point denseRebuildQueuedAt{};
	bool hasDenseRebuildTiming = false;
	// True while a flow freeze-back redraw is riding the dense rebuild pipeline (BeginFlowRedrawRebuild).
	bool isFlowRedrawRebuild = false;
	std::chrono::steady_clock::time_point lastCommitWallClock{};
	bool hasLastCommitWallClock = false;
	// Ticks since the last level spike specifically (never set by the onset-opened
	// window). A decaying ring cannot spike, so recent spike energy is the one
	// signal that separates a fresh pick of a same-pitch successor from the
	// previous note still ringing at the identical pitch.
	int32_t spikeRecencyTicks = 0;
	// A played-ahead pick rescued from the arming's edge-drain (see
	// StashPrimedOnset); adopted by the first holding tick, single-shot.
	int pendingPrimedOnset = -1;
	// Departure-then-lock acceptance for picked targets, the same rule the legato
	// path uses: the gated current-pitch query must first report a different pitch
	// during the hold (a re-pick's attack transient always provides one; a decaying
	// ring only ever reads the expected pitch or nothing, so it can never arm this),
	// then the expected pitch across consecutive polls commits. Tracked in the holding
	// phase only.
	bool hasPickPitchDeparted = false;
	uint32_t pickPitchConfirmTicks = 0;
	uint64_t postReleaseTickCount = 0;
	uint64_t commitTickCount = 0;
	uint32_t inputReleaseTickCount = 0;
	// Bend-target input-release confirmation: the lowest raw pitch seen during the wait
	// (a fresh bend climbs above it; a decaying ring does not) and how many consecutive polls
	// the sounding pitch has sat on the bent target. Both reset at every wait entry.
	int bendWaitPitchFloor = -1;
	uint32_t bendWaitOnTargetTicks = 0;
	bool wasCommitOverrideLogged = false;
	bool isReleaseRequested = false;
	// Latched by RequestReArm on every NBN enable, honored on the next scoring tick:
	// forces a clean ResetBootstrap so a section change that kept the same
	// GamePlaysongLAS owner still re-arms for the new section. See RequestReArm.
	bool reArmRequested = false;
	std::unordered_set<uintptr_t> consumedRecords;
	DenseSuccessor denseSuccessor;
	float observedScoringUpdateTime = 0.0f;
	std::array<BendDecisionObservation, 8> bendDecisionObservations = {};
	size_t bendDecisionObservationCount = 0;

	// Render-side observation counters (evidence only; never a control input).
	uint64_t renderFramesWhileHeld = 0;

	// The onset core 0x48DC40 takes the arrangement kind in EAX (0 guitar, 1 bass) and
	// passes the resolved detector through EDI/ECX internally; no C calling convention
	// matches, so the call needs explicit register setup (a plain cast call leaves EAX
	// as garbage, the mode resolver returns null, and the query returns -1 forever).
	// Host input-onset shift, in semitones, to add to a native pitch reading. Disabled
	// (kApplyInputOnsetShift = false): native readings are converted from the detector's
	// tuning frame to expectedMidi's played frame by NativeFrameOffset instead, which is
	// correct for plain play and any transpose (Speaker Mode). The shift hook is kept as a
	// no-op in case an ASIO input retuner (one that actually retunes the input, unlike
	// Speaker Mode) is ever added; that would need real Speaker-Mode-vs-ASIO detection.
	constexpr bool kApplyInputOnsetShift = false;
	int NativeFrameOffset();
	int ShiftNativePitchToDisplay(int nativeMidi)
	{
		// The detector-frame offset (NativeFrameOffset) replaces the disabled host shift.
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
		// Normalise the onset into expectedMidi's display frame (see ShiftNativePitchToDisplay).
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

	// The Guided Experience "freeze" sound dispatched before every single-note hold. It is
	// a percussive hit on every correct note, so it ships silenced (default off).
	// probe_freeze_sound_on restores it.
	volatile bool isFreezePromptSoundEnabled = false;

	void PlayFreezeNoteTrack()
	{
		if (!isFreezePromptSoundEnabled)
		{
			LOG_INFO("(NBN LAS PROMPT) " << FREEZE_NOTE_TRACK_EVENT
				<< " skipped (probe_freeze_sound_off)." << std::endl);
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

	// The onset dedupe global is plain writable game data and, during Learn a Song, the
	// controller owns its state outright (nothing else calls the onset query; see the
	// ONSET_NOTE_QUERY notes). Writing -1 "un-consumes" an edge: the next query re-reports
	// whatever settled pitch is in the ring.
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

	// Same contract as TryWriteDedupeGlobal, generalized to an instance field. Used
	// only for the held chord's window deltas, written and restored around a single
	// original-decision call on the scoring thread, which is the thread evaluating
	// the note; nothing else reads the deltas in between.
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

	// The raw values TryReadNativeSectionRange judged, for the one-time failure line: which of
	// its checks refused (owner class, a failed read, a non-finite or negative start, end not
	// after start, or a mirror disagreeing) is otherwise invisible in the log.
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

	// Enumerate Rocksmith's stable authored phrase-section grid for this owner into the
	// cached timeline, per song lifecycle. The cache is keyed on the grid identity (owner +
	// the +0x78 container and its vector begin/end), not the owner alone, so a new song that
	// repopulates the grid (even under the same owner pointer) invalidates it and rebuilds.
	// Every read is guarded and any failure/transitional read leaves the cache empty so the
	// caller retries rather than arming on garbage. See LAS_PHRASE_SECTION_CONTAINER.
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

		// Reuse only when the whole grid identity is unchanged.
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
			// A row with no playable length (end <= start) that is still in order holds no notes, so
			// it is skipped rather than rejecting the whole grid. Official charts carry them (for
			// example a trailing marker row authored backwards). Out-of-order and non-finite rows
			// below still reject the grid.
			if (std::isfinite(start) && std::isfinite(sectionEnd) && sectionEnd <= start + BOUNDARY_EPSILON
				&& start >= previousEnd - BOUNDARY_EPSILON)
			{
				++skippedEmpty;
				continue;
			}
			// The grid is authored, monotonic and half-open. A non-finite or out-of-order
			// read means the wrong object was snapped (or mid-alloc garbage); reject the
			// whole build rather than cache a bad row.
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

	// Find the cached section index whose start (or end) matches a native boundary time.
	// Returns -1 when nothing is within BOUNDARY_EPSILON.
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

	// Latch the loop bounds directly from the stable grid, immediately, with no rollback
	// wait. The native loop range is read once here to identify which grid rows the loop
	// spans; the bounds themselves come from the grid, so a moved +0x3C0 or a churning
	// mirror cannot corrupt the latched values. +0x3C8 (end) stays pinned to the loop's
	// last section end; +0x3C0 (start) is clean until NBN owns its first hold (which has
	// not happened at first confirmation). Returns false on a transient read failure so
	// the caller simply retries next tick, never going inert or hanging on "Waiting".
	// Report a grid-latch refusal once it has persisted ~3 s of scoring ticks (transient reads at
	// song start retry and succeed within a few ticks), with what was compared.
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

		// Anchor on the section end (+0x3C8). It is the stable half of the moving pair:
		// the +0x54 writer sets it to max(heldEpoch, ref)=ref, so it stays pinned to the
		// loop's authored end and its own mirror (+0x3CC) agrees even while +0x3C0 churns.
		// Requiring only the end mirror (not both) makes this immune to that churn.
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

		// The loop start. +0x3C0 moves during play (to the held epoch) and is not reliably
		// rewritten when the player only extends the range rightward, so it cannot be blindly
		// trusted. Precedence:
		//   1. +0x3C0 when it reads clean (mirror agrees) and snaps exactly to a grid start
		//      at/before the end row. That is a fresh selection the game just wrote (a move,
		//      or an extend-left), so it is the authoritative new start.
		//   2. otherwise the caller's startHint (the previously latched start): extending the
		//      range to the right never changes the loop start. This keeps a multi-section
		//      loop's start pinned instead of collapsing to the last phrase.
		//   3. last resort: a single-phrase loop (start = the end row's own start).
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
		// Keep the last measured speed (a menu change is re-measured within the lead-in, and the
		// Armed hold boundary follows it - see SampleSongSpeed); only restart the sample window.
		songSpeedSampleTime = -1.0f;
		consumedRecords.clear();
		LOG_INFO("(NBN LAS BOOTSTRAP) Section latched from the authored grid: rows "
			<< startIndex << ".." << endIndex << " -> [" << std::fixed << std::setprecision(6)
			<< gridStart << ".." << gridEnd << "]; greyCutoff=" << gridStart
			<< " end=" << gridEnd << "; epoch 1 begins immediately (no rollback wait)."
			<< " Native loop read was [" << loopStart << ".." << loopEnd << "]." << std::endl);
		return true;
	}

	// True when the stable RR section end (+0x3C8) has moved to a different grid section
	// than the one currently latched, i.e. the player navigated to a new Riff Repeater
	// section in Practice Selection. +0x3C8 is the ref (max) half of the moving pair, so
	// it does not drift during play; a change is a real re-selection, not restart churn.
	// This lets NBN follow the selection without being toggled off and on.
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
		// Only react when the new end actually snaps to a grid section, so a momentary
		// garbage read can never trigger a spurious re-latch.
		return FindTimelineIndexByEnd(currentEnd) >= 0;
	}

	// The sounding fractional pitch nearest to a wanted pitch, from Rocksmith's own
	// continuous trackers.
	//
	// Every read is guarded, and any failure anywhere in the chain reports false so the
	// caller falls back to the integer query rather than mis-scoring the hold. Nothing is
	// registered or released: this reads records the game is already maintaining.
	//
	// It picks the nearest record rather than a particular tracker id because what
	// registers these trackers during ordinary play, and against what target, is not
	// established. Selecting by proximity to the awaited pitch uses the record purely as
	// a measurement of what is sounding; the decision about which pitch matters stays here.
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

			// Both guards are the native accessors' own preconditions: _GetLocation
			// refuses a record whose +0x3C is not 1, and _IsLocated's return value at
			// +0x3D is what says the string is still sounding rather than merely
			// registered.
			uint8_t active = 0;
			uint8_t located = 0;
			if (!TryRead(record + MOTION_NOTE_RECORD_ACTIVE, active) || active != 1) continue;
			if (!TryRead(record + MOTION_NOTE_RECORD_LOCATED, located) || located == 0) continue;

			float pitch = 0.0f;
			if (!TryRead(record + MOTION_NOTE_RECORD_PITCH, pitch)) continue;
			// -1.0f is the sentinel for "not located"; the flag above should already have
			// excluded it, but the two are written independently so both are checked.
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

	// Every tracker record, so the bend fallback is not silent.
	//
	// TryGetSoundingPitchNear can decline for four different reasons and reports one bool for
	// all of them: the pointer chain not resolving, a zero or implausible count, no record
	// being both active and located, or every located record sitting outside the band. Those
	// need different fixes, so they are separated here. If tracker registration is driven by
	// the advancing transport, a frozen transport shows here as records present but never
	// active or located.
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
			// Every record, flags and pitch, active or not, to show whether the pitch field
			// keeps updating while registration (the flags) is dead.
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

	// Every condition the two native input queries test, read directly rather than inferred
	// from their -1 return.
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

	// Cached gate sample for the state feed. GetResearchState is polled up to three
	// times per rendered frame (highway refresh, overlay input line, bend meter), and
	// TryRead pays a VirtualQuery syscall per read, so a fresh detector-chain read per
	// poll costs real frame time. LogDetectorGates already samples every scoring tick;
	// the feed copies that sample instead of re-reading. The timestamp bounds
	// staleness: outside active scoring (menus, idle) the sample stops refreshing
	// and the overlay line withdraws rather than showing a stale or misleading
	// number (the pause menu legitimately quiets the input).
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

	// The bend visualizer's "reached" verdict (published as state.bendReachedTarget): set
	// where the bend evaluation actually advances the gesture (the legato-run bend element
	// surviving its vetoes), never from pitch alone, so the meter's green is the engine's own
	// decision. The selected record and Riff Repeater epoch identify the exact gesture for
	// which it was accepted, preventing a previous loop pass from leaking into the next one.
	bool bendVisualReached = false;
	uintptr_t bendVisualReachedRecord = 0;
	uint64_t bendVisualReachedEpoch = 0;

	// Input-present floor for gating the high-frequency trace logs. The console buffer holds
	// only ~9000 rows, so per-frame/per-tick idle output (BEND FEED, LAS DETECT) would scroll
	// real events (a commit, a chord accept) out of it. Idle sits near -80 dB; real playing
	// and its ring tail sit above -60, so the floor keeps a decaying chord's diagnostics
	// alive while silencing a put-down guitar.
	constexpr float NBN_INPUT_PRESENT_FLOOR_DB = -60.0f;

	// True when the guitar is currently producing input, read from the same per-tick
	// level cache that drives the top-left input display (fresh within 2s, above the
	// floor). Cheap; used to gate high-frequency trace logs.
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

	// Bass arrangements use the same detector slot as guitar (root+0x10); root+0x14 is the
	// second player, not bass. What tells them apart is the detector's own open-string table
	// at detector+0x134C (int16[6], the detector's tuning frame): bass {28,33,38,43,0,0},
	// guitar six nonzero notes. Refreshed when a hold is established; everything
	// bass-specific branches on this flag so guitar behaviour is unchanged when it is false.
	constexpr uintptr_t DETECTOR_OPEN_STRING_MIDI = ArrangementInstrument::DETECTOR_OPEN_STRING_MIDI;
	bool isBassArrangement = false;

	bool IsBassArrangement() { return isBassArrangement; }

	// A pick confirmed as another pitch stays queued (for the next target) instead of being
	// dropped, in every mode. Otherwise a pick for the next note played before the current
	// one commits is discarded as a mismatch and has to be re-picked. A kept pick still only
	// ever commits a target of its own pitch.
	bool KeepsMismatchedPicks() { return true; }

	// G string (43) at fret 24, plus room for a tuning up to +2: the highest note a bass plays.
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

	// The native detector reports pitches in its tuning frame (detector+0x134C open strings), while
	// expectedMidi is the player's physical pitch (PlayedNotePitch: standard open string + the
	// tuning offset + fret). Under a Speaker Mode transpose the two differ by a constant, so every
	// "native hears the target" test would fail by a semitone. The offset is measured from the
	// selected single note (expected - (detector open string + fret)), kept across notes when a note
	// cannot be measured, and limited to +-2 so a misread table can never shift by more.
	int nativeFrameOffset = 0;
	bool hasNativeFrameOffset = false;
	std::chrono::steady_clock::time_point nativeFrameOffsetReadAt{};
	int NativeFrameOffset()
	{
		// Measured from the tuning tables alone (the game's per-string tuning offset against the
		// detector's open string), so it is right from the first note of a session rather than
		// only after the first held target. Re-read at most twice a second.
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

		// The two gates, in the order the accessor tests them.
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
			// Which frame field carries the pitch is selected by the detector itself.
			const uintptr_t pitchField = sample.pitchMode == 2
				? RING_FRAME_PITCH_MODE_TWO
				: RING_FRAME_PITCH_DEFAULT;
			// The accessor's own wrap: one before index 0 is the last slot.
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

	// The loudest-played-note query is a candidate selector and can report unrelated
	// noisy values while the detector has no settled input. Release gating must use the
	// detector ring's current pitch, with both adjacent frames settled and the native
	// input gates passing, so silence is determined from the same evidence as scoring.
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

	// The ND sounding streak. The accepter requires the expected pitch to sit in the
	// detector's current-sounding table above the native threshold continuously for the
	// lesson duration; one lapse resets the streak, so a decaying ring that dips under the
	// bar cannot accept.
	std::chrono::steady_clock::time_point ndSoundingStreakStart{};
	bool hasNdSoundingStreak = false;
	float ndSoundingLastStrength = 0.0f;

	// Returns the current sounding strength of `pitch` in the native table, or
	// -1.0f when absent/below threshold/unreadable. Pure reads.
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

	// Diagnostic sibling of ReadNdSoundingStrength: returns the tone's raw strength from the
	// sounding table without the accept threshold, so a weak tone that is present but below
	// the bar (e.g. an open string ringing sympathetically) shows its real number in the log
	// instead of just "no". Returns NaN when the tone is not in the table at all. Reads only.
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

	// The whole sounding table (pitch, raw strength), for the same-time group baseline.
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

	// Called every tick. A newly selected single on a string the group has not used, at the
	// group's authored time, after at least one member committed, is a sibling; anything else
	// starts a new group and snapshots the sounding table as the "before the gesture" baseline.
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

	// From CompleteHeldCommit: a committed single at the group's time joins it.
	void RecordSameTimeGroupCommit()
	{
		if (selectedChordId != -1 || sameTimeGroupTime < 0.0f || expectedMidi < 0
			|| std::fabs(selectedRecordTime - sameTimeGroupTime) >= SAME_TIME_GROUP_EPSILON_SECONDS
			|| sameTimeGroupPitchCount >= static_cast<int>(sameTimeGroupPitches.size())) return;
		sameTimeGroupPitches[sameTimeGroupPitchCount++] = expectedMidi;
	}

	// The sibling's pitch sits in the sounding table above the native threshold for
	// SAME_TIME_SIBLING_CONFIRM_TICKS ticks and at least twice its strength at the group
	// baseline (absent counts as 0), so a tone left ringing from before the gesture cannot pass.
	// Not used when a committed member masks it (octave, octave+fifth, two octaves above: the
	// member's own partial) or is the same pitch; the sibling then needs its own attack as before.
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
		// The sounding table is in the detector frame, expectedMidi in the played frame. Under a
		// Speaker Mode transpose they differ, so a sibling would look for the wrong pitch and never
		// confirm. Same conversion the pick buffer uses.
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
		// Same native ring-carryover rule as the chord path: duration credit
		// requires fresh-attack evidence since this latch (spike gate or the
		// ring's own onset stamp). A previous target's ring cannot stamp an
		// attack, so it can never earn credit here.
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

	// What the chord matcher 0x4E7B30 actually evaluates: the CURRENT analysis
	// frame's level (frame+0x700, gated against the -55dB global) and its
	// per-pitch energy pairs (20 pairs at frame+0x1C, count at frame+0xBC).
	// Logged per refused evaluation so a "clean strum still refused" names its
	// own failure mode: level gate, empty pairs, or wrong pitches.
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
			// The pitch half of each pair is an int32 MIDI note, not a float.
			int32_t pitch = -1;
			float energy = 0.0f;
			TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8, pitch);
			TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8 + 4, energy);
			at += std::snprintf(buffer + at, bufferLength - at, " %d:%.2f",
				pitch, energy);
		}
		return true;
	}

	// Resolves the analysis ring for the onset-evidence scan. Same chain as
	// TryDescribeCurrentAnalysisFrame; returns false when any link is unreadable.
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

	// The pitch buffered attacks are judged against: the held target, or under flow the Armed
	// target resolved before any hold (armedExpectedMidi).
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

	// The strum a just-committed single confirmed off but did not own (StrumHandoff.hpp), if
	// this chord hold may still inherit it; 0 otherwise. Binds it to the first chord record
	// that claims it, so the dense successor's second hold reset gets the same strum and no
	// later record can.
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
			// Delivery is delayed for attack validation. Use the captured audio
			// boundary so a pre-latch strum cannot arrive later as a new attack.
			TickPickedAttackStream();
			chordHoldNeedsCaptureBoundary = !rawAttackStreamAvailable;
			// Except a strum the single-note predecessor confirmed off but did not own: the
			// boundary sits just before it and it becomes this hold's pending attack
			// (StrumHandoff.hpp, ChordAttackGate::Inherit). 0 = nothing to inherit.
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

	// How long after an attack the detector's readings are recorded into it (PickedAttack::heard).
	constexpr double ATTACK_EVIDENCE_SECONDS = 0.30;

	// Once per tick: the loudest-note reading and the sounding table, merged into every queued
	// attack still inside its evidence window. The first sample is the attack's "before" state,
	// so a pitch that was already sounding (the previous note's ring) cannot pass as a rise.
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

	// Did the detector hear `midi` during this attack? Either the loudest-note query named it at
	// some tick after the attack, or its sounding-table strength rose above the native threshold
	// to at least twice what it was at the attack. A pitch that equals the still-ringing previous
	// note must show the rise (its ring alone keeps it loudest); one the previous note masks
	// (octave, octave+fifth, two octaves up: its own partial) gets no sounding-table credit.
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

	// Bass: when the target moves on, attacks the old target refused (confirmed as another pitch)
	// are re-judged against the new one from their recorded evidence - the note played early.
	uintptr_t retargetedAttacksRecord = 0;
	void RetargetBufferedAttacks()
	{
		if (selectedRecord == 0 || selectedRecord == retargetedAttacksRecord) return;
		retargetedAttacksRecord = selectedRecord;
		// Every mode: picks for the next notes stay in flight (see KeepsMismatchedPicks).
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

	// Shadow only (PickClick.hpp): the level just before/after a single-note attack, once per
	// attack, so real picks and slides can be told apart by the pick's transient. Decides nothing.
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

	// The latest attack is re-tried every tick as its window grows. Cut-short attacks the queue kept
	// (PickedAttackQueue::Push, fast repeated picking) each get ONE evaluation once their full window
	// has passed, borrowing the next pick's audio; a failure marks them rejected so Take skips them.
	void ConfirmLatestPickedAttack(uint64_t maximumSample = 0)
	{
		const unsigned count = pickedAttacks.Count();
		for (unsigned index = 0; index + 1 < count; ++index)
		{
			auto* older = pickedAttacks.At(index);
			if (older == nullptr || older->confirmedMidi >= 0 || older->rejected || older->sampleRate == 0) continue;
			// A play-ahead pick still waiting for its target is not "failed", it is not due yet.
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
			// Play-ahead pick (see the capture): nothing to judge it against until the next note
			// is the target. Checking it now would test it against the note it followed.
			if (selectedRecord == 0 || selectedRecord == attack->awaitingTargetAfter || JudgeMidi() < 0) return;
			attack->awaitingTargetAfter = 0;
			attack->candidateMidi = JudgeMidi();
			LOG_INFO("(NBN PICK BUFFER) Play-ahead attack=" << attack->time
				<< " now judged against the next target midi=" << expectedMidi << std::endl);
		}
		// A pick made while the next note is Armed (selected, hold not yet latched) is captured
		// with candidateMidi = -1, because Armed clears expectedMidi until the hold latches.
		// Once the target is known, test the pick against it; otherwise a correct pick played
		// just as the note arrives would have to be played again. Picks before the previous
		// commit never reach the queue (pickAttackFloorSample), so this cannot hand the previous
		// note's pick to this one.
		if (attack->candidateMidi < 0 && JudgeMidi() >= 0) attack->candidateMidi = JudgeMidi();
		const int frameOffset = NativeFrameOffset();
		const int nativeRaw = QueryNativeLoudestPlayedNote();
		const int nativeMidi = nativeRaw >= 0 ? nativeRaw + frameOffset : nativeRaw;   // played frame
		const int candidates[] = { attack->candidateMidi, nativeMidi };
		// Why the target candidate was refused, once per attack and reason, and only after the
		// attack has had the audio a confirmation needs (before that "unconfirmed" just means
		// "too early"). Without this, an attack that ends Unresolved or Expired leaves no reason
		// in the log.
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
			// A play-ahead pick must not confirm as the note it followed: that note is still ringing
			// in the detector for a few hundred ms, and "confirming" as it discards the pick. Skip it
			// and retry next tick as the detector moves on.
			if (isPlayAhead && index != 0 && midi == attack->playAheadFrom) continue;
			// Bass: the loudest-note query often reads a pluck's transient partial far above the
			// instrument (e.g. 72-73 over an A-string 36), and confirming the attack as that "other
			// note" would end it before the detector settles on the real note a tick later. Nothing
			// above the bass's range is a played note: skip it and let the target be re-checked as
			// the window grows.
			if (index != 0 && IsBassArrangement() && midi > BASS_HIGHEST_PLAYABLE_MIDI) continue;
			// Bass: the loudest note right after a pluck is usually the previous note still ringing.
			// Confirming the attack as that pitch would end it before the new note surfaced, so an
			// attack is only resolved as another pitch once its evidence window is over; until then
			// the target keeps being re-checked every tick.
			if (index != 0 && IsBassArrangement() && attack->sampleRate != 0 && latestRawAudioSample
				< attack->minimumSample + static_cast<uint64_t>(ATTACK_EVIDENCE_SECONDS * attack->sampleRate)) continue;
			ResearchProtocol::RawNoteConfirmation raw;
			const double frequency = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
			if (!ResearchProbeRuntime::QueryRawNoteConfirmation(frequency, raw, attack->minimumSample, maximumSample))
				continue;
			// Bass (E1 41 Hz .. ~G3): the raw pitch checks below use 20-150 ms windows that cannot
			// separate neighbouring semitones under ~80 Hz, and a finger pluck has no pick click. On
			// bass the game's own detector hearing exactly this note stands in for the raw pitch,
			// fresh-attack, click and neighbour checks; the HFC attack that queued this pick and the
			// masking check further down still apply. Guitar (flag false) is unchanged.
			const bool nativeHearsTarget = nativeMidi == midi || AttackHeardByNative(*attack, midi - frameOffset);
			const bool bassNativeHears = IsBassArrangement() && nativeHearsTarget;
			// Guitar: the raw dominance check fails while the previous note still rings, refusing
			// picks the native detector heard correctly. The game's own detector hearing exactly this
			// note on a fresh attack stands in for it, like bass; the click, neighbour and masking
			// checks below still apply.
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
			// A pick has a click; a slide into the note does not (PickClick.hpp). Slide-ins show a
			// high-frequency rise of roughly -5..+5 dB, real picks 15+ dB. Unmeasurable (ring too
			// old) never blocks.
			{
				// A re-pick of the note that is still ringing barely lifts the level (a click of ~1 dB
				// over the ring). Its evidence is the phase break at the pitch (attackChange >= 0.5, the
				// repeatsRingingNote rule below), which a ringing note cannot fake and a slide, which moves
				// the pitch, does not produce at the repeated pitch.
				const bool rePickOverRing = midi == previousExpectedMidi && raw.attackChange >= 0.5f;
				// A slide only reaches a note along the string that was already sounding. A note on
				// another string cannot be slid into, and there the previous note's ring hides the click.
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
			// A re-pick of the note that is still ringing cannot show a level rise over its own ring.
			// raw.confirmed already requires the target to dominate its neighbours after the pick,
			// so for a repeated note a strong phase break is the attack.
			const bool repeatsRingingNote = midi == previousExpectedMidi && raw.attackChange >= 0.5f;
			if (!repeatsRingingNote && !bassNativeHears
				&& neighbourRise > 1e-3f && neighbourRise > (targetRise > 0.0f ? targetRise : 0.0f))
			{
				logTargetReject(index, "a neighbouring semitone rose more than the target", raw,
					targetRise, neighbourRise);
				continue;
			}
			// Ringer / boundary veto: raw evidence cannot tell a fresh pick from a ringing
			// neighbour's decay or a chord strum's tail, so at these moments native (loudest note,
			// or pitchNow if the loudest query lags behind a louder ring) must actually hear the
			// note. Three triggers: (1) the just-committed chord rings a tone that masks this note
			// (octave up, or an octave/fifth/2-octave below landing on its fundamental band); (2)
			// the just-committed single note masks it the same way; (3) a plain single note right
			// after a chord always needs its own attack. Legato successors (hammer/pull/tap) are
			// exempt. See OctaveCollisionGate.hpp.
			const bool chordRings = lastCommittedChordRecord != 0
				&& researchChordTonesRecord == lastCommittedChordRecord
				&& std::chrono::duration<double>(std::chrono::steady_clock::now()
					- lastCommittedChordAt).count() < COMMITTED_CHORD_RESELECT_GUARD_SECONDS;
			const bool noteRings = hasLastCommitWallClock && previousExpectedMidi >= 0
				&& std::chrono::duration<double>(std::chrono::steady_clock::now()
					- lastCommitWallClock).count() < COMMITTED_CHORD_RESELECT_GUARD_SECONDS;
			const bool chordMasks = chordRings
				&& NoteByNote::RingingTonesMaskTarget(midi, researchChordTones, researchChordToneCount);
			// Native cannot vouch here: it hears the chord's own octave-up tone as the target. An
			// attack inside the strum's tail is the chord's later strings (OctaveCollisionGate.hpp).
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
			// A single after a chord needs its own attack, scoped to the strum's tail; see
			// ChordToSingleAttackNeedsNative.
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
					// Once per attack, so every withheld attack is logged.
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
			// Confirmed, but is the attack this note's own? A strum that cut the note's own pick
			// short while native hears another pitch is left for the successor (StrumHandoff.hpp).
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
			// A fret-hand mute does not bypass the attack test. It has no reliable pitch,
			// so ConfirmChordAttack cannot judge it, but a mute is still a percussive strike:
			// it must raise broadband energy. ConfirmMutedAttack enforces exactly that, which a
			// decaying ring (e.g. a spurious low-E onset ~2s into its sustain) cannot pass.
			// The pitched chord path is unchanged.
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
						// Bass chords (mostly power-chord dyads at 41-150 Hz): the per-tone 50 ms
						// harmonic fit cannot separate the tones, so on bass the attack is taken
						// pitch-free like a muted strum; the native vote/matcher decides the pitches.
						changed = (fretHandMuted || IsBassArrangement())
							? NoteByNote::ConfirmMutedAttack(frames, window, snapshot.sampleRate)
							: NoteByNote::ConfirmChordAttack(frames, window, snapshot.sampleRate,
								candidates, candidateCount);
						// A plain strummed chord (no hammer/pull/tap authored) must not accept from a
						// fret-hand slam onto still-ringing strings: the HFC test above cannot tell it
						// from a strum, but its loudness envelope (damped run-up, weak re-excitation)
						// can. See StrummedChordAttack.hpp; the envelope is logged either way so the
						// thresholds can be retuned.
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
			// Bass single notes: the sustain filter's 50 ms change fit cannot resolve a semitone at
			// 41-98 Hz and refused real plucks; the pick is instead confirmed against the native
			// detector in ConfirmPickedAttack, which re-tries it as the window grows.
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
			// The HUD's accept readout (which detectors passed the last note) is not cleared
			// here: in fast playing the next attack lands within ~200 ms and would wipe the
			// green before it could be seen. It runs its own hold/fade timer and the next
			// accept replaces it.
			mlRescueRecord = 0;
			enhancedRescueFeedback = {};
			attack.minimumMlSample = ResearchProbeRuntime::GetMlAudioSampleIndex();
			// Bass play-ahead: a pick made while the current note is already secured (commit /
			// release / rebuild phases) is for the next note. Taking the secured note as its
			// candidate would let it "confirm" as that still-ringing note and then be discarded
			// as the wrong pitch when the next target arrives. Such a pick waits for the next
			// target and is judged against it. Bass only for now.
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
				// Push drops the older attack. If it never had the audio a confirmation needs,
				// remember it: this new attack may be the successor's strum (StrumHandoff.hpp).
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

	// Sequence inference. A pick on the same string as the still-ringing previous note is
	// often drowned by it: neither the raw check nor the native detector hears the new pitch,
	// the pick stays unconfirmed, and the player carries on with the following notes, which
	// are heard. When an attack after the last consumed pick went unconfirmed (or confirmed
	// only as the ringing previous pitch) and a later attack confirmed as the next note's
	// pitch, that order is the evidence the target was played: it is accepted from the
	// earlier attack, and the later one stays queued for the next note. A pick confirmed as
	// some other pitch is a wrong note and never qualifies.
	bool TryInferFromFollowingPick();

	bool TakeBufferedPick()
	{
		// An accepted pick that does not match the target could never commit (IsAcceptedOnset) and would
		// block every later pick: drop it so the next pick is judged.
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

	// The onset-flag half of the attack evidence (the spike gate is the other
	// half; see sawSpikeDuringHold). Walks every ring frame written since the last
	// scan and latches evidence when a post-latch frame carries the ring's own onset
	// flag at a credible level. Also runs while a repeated picked note waits to arm.
	void TickOnsetEvidence()
	{
		uintptr_t ring = 0;
		int32_t writeIndex = 0;
		int32_t capacity = 0;
		if (!TryResolveAnalysisRing(ring, writeIndex, capacity)) return;
		if (!hasOnsetScanAnchor)
		{
			// The latch could not anchor (chain unreadable at that moment); anchor on
			// the first readable tick instead. Frames before this tick are lost to the
			// scan, which only delays evidence, never falsifies it.
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
				// One line per tick so a loud ring's many onset frames cannot flood the
				// log; the measured rise is what calibrates ONSET_EVIDENCE_RISE_DB.
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


	// The clock every decompiled window leaf gates on, resolved through the same
	// chain as the gate sample above.
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

	// Which condition is refusing, named in the accessor's own order of tests.
	//
	// "none-visible" is deliberately not "none". Three further conditions live inside
	// 0x004E4B60 and 0x004DD420 (locating the onset frame, requiring it to be at least two
	// frames old, and comparing two extracted analysis frames) and none of them is
	// observable without calling into the game, so everything visible passing does not prove
	// the query would return a note.
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

	// Defined later in this TU; declared here so the hold-phase shadow sampler in
	// LogDetectorGates can reach them.
	void LogTier0ShadowEvidence(int expectedMidi, const char* context);
	void LogTier1ShadowPitch(int expectedMidi);
	// Tier-0 positive raw confirmation (defined with the rest of tier-0 below);
	// declared here so the reattack tick can consult it.
	bool Tier0ConfirmsExpected(int expectedMidi);
	extern volatile bool g_tier0Enforcement;

	// The wall-clock tick of the last detector level spike, in any gate phase. A pick
	// played during the dense rebuild happens while hit decisions are blocked, and by the
	// time the next hold establishes its attack is history, but its string is still
	// ringing. This stamp lets hold establishment know an attack just happened and open
	// the raw-confirmation window on the sustain.
	ULONGLONG g_lastAttackSpikeTick = 0;
	// Per-note pipeline stamps: armed->adopt is the detect+accept+commit+release+rebuild
	// span for one note; adopt->armed is the native transition (play packet + relatch).
	// Logged as (NBN FLOW) per note.
	ULONGLONG g_flowAdoptTick = 0;
	ULONGLONG g_flowArmedTick = 0;

	// One line per second while a hold is unsatisfied, naming the refusing condition first
	// because read-process-console truncates at the console buffer width.
	//
	// `force` bypasses the interval so the moment of failure (a safety release) is always
	// captured, whatever the interval happened to be doing.
	void LogDetectorGates(const char* context, bool force)
	{
		const auto now = std::chrono::steady_clock::now();

		// The sample is read on every call (the reads are a handful of SEH-guarded
		// peeks), because spike detection needs the per-tick level even when the
		// interval logger would stay silent.
		DetectorGateSample sample;
		const bool sampleReadable = TryReadDetectorGates(sample);
		if (sampleReadable)
		{
			lastStateGateSample = sample;
			hasLastStateGateSample = true;
			lastStateGateSampleAt = now;
		}

		// Dedupe unstick. If the controller consumed the expected pitch's edge without
		// committing (e.g. a rollback fault landed in between), the value-dedupe suppresses
		// every repluck of the same pitch indefinitely. If a hold spends a run of ticks
		// refusing "already-consumed" for the expected pitch with both gates passing, reset
		// the dedupe global so the next query re-reports it.
		//
		// Spike-gated: the reset additionally requires attack energy during the streak. A
		// strong sustain can hold both gates open past the threshold (a just-released bend
		// rings loudly at exactly the pitch parked in the dedupe), and an ungated reset would
		// re-report that ring and auto-commit the follower. A decaying ring cannot spike the
		// level meter; a stuck player replucking does, every attempt.
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
				// spikeBurstTicksRemaining is refreshed by the spike detection below;
				// reading last tick's burst here only shifts the evidence by one tick.
				if (spikeBurstTicksRemaining > 0) streakSawSpike = true;
			}
			else
			{
				consumedStreakTicks = 0;
				streakSawSpike = false;
			}
			// Unstick on the first stuck tick that carries spike evidence. Both conditions here
			// already prove a genuine fresh attack of the wanted note: stuckOnExpected requires
			// the pitch to have settled onto expectedMidi (so the transient is already past), and
			// the spike gate still blocks a decaying ring / bend-release carry-over (those cannot
			// spike). A longer wait lets the ring decay below the level gate before the streak
			// accrues, and drops fast same-pitch repeats when the note advances first.
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
				// Chord holds: attack evidence for the held chord's acceptance.
				// Cleared at hold establishment, so only a strum that happened during
				// this hold can satisfy the chord gate; the previous strum's ring
				// (and its spike) belong to the previous target. The minimum hold age
				// exists because the previous strum's envelope is still rising or
				// wobbling when the next hold latches a few hundred ms later, and such
				// a wobble can read as a spike and pass a same-chord ring through.
				if (gatePhase == GatePhase::Holding && holdTickCount >= 8)
				{
					sawSpikeDuringHold = true;
					// The strict level-only flag the chord matcher accept gates on: set
					// here (a genuine level jump) but not in the onset-flag scan, so a
					// ring's onset stamps cannot fake it.
					sawLevelSpikeDuringHold = true;
				}
				spikeBurstTicksRemaining = DETECTOR_SPIKE_BURST_TICKS;
				g_lastAttackSpikeTick = GetTickCount64();
				if (!IsPlainPickedTarget())
				{
					// detectionFeedback (the accept readout) is left to its timer; see TickPickedAttackStream.
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

		// The onset-flag half of the attack evidence, cursor-scanned so no ring
		// frame is missed between scoring ticks. Unthrottled like the spike tracker
		// above; skipped once evidence is latched (it clears at the next hold).
		// Runs for every hold since the ND acceptance gates on fresh-attack evidence
		// (the game's own attack stamp is the native answer to ring carryover).
		if (gatePhase == GatePhase::Holding && !IsPlainPickedTarget() && !sawSpikeDuringHold)
		{
			TickOnsetEvidence();
		}

		// Hold-phase shadow sampler: the accept/reject shadow points never fire for a wrong
		// note (the upstream level/quality gates and the bin matcher refuse it before either
		// is reached), so the reject side of the tier-1 dataset would stay empty. While a hold
		// has live input, sample the raw-route evidence and the companion's opinion with the
		// expected note alongside so offline comparison sees the wrong-note moments too.
		// Independent of verboseTrace: this is the tier-1 dataset, not diagnostic output, and
		// its own throttle keeps it to ~3 lines/s.
#if defined(_DEBUG)
		if (gatePhase == GatePhase::Holding && NbnInputPresent())
		{
			static ULONGLONG lastHoldShadowTick = 0;
			const ULONGLONG nowShadowTick = GetTickCount64();
			if (nowShadowTick - lastHoldShadowTick >= 300)
			{
				lastHoldShadowTick = nowShadowTick;
				LogTier0ShadowEvidence(expectedMidi, "HOLD");
				LogTier1ShadowPitch(expectedMidi);
			}
		}
#endif

		// Live native-vs-ML agreement for the corner HUD and the comparison CSV. Unlike the
		// tier-0/tier-1 diagnostic shadow above this is not _DEBUG-gated, so the A/B is visible
		// in Release. Same ~3 Hz throttle so it stays cheap.
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
			// Verbose-gated: the refusal line is per-tick diagnostic output. Off, it never prints.
			// On, it is additionally input-gated so a guitar set down mid-session (idle ~ -80 dB)
			// does not flood "refusing=level" and scroll real events out of the ~9000-row console
			// buffer. The sample read above still runs every call, so spike detection is
			// unaffected; only this emit withdraws. A forced call (an event site) always logs.
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

		// Compact companion line. The full line above wraps at the console width, which
		// truncates the fields needed for same-pitch re-attack analysis (dedupe, pitchNow,
		// pitchPrev). This one stays under ~90 characters so every field survives; the
		// refusal tag is repeated so the line stands alone.
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

	// The spike re-attack acceptance tick (see REATTACK_WINDOW_TICKS). Called once per
	// tick in the holding and input-release-wait phases; returns expectedMidi when the
	// pick is accepted, -1 otherwise. Reads the detector directly and never calls either
	// native query, so it cannot consume the edge or disturb the dedupe global. The window
	// is opened by a level spike here, or externally by an onset whose pitch was
	// attack-transient garbage.
	//
	// allowAccept=false keeps the level/window/streak tracking ticking but never
	// accepts: the input-release wait uses it for a same-pitch successor, where the
	// previous note's own ring reads the expected pitch with passing gates and only
	// the normal release confirmation can tell the two apart.
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
		// After a chord, a legato successor (a hammer-on on a chord-fretted string) must not
		// be satisfied by a chord tone that is merely still ringing at the successor's pitch.
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

		// Attribution guard: a spike observed in a hold's first ticks is the predecessor's
		// attack envelope still rising across the latch, not a re-pick of this target (e.g.
		// a chord's strum tail spiking while the chord ring carries the successor single's
		// pitch at high quality). A genuine re-pick lands later than the transient.
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

		// Quality gate plus pitch agreement from the ungated current-note field
		// (bend-window aware: see MatchesPickPitch). The absolute level gate is
		// deliberately not required here: the window only opens on a spike (or a
		// fired onset), which is level evidence in itself (a decaying ring never
		// spikes), and a quiet signal chain can sit below the game's -55 dB gate for
		// every pick.
		// The detector's read is -1-bin biased and lags the freeze, so an early pick
		// of the correct successor routinely reads expected-1 here and the exact-pitch
		// match never builds a streak. Fall back to the raw route: Tier0ConfirmsExpected
		// resolves the true fundamental at cent resolution with the full sub-harmonic
		// guard set, so a one-fret wrong or a lower note's harmonic cannot confirm. Only
		// inside the spike/onset-opened window, so attack evidence is still always required.
		const bool detectorMatches = sample.passesQuality
			&& MatchesPickPitch(sample.currentNote);
		// Tier-0 covers exactly one native weakness: the detector's -1-bin bias / lag on an early
		// pick (it reads expected-1, or has no settled pitch yet). It does not get to overrule a
		// confident native read of some other pitch: tier-0 can report the target well over its
		// neighbours while the note played is actually one fret higher (with ML agreeing with
		// native). So a quality-passing native read that is neither the expected pitch nor
		// expected-1 refuses the raw route; the bias case it was built for still goes through.
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
		// A raw confirmation needs a 2-tick streak, not 3: Tier0ConfirmsExpected is a
		// single strong measurement (>=1e-5 target, 5x over neighbours, sub-harmonic
		// guards) where the detector match is a wobbly per-tick read; the streak there
		// exists for that wobble. One tick shaved is one tick less lag per flowed note.
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
		// Observe the native hit decision for bend notes with a real authored bend amount, to
		// see whether Rocksmith's own decision would judge a bend. This fires only in the
		// non-controlling path (HitDecisionDetour), so it captures the native decision as the
		// bend passes in normal flow, not under the freeze hold; an under-freeze test would
		// need a speculative native call, which has side-effect risk.
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
		if (next < 0 || next == expectedMidi) return false;
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
				// It counts as this target (IsAcceptedOnset checks confirmedMidi == expectedMidi). Left at
				// -1 the note would be accepted but could never commit, and no other pick could replace it.
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
		// The bend amount rides every candidate event, read fresh from the record so it
		// is correct even for CandidateChanged events emitted before ResolveBendAcceptance
		// runs. The cue needs it because the frozen hold repaints the bend visual as a
		// plain note, leaving the player no way to see how far to bend.
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

	void ClearSelection()
	{
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
		// previousSelectedString is deliberately not cleared here.
		//
		// It records which string is still ringing, which decides whether a no-pick pull-off
		// or tap is physically playable and therefore whether it may be a target of its own.
		// Clearing it when a selection ends would make the next fresh selection see -1, fail
		// the skip test's >= 0 guard, and split a legato expression into separate targets. An
		// expression may be any length (for example fret 15 to 13 to 12), and the run is defined
		// only by the chart's own legato flags plus same-string continuity.
		//
		// It is reset in ResetBootstrap instead, where the section really does change and
		// no string can still be ringing from the previous selection.
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
		// A spike window opened by the previous note's own accepted pick must not
		// survive into this hold: a same-pitch successor would accept itself from
		// its predecessor's still-ringing attack. The level tracker itself persists
		// (a pick across the boundary is still a real pick).
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

	// The song speed (1.0 = normal) from what was asked for: the speed flow requested this pass, else
	// the player's SPEED slider through the game's curve (HostApi v12, honours Linear Riff Repeater and
	// speed above 100), else the transport measurement. The transport measurement reads high against
	// the real speed, so it is only the fallback and is logged against the slider.
	double CurrentSongSpeed()
	{
		if (flowSpeedPassRequestedPercent > 0.0f) return flowSpeedPassRequestedPercent / 100.0;
		const float player = ResearchProbeRuntime::GetPlayerSpeedRealPercent();
		if (player >= 1.0f && player <= 400.0f) return player / 100.0;
		return hasMeasuredSongSpeed ? measuredSongSpeed : 1.0;
	}

	// The 0.053 engine compensation is a real-time allowance (the gem reaches the strike line that much
	// before the authored time on screen), so in song time it shrinks with the song speed. Subtracting
	// it unscaled freezes slow songs early: the frozen gem rests short of the line while the relatched
	// fretboard marker sits on it (a doubled note).
	// The transport rate decides where the gem sits, so the measurement leads here; the asked-for
	// speed (CurrentSongSpeed) is only the fallback before the first measurement. The rate is the
	// slope over the last ~0.25 s of free-running ticks: after a loop restart (and after a hold
	// release) the game ramps from ~100% down to the set speed over ~2 s, and an averaged measurement
	// lags far above the real rate.
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
			// A jump (rollback, rebuild, loop) or a stall starts a fresh run.
			if (updateTime < last.time || updateTime - last.time > 0.5f
				|| now - last.at > std::chrono::milliseconds(300)) transportSampleCount = 0;
			else if (updateTime == last.time) return;
		}
		transportSamples[transportSampleNext] = { now, updateTime };
		transportSampleNext = (transportSampleNext + 1) % transportSamples.size();
		if (transportSampleCount < transportSamples.size()) ++transportSampleCount;
	}

	// The newest live rate, kept for holds computed while the transport is held (dense successors and
	// relatched holds), where the lagging average would offset the hold.
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

	// The settled speed, not the live one. Every release (and loop restart) restarts PlayerSong
	// and the game ramps from ~100% back down to the set speed over ~2 s; a freeze lands during that
	// ramp, and once frozen the game finishes the ramp, so the gem would settle short of the line by
	// the difference. The settled speed is what the game ramps to: this pass's flow request or the
	// player's slider through the game curve (slider 1 = 25%). Measurements only when neither
	// is known.
	float HoldCompensationSeconds()
	{
		double speed = 0.0;
		// Fitted offset: compensation * (2r - 1) with r the settled real speed (game curve):
		// 0.053 s before the note at 100%, on the note at 50%, 0.026 s after at 25%. A negative
		// result holds after the note's time, still inside its native window (+0.3 s).
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
		// Only a freely running transport measures the speed: holds pin it, releases and dense
		// rebuilds jump it, rollbacks reverse it.
		// PostRelease is left out: the release restarts PlayerSong and the transport ramps back to
		// speed, which reads high and moves the hold off the line.
		if (gatePhase != GatePhase::Armed && gatePhase != GatePhase::Idle)
		{
			songSpeedSampleTime = -1.0f;
			transportSampleCount = 0;
			return;
		}
		PushTransportSample(updateTime);
		// The Armed target's boundary follows the live rate every tick until the transport reaches it.
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
		// Steady average: the hold compensation is only as right as this speed, and a single
		// ~0.3 s sample swings by several percent.
		measuredSongSpeed = hasMeasuredSongSpeed ? previous * 0.8 + ratio * 0.2 : ratio;
		hasMeasuredSongSpeed = true;
		// The Armed target's boundary was set at selection, possibly from an older speed.
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

	// Native freeze announcement. The game's own GE_FreezeOnTag does the same mechanical
	// freeze (Stop_TMusic via PlayerSong vtable+0x24, owner vtable+0x50 five-clock set)
	// and then announces it: dispatches the tag through 0x407840 and sets the
	// frozen-on-tag byte at owner+0x5E3, which native systems read to enter the
	// freeze presentation. The controller otherwise does only the mechanical half;
	// this writes the flag around owned holds.
	constexpr uintptr_t OWNER_FROZEN_ON_TAG = 0x5E3;
	// Default off: the freeze-family fields (+0x5C8..+0x5E8) belong to the frozen-object
	// class, and GamePlaysongLAS's allocation is likely smaller, which would make every
	// freeze-flag write an out-of-bounds heap write. The one-shot owner-size diagnostic on
	// the first hold reports the size; freeze-flag-on re-enables it for study only.
	volatile bool isNativeFreezeFlagEnabled = false;

	// Native-drive diagnostics. Pure reads, no calls, no writes: at every hold
	// establishment and release the controller reports the state the native
	// freeze/resume swap depends on:
	//   - the GE handlers' mode-object resolve chain ([[0x0135F54C]+0x10]+0x50)
	//     and whether it matches the tracked owner (it resolves GamePlaysong in
	//     lessons; expected dead or foreign in Learn A Song);
	//   - the scheduler global 0x0135F5AC the frozen-span destructor shifts
	//     (0x57EB00 op 3 = entry.time += frozenDuration, the anti-sweep);
	//   - the owner's freeze-family fields the frozen-span object maintains
	//     (+0x5C8 tag string begin/end at +0x5D8/+0x5DC, +0x5E4 resume flag,
	//     +0x618 freeze-start-time double), to compare what the mechanical
	//     freeze leaves them in versus what GE_FreezeOnTag would.
	constexpr uintptr_t NATIVE_SCHEDULER_GLOBAL = 0x0135F5AC;
	constexpr uintptr_t OWNER_FREEZE_TAG_STRING = 0x5C8;
	constexpr uintptr_t OWNER_FREEZE_TAG_BEGIN = 0x5D8;
	constexpr uintptr_t OWNER_FREEZE_TAG_END = 0x5DC;
	constexpr uintptr_t OWNER_RESUME_FROM_TAG = 0x5E4;
	constexpr uintptr_t OWNER_FREEZE_START_TIME = 0x618;

	// The lesson engine's own chord panel: GE_ShowChordDisplay's implementation.
	// __cdecl(int index); resolves the GamePlaysong owner itself, bounds-checks the index
	// against the 0x48-stride chord template array on the chord-display component at
	// owner+0x78, copies the template and sets the visible flag. The index space equals
	// the SNG chordId. This is the presentation half a real lesson pairs with the
	// frozen-on-tag flag.
	// Calling 0x405C00 directly crashes in Learn A Song (AV inside FUN_006106C0+7): it
	// resolves a "ge_game" registry entry that exists only in lesson mode. Writing the
	// template against the scoring owner's +0x78 instead corrupts the heap: the object at
	// the LAS owner's +0x78 is not the chord-display component but a smaller allocation,
	// so the 0x48-byte write runs into its heap neighbour's free-list links. Therefore:
	//   1. owner+0x78 is only the chord display on the object 0x405C00 itself
	//      resolves: wrapper = [[[0x0135F54C]]+0x10]+0x50], type-checked as
	//      GamePlaysong. ShowNativeChordPanel resolves through that same chain
	//      and ignores the scoring owner entirely; when the chain is dead
	//      (Learn A Song) the panel is not driven and nothing is written.
	//   2. The toggle defaults off: the panel only exists in lesson mode, and
	//      Learn A Song gets its chord identity from the overlay.
	// The +0x1DC sub-object branch is deliberately skipped.
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

	// Schedule-shift experiment. The frozen-span mode object's destructor compensates
	// the engine for the frozen time: it calls the scheduler service at 0x57EB00
	// with op 3 (entry.time += delta) so nothing downstream reads as "missed"
	// when the transport resumes (the anti-sweep mechanism). The call, from the
	// destructor's disassembly (0x471B7B..80):
	//   EAX = 4 (scheduler entry id), EDI = 0 (sub-list index),
	//   stack: push elapsed (double), push 3, push scheduler; RET 0x10
	//   (callee-cleaned). Guarded: scheduler global must be non-zero and the
	//   elapsed span sane. Elapsed uses wall-clock (the native path reads a
	//   global clock member; a steady_clock span is the same unit at this
	//   precision).
	// Default off; toggle with schedule-shift-on|off. One native call per release,
	// only while the toggle is on.
	volatile bool isScheduleShiftEnabled = false;
	uint64_t scheduleShiftCount = 0;
	// When the transport was last stopped (Stop_TMusic latch at establish or a
	// dense relatch); the release-side shift compensates by now-minus-this.
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
		// Constants inlined as literals (inline asm cannot see constexpr symbols):
		// eax = 4 = NATIVE_SCHEDULE_SHIFT_ENTRY, push 3 = OP_ADD.
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

	// The game's own spectral harmonic chord matcher (0x4E6E90), the core behind Lua's
	// IsChordBeingPlayed. ABI: __stdcall(int* det, int* midiArray, int count, int
	// frameOffset, char flag) -> bool in AL, ret 0x14 (callee cleans the 5 args, so ESP
	// must not be touched after the call); EBX/ESI/EDI/EBP preserved. It matches the
	// chord's pitches against the raw analysis spectrum + per-note harmonic templates, so
	// unlike the sounding table it is immune to tone masking, and it has no
	// detector-clock/window gate, so the frozen transport does not desync it. det/state
	// and the two amplitude gates are resolved exactly as the game's caller FUN_00406460
	// does: det=*(*(*(0x135F57C)+0x10)+0x08), state=*(det+4). Returns 1 = chord sounding,
	// 0 = not sounding / input too quiet, -1 = could not run.
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
		// The game's own precondition: only match when real input energy is present. The
		// fields are floats; the thresholds are doubles (fcomp qword). Promote to compare.
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
			// __stdcall, args right-to-left: flag, frameOffset, count, midiArray, det.
			// ret 0x14 cleans all five; do not adjust esp afterward.
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

	// The native single-note spectral matcher (0x4E7B30), the matcher the game's own
	// single-note onset scan (0x4E6A70) calls per onset frame. ABI: bool __stdcall(int* det,
	// int* expectedMidiBuf, int flag, int frameOffset), ret 0x10 (callee cleans the 4 args;
	// do not fix esp after the call), returns AL, preserves EBX/ESI/EDI/EBP.
	// det=root->arr->det (state=det+4) resolved exactly as the chord matcher does.
	// expectedMidiBuf is a 6-int array; entries < 0 are ignored, so a lone note passes
	// {expectedMidi,-1,-1,-1,-1,-1}. This runs the game's own harmonic analysis of the
	// frame, so unlike a raw ring-frame pitch read a one-frame attack transient cannot
	// graze a neighbour and a correctly played note is not lost to a noisy attack frame.
	// It is octave/open-string aware but has no +/- semitone tolerance.
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
			// __stdcall, args right-to-left: frameOffset, flag, expectedBuf, det.
			// ret 0x10 cleans all four; do not adjust esp afterward.
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

	// Reads the newest analysis-ring frame's capture timestamp (frame+0x730, live audio
	// seconds; it keeps advancing while the transport is frozen). Used to stamp the hold
	// latch and as the "now" the native scan walks back from.
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
		// Only trace scans that actually examined frames. The chord matcher runs on every hold
		// tick, so an empty scan (no attack window, framesMatched == 0) would otherwise flood
		// the console every 100 ms for the whole hold.
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

	// Shadow only (StrumChordPresence.hpp): were the chord's played tones in the raw audio
	// right after this strum? Measured once per attack; decides nothing. Returns -1 while the
	// window is not captured (or cannot be), 0 no, 1 yes.
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

	// Strum Ledger, stage 0 (StrumLedger.hpp): per-tone rise at each chord strum, read at
	// 65/85/120 ms after the attack and logged with what the current rules did. Decides
	// nothing. Driven from HandleAfterUpdate so the reads complete even after the hold has
	// accepted and moved on.
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
		// A ring that no longer holds the strum is not a verdict: leave it unmeasured (-1).
		finishedStrumLedgerVerdict = ledger.strummedByLate ? 1 : ((ledger.readsDone & 4u) ? 0 : -1);
		// The 65 ms wrong-strum flag is logged but does not veto: it catches slides (which the
		// strum-rise gate blocks before a strum reaches the ledger) and also fires on clean
		// strums whose later strings have not developed yet.
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

	// 1 strummed, 0 not strummed (every read done), -1 not decided yet / unmeasurable.
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
		// Bridge, not RawPitchVerifier::QueryNoteConfirmation (stubbed in the probe): route through
		// the host like ConfirmLatestPickedAttack does, so unison confirms in a loaded probe too.
		ResearchProtocol::RawNoteConfirmation evidence;
		const double frequency = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
		if (!ResearchProbeRuntime::QueryRawNoteConfirmation(frequency, evidence, attackSample)
			|| !evidence.confirmed) return false;
		LOG_INFO("(NBN RAW UNISON) Confirmed pitch=" << midi << " samples=" << attackSample
			<< ".." << evidence.endSampleIndex << " record=0x" << std::hex << selectedRecord
			<< std::dec << std::endl);
		return true;
	}

	// Power chord: every probe tone (the fifth, PowerChordConfirmation.hpp) must be confirmed at its
	// own pitch in the raw audio with a fresh attack (change >= 0.2, the pick stream's bar), so a
	// root re-strummed over a ringing fifth, whose phase just continues, does not count. Measured
	// once per strum, as soon as the verifier has the audio it needs after the attack (150 ms
	// below 330 Hz); the snapshot still covers the pre-attack reference until ~240 ms.
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

	// Offline-capture diagnostic: logs the raw spectral peak list at each fresh-onset frame
	// during a single-note hold, labelled with the expected pitch, so the accept rule can be
	// validated offline against real correct-vs-wrong plays. One line per pluck: expected,
	// frame level, and the loudest peaks {midi:energy}. Passive: it does not affect acceptance.
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
		// Log the native clock-independent loudest-note query (NDGetLoudestPlayedNote 0x48E5F0)
		// next to the gate's dominant peak (peaks[0]), to compare the native query and the
		// dominant spectral peak for correct vs wrong frets. NDGetLoudestPlayedNote has no onset
		// dedupe side effect, unlike NDGetOnsetNote, so reading it here is passive.
		const int ndLoudest = QueryNativeLoudestPlayedNote();
		LOG_INFO("(NBN CAPTURE) exp=" << expected << " ndLoudest=" << ndLoudest
			<< " domPeak=" << (found > 0 ? peaks[0].midi : -1) << " lvl=" << std::fixed << std::setprecision(1)
			<< level << " peaks=[" << ss.str() << "]" << std::endl);
	}

	// Tier-0 shadow logging: alongside every single-note accept and (throttled) reject, log
	// the raw-route Goertzel evidence for the expected note (cent-resolution powers measured
	// upstream of the engine's semitone bins, via the host's RawPitchVerifier over the same
	// GetBuffer tap). Shadow only: it never gates the decision. expectedMidi is already the
	// detector-frame pitch, i.e. the frame of the audio the observer hears, so the
	// equal-tempered frequency is the right target directly.
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

	// Tier-1 shadow logging: next to the tier-0 shadow lines, log what the 64-bit companion
	// pitch service currently hears, from the same accept/reject moments. Shadow only: it
	// never touches the decision. ml is a float midi in the observed route frame (the
	// companion hears post-shifter audio; the applied shift travels in the shared header,
	// so the comparison against exp is done offline). Throttled because the freeze
	// re-evaluates every tick and the reject path fires per tick while a wrong note rings.
	void LogTier1ShadowPitch(int expectedMidi)
	{
		if (expectedMidi < 0) return;
		if (!ResearchProbeRuntime::IsMlPitchServiceAlive()) return;
		// No internal throttle: every call site paces itself (accept is a rare event,
		// reject and the hold-phase sampler are throttled where they fire), and a shared
		// throttle here would let the 300ms HOLD sampler starve the accept-moment lines.
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

	// Per-session native-vs-ML comparison CSV. One row per sample, so the A/B can be scored
	// offline while the corner HUD shows it live. Lives beside nbn-trace.log in
	// <game>\RSModsResearch\; a fresh probe load starts a new session block via the header.
	void AppendDetectionCompareCsv(int technique, int expectedMidi, int nativeMidi,
		bool nativeMatch, bool mlHadOpinion, int mlMidi, bool mlMatch, int code)
	{
#if defined(RSMODS_PUBLIC_RELEASE)
		// Master builds write no comparison CSV (same reasons as the trace file above).
		(void)technique; (void)expectedMidi; (void)nativeMidi; (void)nativeMatch;
		(void)mlHadOpinion; (void)mlMidi; (void)mlMatch; (void)code;
		return;
#endif
		static std::ofstream csv = []()
		{
			char modulePath[MAX_PATH] = {};
			HMODULE module = nullptr;
			std::string path = "RSModsResearch\\nbn-detection-compare.csv";
			if (GetModuleHandleExA(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
						| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(&AppendDetectionCompareCsv), &module)
				&& GetModuleFileNameA(module, modulePath, MAX_PATH) != 0)
			{
				std::string full(modulePath);
				const auto lastSlash = full.find_last_of('\\');
				if (lastSlash != std::string::npos)
				{
					const auto parentSlash = full.find_last_of('\\', lastSlash - 1);
					if (parentSlash != std::string::npos)
						path = full.substr(0, parentSlash + 1) + "nbn-detection-compare.csv";
				}
			}
			std::ofstream out(path, std::ios::app);
			if (out.is_open())
				out << "# session " << GetTickCount64()
					<< "\ntickMs,technique,expectedMidi,nativeMidi,nativeMatch,mlHadOpinion,mlMidi,mlMatch,result\n";
			return out;
		}();
		if (!csv.is_open()) return;
		const char* techniqueName = technique == 1 ? "chord" : (technique == 2 ? "bend" : "single");
		const char* result = code == 1 ? "agree" : (code == 0 ? "disagree" : "one-sided");
		csv << GetTickCount64() << ',' << techniqueName << ',' << expectedMidi << ','
			<< nativeMidi << ',' << (nativeMatch ? 1 : 0) << ',' << (mlHadOpinion ? 1 : 0) << ','
			<< mlMidi << ',' << (mlMatch ? 1 : 0) << ',' << result << '\n';
		csv.flush();
	}

	// Sample both detectors passively while a hold is live and record whether they agree that
	// the expected note (or chord tones / bent top) is being played. Native's opinion comes
	// from the game's own loudest-played-note query (no onset dedupe side effect); ML's from
	// the companion pitch service. Neither read touches the acceptance decision. Runs in all
	// builds.
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

	// Tier-0 enforcement: a conservative veto layered on top of the semitone-bin gate. An
	// accept is refused only when the raw route shows a neighbour pitch strongly and
	// positively dominating the expected fundamental, the +/-1-fret wrong the bins cannot
	// see (their reads collapse to exp/exp-1 either way). No evidence, silence, or
	// ambiguity never vetoes: correct notes carry a large margin (most notes separate
	// +/-1 semitone at >=10x raw power), so this cannot starve them, and the freeze
	// re-evaluates every tick anyway.
	volatile bool g_tier0Enforcement = true;

	// Cent-tolerant target band: the 0.15s rectangular window gives a ~6.7 Hz main lobe,
	// which above ~400 Hz is narrower than ordinary pitch reality (fret-12..15 intonation
	// and vibrato run +/-15-25 cents, ~+/-7-11 Hz at 660-800 Hz), so a correctly played
	// high note can read a near-empty target bin. The target is therefore the max of three
	// probes at f and f*2^(+/-25 cents); the +/-1/+/-2 neighbours are taken from the center
	// query only. A wrong note a full semitone away gains nothing from a +/-25-cent
	// widening, so the veto/rescue discrimination thresholds (3x/5x) are unchanged.
	constexpr double TIER0_TARGET_DETUNE_RATIO = 1.0145453349375237; // 2^(25/1200)

	// Frequency-adaptive analysis window. The rectangular window's main lobe is
	// ~1/windowSeconds Hz; at 0.15s that is ~6.7 Hz, wider than a semitone below ~130 Hz
	// and wide enough at 100-200 Hz that a loud +/-1 neighbour leaks enough into the target
	// probes to defeat the "empty target" veto signature. Longer windows below G3 narrow
	// the lobe to ~3.3 Hz, separating semitones down to the open low E. The freeze waits
	// for the player, so the extra look-back costs nothing that matters here.
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

	// Log-only: compare window lengths on one snapshot before evaluating attack-pitch hypotheses.
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

	// Wrong-note latch state, shared by the veto and the rescue: a recent veto for this
	// midi means wrong-note evidence was positively seen, and every acceptance path must
	// then demand stronger positive evidence than usual. Single slot, since NBN
	// serializes notes.

	// One +/-25-cent-widened power probe at an arbitrary frequency, the shared shape of
	// every sub-harmonic guard: a lower note played slightly off-center shifts its harmonics
	// by the same cents, and an under-read sub probe weakens exactly the guard that keeps
	// a lower note's harmonic from masquerading as the expected note.
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

	// Fill the low/slight part of a bend the native tracker misses. The motion-note tracker
	// only registers once the bend is already ~0.6 semitone up and drops out intermittently,
	// and the integer detector reports out-of-band values there, so a slight bend does not
	// move the needle and it jumps when the tracker finally catches. The raw tap measures
	// the sounding pitch anywhere; a probe plus parabolic peak refinement gives a fractional
	// pitch. Called only on a tracker miss during a bend. Returns false on silence or no
	// clear tone, so the needle holds rather than chasing noise.
	bool TryEstimateRawBendPitch(double baseMidi, double targetMidi,
		float& outMidi, float& outConfidence)
	{
		outMidi = -1.0f;
		outConfidence = 0.0f;
		if (!std::isfinite(baseMidi) || !std::isfinite(targetMidi) || targetMidi <= baseMidi)
			return false;

		// Throttle: recompute at most every ~40 ms and reuse the last estimate between, so the
		// needle updates ~25x/s (the host low-pass smooths it) at a fraction of the per-frame
		// cost.
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

		// One probe centered at mid-bend: its built-in target / +-1 / +-2 powers span the whole
		// bend for depths up to 3 semitones, so a single QueryRawToneEvidence covers it. Parabola-
		// refine the power peak for a sub-semitone pitch.
		const double center = std::floor((baseMidi + targetMidi) * 0.5 + 0.5);
		const double freq = 440.0 * std::pow(2.0, (center - 69.0) / 12.0);
		// Half the tier-0 window. Tier-0 wants 150-300 ms to separate a steady note from its
		// neighbours, but a bend is a moving pitch: that window averages in the lower part of
		// the climb, so when the string reaches target the estimate still reads ~0.3-0.5
		// semitone low and the fractional veto refuses an on-pitch bend. Half the window still
		// resolves semitone-spaced bins (1/T <= one semitone above ~110 Hz) and tracks the
		// glide within ~one scoring tick.
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
			// Parabola on log power (Gaussian interpolation). On linear power the fit is biased
			// toward the peak bin's centre, so a pitch between two semitones clings to one of them
			// and the needle sticks then jumps.
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
		// Sounding pitch, most continuous source first: the native motion tracker's fractional
		// pitch, then the raw-tap fractional estimate (fills the low part of a bend the tracker
		// does not report, and its drop-outs), then the integer detector as the last resort.
		// Per tick, not per frame, to bound the cost (the estimator is itself throttled to
		// ~25 Hz and shared with the fractional veto).
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

	// Tier-0 positive rescue. Other strings' ring (a distant pitch dominating the analysis
	// window) can make the bin verdict fail on a perfectly played note until the ring
	// decays. The raw comparison is immune to distant ring: a correct note's fundamental
	// still dominates its own semitone neighbours regardless of what else sounds far away.
	// So when the bin gate cannot accept, a strong positive raw confirmation accepts
	// instead. Guards: real energy required, >=5x over the semitone neighbours, and the
	// fundamental must beat the sub-octave and sub-twelfth measures (a lower note's 2nd/3rd
	// harmonic lands exactly on the expected bin and must never masquerade as the note).
	// The onset gate still applies upstream: a fresh attack is always required.
	bool Tier0ConfirmsExpected(int expectedMidi)
	{
		if (expectedMidi < 0) return false;
		// Probe expectedMidi as is: it is already in the frame the raw tap hears.
		//
		// The raw-pitch ring is filled after the Drop Pedal shifter on both capture paths
		// (AsioHook: processor->Process, then RawPitchVerifier::Observe, in Hook_ProxyInput and
		// in Hook_CaptureGetBuffer alike), and expectedMidi is built as authored + fret +
		// inputShift, i.e. the same post-shifter route frame the native detector and the ML
		// companion read. Subtracting the input shift again here would make the rescue look one
		// semitone off under Drop Pedal (confirming a wrong fret and never the correct one).
		// Never re-apply the shift on this side.
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

		// The sub-harmonic guards get the same +/-25-cent widening as the target (shared
		// helper QueryTier0WideProbePower, so the veto runs the same guards).
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
		// 0.4x, not 1.0x: a ringing sub-octave's fundamental can read slightly below its own
		// 2nd harmonic (the "target"), while a correct note's sub bins read essentially zero.
		// The classes are separated by orders of magnitude; 0.4x sits in the middle with margin
		// for sympathetic low-string ring on a real cable.
		if (subOctave.targetPower >= evidence.targetPower * 0.4f
			|| subTwelfth.targetPower >= evidence.targetPower * 0.4f)
		{
			return false;                  // a lower note's harmonic, not the note itself
		}
		// Octave-phantom probe: on guitar, the sub-octave string's fundamental can be weaker
		// than its own 2nd harmonic (the open low E especially), which walks straight past the
		// guard above. But a note at f/2 cannot hide its 3rd harmonic, which lands at 1.5f, a
		// frequency nothing in the expected note's own series occupies. Substantial 1.5f energy
		// means a sub-octave note is sounding and the "fundamental" is its ghost. 0.1x because a
		// ringing sub-octave's 3rd harmonic can read as low as ~0.2x target, while a correct
		// note's 1.5f bin reads essentially zero.
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

	// Chord tier-0 rescue. Single notes have a positive tier-0 rescue (Tier0ConfirmsExpected);
	// without this, a correctly strummed chord the native scan + ND both miss just fails,
	// forcing a re-strum. Knowing the expected tones, a per-tone energy check confirms far
	// more chord tones than the blind matcher. Tier0ConfirmsChord confirms every expected
	// tone by energy, chord-aware (a neighbour bin that is itself another chord tone is
	// exempt from the dominance test). Conservative: every expected tone present + dominant,
	// on a fresh strum, only after the native scan and ND both declined, so it can rescue a
	// genuinely all-sounding chord but not accept a partial/wrong grab. Toggled over the
	// bridge (set_chord_tier0_rescue) to tune the floor/margin (1e-4, 2x) live.
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
		// Chord-aware dominance: each tone must beat its +/-1 semitone neighbours, EXCEPT a
		// neighbour that is (near) another chord tone - those bins are legitimately lit by the
		// chord itself and must not veto a present tone.
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

	// Native-drive phase A: the StartAt core (0x4749C0), the game's own
	// resume-and-seek. Convention (0x405F85..8B and the core's own prologue/RET):
	//   EAX = owner (also copied to ECX internally for the thiscall chain),
	//   one stack arg = pointer to a game tag-string object, RET 0x4 (callee
	//   cleans). Body: owner+0x5E3 word <- 0x100 (frozen-on-tag off,
	//   resuming-from-tag on), unfreeze core 0x474890, seek 0x7E8A50(tag),
	//   RTPC 0x3F1 <- 1.0 on owner+0x37C (speed), 0x7E98A0(1.0), and
	//   [owner+0x348]+0x9C <- 1 (start pending).
	//
	// The tag-string layout, from the seek's own reads (0x7E8AB4..C3) and the lesson
	// constructor's init (0x471A30): +0x00 inline buffer (or heap pointer), +0x10 end
	// pointer, +0x14 the inline marker (it holds the address of the +0x10 field while
	// the string is inline). An empty tag is a zeroed buffer with end pointing at the
	// buffer, the shipped GE_StartAt("") path, which seeks to the current position.
	//
	// Test entry: `native-seek-test` over the bridge sets the request flag; the
	// scoring detour (main thread, the same thread the GE handlers run on)
	// performs one call while the controller is idle, so the experiment never
	// races the music service from the pipe thread or lands mid-hold.
	constexpr uintptr_t NATIVE_STARTAT_CORE = 0x4749C0;
	volatile bool isNativeSeekTestRequested = false;

	struct GameTagString
	{
		char inlineBuffer[0x10];
		char* endPointer;
		void* inlineMarker;
	};
	static_assert(sizeof(GameTagString) == 0x18, "tag string layout is 0x18 bytes");

	void CallNativeStartAtNow(void* owner)
	{
		// StartAt core 0x4749C0 is a frozen-object method: its prologue does
		// `mov esi, eax; mov word [esi+0x5E3], 1` unconditionally, writing the
		// frozen-on-tag field on entry. On a real frozen-object that field is in
		// bounds; on a GamePlaysongLAS owner (0x5D0 bytes) +0x5E3 is 0x13 past the
		// object end and corrupts the next heap block's free-list entry. The
		// coordinated PlayerSong rebuild that follows carries the music on its own
		// (StartAt never restarts the PlayerSong), so refusing this call on a LAS
		// owner loses nothing.
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

	// Native-drive hybrid release: the owned release runs the StartAt core (the game's own
	// unfreeze + seek + speed reset + show-state tail) and then the coordinated PlayerSong
	// restart for the music itself, since StartAt alone leaves the PlayerSong stopped (see
	// the note below). The toggle is an emergency revert.
	volatile bool isNativeReleaseEnabled = true;
	// The +0x348 component's +0x9C byte: "presentation active" (see the note below).
	// Read for the ground log only.
	constexpr uintptr_t OWNER_PRESENTATION_COMPONENT = 0x348;
	constexpr uintptr_t PRESENTATION_ACTIVE_FLAG = 0x9C;

	// The game's freeze is a mode transition: GE_FreezeSong's core 0x474840 (EAX=owner)
	// sets owner+0x5E2 and calls wrapper->vtbl+0x20(2,0), which stands up a fresh
	// 0x620-byte frozen-mode object (factory 0x46C650, ctor 0x471A30, vtable 0x011A0B70)
	// and makes it the active PlaySong-GE. The frozen class's own update (+0x6C) watches
	// deadline doubles at +0x5E8/+0x5F8 and unfreezes itself natively. Flag writes alone
	// never produce the freeze presentation because the presentation is this other
	// object. 0x474890 (ECX=owner) is the exact mirror: runs the ResumeFromTag core if
	// +0x5E3 is set, then mode (0,2). Both cores are self-contained (they resolve the
	// 0x0135F54C root and bracket the call themselves), so the experiment calls them
	// directly. One-shot per arm (bridge freeze-mode-test): the next established hold
	// enters mode 2 after the mechanical freeze, and its release exits before the
	// mechanical restart.
	constexpr uintptr_t NATIVE_FREEZESONG_CORE = 0x474840;
	constexpr uintptr_t NATIVE_UNFREEZESONG_CORE = 0x474890;
	volatile bool isFreezeModeTestArmed = false;
	bool didFreezeModeEnter = false;
	// Set once a hold's Stop_TMusic has run, cleared by any release (PerformOwnedReleaseAtEpoch). A fault path
	// that abandons the hold with the music still stopped would leave the song frozen (nothing restarts it, N has
	// no owned hold to release, and the safety release is off in Master), so the fault and "disabled" resets
	// release first, at the position the music was stopped at (heldEpoch may still be the previous hold's).
	bool musicStoppedByHold = false;
	float musicStoppedEpoch = 0.0f;

	void CallNativeFreezeSongCore(void* owner)
	{
		const uintptr_t fn = NATIVE_FREEZESONG_CORE;
		const uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		__asm {
			push esi
			push edi
			mov eax, ownerAddress
			mov ecx, fn
			call ecx
			pop edi
			pop esi
		}
		LOG_INFO("(NBN MODE) FreezeSong core returned." << std::endl);
	}

	void CallNativeUnfreezeSongCore(void* owner)
	{
		const uintptr_t fn = NATIVE_UNFREEZESONG_CORE;
		const uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		__asm {
			push esi
			push edi
			mov ecx, ownerAddress
			mov edx, fn
			call edx
			pop edi
			pop esi
		}
		LOG_INFO("(NBN MODE) UnfreezeSong core returned." << std::endl);
	}

	// Native behaviours that do not work, kept so they are not retried:
	//   - "[owner+0x348]+0x9C is a deferred start consumed by 0x4729E0" is wrong
	//     on both halves. 0x4729E0 is GE_HideGame's core (its sole caller 0x405140
	//     is GE_HideGame's body): calling it fades the running game to black, and
	//     its flag write is part of hiding. Its sibling 0x472A20 (flag <- 1) is
	//     GE_ShowGame's core, and the StartAt core ends with the same show tail,
	//     so the byte reads as "presentation active": 1 is the steady running
	//     state, not an armed start. Never gate anything on it reading 0.
	//   - Neither the StartAt core alone nor StartAt + 0x4729E0 restarts the
	//     PlayerSong after a Stop_TMusic latch: both leave running=0 stopped=1
	//     and the transport frozen on an accepted note. The coordinated
	//     PlayerSong restart (COORDINATED_REBUILD) remains the only working
	//     play packet.

	// The native freeze ground-truth line (pure reads; see the constants block above
	// OWNER_FROZEN_ON_TAG for what each field answers).
	void LogNativeFreezeGroundTruth(void* owner, const char* moment)
	{
		const auto ownerAddress = reinterpret_cast<uintptr_t>(owner);
		// The GE resolve chain, exactly as the handlers walk it.
		uintptr_t chainRoot = 0, chainHop = 0, resolved = 0, resolvedVtable = 0;
		const bool chainAlive = TryRead(CHORD_DISPLAY_GLOBAL, chainRoot) && chainRoot != 0
			&& TryRead(chainRoot + CHORD_DISPLAY_GLOBAL_HOP, chainHop) && chainHop != 0
			&& TryRead(chainHop + CHORD_DISPLAY_WRAPPER_SLOT, resolved) && resolved != 0;
		if (chainAlive) TryRead(resolved, resolvedVtable);
		// The scheduler the frozen-span destructor shifts.
		uintptr_t scheduler = 0;
		TryRead(NATIVE_SCHEDULER_GLOBAL, scheduler);
		// The owner's freeze-family state.
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
	// Defined with the freeze-flag block below.
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
		// Re-resolve through the same chain Show used; only clear the visible
		// byte if the live component still is the one that was driven, so a component
		// freed by a rebuild between Show and Hide is never written through.
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

	// The owner whose +0x5E3 byte this controller last set, so every release and
	// abandon path can unwrite it. Clears deliberately ignore the enable toggle:
	// freeze-flag-off issued mid-hold must not leak a set byte into native state,
	// and an abandoned hold must not leave the game believing it is frozen-on-tag.
	void* freezeFlagSetOwner = nullptr;

	void SetNativeFreezeFlag(void* owner, bool frozen)
	{
		if (owner == nullptr) return;
		if (frozen && !isNativeFreezeFlagEnabled) return;
		if (!frozen && owner != freezeFlagSetOwner) return;
		// First-hold corruption guard: on a fresh session's first hold the owner's freeze-tag
		// fields can read uninitialized (tagLen in the billions, resumeFlag garbage; healthy
		// sessions read ~1.4M and 0/1). Announcing frozen-on-tag then sends the game's
		// scheduler walking a garbage-length tag, and the game corrupts its own heap
		// (0xc0000409/0xc0000374 on the main thread with no mod frames). So the write requires
		// a sane tag state; a skipped hold simply runs without the frozen-on-tag announcement
		// (the freeze-flag-off behavior) and later holds re-evaluate once the game initializes
		// the structure.
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
			// An empty tag is uninitialized too (tagLen=0 also precedes the crash). The flag may
			// only announce frozen-on-tag over a real populated tag; until the game initializes
			// one, holds run flag-less, which is fully playable.
			const bool tagSane = readable
				&& tagBegin != 0
				&& tagEnd > tagBegin
				&& tagLength <= TAG_LENGTH_SANITY_LIMIT
				&& (resumeFlag == 0 || resumeFlag == 1);
			if (!tagSane)
			{
				// The "tag" fields are the internals of the std::string at owner+OWNER_FREEZE_TAG_STRING,
				// which only the frozen-object constructor initializes. No writes here: writing into
				// this region raises the crash rate sharply, and these offsets likely lie past the end
				// of the GamePlaysongLAS allocation (frozen-object fields on a smaller class).
				// Read-only log; the owner-size diagnostic below reports the layout.
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

	// The owner's freeze tag std::string and its sibling bytes are initialized only by the
	// frozen-object constructor, so a fresh GamePlaysongLAS owner carries garbage there.
	// The first native path that assigns into that string (the game's FreezeOnTag
	// machinery when the freeze flag announces, or the StartAt core's tag assignment at
	// release) consults the garbage capacity/pointer and frees a wild pointer: heap
	// corruption, detected milliseconds later on the main thread with no mod frames. One
	// surviving assignment normalizes the string. So: initialize the string to the valid
	// empty inline form (the constructor's own layout: begin -> inline buffer, inline
	// marker -> the begin field's address, NUL first byte) once per owner, at bootstrap,
	// before any freeze machinery can touch it.
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
		// Exit the native frozen mode before the controller's own flag clear, so the
		// UnfreezeSong core still sees +0x5E3 set and runs the full native ResumeFromTag
		// unwind, then mode (0,2). The mechanical restart below still runs afterward.
		if (didFreezeModeEnter)
		{
			didFreezeModeEnter = false;
			LogNativeFreezeGroundTruth(owner, "mode-unfreeze-before");
			LOG_INFO("(NBN MODE) Exiting the native frozen mode: UnfreezeSong core"
				<< " (ResumeFromTag unwind if tagged, then mode call 0,2)." << std::endl);
			CallNativeUnfreezeSongCore(owner);
			LogNativeFreezeGroundTruth(owner, "mode-unfreeze-after");
		}
		SetNativeFreezeFlag(owner, false);
		// Native schedule shift: compensate the engine for the frozen span exactly as the
		// frozen-span destructor does, right as the transport is about to resume.
		// Toggle-gated, default off.
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
			// Bookkeeping teardown (the notes are still credited through MarkNotes), then the
			// game's own resume. The empty-tag StartAt seeks to the current transport position,
			// which during a frozen hold is the held epoch; a releaseEpoch that differs is logged.
			LogNativeFreezeGroundTruth(owner, "native-release-before");
			LOG_INFO("(NBN NATIVE RELEASE) Running the game's own resume (StartAt core,"
				<< " empty tag) instead of the coordinated PlayerSong restart at epoch="
				<< std::fixed << std::setprecision(6) << releaseEpoch
				<< " heldEpoch=" << heldEpoch << " (" << reason << ")." << std::endl);
			reinterpret_cast<MarkNotesFn>(MARK_ACTIVE_NOTES)(nullptr, owner);
			CallNativeStartAtNow(owner);
			// The music itself: the StartAt core does not restart the PlayerSong after a
			// Stop_TMusic latch, so the coordinated restart carries the play.
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

	// A controller fault: the native state diverged from this controller's selection/hold.
	// NBN must never turn itself off, so recover in place instead of calling
	// HandleControllerFault (which routes to DisableAutomatic and drops the player out of
	// the feature). During fast Riff Repeater enumeration the fault is a transport
	// discontinuity from the section change; abandon any owned hold safely (ResetBootstrap
	// clears the freeze flag via the live owner so the transport is never left stuck) and
	// re-bootstrap, and the authored-grid timeline re-latches the current section on the
	// next tick.
	void FaultWithoutRelease(const std::string& reason)
	{
		void* owner = trackedOwner;
		LOG_ERROR("(NBN LAS FAULT) Recovering in place instead of disabling NBN ("
			<< reason << "); re-bootstrapping for the current section." << std::endl);
		ResetBootstrap(reason.c_str(), owner, true);
		trackedOwner = owner;
	}

	// liveOwner: the caller's proof of a still-current owner object. When it is
	// the same owner the frozen-on-tag byte was set on, the abandon unwrites it:
	// the RR range change abandons holds on a still-live owner, and a leaked 1
	// leaves native consumers in the freeze presentation. Without that proof no
	// write happens (a freed owner's byte no longer matters, and writing into
	// recycled memory corrupts the heap); the tracking is dropped so a later
	// release cannot write to the stale pointer.
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
			// Never leave the game in mode 2 across an abandon. With a live owner the native
			// exit runs; without one nothing can, and the error line records it.
			if (didFreezeModeEnter)
			{
				didFreezeModeEnter = false;
				if (liveOwner != nullptr)
				{
					LOG_ERROR("(NBN MODE) Hold abandoned while the native frozen mode was"
						<< " entered; exiting via the UnfreezeSong core on the live owner."
						<< std::endl);
					CallNativeUnfreezeSongCore(liveOwner);
				}
				else
				{
					LOG_ERROR("(NBN MODE) Hold abandoned while the native frozen mode was"
						<< " entered and no live owner is available; the game may remain"
						<< " in mode 2." << std::endl);
				}
			}
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
		ClearSelection();
		// The section changed, so no string can still be ringing from the selection that
		// ClearSelection deliberately preserves it across.
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

	// legatoReferenceString is the string that is already ringing, which is what decides
	// whether a no-pick pull-off or tap is physically playable and therefore whether it may
	// be a target in its own right. It differs by caller and getting it wrong hangs the
	// feature. A fresh selection compares against the last committed note, so it passes
	// previousSelectedString. A successor search runs while a hold is still owned, so the
	// ringing string is the currently selected one and it must pass selectedString.
	// (Passing previousSelectedString from the successor search would queue a pull-off from
	// the held note as its own target whenever the note before it was on another string;
	// nothing can satisfy it, so the hold would never release.)
	// Reports how the live vector is distributed across phrase iterations, and where the
	// selected note sits within that.
	//
	// Diagnostic only: it refuses nothing. Gating selection on the phrase iteration has the
	// same shape as the section-end boundary filter: refuse targets by an inferred boundary
	// and, if the boundary is wrong, every later note is played unheld and turns grey. This
	// reports whether a Riff Repeater loop occupies one phrase iteration or several, which
	// decides whether such gating could be safe.
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
		// Verdict first: the console reader truncates at the buffer width.
		LOG_INFO("(NBN LAS PHRASE) selected=it" << selectedIteration
			<< "@" << std::fixed << std::setprecision(3) << selectedTime
			<< " liveIterations=" << spans.size()
			<< (unreadable != 0 ? " unreadable=" + std::to_string(unreadable) : "")
			<< " | " << summary.str() << std::endl);
	}

#if defined(_DEBUG)
	// Logs each live note's mask and per-note scoring state once, so authored technique and
	// scoring flags can be correlated with selector behavior from the console. Deduped by
	// record so a tapping run logs each note once, not every scan.
	std::unordered_set<uintptr_t> g_scan69LoggedRecords;
	void LogNoteScan69(const LiveNote& note)
	{
		if (note.record == 0) return;
		if (!g_scan69LoggedRecords.insert(note.record).second) return;
		LOG_INFO("(NBN SKIP69) record=0x" << std::hex << note.record
			<< " mask=0x" << note.mask << std::dec
			<< (note.mask & 0x4000 ? " TAP" : "")
			<< (note.mask & NOTE_MASK_CHILD ? " CHILD" : "")
			<< (note.mask & NOTE_MASK_IGNORE ? " IGNORE" : "")
			<< (note.mask & NOTE_MASK_BEND ? " BEND" : "")
			<< (note.mask & (NOTE_MASK_HAMMERON | NOTE_MASK_PULLOFF) ? " LEGATO" : "")
			<< " time=" << std::fixed << std::setprecision(3) << note.recordTime
			<< " states=" << (int)note.stateC0 << "," << (int)note.stateC1 << ","
			<< (int)note.stateC2 << "," << (int)note.stateC3 << std::endl);
	}
#endif

	bool FindEarliestEligibleNote(void* owner, LiveNote& earliest, int legatoReferenceString)
	{
		(void)legatoReferenceString;
		bool hasEarliest = false;
		ForEachLiveNote(owner, [&](const LiveNote& note)
		{
#if defined(_DEBUG)
			LogNoteScan69(note);
#endif
			// Child records are continuations sounded by their parent, so they are not
			// targets in their own right. A bend is the exception. Rocksmith authors a bend
			// as a parent and a child on the same string and fret a fraction of a second
			// apart, and the child is the return, which the player has to pick again.
			// Skipping it makes the bend accept itself the moment the parent is played.
			// In flow mode a bend child is skipped too and the game scores it natively, as
			// Rocksmith does.
			if ((note.mask & NOTE_MASK_CHILD) != 0
				&& ((note.mask & NOTE_MASK_BEND) == 0 || isFlowUntilMissEnabled))
			{
				return true;
			}
			// Ignore excludes a record from Rocksmith's ordinary scoring/rating, but it is also
			// authored onto visible playable notes in rapid passages. Note-by-Note practices
			// those visible note identities, so Ignore alone must not make one ineligible.
			// A note Rocksmith has already scored cannot be a target. Holding on one asks
			// the player to articulate something the engine considers finished, and the
			// hold ends the moment it starts: CommitBeforeRelease commits as soon as it
			// sees stateC0 and stateC1 both set, which is already true on arrival (the
			// target appears to skip a note, or notes complete by themselves).
			//
			// This reads the engine's own per-note scoring state rather than assuming
			// anything about how a phrase is authored, so it holds for any chart. The pair
			// is the same one the commit path already treats as authoritative.
			if (note.stateC0 != 0 && note.stateC1 != 0) return true;
			// Hammer-ons and pull-offs are targets. The feature is the note sequence, not the
			// technique: every note is its own stop on the timeline, revealed one at a time on
			// the fretboard. Pull-offs retain detector-pitch acceptance because they have no pick
			// edge. Hammer-ons deliberately use the normal picked-note path so wrong or carried
			// pitches cannot advance them. legatoReferenceString is retained by the callers but
			// no longer consulted.
			// Grey lead-in notes are strictly earlier than the section boundary. The note
			// at the boundary time is a real target (it renders as a normal note and turns
			// grey only after being missed).
			if (note.recordTime < greyCutoff - GREY_EPSILON) return true;
			// A note at or beyond Rocksmith's selected section end belongs to the next loop
			// iteration and can never be reached before the loop restarts, so it must not
			// become a target. The section is a half-open interval [start, end): the authored
			// phrase-section end equals the next iteration's start exactly (bit-identical floats
			// from the phrase-section vector at owner+0x78 -> +0xF4/+0xF8, stride 0x58, end at
			// +0x28), and a note at the next iteration's start has exactly that recordTime. A
			// strict "> end + GREY_EPSILON" would leave that boundary note eligible, demanding an
			// extra note before the loop restarts. The start gate above stays strict-less because
			// a note at the section start is a real target; only the end side is half-open-exclusive.
			if (hasSectionEndBoundary
				&& note.recordTime >= sectionEndBoundary - GREY_EPSILON)
			{
				return true;
			}
			// A partially progressed record is still the earliest unresolved target. The
			// full C0/C1 guard above excludes records the engine has already scored; letting
			// any individual state byte exclude a record here would skip live notes that are
			// already present in the phrase but have only partially advanced.
			if (consumedRecords.count(note.record) != 0) return true;
			// Double-strum guard: the just-committed chord stays ineligible across the
			// bootstrap reset its own release triggers at the section start. See
			// COMMITTED_CHORD_RESELECT_GUARD_SECONDS.
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
		// Fresh selection: the ringing string is the last note committed.
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
		// The hold boundary is the strike line in both modes (stepping the clock back from a
		// later boundary would leave the highway drawn for a different moment, so the frozen note
		// would not sit on the line). Flow keeps the transport running past it only while a pick
		// is being confirmed or has already claimed the note (see HasPendingFlowAttack at the
		// boundary); with nothing played it freezes right here, exactly like freeze mode.
		// flowLateGraceSeconds is no longer used for the boundary.
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
			// The native detect window's exit bounds how late a flow hold boundary may sit.
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

		// Guitar or bass decides the open-string table below and every bass-specific branch.
		RefreshArrangementInstrument();
		int16_t tuningOffset = 0;
		if (selectedChordId != -1)
		{
			// Chord hold: the string index is the 0xFF chord sentinel and no single
			// expected pitch exists, so all pitch machinery is neutralized. Acceptance
			// lives in the hit-decision detour.
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
			// The onset release needs the selected record's expected MIDI note.
			if (selectedString < 0 || selectedString > 5 || selectedFret < 0)
			{
				FaultWithoutRelease("The selected record's string/fret identity is outside the supported guitar range");
				return false;
			}
			// expectedMidi is the authored tuning (0x1199D2C, the player's own guitar frame) plus
			// the fret, plus the input pitch shift. The detector table (state+0x134C) is not used:
			// it reads the chart frame under Speaker Mode while the input stays in the guitar's
			// own frame, which would put expected a semitone off. inputShift is 0 for Speaker/Off
			// (the input is not retuned) and the Drop amount for the Drop Pedal (which does retune
			// the input), so authored+fret+inputShift == the pitch the player produces for the
			// correct fret, in every mode. See HostGetInputOnsetShiftSemitones.
			const int inputShift = ResearchProbeRuntime::GetInputOnsetShiftSemitones();
			// The low-string E<->Eb detector flicker under Speaker Mode is tolerated at the accept.
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
			// Authored template pitch (detector frame) for the native single-note accept.
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
			// Hammer-ons, pull-offs and taps all accept on the no-pick legato path. After a
			// chord the predecessor string is the sentinel, so a hammer-on on a chord-fretted
			// string still qualifies (LegatoContinuesPreviousString). A picked hammer-on also
			// commits through the pick buffer (isHammerOnTarget keeps IsPlainPickedTarget true).
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
			// Two play-through records in a row can let the running transport overshoot the next
			// chord's boundary. A record that can no longer be held cleanly is one missed hold, not
			// a controller failure: it plays through natively like any suppressed record and the
			// scan selects the next target once it expires.
			isHoldSuppressed = true;
			LOG_ERROR("(NBN LAS HOLD) Hold boundary overshot by "
				<< std::fixed << std::setprecision(3) << (updateTime - selectedHoldTime)
				<< "s (max " << maxOvershoot << ") for record=0x"
				<< std::hex << selectedRecord << std::dec
				<< "; the record plays through natively instead of faulting."
				<< std::endl);
			return false;
		}
		// FreezeOnTag pins the owner to the exact compensated event epoch. Using the
		// already-overshot callback time lets the noteway retire the target after the
		// first held frame, so its native prompt instance can no longer animate.
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

		// Shipped lesson order: start the freeze-note presentation, stop the
		// music/publication source, then pin the owner clocks.
		//
		// Chord holds: the freeze-note prompt machinery is built around a single note's
		// NoteVfx and cannot arm for a chord record (string sentinel 0xFF). The prompt is
		// presentation, not transport: a chord hold proceeds without it, and the freeze
		// itself comes from Stop_TMusic and the pinned owner clocks below.
		if (!ArmSelectedNativePrompt(selected))
		{
			if (selectedChordId != -1)
			{
				// Diagnostic for the chord prompt route (without the prompt, the chord panel
				// floats short of the line). The chord's Vfx controller carries its own vtable;
				// logging it names the slots needed for chord arming to join
				// ArmSelectedNativePrompt.
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
		// The Play_FreezeNoteTrack event is what sweeps the note track at freeze, and
		// the armed NoteVfx prompt is what carries the frozen note through that
		// sweep. A chord cannot arm the prompt, so dispatching the event would sweep the
		// chord's own panel off the highway and the player would read the next chord as
		// the target. The freeze itself is Stop_TMusic plus the pinned clocks
		// (record-agnostic), so the event is presentation-only and skipped for chords:
		// the chord panel stays on the unswept track.
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
		// Corruption checkpoints: a heap corruption on a fresh session's first hold is
		// detected only moments later, so checkpoints bracket each step of hold
		// establishment and the early ticks.
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

		// Flow-through grace: a pick played while hit decisions were blocked (dense rebuild,
		// commit, release) left an attack spike behind and its string is still ringing now.
		// Open the raw-confirmation reattack window so the input-release wait can accept that
		// sustain through tier-0 instead of discarding it as carry-over and forcing a second
		// pick. The window only ever accepts on the full tier-0 positive confirmation (or an
		// exact detector match), and same-pitch successors stay barred by allowAccept, so a
		// predecessor's ring cannot ride this in.
		{
			const ULONGLONG nowTick = GetTickCount64();
			// How long before this freeze did the player last attack? A spike inside the last
			// ~150 ms means the player was on time and the hold boundary beat the native commit.
			// Only meaningful once the spike sampler also runs in the Armed phase (see
			// SampleArmedPhaseSpike); logged unconditionally so the blind case is visible.
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
		// Stamp the native onset-scan window origin at this latch (the live ring timestamp).
		hasHoldLatchRingTime = TryReadCurrentRingTime(holdLatchRingTime);
		// The ND sounding streak is per-hold; ring carryover is handled natively
		// by gating all ND credit on fresh-attack evidence since this latch.
		hasNdSoundingStreak = false;
		// The spike-reattack window must not survive into a new hold: a chord strum's spike
		// can open the window just before the latch, and the chord's ringing tone at the
		// successor's pitch would then be accepted without a fresh pick. A decaying ring
		// cannot spike, so requiring the spike to happen during the hold closes the leak; a
		// genuinely early pick is still covered by the primed-onset stash, which rides the
		// native edge rather than the level meter.
		reattackWindowTicks = 0;
		reattackStreak = 0;
		spikeRecencyTicks = 0;
		ResetChordOnsetEvidenceAnchor();

		// Prime the onset detector's edge-latch so a still-ringing previous note cannot
		// release this hold; only a new pluck after this point reports. A drained
		// onset that is distinguishable fresh playing is kept, not discarded.
		int primedOnset = QueryNativeOnsetNote();
		StashPrimedOnset(primedOnset);

		SetNativeFreezeFlag(owner, true);
		// Freeze-mode test: one armed hold enters the game's own frozen mode on top of the
		// mechanical freeze. One-shot; the release exits.
		if (isFreezeModeTestArmed)
		{
			isFreezeModeTestArmed = false;
			didFreezeModeEnter = true;
			LogNativeFreezeGroundTruth(owner, "mode-freeze-before");
			// owner+0x5E2 is only ever initialized by the frozen-object constructor, so a LAS
			// owner carries garbage there and the core's "already frozen" check would skip the
			// whole transition. Zero it first so the game sees not-frozen and actually performs
			// mode (2,0).
			uint8_t frozenSongByte = 0xFF;
			TryRead(reinterpret_cast<uintptr_t>(owner) + 0x5E2, frozenSongByte);
			TryWriteGameByte(reinterpret_cast<uintptr_t>(owner) + 0x5E2, 0);
			// The tag std::string at owner+0x5C8 is also only initialized by the frozen-object
			// constructor, so the exit's ResumeFromTag unwind would read garbage pointers (AV in
			// memcpy under 0x407840). Initialize it to a valid empty inline string exactly as the
			// constructor does: end (+0x5D8) -> buffer (+0x5C8), inline marker (+0x5DC) -> the end
			// field, first buffer byte NUL.
			{
				const uintptr_t tagBase = reinterpret_cast<uintptr_t>(owner) + OWNER_FREEZE_TAG_STRING;
				const uint32_t bufferAddress = static_cast<uint32_t>(tagBase);
				const uint32_t endFieldAddress = static_cast<uint32_t>(
					reinterpret_cast<uintptr_t>(owner) + OWNER_FREEZE_TAG_BEGIN);
				TryWriteGameBytes(reinterpret_cast<uintptr_t>(owner) + OWNER_FREEZE_TAG_BEGIN,
					&bufferAddress, sizeof(bufferAddress));
				TryWriteGameBytes(reinterpret_cast<uintptr_t>(owner) + OWNER_FREEZE_TAG_END,
					&endFieldAddress, sizeof(endFieldAddress));
				TryWriteGameByte(tagBase, 0);
			}
			LOG_INFO("(NBN MODE) Entering the native frozen mode for this hold:"
				<< " owner+0x5E2 was 0x" << std::hex << static_cast<int>(frozenSongByte)
				<< std::dec << " (zeroed), tag string initialized empty;"
				<< " FreezeSong core (mode call 2,0)." << std::endl);
			CallNativeFreezeSongCore(owner);
			LogNativeFreezeGroundTruth(owner, "mode-freeze-after");
		}
		LogNativeFreezeGroundTruth(owner, "hold-established");
		LOG_INFO("(NBN LAS HOLD) Established. All five owner clocks hold " << std::fixed
			<< std::setprecision(6) << epoch
			<< "; expectedMidi=" << expectedMidi
			<< " (string " << selectedString << " open "
			<< (expectedMidi >= 0 ? expectedMidi - selectedFret : -1)
			<< " fret " << selectedFret << "), primedOnset=" << primedOnset
			// authoredTemplateTone (detector frame) and inputShift are logged to verify the
			// transpose frame against expectedMidi.
			<< "; authoredTemplateTone=" << selectedNativeTone
			<< " inputShift=" << ResearchProbeRuntime::GetInputOnsetShiftSemitones()
			<< ". Scoring and the native onset detector continue at the frozen time."
			<< std::endl);
		// The hold may pin the clocks up to HOLD_BOUNDARY_MAX_OVERSHOOT behind the transport (flow's
		// freeze-back). That step back is the controller's own, so the rollback detector must compare
		// the next tick against the held epoch, not the pre-hold transport; otherwise a freeze-back
		// reads as a loop turnover and abandons the hold.
		lastUpdateTime = epoch;
		EmitCandidateEvent(owner, updateTime, selected,
			ResearchProtocol::ExpectedAttackEventKind::HoldEstablished);
		return true;
	}

	DenseChainResult TryAdvanceDenseChain(void* owner, float /*updateTime*/)
	{
		LiveNote next;
		// Successor search: the hold is still owned, so the ringing string is the selected
		// one. A legato continuation of the held note is sounded by it and must not become
		// a target of its own.
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
		// Chord successors take the dense rebuild too. The plain release path skips the
		// noteway rebuild, which leaves the chord panel showing one chord while the evaluator
		// judges another in tight chord chains. Only chords that would not hold are excluded.
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

		// The successor's expected pitch is the authored tuning + fret + input shift, exactly like
		// the single-note hold above (authored guitar frame, not the detector table). A chord
		// successor has no single pitch (-1).
		int denseNoteMidi = -1;
		if (!isChordSuccessor)
		{
			const int inputShift = ResearchProbeRuntime::GetInputOnsetShiftSemitones();
			int16_t tuningOffset = 0;
			// Player's physical frame, same as the single-note hold above.
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
		// A chord has no single expected pitch; -1 routes the successor into the
		// same evaluated-decision acceptance every plain chord hold uses.
		denseSuccessor.expectedMidi = denseNoteMidi;
		denseSuccessor.isBend = (next.mask & NOTE_MASK_BEND) != 0;
		denseSuccessor.isBendChild = denseSuccessor.isBend
			&& (next.mask & NOTE_MASK_CHILD) != 0;
		denseSuccessor.isLegato = NoteByNote::UsesNoPickLegatoAcceptance(next.mask);
		denseSuccessor.isHammerOn = NoteByNote::IsHammerOn(next.mask);

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

	// A bend sweeps upward from its fretted pitch, and NDGetOnsetNote reports each
	// crossed semitone as a distinct event. Requiring the unbent fundamental means the
	// only acceptable pitch exists for a few tens of milliseconds before the bend
	// removes it, which at a degraded tick rate is routinely missed entirely. Since
	// Rocksmith itself grades a bend on reaching the target pitch, the bent pitches are
	// correct answers, not tolerance.
	// Resolves the exact pitch a bend has to reach, from the chart's own bend amount.
	// Leaves bendAcceptMidi at -1 when the value is missing or outside a musical range, so
	// acceptance falls back to the tolerance range rather than becoming impossible.
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
		// A bend with a known amount must reach its actual pitch. Accepting the whole
		// window lets a bend finish part-way through, and the top of the window can also
		// be the next note's pitch, which then could not be played.
		if (bendAcceptMidi >= 0) return onset == bendAcceptMidi;
		return onset > expectedMidi && onset <= expectedMidi + MAX_BEND_SEMITONES;
	}

	// What a raw-path acceptance counts as "the player picked this target". For a
	// plain note that is the exact pitch. For a bend it is the whole bend window:
	// picking into a bend routinely never sounds the base at all. Any pitch from base
	// to bent top proves the pick; the bend confirmation still requires the top before
	// the note commits.
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

	// The arming's edge-drain (both prime sites) exists to discard a still-ringing
	// previous note, which re-reports its own pitch once the dedupe clears. But the
	// same drain would eat a played-ahead pick: a player who knows the riff picks the
	// next note during the commit/rebuild dead window, and the edge latches it. So a
	// primed onset is kept for the first holding tick when it is distinguishable from
	// the previous note (the same rule as the release wait's fresh-playing shortcut)
	// and matches the new target.
	void StashPrimedOnset(int primedOnset)
	{
		pendingPrimedOnset = -1;
		if (primedOnset < 0) return;
		// previousExpectedMidi is bend-adjusted (the bent pitch), and a released bend
		// sweeps down from there toward its base; it cannot ring above the bent pitch.
		// An upward range [previous, previous+3] would double-count the bend and swallow
		// a genuine next note sitting just above it.
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


	// Builds the target's legato gesture: the contiguous run of hammer-on / pull-off
	// notes that immediately follows it on the same string.
	//
	// These notes are played as part of the target's gesture and have to stay on the
	// highway. The renderer cannot work this out for itself because it sees one note
	// per call with no ordering, so the run is computed here, where the whole note
	// vector is available in time order, and published. Only the contiguous same-string
	// run qualifies; sparing every legato note in the song would draw unrelated notes
	// next to the target and imply gestures that do not exist.
	void BuildVisualGroup(void* owner, uintptr_t groupRecord, float groupTime, int groupString)
	{
		// The group is always published empty. Every legato note is its own target, so no
		// note needs group-exemption from the presentation gates: the target itself is the
		// whole whitelist. The protocol fields stay for a possible mode that lights whole
		// gestures.
		(void)owner; (void)groupRecord; (void)groupTime; (void)groupString;
		visualGroupCount = 0;
	}

	// Puts the bend at the front of the gesture and starts confirming it from the continuous
	// pitch tracker.
	//
	// The bend is the first element whether or not the chart continues it with legato notes;
	// otherwise a bend that also began a legato run would fall through to the run branch and
	// its bend requirement would be dropped.
	//
	// Shared by the two ways a bend gesture starts. A bend parent starts when its pick is
	// accepted. A bend child has no pick to accept, so it starts as soon as the hold is
	// established.
	void BeginBendConfirmation()
	{
		if (legatoRunCount < MAX_LEGATO_RUN)
		{
			// Push the existing continuations back one slot and put the bend in front of
			// them, so the gesture is confirmed in the order it is played.
			for (uint32_t index = legatoRunCount; index > 0; --index)
			{
				legatoRunMidi[index] = legatoRunMidi[index - 1];
				legatoRunIsBend[index] = legatoRunIsBend[index - 1];
			}
			++legatoRunCount;
		}
		else
		{
			// A full run is not a reason to drop the bend, so the last continuation gives way
			// to it rather than the bend being lost.
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
		// So the first fallback of a new bend reports immediately rather than inheriting the
		// previous bend's throttle.
		hasTrackerSampleAnchor = false;
		legatoRunIndex = 0;
		legatoConfirmTickCount = 0;
		holdProgressAnchor = std::chrono::steady_clock::now();
		mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
		mlBendConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
		enhancedLegatoConfirmation.Reset(latestRawAudioSample);
	}

	// The input-release wait's onset shortcuts commit the target directly, which for a bend
	// target would skip the bend entirely (committing on the unbent base). Route an accepted
	// onset on a bend target into the same bend confirmation the Holding phase uses for a pick
	// (see "Pick accepted at"), so the gesture is still required. Bend children are the held
	// sustain of a bend already confirmed, so they keep the direct path.
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
		// Continuations are not collected: every hammer-on / pull-off is its own target, so
		// there is no run to hold as a unit. This only resets the run machinery, which bends
		// still use: BeginBendConfirmation inserts the bend as element [0] of an empty run
		// and confirms it from the continuous pitch tracker.
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
		// Kept so the input-release wait can tell a genuinely carried onset (which
		// necessarily has the previous note's pitch) from fresh playing.
		// What is still ringing, which for a bend is the bent pitch rather than the fretted
		// one. The carry-over guard compares an incoming onset against this to tell a
		// still-sounding previous note from a fresh pick; recording the unbent pitch would
		// let a ringing bend be accepted as a fresh pick of a next note at the bent pitch.
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
		// After both the record and the expected pitch are in place, since the accepted
		// bend pitch is derived from the two together.
		ResolveBendAcceptance(selectedRecord);
		isHoldSuppressed = false;
		holdTickCount = 0;
		postReleaseTickCount = 0;
		commitTickCount = 0;
		wasCommitOverrideLogged = false;
		renderFramesWhileHeld = 0;
		inputReleaseTickCount = 0;
		// A dense successor is a new target and must require its own fresh attack, exactly
		// as EstablishHold does. Otherwise the previous strum's spike/onset evidence and
		// the still-ringing prior chord carry across the whole chain, so one pick (even a
		// single string) commits an entire repeat-strum section at once. Reset the same
		// fresh-attack state here so each strum in the chain needs its own re-pick.
		sawSpikeDuringHold = false;
		sawLevelSpikeDuringHold = false;
		// New window origin for the native onset scan: this successor's own latch time, so
		// the previous strum's onset cannot carry across the dense chain.
		hasHoldLatchRingTime = TryReadCurrentRingTime(holdLatchRingTime);
		// Dense single-note successors keep the legacy onset path (the native single-note
		// accept is wired at EstablishHold only).
		selectedNativeTone = -1;
		hasNdSoundingStreak = false;
		reattackWindowTicks = 0;
		reattackStreak = 0;
		spikeRecencyTicks = 0;
		// Flow-through grace on the dense path too (flowing play advances through here rather
		// than EstablishHold). The reset above stays authoritative for the repeat-strum leak:
		// a same-pitch successor is still barred from the reattack accept by allowAccept, and a
		// different-pitch successor only accepts on the full tier-0 positive confirmation of
		// its own pitch, which the previous strum's ring cannot produce. So a recent attack
		// spike may re-open the window for a plain different-pitch single note: the pick that
		// caused it was played through the transition and its sustain deserves to be judged.
		// The window does not tick down until the input-release wait starts consuming it.
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
#if defined(_DEBUG)
			if (g_flowArmedTick != 0)
			{
				LOG_INFO("(NBN FLOW) armed->adopt " << (nowTick - g_flowArmedTick)
					<< "ms (detect+accept+commit+release+rebuild for the note just scored)."
					<< std::endl);
			}
#endif
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
				// Same shape as EstablishHold's chord branch: the prompt is a
				// single-note NoteVfx and cannot arm for the 0xFF chord sentinel.
				// Presentation only; the freeze comes from Stop_TMusic below.
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
		// Same skip as EstablishHold: the freeze event sweeps the track and only an
		// armed prompt survives it, which a chord successor does not have.
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
		// Same reattack-window reset as EstablishHold (see the comment there): a
		// spike belonging to the previous target must not open this hold's
		// same-pitch acceptance window.
		reattackWindowTicks = 0;
		reattackStreak = 0;
		spikeRecencyTicks = 0;
		// Flow-through grace, re-seeded after the reset above (which would otherwise wipe the
		// AdoptDenseSuccessor seeding before the input-release wait ever ticks the window).
		// The same-pitch auto-advance the reset protects against stays closed: the wait calls
		// the reattack with allowAccept barred for a successor whose pitch the previous note
		// could satisfy, and a different-pitch successor only accepts on an exact detector
		// match or the full tier-0 positive confirmation of its own pitch; a decaying
		// predecessor ring can produce neither. Only a plain different-pitch single note
		// qualifies.
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
#if defined(_DEBUG)
			if (g_flowAdoptTick != 0)
			{
				LOG_INFO("(NBN FLOW) adopt->armed " << (nowFlowTick - g_flowAdoptTick)
					<< "ms (native play packet + relatch)." << std::endl);
			}
#endif
			g_flowArmedTick = nowFlowTick;
		}
		// Every relatch demands a fresh strum, so a repeated chord cannot advance on the
		// previous chord's ring. Both evidence sources reset here, so a same-chord successor
		// cannot commit until a new attack frame lands after this relatch. The dense chain's
		// purpose (avoiding a full release/rebuild) is unaffected; only the "is there fresh
		// attack" question is re-asked, and a real re-strum answers it within ~100 ms (the
		// onset scan runs every tick against a ~95 frame/s ring).
		sawSpikeDuringHold = false;
		sawLevelSpikeDuringHold = false;
		lastStuckWarnHeldSeconds = 0.0;
		ResetChordOnsetEvidenceAnchor();
		SetNativeFreezeFlag(owner, true);
		// Each note in a dense chain gets its own safety budget; otherwise every successor
		// would inherit however much of the budget its predecessors had already spent.
		holdProgressAnchor = std::chrono::steady_clock::now();
		mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
		mlBendConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
		enhancedLegatoConfirmation.Reset(latestRawAudioSample);
		hasHoldProgressAnchor = true;
		lastStuckWarnHeldSeconds = 0.0;
		int primedOnset = QueryNativeOnsetNote();
		int currentMidi = QueryNativeLoudestPlayedNote();
		// The successor is now the held note, so its own legato run is what the transport
		// must wait for.
		BuildLegatoRun(owner, selectedRecordTime, selectedString);
		// A bend child skips the input-release wait entirely.
		//
		// That wait exists to stop the previous note, still ringing, from satisfying the
		// successor as though it were fresh playing. For a bend child the still-ringing sound
		// is the correct satisfaction: the player is holding one gesture across the parent
		// and the child, and the child's requirement is that the string be at the bend pitch,
		// which it already is. The wait only clears once the string falls silent, by which
		// time the bend is over and the tracker can never read the bend pitch again.
		if (isBendChildTarget && bendAcceptMidi >= 0)
		{
			gatePhase = GatePhase::Holding;
			inputReleaseTickCount = 0;
		}
		// Chord successors skip the wait too: the wait clears only after consecutive silent
		// polls, and the previous chord's ring keeps the detector loud for seconds, so the
		// player's first strums would land before the evaluator is even armed. Chord
		// acceptance carries its own fresh-attack evidence (the spike gate plus the native
		// onset flag), so the ring cannot auto-advance a same-chord successor even without
		// the wait.
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
			// Delayed inference may credit the accepted attack, but never audio recorded after it.
			NoteByNote::CreditConfirmedMlFeedback(detectionFeedback, evidence.analyzedSampleIndex,
				feedbackMinimumMlSample, feedbackMaximumMlSample, now);
		}
	}

	// A chord readout names the chord that was asked for, for every chord commit path (the
	// game's own decision, flow, ND sounding, fret-hand mute), not only the raw-attack chord
	// accept; otherwise the HUD shows the bare word "chord". Same naming rule as that accept:
	// the authored root, else the lowest chord tone.
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
		// Every commit publishes which detector(s) passed it (the HUD's accept readout, green
		// only on those rows), even when a newer attack has already arrived.
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
			// Chords have no single target pitch; the roles come from the accept decision.
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
				// Speaker Mode reads native one semitone off (OctaveCollisionGate.hpp), so agreement
				// is the same +-1 the pass logic uses, not exact equality.
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

	// A note that flows (natural commit while Armed, no hold) also lights the detection HUD,
	// so flow mode shows its detections too. Buffered = the mod's own pick confirmation (its
	// recorded roles); otherwise the game's decision.
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
		// A strum this note confirmed off but did not own is left for a successor chord;
		// ClaimSuccessorStrum decides at the next chord hold whether it is still usable.
		successorStrumSample = IsPlainPickedTarget() && acceptedPickRecord == selectedRecord
			&& acceptedPick.strumBelongsToSuccessor ? acceptedPick.minimumSample : 0;
		// Play-ahead: the note confirmed on its own pick, but a later attack (kept when it cut
		// the note's pick short, PickedAttackQueue::Push) is still waiting: the player already
		// moved on. A following chord inherits it the same way; a following single note takes it
		// from the queue as before. ClaimSuccessorStrum still drops it when stale or no chord
		// claims it.
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
			// Only name the pick when this record took one. A hammer-on accepted on the legato path
			// is a plain-picked target too, and would otherwise reprint the previous note's pick,
			// which reads as one pick committing two notes.
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
		// Only the committed record is consumed. Each legato continuation is a target of its own
		// and must stay selectable, so consuming anything beyond the selected record would skip
		// real notes.
		// This note is now the one that is ringing, whichever path the commit took, so a legato
		// continuation that arrives through a fresh selection keeps its reference string.
		previousSelectedString = selectedString;
		// The ringing pitch too, with the same bend adjustment AdoptDenseSuccessor makes:
		// a bend rings at its bent pitch, not its fretted one. Without it the next legato
		// target's still-ringing guard would compare against a stale pitch.
		previousExpectedMidi = (isBendTarget && bendAcceptMidi >= 0)
			? bendAcceptMidi
			: expectedMidi;
		previousWasBend = isBendTarget;
		// Every commit names the path that produced it, so an automatic advance identifies
		// itself instead of having to be inferred from surrounding lines. The reason leads
		// the line because console capture truncates at the buffer width.
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

		// Flow resumes the song after a hold instead of chaining the next close note into another
		// hold (otherwise every later note goes hold -> dense rebuild -> hold and flow never
		// resumes). The plain release restarts the transport at the held note, so the player can
		// catch up in time.
		if (!isFlowUntilMissEnabled)
		{
			DenseChainResult chainResult = TryAdvanceDenseChain(owner, updateTime);
			if (chainResult != DenseChainResult::NotRequired) return;
		}

		PerformOwnedRelease(owner, reason);
		gatePhase = GatePhase::PostRelease;
		postReleaseTickCount = 0;
	}

	// Flow: how far past a note the freeze may wait for a pick that is still being confirmed
	// (song time; the native window closes at ~+0.30).
	// The freeze then lands back on the missed note (FreezeBackToNote), which the hold can do for
	// up to HOLD_BOUNDARY_MAX_OVERSHOOT past that note's own boundary, so the wait stays inside it.
	// The wait stretches with the room before the next note: low notes take ~0.2-0.25 s to
	// confirm, so a fixed 0.16 s would freeze many on-the-beat low notes. The wait is the gap to
	// the next note minus the compensation and a margin, never below 0.16 s (dense runs) and never
	// above 0.26 s (the native late window closes at +0.30).
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

	// A pick played as the note hits the line is only captured ~60 ms later (median ~60 ms,
	// p90 up to ~90 ms). So at the line flow first waits up to FLOW_CAPTURE_WAIT_SECONDS of
	// real time for a capture; with nothing captured by then it freezes (a short step back),
	// otherwise the pick is pending and the note keeps flowing. Long enough that a pick a few
	// tens of ms behind the beat is still captured before the freeze.
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

	// The in-flight extension stops this far before the note's native window exit.
	constexpr float FLOW_WINDOW_EXIT_MARGIN_SECONDS = 0.03f;
	uintptr_t flowExtensionLoggedRecord = 0;

	// Flow chords wait at most this long past the chord's time for the game's own decision; the native
	// window closes at +0.30, so a chord that then freezes is still inside it.
	constexpr float FLOW_CHORD_MAX_WAIT_SECONDS = 0.20f;

	// A strum heard for the selected chord within the last 0.3 s and not consumed by a commit.
	bool HasPendingFlowStrum()
	{
		return pickSampleRate != 0 && chordAttacks.HasFreshAttack(latestRawAudioSample, pickSampleRate);
	}

	// An attack captured in the last 0.25 s that has not been confirmed or refused yet, or any attack
	// captured in the last FLOW_RECENT_CAPTURE_SECONDS. One pluck is often captured twice: an early
	// capture that confirms as the still-ringing previous note and the true onset 60-100 ms later.
	// Waiting only on unresolved attacks would freeze between the two.
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

	// Flow phase 3: while Armed, resolve the target pitch early and let a confirmed buffered
	// attack commit the record with no hold. Plain picked singles only: bends, legato
	// continuations (tap/hammer/pull on the ringing string) and chords keep the game's own
	// natural decision under flow and, when that misses, the hold path.
	void TickArmedFlowBuffer(const LiveNote& selected)
	{
		if (selectedChordId != -1 || armedBufferCommitRecord == selectedRecord) return;
		// Bends flow like native Rocksmith: the pick at the bend's base pitch commits it, the
		// bend itself is not proven. Excluding bends here would freeze flow on every bend and
		// then wait for input release and the bend. A frozen bend keeps the full check.
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

	// Flow freeze-back redraw. A flow miss freezes after the transport already ran past the
	// note, and pinning the clocks back (EstablishHold) leaves the PlayerSong fretboard target
	// and the gems drawn at the later time, so the note shows twice. The dense chain already
	// solves exactly this between close notes: release at the held epoch through the
	// coordinated rebuild, wait for the play packet, then re-latch Stop_TMusic at the same
	// epoch. The redraw reuses that pipeline with the same, still unscored note as the
	// "successor": the reset-state recommit branch is skipped (an unscored note is supposed
	// to read all-zero) and the previous-note bookkeeping is restored after adoption so the
	// missed note does not count as the one still ringing. It runs while held (a rebuild on
	// a running transport breaks the stop latch).
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

		// The rebuild drops every note before its epoch, and a slow-speed hold can sit after the
		// note's time, which would lose the note being redrawn. Rebuild just before the note so
		// it survives; the relatch then pins the clocks to selectedHoldTime (RelatchDenseSuccessor).
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

	// Flow speed cap. A pick on the beat takes real time to confirm (low notes ~0.2 s), while
	// flow only waits min(gap to the next note, FlowPendingAllowanceForGap(gap)) of song time
	// past the note. Slowing the song stretches that song-time allowance into more real time,
	// so the speed that lets an on-time pick confirm is allowance / confirmSeconds. Each Riff
	// Repeater section gets its own cap from its typical tight gap (the gap 80% of its notes
	// are wider than, so one grace note does not drag the section down). The cap only ever
	// pulls the speed down; slower is always allowed, and a player who wants full speed turns
	// flow off.
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
		noteCountOut = times.size();
		return FlowSpeedCap::CapFromNoteTimes(times.data(), times.size(), IsBassArrangement(), &typicalGapOut);
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
		// Recomputed every 2 s in case the live note vector only covers part of the section;
		// the cap only ratchets down within one section so the speed does not wander.
		// Every 250 ms while a pass's apply window is open (the note vector is still loading), then 2 s.
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
					<< cap << "% (typical tight gap " << std::setprecision(3) << typicalGap
					<< " s over " << noteCount << " notes, " << (IsBassArrangement() ? "bass" : "guitar")
					<< ")." << std::defaultfloat << std::endl);
			}
		}

		// The speed the game was asked for (CurrentSongSpeed: player slider, or this pass's request); the
		// Time_Stretch RTPC read is not refreshed by the game at 100% and lagged a manual change.
		const bool hasPlayerSpeed = ResearchProbeRuntime::GetPlayerSpeedRealPercent() > 0.0f;
		if (!hasPlayerSpeed && !hasMeasuredSongSpeed) return;
		// Applied only at the start of a pass (section start or loop restart), never mid-pass, so
		// the speed never changes on its own part-way through. The first FLOW_SPEED_APPLY_WINDOW
		// of a pass covers the lead-in, where the game re-sends the player's own speed and ramps
		// to it. A tighter cap found later in a pass (the live note vector fills as the section
		// plays) waits for the next pass. Only ever lowers.
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
		// Before this pass's request: the player's speed. After it: whether the game's lead-in re-send
		// overtook it shows only in the transport, so the second request needs the measurement well
		// above the request (the measurement reads 10-20% high, hence the 1.25 margin).
		const float speed = flowSpeedPassRequestedPercent > 0.0f
			? (hasMeasuredSongSpeed && measuredSongSpeed * 100.0 > flowSpeedPassRequestedPercent * 1.25
				? static_cast<float>(measuredSongSpeed * 100.0) : flowSpeedPassRequestedPercent)
			: static_cast<float>(CurrentSongSpeed() * 100.0);
		// The cap is computed a moment after the latch (the note vector loads): within the apply
		// window the pass takes the tighter value.
		if (now - flowSpeedPassStartedAt <= FLOW_SPEED_APPLY_WINDOW)
			flowSpeedPassCap = (std::min)(flowSpeedPassCap, flowSpeedCapPercent);
		// At most two requests per pass (the game pops its speed notification on each), the second
		// only if the first was overtaken by the game's own lead-in re-send.
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

	// The FLOW MODE switch lives in the host (menu row, overlay toggle). Follow it every tick so a
	// reloaded controller does not keep its own stale copy.
	void SyncFlowModeFromHost()
	{
		bool hostFlow = isFlowUntilMissEnabled;
		if (!ResearchProbeRuntime::GetHostFlowModeEnabled(hostFlow) || hostFlow == isFlowUntilMissEnabled) return;
		isFlowUntilMissEnabled = hostFlow;
		LOG_INFO("(NBN FLOW) FLOW MODE switched " << (hostFlow ? "On" : "Off")
			<< " from the menu/overlay; the controller follows." << std::endl);
	}

	// RIGHT ARROW while a note or chord is frozen: release it without scoring and go on to the
	// next note, so a hold can never get stuck. The same release the safety timeout uses: the
	// record is consumed so it is never selected again, and the game scores it as it would any
	// note the transport passes. A press while nothing is frozen is dropped, not kept for the
	// next hold. LEFT ARROW (back to the previous note) is queued by the host but not acted on:
	// it needs a rewind while held, and rewinds on a running transport can loop.
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
					// Flow bookkeeping, as a held commit does it: the committed pitch is what rings
					// now (the next attack's masking and ringer checks), and a native commit takes
					// one matching buffered attack with it so the same pick cannot also commit a
					// repeated next note.
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
					// A chord the game scored on its own (flow): its strum is used up, so the next chord cannot
					// reuse it, and the same-chord re-strum rule knows what was just played.
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
				// Flow + Riff Repeater loop: on the second pass the game sets stateC3 the moment a note
				// reaches its record time, so with flow's late boundary every note of the repeat would be
				// skipped as missed. Freeze mode never sees it: its hold lands before the record time.
				// Under flow stateC3 only means expiry once the note's native window has closed; until
				// then the note stays a target and the boundary logic below runs.
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
					// Chord records do not share the single-note state-byte semantics: stateC3 can read
					// nonzero seconds before the chord's own time, so for chords this byte is not the
					// expiry marker and faulting on it would kill every chord hold at selection. Until the
					// chord state packing is decoded, log it and let the hold proceed; the safety timeout
					// and the kill switch bound the damage if the record really is dead.
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
						// A single-note target whose native record expired before its hold boundary. In a dense
						// run the release -> input-release-wait -> rebuild latency can let the running transport
						// overshoot a tightly-packed successor, so the game marks it expired (stateC3) before
						// EstablishHold could freeze on it. This is not a controller-ownership fault (no hold is
						// owned yet in Armed, so there is nothing native to unwind). Accept the game's own miss
						// verdict, consume the record and advance; the next tick selects the next live note and
						// NBN stays alive. The overshoot is logged so its frequency stays visible.
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
				// The original update above gets first refusal. A natural on-time hit clears
				// the selection; only a still-unresolved note can reach this hold boundary.
				if (!isHoldSuppressed && updateTime >= selectedHoldTime)
				{
					if (selectedChordId != -1 && !areChordHoldsEnabled)
					{
						// Fallback behind the toggle: the chord plays natively while successors stay gated.
						isHoldSuppressed = true;
						LOG_INFO("(NBN LAS HOLD) Chord record 0x" << std::hex << selectedRecord
							<< std::dec << " (chordId " << selectedChordId
							<< ") is not held (chord holds disabled); native play continues."
							<< std::endl);
						return;
					}
					// Repeat chord strums. Playing them through counts them as missed, and two
					// play-through records in a row let the running transport overshoot the next
					// chord's hold boundary. If bare-0x2 records stall, the chord safety budget frees
					// them in CHORD_HOLD_SAFETY_RELEASE_SECONDS and repeat-holds-off restores the
					// play-through classification.
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
					// Flow: a pick made on the beat is still being confirmed when the boundary arrives
					// (bass: ~0.10 s from capture to confirmation). The freeze waits out the note's flow
					// window (FlowPendingAllowance, inside the native window) whether or not a pick was
					// captured, so the game's own decision always gets its full chance. A real miss
					// freezes at the late edge and steps back with the redraw.
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
					// Flow chords: without a wait at the line a chord freezes before the game's own chord
					// detection (~50-100 ms after the strum) can score it. The freeze waits up to
					// FLOW_CHORD_MAX_WAIT_SECONDS past the chord (inside the native window) with no strum
					// condition, so the game's own decision can commit the chord with no hold, and a chord
					// that does freeze is still judged. If it misses, the chord freezes where the transport
					// is (below) and the strict held-chord path takes over.
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
					// Past the window, keep flowing while a pick or strum is still being confirmed, up to just
					// before the note's native window closes (after windowExit flow treats the record as
					// expired and would skip it). Low notes often confirm 0.15-0.2 s after capture.
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
					// A chord that waited and still missed steps back to the line with the same highway
					// redraw as a single note (freezing in place would leave it drawn beyond the strings).
					// Flow miss: freeze back on the missed note, not at the late flow boundary (which would
					// stop between notes with several boxes on the neck). The hold pins the owner clocks to
					// selectedHoldTime, and it may sit up to HOLD_BOUNDARY_MAX_OVERSHOOT behind the
					// transport, so the song steps back to the note the player has to play.
					// Only after a pending pick ran out of time is the transport past the boundary;
					// the hold then pins the clocks back to it (the rare step-back case).
					if (isFlowUntilMissEnabled && updateTime - selectedHoldTime > 0.03f)
					{
						LOG_INFO("(NBN FLOW) Pending pick did not confirm; freezing back at the note's boundary "
							<< std::fixed << std::setprecision(3) << selectedHoldTime << " (transport "
							<< updateTime << ")." << std::defaultfloat << std::endl);
					}
					const float flowStepBack = updateTime - selectedHoldTime;
					if (EstablishHold(owner, updateTime, selected)
						&& isFlowUntilMissEnabled
						&& flowStepBack > FLOW_REDRAW_MIN_STEP_SECONDS)
					{
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
					// The redrawn note is unscored, so all-zero states are expected, not a reset.
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
				// Chord records do not share single-note state-byte semantics: stateC3 can read
				// nonzero seconds before a chord's own time, so for a chord successor only C0/C1
				// gate here (the same relaxation EstablishHold's expiry check uses).
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
					// A hammer-on is plain-picked only so a picked hammer-on commits through the
					// buffer; the hammer itself must still accept with no pick. Returning early here
					// would skip the Holding phase's legato check, so on the dense path every hammer-on
					// would need a pick-strength attack.
					// A same-time sibling rings with its partner, so the input-release wait never ends
					// for it; it is judged here as in Holding (ConfirmSameTimeSibling).
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

				// The raw-confirmation tick, mirroring the accepted-onset shortcut
				// below and run FIRST so this tick's spike is already recorded when
				// the onset decision needs it as same-pitch evidence. Acceptance
				// requires a distinguishable successor (for a same-pitch successor
				// the previous ring itself reads the expected pitch with passing
				// gates); allowAccept=false still keeps the tracking continuous.
				{
					// Barred whenever the previous pitch itself would satisfy the
					// matcher: then the old ring is indistinguishable from a fresh
					// pick by pitch alone (covers both the same-pitch successor and
					// a bend window that contains the previous note).
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
					// A genuinely carried onset is the previous note still ringing, so it
					// carries the previous note's pitch. An onset matching the successor
					// therefore cannot be carry-over: it is the player having already moved
					// on. On guitar that is ordinary technique, because the next note is
					// routinely played while the previous string is still sounding.
					//
					// The wait still applies when the two pitches are equal, which is the
					// only case where the detector genuinely cannot distinguish them.
					// A bend just played can still be sounding anywhere between its base and
					// its bent pitch, so an onset in that range may be the previous note rather
					// than fresh playing; there the shortcut is unsafe and the normal
					// input-release confirmation has to do the work.
					// previousExpectedMidi is bend-adjusted (the bent pitch), and a released bend
					// sweeps down toward its base; it cannot ring above the bent pitch, so the
					// range does not extend upward (that would swallow a genuine next note just
					// above it).
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

					// Same-pitch successor with spike evidence: pitch alone cannot tell a fresh
					// pick of the identical pitch from the previous note's ring, but a decaying
					// ring cannot spike the level meter. Attack energy moments before an onset
					// matching the target is a pick, whatever the previous note was.
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

					// An onset that is neither the previous note's pitch nor its bend
					// range cannot be carry-over: it is a fresh attack whose transient
					// pitch was garbage. Open the raw-confirmation window so the pick
					// is not lost with its consumed edge.
					if (discardedOnset != previousExpectedMidi && !mayBeCarriedBend)
					{
						reattackWindowTicks = REATTACK_WINDOW_TICKS;
						reattackStreak = 0;
					}
				}

				// A bend target confirmed by its raw pitch. When the target itself is a bend, a
				// pick-and-bend played straight after the previous note has its onset eaten as
				// carry-over (the string was already sounding and only the fretted pitch climbs), so
				// none of the onset shortcuts above fire; and the silence wait below never completes
				// because the bent note keeps sounding, resetting the count every tick. The bent pitch
				// is evidence in itself: a fresh bend rises from the base toward the bent target, which
				// a decaying previous ring cannot do, and the bent pitch is distinguishable from the
				// previous note. Keep the same-pitch bar (never accept a pitch the previous note could
				// be producing).
				if (isBendTarget && !isBendChildTarget && bendAcceptMidi >= 0)
				{
					const int rawBend = QueryNativeLoudestPlayedNote();
					// The previous note, if it was a bend, sweeps down from its bent pitch toward
					// its base; a raw pitch in that decay range may be that ring, not the player.
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
						// Require a brief hold at target even when rising, so a single momentary
						// at-target read on a partial bend cannot commit. A genuine bend climbing
						// into target holds there.
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

				// A held bend's sustained ring can mask the follower's onset entirely
				// (QueryNativeOnsetNote returns -1 while the loud ring dominates), so the onset
				// shortcuts above never fire and the silence wait below never completes
				// (currentMidi stays non-silent on the ring, resetting the count every tick). A
				// level spike is the fresh pick regardless of the masking: a decaying ring cannot
				// spike the meter. Arm on it so the pick registers now; the Holding phase then
				// confirms it by pitch as usual, and a spike with no real follow-through simply
				// fails that pitch check.
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
				// Stale-overlay repair: the target text is built from the note object's chord
				// template at hold establishment, but the noteway refreshes recycled objects
				// asynchronously, so the overlay can show the previous chord while the evaluator
				// reads the fresh template. Re-emitting the candidate event once a second lets the
				// host rebuild the text from the settled object.
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
				// Safety release, default off: from the player's seat a release is indistinguishable
				// from an accept, so it both misleads and buries the refusing state. Disabled, a stuck
				// hold stays stuck and visible; the escape hatches are the N toggle (Stop releases an
				// owned hold) and the pause/rollback paths. Re-enable live with
				// safety-release-on.
				if (hasHoldProgressAnchor)
				{
					const double heldSeconds = std::chrono::duration<double>(
						std::chrono::steady_clock::now() - holdProgressAnchor).count();
					const double safetyBudget = selectedChordId != -1
						? CHORD_HOLD_SAFETY_RELEASE_SECONDS
						: HOLD_SAFETY_RELEASE_SECONDS;
					if (heldSeconds >= safetyBudget && !isSafetyReleaseEnabled)
					{
						// Keep the stuck state on record at every budget interval, but keep holding:
						// the refusal evidence is the point.
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
						// The state at the moment of failure, unthrottled: this is the one
						// sample that is always worth having.
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

				// A bend child has no pick of its own, so it enters bend confirmation
				// immediately instead of waiting for an onset that cannot arrive.
				//
				// The parent of this record has already been picked and accepted, and the
				// player is holding one continuous gesture across both. Waiting for an onset
				// at the unbent pitch would ask them to release the bend and pluck again,
				// which nothing on screen suggests.
				//
				// The child is still a target rather than skipped: skipping bend children
				// makes a bend accept itself the moment its parent is played. What changes
				// here is only what satisfies it. The tracker still has to see the string at
				// the bend pitch, so a child cannot pass while the bend is not being held.
				// Chord hold: no single expected pitch exists, so none of the pitch
				// machinery below can accept a strum. Acceptance lives in the hit-decision
				// detour, which evaluates the game's own decision while this hold lasts;
				// this branch only keeps the diagnostics alive while waiting.
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

				// A sibling played together with its same-time partner (TrackSameTimeGroup). A fresh
				// attack still works below as before; this only spares the second one.
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

				// The windowed decision cannot release a long hold (onset timestamps live
				// on the advancing input-stream clock), so the release trigger is the
				// lesson's own un-windowed onset query.
				{
					CaptureOnsetSpectrum(expectedMidi);
					// While a run is being played the pick has already been accepted, so a
					// further onset is the player picking into the run rather than a new
					// gesture, and must not restart or complete it.
					int onset = -1;
					if (IsPlainPickedTarget())
					{
						if (!TakeBufferedPick()) return;
						onset = expectedMidi;
					}
					else if (!isConfirmingLegatoRun)
					{
						// Bend and legato targets. A bend needs the detected onset pitch here so
						// the bend-band entry below can see it (the gesture is proven by the
						// continuous-tracker confirmation, not by an attack==pitch match). Retained
						// no-pick legato targets (pull-off/tap) produce no fresh attack, so the
						// picked-note attack scan can never accept them; native routes legato
						// through its own path (0x4E9670), not ported, so the sounding-based
						// departure-then-lock rule applies.
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
						// Bend target rescue: the detector read wobbles on a high bend, so the
						// departure-then-lock can miss it, but the exact bend target (bendAcceptMidi) is
						// known. Confirm the pitch actually reached it by tier-0 raw energy and/or the ML
						// companion, both robust where the detector is noisy. Additive: only when native's
						// onset paths gave nothing (onset still -1); streak-gated so a passing glide-through
						// cannot trip it, only a held-at-target bend.
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
						// Near-miss forensics: the frame's per-pitch energy bins tell sharp fretting from
						// a genuinely wrong fret. A sharp-fretted note splits energy between the expected
						// bin and its neighbor; a wrong fret concentrates in the neighbor alone. Logged for
						// every near-miss onset as the basis for a possible sharp-fretting grace rule.
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
						// Bend-release cascade guard: a bend's decaying ring sweeps down through the
						// followers' pitches and the onset detector fires on the loud ring, skipping the
						// next notes with no pluck. For the release window, a follower must show attack
						// energy (sawSpikeDuringHold, set by the level spike or a ring-frame onset stamp);
						// a decaying ring shows none, a real repluck does. Applies to any follower, picked
						// or bend (a ring cannot legitimately start a bend either).
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
						// A bend's pick lands in the band base..bentPitch and the bent pitch itself never
						// arrives as an onset (the player bends up to it, no fresh pick), so IsAcceptedOnset,
						// which for a known-amount bend demands the exact bent pitch, can never admit the pick
						// that starts the gesture, and the confirmation phase below would never begin. An
						// in-band onset is exactly what MatchesPickPitch defines as "the player picked this
						// bend", so admit it here to start BeginBendConfirmation. The bent-pitch requirement
						// is enforced at confirmation, not at this entry gate, and the decaying-ring guard
						// above still zeroes a ringing false onset before it reaches this point.
						if (IsAcceptedOnset(onset) || (isBendTarget && MatchesPickPitch(onset)))
						{
							// A bend is the same shape as a legato run: the pick starts the gesture and the
							// pitch completes it. Bending produces no new pick attack, so the bent pitch can
							// never arrive as an onset and only the current-pitch query observes it. Accepting
							// the pick alone would let a bend complete before it was bent.
							//
							// Every accepted onset on a bend target starts confirmation, not only an exact
							// base-pitch match: the bend acceptance band spans base..base+3, and an in-band
							// onset above base (for example the previous note's ring or re-attack) must not
							// commit the bend directly. Routing the whole band here costs nothing for a
							// genuinely pre-bent pick (the tracker confirms immediately) and makes the gesture
							// mandatory for everything else.
							if (bendAcceptMidi >= 0)
							{
								BeginBendConfirmation();
								// The pick landed at or below the bend target, which is the below-target
								// approach (pick the base, then bend up), so pre-arm the raw accept.
								// BeginBendConfirmation clears the arm; without re-arming here, a bend that
								// is already at the target pitch when confirmation starts sampling never
								// sees a non-credible read to arm, so a perfect bend would never accept. An
								// inherited ring picked at the target (onset == bendAcceptMidi) is excluded,
								// keeping the anti-cascade guard.
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

							// The pick is only the first note of the gesture. When the chart
							// continues it with legato notes, the transport keeps holding
							// until every one of them has been played.
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
							// A rejected onset is still an attack. The transient regularly reports garbage
							// for a correct pick, so it opens the raw-confirmation window and the settled
							// pitch decides over the next few ticks.
							reattackWindowTicks = REATTACK_WINDOW_TICKS;
							reattackStreak = 0;
						}
					}

					// Run confirmation. Each continuation is verified by pitch, in order,
					// and only the last one commits the whole gesture.
					if (isConfirmingLegatoRun && legatoRunIndex < legatoRunCount)
					{
						const int expectedRunMidi = legatoRunMidi[legatoRunIndex];
						const int currentMidi = QueryNativeLoudestPlayedNote();

						// A bend only has to reach its pitch, because the sweep is
						// continuous and the top wobbles; a fretted legato note is exact.
						//
						// The integer query cannot judge that well. It reports whole
						// semitones, so a bend resting just under its target reads as a
						// full semitone short and only registers once it overshoots, and
						// `>=` then accepts any higher pitch from anywhere, including a
						// different string. Rocksmith's own continuous trackers report
						// fractional pitch, so prefer them and keep the integer query as
						// the fallback for when they are unreadable.
						// Judged per element, not per run: a run can mix a bend with fretted
						// legato notes and the two need different tests.
						const bool isBendElement = legatoRunIsBend[legatoRunIndex];
						bool hasReachedRunPitch = false;
						// True when the native motion tracker's fractional pitch made the reach;
						// the fractional veto below exists for the integer paths' rounding and
						// must not overrule the engine's own continuous measurement.
						bool reachedByTracker = false;
						// A bend child is the parent bend's sustain: the player holds one gesture across
						// the parent and the child, so the string is already at the bent pitch with no fresh
						// below-target approach. The base game treats the bend and its sustain as a single
						// action and never re-demands the sustain. The approach guard
						// (hasBendApproachBeenObserved) exists to stop a stray decaying ring from a different
						// note confirming a bend that was never bent, but this ring is the held bend, so the
						// guard must not apply. Pre-satisfy the approach for a bend child so the held bent
						// pitch confirms the sustain immediately (via the native sounding table, which lists
						// the bent pitch while it sounds). A released child is still safe: its pitch falls
						// below target, so nothing confirms.
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
								// The lesson band, asymmetrically widened upward (see
								// BEND_OVERBEND_ALLOWANCE_SEMITONES): reached once the pitch
								// is within sUnderbend below the target, and overshoot up to
								// the allowance counts as reached.
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
								// No tracker is sounding near the target. That is the
								// normal state before the bend arrives, so it is not an
								// error; fall back to the integer behaviour so a
								// missing tracker can never make a bend unplayable.
								// Off-target-first: the gesture must be observed below the
								// accept pitch before >= can complete it, or a carried
								// in-band ring already at the target confirms a bend that
								// was never bent (see hasBendApproachBeenObserved).
								if (currentMidi >= 0 && currentMidi < expectedRunMidi)
								{
									hasBendApproachBeenObserved = true;
								}
								hasReachedRunPitch = hasBendApproachBeenObserved
									&& (currentMidi >= expectedRunMidi);

								// Second fallback, the one that carries a bend while the
								// hold is frozen (see DETECTOR_RAW_BEND_QUALITY_FLOOR):
								// the ungated current-note field, accepted only when armed
								// and only on a streak.
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
											// Overshoot counts as reached; see
											// BEND_OVERBEND_ALLOWANCE_SEMITONES.
											&& rawPitch < (wanted + BEND_OVERBEND_ALLOWANCE_SEMITONES);
										if (!credible)
										{
											// Arm on any non-credible read: silence before the bend,
											// the approach, or a wobble at the top. This makes a fast
											// bend (silence -> bent, no clean below-target frame) still
											// arm. Anti-cascade kept: an inherited ring already at
											// target is credible from tick one, never reaches this
											// branch, so it never arms.
											isRawBendAcceptArmed = true;
											ndBendSightingStreak = 0;
											// Reset the streak only on a genuine release toward the unbent base, not on the
											// integer detector flickering to target-1 at the top of the bend. The on-screen
											// meter reads the fractional pitch and stays green there, but the integer field
											// briefly quantizes one semitone low; resetting just below the target would zero
											// the streak on every such frame, so a reached bend would never accumulate its
											// ticks. For bends deeper than a semitone the reset point is lowered to just above
											// the unbent base; a drop back there is a release, a one-integer top wobble is not.
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
											// A non-below dip is the wobble at the top of the bend
											// (or a momentary quality drop), not a failure: hold
											// the streak through it. The below-target re-arm above
											// still resets a genuinely released bend.
										}
										else
										{
											// A credible read is the bend at its target, the same
											// on-pitch, high-quality signal that turns the on-screen
											// bend meter green. Arm on it directly (not only after a
											// clean below-target approach) so a fast or half bend,
											// which flashes the target only briefly, still confirms.
											isRawBendAcceptArmed = true;
											++rawBendAcceptStreak;
											// A decaying bend-release ring is excluded by the release
											// guard (within the release window with no attack spike),
											// so trusting a strong green cannot cascade a follower.
											const bool inBendReleaseGuard = hasLastBendCommit
												&& !sawSpikeDuringHold
												&& std::chrono::duration<double>(
													std::chrono::steady_clock::now()
														- lastBendCommitAt).count()
													< BEND_RELEASE_GUARD_SECONDS;
											const bool strongGreen =
												raw.quality >= DETECTOR_RAW_BEND_STRONG_QUALITY
												&& !inBendReleaseGuard
												// Require the strong read to actually be held, not just
												// flash for one tick. rawBendAcceptStreak was incremented above.
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

								// Third source, the native sounding table (see
								// ndBendSightingStreak): the engine lists the bent
								// pitch as integer MIDI once it sounds. Below-target
								// sightings arm the approach; a target-or-over sighting
								// while armed reaches, on a 2-tick streak.
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

								// Fourth source: the raw-audio estimate, in the player's pitch frame.
								// Every source above is the game's detector, which in Speaker Mode reads
								// one semitone low most of the time but not always (especially in the high
								// register). So a fixed +1 would let an unbent half bend pass whenever the
								// base read exact, and no shift at all makes a half bend need a whole step.
								// The estimate measures the audio itself, which the veto below already
								// trusts to say "not there yet"; trust it the same way to say "reached":
								// confident, inside the band, held for two ticks, and only after the bend
								// was seen below target (an unbent half bend reads the base, so it never
								// gets here; a ring already at pitch never arms).
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

								// Never silently: the fallback is the open-ended comparison the tracker
								// replaces, so every time it carries a bend, the reason the tracker
								// declined is reported. Throttled, because this runs every tick.
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
						// Fractional veto: the integer detector rounds a bend that has not reached pitch
						// up to the target, and the strong-green / streak / sounding-table paths would then
						// accept it on a single frame. The raw-tap fractional estimate knows the true pitch;
						// when it is confident and clearly below the target band, veto the reach. It
						// releases the instant the pitch climbs into the band, and never fires when the
						// estimate is unavailable or low-confidence, so it cannot make a bend unplayable.
						// Bend elements only; a fretted legato note is judged exactly by the polls below.
						bool enhancedCheckedBend = false;
						float enhancedBendMidi = -1.0f;
						// The veto does not overrule a reach made by the native tracker's own fractional
						// pitch (it targets integer rounding, and the tracker does not round), and it allows
						// the estimator BEND_VETO_ESTIMATOR_SLACK_SEMITONES of error below the accept band,
						// since a moving pitch still reads slightly low even on the halved window.
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
									// Logged once per veto episode.
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
						// A bend counts the moment it reaches pitch and is not required to stay
						// there; requiring two consecutive polls would let a bend that touched its
						// target and wobbled below reset the counter and need re-bending. A
						// fretted legato note still needs the two polls, because its pitch is
						// stable once fretted and a single poll is more prone to noise.
						const uint32_t requiredPolls = isBendElement
								? 1u
								: LEGATO_CONFIRMATION_TICKS;
						if (hasReachedRunPitch)
						{
							// The engine's own bend verdict, post-veto: a bend element needs a
							// single poll, so reaching here means the note progresses this
							// tick. This, not the raw pitch, is what the bend meter's green
							// publishes, so green can never show without progress.
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
								// A tracker-confirmed bend never consumes an onset, so unlike every picked
								// commit it leaves the dedupe global holding a stale value while the string
								// rings at the bent pitch, and the next hold's onset query would report the
								// ring as a fresh onset. Seeding the dedupe with the bent pitch restores the
								// symmetry: the ring is consumed exactly as a picked note's onset would have
								// been, and a same-pitch follower goes through the departure-then-lock and
								// spike paths like any repeated note.
								if (isBendElement)
								{
									TryWriteDedupeGlobal(expectedRunMidi);
									// Open the release-decay guard so the bend's ring, sweeping down through the
									// followers' pitches, cannot auto-commit them without a pluck.
									lastBendCommitAt = std::chrono::steady_clock::now();
									hasLastBendCommit = true;
								}
								// The next element starts its own raw-acceptance arming; a
								// carried streak would let the element it belonged to leak
								// into its successor.
								isRawBendAcceptArmed = false;
								rawBendAcceptStreak = 0;
								ndBendSightingStreak = 0;
								hasBendApproachBeenObserved = false;
								// Real progress within the run, so the safety timeout
								// restarts and a long but advancing run is never cut short.
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
									// Consume the committed record so the eligibility scan does not re-select it. A
									// bend note's window extends through its sustain, so without this the just-bent
									// record stays eligible and is re-selected the instant it commits, demanding a
									// second bend. Chords already consume on commit; runs (bend or legato) need it too.
									// lastCommittedChordRecord carries it across the release's PlayerSong restart
									// (which clears consumedRecords), same as chords.
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

					// Legato fallback. A pull-off or tap changes a string that is already sounding,
					// so it produces no pick attack and the edge-detected onset query often never
					// reports it at all. The current-pitch query does observe it. This is only
					// consulted for records classified for no-pick legato acceptance, and only when
					// the successor's pitch differs from the note just played, so a still-ringing
					// previous note cannot satisfy it.
					//
					// The pitch must arrive, not merely be present: a bend release sweeps the string
					// down through real pitches while it is still ringing loudly, and a bare equality
					// poll would accept those as played notes. So acceptance arms only once the
					// detector has reported something other than the expected pitch since this target
					// became active: the note counts when its pitch appears, not when it is inherited.
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
		// Native-drive phase A: the StartAt-core test call, executed here because this is
		// the main thread the game's own GE handlers run on (the bridge pipe thread must
		// never call into the music service). Idle-only and vtable-guarded; one call per
		// request. See CallNativeStartAtNow.
		if (isNativeSeekTestRequested)
		{
			isNativeSeekTestRequested = false;
			uintptr_t ownerVtable = 0;
			if (gatePhase == GatePhase::Idle
				&& TryRead(reinterpret_cast<uintptr_t>(owner), ownerVtable)
				&& ownerVtable == LAS_OWNER_VTABLE)
			{
				LogNativeFreezeGroundTruth(owner, "native-seek-before");
				CallNativeStartAtNow(owner);
				LogNativeFreezeGroundTruth(owner, "native-seek-after");
			}
			else
			{
				LOG_INFO("(NBN NATIVE SEEK) Test skipped: phase="
					<< DescribeGatePhase(gatePhase)
					<< " ownerVtable=0x" << std::hex << ownerVtable << std::dec
					<< " (requires Idle + GamePlaysongLAS)." << std::endl);
			}
		}

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

		// A pending enable-time re-arm. The enable event fires host-side, off this loop,
		// so a section change that keeps the same owner is invisible to the owner-identity
		// reset below and would otherwise leave stale confirmed bounds from the previous
		// section. Reset here for the current owner so the bootstrap proceeds fresh;
		// adopting the owner skips the redundant owner-change reset immediately below.
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

		// Song-reload guard. The owner+0x78 grid is repopulated when a new song loads, and it
		// can settle after NBN has already confirmed against the previous song's grid (the
		// owner-identity reset above misses it because at re-arm time the grid still held the
		// old container). If the live grid identity no longer matches the cached timeline, the
		// song changed: re-arm so the new song's sections are latched and no stale/out-of-range
		// section carries over.
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

		// Primary arming path: latch the loop bounds directly from the stable authored
		// phrase-section grid, immediately, on the first valid tick after enable, with no
		// waiting for a loop turnover and no dependence on the moving +0x3C0/+0x3C8 mirror.
		// On a transient read it leaves isEpochConfirmed false and retries next tick. The
		// delayed-identity path below runs only when this has not confirmed, as a fallback
		// for an owner whose grid is unreadable.
		if (!isEpochConfirmed)
		{
			TryLatchSectionFromGrid(owner);
		}
		else if (HasSelectionMovedToNewSection(owner))
		{
			// The player navigated to a different Riff Repeater section while NBN stayed on.
			// Follow it: abandon any hold from the old section (ResetBootstrap clears the
			// freeze flag safely) and re-latch to the new selection immediately, without
			// turning NBN off. ResetBootstrap does not touch the cached timeline, so the
			// re-latch reuses the grid.
			// Preserve the current start as a hint: extending the range rightward moves only
			// the end, and +0x3C0 stays moved, so without this the re-latch would collapse
			// the loop to its last phrase.
			const float previousStart = greyCutoff;
			LOG_INFO("(NBN LAS BOOTSTRAP) Riff Repeater section selection changed while enabled;"
				<< " auto-following to the newly selected section (no toggle needed)."
				<< std::endl);
			ResetBootstrap("the Riff Repeater section selection changed", owner);
			trackedOwner = owner;
			TryLatchSectionFromGrid(owner, previousStart);
		}

		// Periodic arming diagnostic. The one-time latch line is buried within a second by
		// play output, so re-state the latched bounds vs the live transport every ~90 ticks.
		// This tells "wrong section" from "wrong note": if updateTime is outside
		// [greyCutoff, sectionEndBoundary] the section is wrong; if inside, the bounds are
		// right and the problem is elsewhere.
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
			// The native range is needed only to latch the section end at confirmation.
			// Before that, stay inert until Rocksmith publishes a valid range. After
			// confirmation the owner fields move with every restart and periodically read
			// back invalid (the mirrored pair disagrees mid-restart), but
			// greyCutoff/sectionEndBoundary are already latched, so a failed read must not
			// send the confirmed controller inert (that would grey every note after the
			// first loop).
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

		// nativeSectionStart is only consumed by TryReadNativeSectionRange's own validity
		// gate (mirror agreement, end>start). greyCutoff never reads it: the +0x3C0 field
		// moves with every owned restart. Delayed-identity supplies the start instead.
		(void)nativeSectionStart;

		// Rollback (loop turnover): a backward jump in the transport clock.
		if (hasLastUpdateTime && updateTime < lastUpdateTime - ROLLBACK_THRESHOLD)
		{
			if (OwnsNativeHold())
			{
				// The game restarts its own section (failure / manual / natural loop). The hold
				// is already moot (the game initiated the rollback and is running again), so
				// abandon it and re-bootstrap in place, staying enabled; delayed-identity
				// re-confirms.
				LOG_ERROR("(NBN LAS LIFECYCLE) External rollback while a native hold was"
					<< " owned; abandoning the hold without release and re-bootstrapping"
					<< " in place." << std::endl);
				SetNativeFreezeFlag(owner, false);
				ResetBootstrap("an external rollback occurred while a native hold was owned");
				trackedOwner = owner;
			}
			else if (!isEpochConfirmed)
			{
				// Not armed yet: capture both endpoints of this first jump. Neither is
				// trusted as the boundary (the first rollback observed is wherever the
				// player happened to enable the feature); the identity check below decides
				// which endpoint is the real section start.
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
				// Armed: a genuine loop turnover. Advance the epoch and free the section's
				// notes for the next pass. Neither boundary is touched - they were latched
				// once at confirmation and are immutable for the life of the section.
				++epochIndex;
				ClearSelection();
				consumedRecords.clear();
				// Clear the pitch dedupe global too. A bend seeds it with the bent pitch on
				// commit (so the sustained ring cannot auto-commit a follower); without this
				// reset that seed survives the loop, and the same bend on the next pass reads
				// its own ring as already-consumed and fails until a spike unsticks it. A fresh
				// loop iteration must start with no carried-over consume state.
				TryWriteDedupeGlobal(-1);
				LOG_INFO("(NBN LAS BOOTSTRAP) Native epoch restart -> epoch " << epochIndex
					<< " at updateTime=" << std::fixed << std::setprecision(6) << updateTime
					<< "." << std::endl);
			}
		}
		lastUpdateTime = updateTime;
		hasLastUpdateTime = true;

		// Delayed-identity confirmation: find the live record whose authored time and native
		// event time both equal one endpoint of the pending jump. Exactly one endpoint must
		// resolve; a spurious rollback matches no record and is rejected, so greyCutoff is
		// only ever written from a real section boundary and is never re-read from the
		// moving owner field.
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
				// Latch the end once, now, while +0x3C8 still holds the real loop end: no
				// owned restart has fired yet, so slot +0x54 has not moved the field. It
				// is never re-read after this. The half-open end gate in
				// FindEarliestEligibleNote drops the next-iteration boundary note.
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
					// The post-jump endpoint is the native section-initialization update.
					// Suppress its scoring pass so it cannot expire the boundary record;
					// selection begins on the next ordinary timeline update.
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
				// Sample the detector gates while Armed too, so the attack-spike stamp
				// (g_lastAttackSpikeTick) sees picks played while the transport is running,
				// including the pick that hold-establishment grace and flow need to measure.
				// The read is a handful of SEH-guarded peeks per tick and calls no native query;
				// the Holding-only branches inside stay inert here.
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
				// What the input gates are doing, for as long as the hold is unsatisfied.
				// Throttled to once a second, and read-only: nothing here calls either
				// native query, because the onset query writes the dedupe global and an
				// extra call would consume the edge the hold is waiting for.
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
					// The rate is reported, not diagnosed. It is the frame rate, and a low
					// frame rate is worth knowing about, but it is not what refuses a note:
					// see the (NBN LAS DETECT) line for that.
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

	// Evaluates the held chord's own hit decision with the detection window
	// slid onto the detector clock when the clock has left the authored window.
	// The slide translates, never widens (the same width the chart authored),
	// so the onset scan looks at "was this chord strummed just now" instead of a
	// span pinned to a time the transport froze at. The authored deltas are
	// restored before returning, whatever the writes did, so a rebuild can never
	// inherit a slid window. When everything reads clean and the clock is still
	// in-window, the original runs untouched.
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
		// Negative window, not a re-centered one. A re-centered slide still evaluates only
		// the current analysis frame (0x4E6A70's positive-window path calls the matcher with
		// frame index 0), and post-attack frames lose a chord's high tones to masking.
		// 0x4E6A70's other path, taken when windowStart is negative, scans every analysis
		// frame of the last 0.2s (_DAT_0119AD60) and accepts if any matches, which includes
		// the attack frame where the full chord's tones exist. That is how chords pass in
		// normal play; a negative start delta routes the frozen evaluation onto the same
		// attack-inclusive path.
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

		// Throttled to once a second, but an accepting slid evaluation always logs.
		// The throttled line is verbose-gated; the accepting slid line is not.
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
			// Stable freeze-per-note mode does not let Rocksmith commit a selected record in
			// the open interval between selection and EstablishHold. A correct early pick is
			// already captured by PickedAttackQueue and is consumed after the hold owns the
			// target; a wrong pitch therefore cannot make the target disappear before that
			// ownership boundary. Flow-until-miss deliberately uses natural Armed commits,
			// and hold-suppressed records deliberately play through, so both remain native.
			// Flow phase 3: a buffered attack confirmed this Armed target (TickArmedFlowBuffer).
			if (gatePhase == GatePhase::Armed && isFlowUntilMissEnabled
				&& armedBufferCommitRecord == selectedRecord)
			{
				if (!wasArmedBufferCommitLogged)
				{
					wasArmedBufferCommitLogged = true;
					// The original decision is not called: it has onset-dedupe side effects.
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
				// Chord hold: instead of blocking, the game's own decision is evaluated. A strum
				// has no single expected pitch, so the native chord detection is the release
				// trigger. A natural true commits (this very decision scores the chord) and hands
				// the release to the commit machinery. Every evaluation is counted.
				// Chords are also evaluated in WaitingForInputRelease, matching single-note flow.
				// A chord successor reached Holding only after the previous input fell silent,
				// which never happens during continuous strumming. The silence wait is redundant
				// for chords: the consume-once boundary (ResetChordOnsetEvidenceAnchor ->
				// chordAttacks.BeginHold at every hold) already bars the previous strum, and the
				// decision still needs a fresh ConfirmChordAttack strum plus a matcher hit, so a
				// decaying ring cannot ride in. A read-ahead strum the player already played can
				// then commit at arm, as a buffered pick does for a single note.
				if (selectedChordId != -1
					&& (gatePhase == GatePhase::Holding
						|| gatePhase == GatePhase::WaitingForInputRelease)
					&& areChordHoldsEnabled)
				{
					const bool naturalResult = EvaluateHeldChordDecision(owner, unusedEdx, note);
					++chordDecisionEvalCount;
					// Native harmonic matcher: the game's own spectral matcher 0x4E6E90 is the reliable
					// chord discriminator under the freeze. It reads the raw analysis spectrum, so it is
					// immune to sounding-table masking, and has no clock/window gate, so the frozen
					// transport does not desync it. Called every tick (the YES peak lasts only a few
					// frames). verdict: 1 YES / 0 ran-and-rejected / -2 input too quiet / -1 could not
					// run (det unresolved or ND tones not set for this record).
					int matcherTones[6] = { 0, 0, 0, 0, 0, 0 };
					int playedTones[6] = { 0, 0, 0, 0, 0, 0 };
					int playedMidiByString[6] = { -1, -1, -1, -1, -1, -1 };
					int matcherToneCount = 0;
					{
						// Native matching uses the game's template. Raw capture validation uses
						// the physical guitar pitch, including the input shift.
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

						// Raw attack validation and the research harness consume played pitches.
						if (matcherToneCount > 0)
						{
							researchChordToneCount = matcherToneCount;
							for (int i = 0; i < matcherToneCount; ++i)
								researchChordTones[i] = playedTones[i];
							researchChordTonesRecord = selectedRecord;
						}
					}
					// Current chord evidence must consume one validated raw attack.
					const bool portRan = matcherToneCount > 0;
					const bool freshAttack = rawAttackStreamAvailable
						&& chordAttacks.HasFreshAttack(pickScanSample, pickSampleRate);
					const uint64_t chordAttackSample = chordAttacks.GetAttackSample();
					// An inherited strum (ChordAttackGate::Inherit) is aged from the hold for the
					// corroboration bar and the native frame scan; raw matchers read at the strum.
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
					// Corroboration for a pitched chord: the game's own vote, or at least one of the
					// chord's tones in its sounding table. A strum with nothing of the chord sounding
					// is not that chord, whatever the raw attack matchers say. Muted chords keep their
					// attack-only path.
					int soundingTargetTones = 0;
					for (int i = 0; i < matcherToneCount; ++i)
					{
						if (ReadNdSoundingStrength(matcherTones[i]) >= 0.0f) ++soundingTargetTones;
					}
					// Without the game's vote the sounding table has to carry the chord on its own, and
					// it has to do so close to the strum. The scan accepts a match anywhere inside the
					// 300 ms attack window, newest frame first, so a strum on the wrong chord followed
					// by a slide onto the right one can match 100-250 ms after the strum with one or two
					// tones sounding. Native-voted accepts land 40-150 ms after the strum with the chord
					// largely sounding, so the unvoted path needs at least half the chord's tones
					// sounding and the strum no older than 100 ms: the chord that was actually strummed,
					// not the one slid into.
					const double attackAgeMs = pickSampleRate != 0 && pickScanSample >= chordAttackOriginSample
						? static_cast<double>(pickScanSample - chordAttackOriginSample) * 1000.0 / pickSampleRate
						: 1e9;
					// Full-chord requirement, on the game's vote and on the mod's path alike: the game's
					// own decision can vote yes with only a few of the chord's tones sounding. Deliberately
					// stricter than the game here: Note by Note is practice, and a chord counts when the
					// chord was strummed. Measured as the peak sounding count since this strum, because one
					// tick's table read is noisy; one tone may be missing on 5- and 6-string shapes.
					if (chordAttackSample != chordSoundingPeakAttack)
					{
						chordSoundingPeakAttack = chordAttackSample;
						chordSoundingPeak = 0;
					}
					if (freshAttack && soundingTargetTones > chordSoundingPeak)
						chordSoundingPeak = soundingTargetTones;
					const int requiredSounding = NoteByNote::GetRequiredChordSoundingToneCount(matcherToneCount);
					// The sounding table under-reports real strums (the game votes yes on strums whose peak
					// sits at 2/4 or 3/4), so the full-chord bar alone is the unvoted path's bar. With the
					// game's vote, half of a chord with at least three tones is enough: the vote is native
					// evidence and the peak rules out a single-note tap. Dyads still require both tones;
					// otherwise half is one and a single note passes. The tier-0 chord probe (every tone with
					// energy at the strum) is logged in shadow at every accept below to see whether it can
					// tell a full strum from a partial at the same peak.
					// What was strummed, measured in the raw audio right after the strum
					// (StrumChordPresence.hpp). Gates the vote-plus-half path below, so a ringing single
					// note with a game vote cannot pass as a half chord.
					const int strumPresence = freshAttack && !fretHandMuted && !isUnison && matcherToneCount >= 2
						? StrumChordPresenceForAttack(playedTones, matcherToneCount, chordAttackSample) : -1;
					if (freshAttack && !fretHandMuted && !isUnison && matcherToneCount >= 2)
						RegisterStrumLedgerAttack(chordAttackSample, playedTones, matcherToneCount);
					const bool nativeMatcherMatches = !isUnison && !naturalResult
						&& NativeChordMatchesAttack(matcherTones, matcherToneCount, chordAttackOriginSample);
					const bool corroborated = NoteByNote::IsChordSoundingCorroborated(matcherToneCount,
						chordSoundingPeak, naturalResult, attackAgeMs,
						// Bass: the strum-presence read (80 ms, +-1 semitone) cannot resolve low tones,
						// so the native evidence alone corroborates.
						strumPresence == 1 || IsBassArrangement(), nativeMatcherMatches);
					const bool rawUnisonMatches = isUnison
						&& RawUnisonMatchesAttack(unisonPitch, chordAttackSample);
					// Exact fresh-attack evidence must confirm a correctly played chord even when the
					// game also votes yes but the sounding table under-reports it. Both matchers still
					// require every authored tone/string, so removing the native-vote guard cannot let a
					// single note pass as a chord; it only stops a redundant vote from masking exact
					// evidence.
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
					// Power chord: the game's vote or matcher on this strum (latched, since the vote
					// flickers tick to tick), plus the fifth confirmed in the raw audio. Measured only
					// once the game agreed, so a strum the game rejects costs no raw query.
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
					// Strum Ledger, stage 2: once the ledger has read this strum it decides. Strummed
					// (every required tone rose at the attack) plus the validated legacy chord decision
					// accepts; not strummed refuses whatever the other paths say. Before the ledger has
					// read (< 85 ms), only a 3+ tone chord with the game's vote and a full table may
					// accept; dyads wait.
					bool ledgerDecided = false;
					bool pitchesMatch = legacyPitchesMatch;
					// Not on bass: the ledger's 80 ms reads cannot resolve low tones (a -1/0 verdict would
					// refuse every bass dyad), so the older native vote/matcher decision stands there.
					if (NoteByNote::STRUM_LEDGER_DECIDES && !IsBassArrangement()
						&& freshAttack && !fretHandMuted && !isUnison && matcherToneCount >= 2)
					{
						ProcessPendingStrumLedger();
						const int ledgerVerdict = GetStrumLedgerVerdict(chordAttackSample);
						// The legacy decision has already applied each chord detector's
						// corroboration contract. Raw vote, matcher, ML, or a partial table
						// must not bypass those guards and turn a ledger strum into an accept.
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
					// Re-strum of the chord just played. Its tones are still ringing, so they cannot rise
					// at the strum and the ledger and the strum-presence reads refuse it. The detected strum
					// is the fresh attack; the pitch evidence is the game's vote plus at least half the
					// chord sounding. Nothing passes without a strum.
					// The same applies to every chord of 3+ tones: a barre's upper strings do not jump
					// enough for the ledger's per-tone rise test. Dyads keep their checks unless it is a
					// re-strum: half a dyad is one note, and a single pick must not pass as a two-note chord.
					const bool isRestrumOfSameChord = lastCommittedChordId >= 0
						&& lastCommittedChordId == selectedChordId && lastCommittedChordRecord != selectedRecord;
					// A dyad the game fully agrees with: its vote or chord matcher on this strum (latched),
					// one of them now, and both tones in its sounding table. The ledger can refuse such a
					// dyad when one tone's harmonic read is too weak. The sounding table can include
					// harmonics; validated pitch confirmation remains required before this path may accept.
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
					// Late native-matcher hit (100-200 ms, full chord sounding, no game vote): logs whether
					// the strum-presence gate (ChordPitchDecision LATE_NATIVE_MATCH_*) let it through.
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
							// Same count the decision used (a second read of the live table in the
							// same tick can differ).
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
							// HUD accept readout: green only on the detector(s) that passed this chord.
							// Native = the game's vote or its own chord matcher; Enhanced = the raw-audio
							// pitch paths (unison, close dyad, tier-0) or a fret-hand mute's attack;
							// ML = the per-string model.
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
							// Name what was matched instead of a bare "chord".
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
						// Shadow verdict of the tier-0 chord probe at the accept, to learn whether it
						// separates a full strum from a partial at the same sounding peak (see the
						// corroboration comment above). Observation only; it decides nothing here.
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
						// Identity in every throttled line: some chords accept in under
						// 500 evaluations while others refuse thousands with real input
						// sounding, and whatever distinguishes them must be visible here.
						// The template identity (name + frets, read from the very note
						// object being judged) is the desync detector: if it disagrees
						// with what the screen shows, the visuals are stale; if it
						// disagrees with the record's chordId, the note object is stale.
						char chordIdentity[64] = "?";
						NoteByNoteNativeScoring::TryDescribeChordTarget(
							reinterpret_cast<uintptr_t>(note), chordIdentity, sizeof(chordIdentity));
						// ND sounding-table chord shadow: what the table holds for each chord tone, next
						// to every throttled refusal. Refused chords showing all playable tones present
						// mean the per-tone check works; missing high tones (masking) mean chords need
						// the 0x4E6E90 harmonic matcher instead.
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

		// Close-note protection: while a selected record owns the gate, no other
		// non-grey identity may consume a hit decision.
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
	// Fail loud if the raw route snapshot bridge is not wired (a loaded probe on a host that
	// predates the HostApi v9 CaptureRawSnapshot slot). Without it, ConfirmChordAttack, close-dyad,
	// unison and fret-hand-mute all read as sustain and no chord ever confirms. Checking the bridge
	// slot (not a live capture) avoids a false alarm before any audio has been captured.
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
	// Keep the grace inside the native detect window's late edge (~0.3s past record time):
	// the frozen clock must still fall within the note's window or native calls it dead.
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
	// This one line always prints (it is not itself gated) so the toggle is visible in
	// the log even when everything else falls quiet.
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

	// The name field is authored ASCII; anything unprintable means the template
	// read is not what it claims to be, so the name is dropped, not sanitized.
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
		// Same rule as TryDescribeChordTarget: an unprintable name means a bad read, so it is dropped.
		char name[sizeof(view.name) + 1] = {};
		for (size_t i = 0; i < sizeof(view.name) && view.name[i] != '\0'; ++i)
		{
			if (view.name[i] < 0x20 || view.name[i] > 0x7E) { name[0] = '\0'; break; }
			name[i] = view.name[i];
		}
		// Only the root note: a D chord is an embellished D, so "Dsus2", "D/F#" and "Dmaj7" all
		// read "D". The authored name's leading letter and accidental carry the chart's spelling.
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
	// No usable name: the lowest tone (player's physical frame), the bass a player hears as the root.
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

void NoteByNoteNativeScoring::RequestNativeSeekTest()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isNativeSeekTestRequested = true;
	LOG_INFO("(NBN NATIVE SEEK) Test requested; the next idle scoring tick"
		<< " performs one StartAt-core call on the main thread." << std::endl);
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

void NoteByNoteNativeScoring::RequestFreezeModeTest()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isFreezeModeTestArmed = true;
	LOG_INFO("(NBN MODE) Freeze-mode test armed: the next established hold enters"
		<< " the game's own frozen mode (FreezeSong core, mode 2,0) and its"
		<< " release exits (UnfreezeSong core, mode 0,2). One-shot." << std::endl);
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
	// Under flow the Armed target is known before any hold (HUD, ML and autoplay use it).
	state.expectedMidi = JudgeMidi();
	RefreshDetectionFeedbackMl();
	state.detectionFeedback = detectionFeedback;

	// The current chord target's tones for the fake-guitar harness, only when the target
	// really is a chord and the published set was read for this exact record (otherwise a
	// previous chord's tones would leak onto a single note or a just-changed target).
	if (selectedChordId >= 0 && researchChordToneCount > 0
		&& researchChordTonesRecord == selectedRecord)
	{
		state.expectedChordToneCount = (std::min)(static_cast<uint32_t>(researchChordToneCount),
			ResearchProtocol::NoteByNoteState::MaxChordTones);
		for (uint32_t i = 0; i < state.expectedChordToneCount; ++i)
			state.expectedChordTones[i] = researchChordTones[i];
	}

	// The target's bend, for the fake-guitar harness to glide up to instead of sounding a
	// static note (available throughout the hold, unlike the live bend-visualizer fields).
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

	// Bend visualizer feed. Populated whenever a bend gesture is the business at
	// hand; the sounding pitch is read fresh on every call so the overlay (which
	// polls per frame) animates like a tuner. Reads only; neither native query is
	// called, so the onset edge and dedupe global are untouched. The feed logs
	// itself (throttled, into the trace file) to tell a branch not taken from
	// values lost in marshalling.
	{
		static std::chrono::steady_clock::time_point bendFeedLogAnchor;
		static bool hasBendFeedLogAnchor = false;
		const auto now = std::chrono::steady_clock::now();
		// Verbose- and input-gated: off it never prints; on, a put-down guitar stops
		// flooding the buffer and it resumes on real input.
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
	// Input-health feed, from the per-tick cache rather than a fresh read: the
	// scoring tick refreshes it 20-60 times a second, plenty for a strum meter,
	// and a fresh read (a VirtualQuery syscall per TryRead, up to three polls a
	// frame) costs real frame time. A sample older than two seconds means scoring
	// is not ticking (menus, idle) and the line withdraws instead of showing a
	// stale or legitimately-muted level as DEAD.
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
		// Why the hold is not progressing right now, for the HUD's veto line: the detector's own
		// refusal (its visible gates, or "none-visible" = the game's hidden onset/confidence gate)
		// and the transport phase the mod is in. The detector rows keep showing detection truth;
		// this names the stage that is actually holding the note back.
		strncpy_s(state.holdRefusal, DescribeDetectorRefusal(lastStateGateSample), _TRUNCATE);
	}
	strncpy_s(state.holdPhase, DescribeGatePhase(gatePhase), _TRUNCATE);
	state.bendBaseMidi = bendVisualizationSnapshot.baseMidi;
	state.bendTargetMidi = bendVisualizationSnapshot.targetMidi;
	state.soundingMidi = bendVisualizationSnapshot.soundingMidi;
	state.soundingQuality = bendVisualizationSnapshot.soundingQuality;
	// The meter's green: the engine's reached verdict, valid only for the exact selected
	// record and Riff Repeater epoch on which it advanced the bend gesture.
	state.bendReachedTarget = bendVisualReached
		&& selectedRecord == bendVisualReachedRecord
		&& epochIndex == bendVisualReachedEpoch ? 1 : 0;

	// Detection-strategy authority + live native-vs-ML agreement, for the corner HUD strip.
	// HUD flag: ML participates in the decision (rescue/veto) whenever the technique is not
	// forced NativeOnly. In Blend (the default) ML is active as rescue + guarded veto.
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
	// Latch only; the next scoring tick runs ResetBootstrap for the live owner. Doing
	// the reset here would touch owner state off the scoring thread. Cheap and
	// idempotent: a redundant enable (no section change) just re-arms the same section.
	reArmRequested = true;
	LOG_INFO("(NBN LAS BOOTSTRAP) Re-arm requested on enable; the next scoring tick"
		<< " re-bootstraps for the current section." << std::endl);
}
