#pragma once

#include "NoteByNoteProbe.hpp"
#include "../Research/ResearchProtocol.hpp"

namespace NoteByNoteNativeScoring
{
	void Initialize();
	bool IsAvailable();

	// Generic observation-hook service. The bridge calls SetRequestedGenericHooks after every
	// probe load and unload with the loaded probe's requested addresses (or an empty list). The
	// request is staged and reconciled from a main-thread render seam: new addresses get a
	// pass-through detour, and slots the current probe no longer wants stop being forwarded.
	// Detours are never removed; an unclaimed hook simply runs the original and notifies no one.
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
	// Re-arm the section bootstrap on the next scoring tick. Called on every false->true
	// enable (N toggle and debug-state restore): the owner-identity reset cannot see a
	// section change that keeps the same GamePlaysongLAS owner (Riff Repeater). Dispatches
	// like Stop; the loaded probe latches the request under the controller lock.
	void RequestReArm();
	bool TryGetResearchState(ResearchProtocol::NoteByNoteState& state);
	// Chord holds: on = held chords release on the game's own hit decision;
	// off = chords play through natively.
	void SetChordHoldsEnabled(bool enabled);
	// Play_FreezeNoteTrack before each single-note hold: the per-note percussion cue.
	// Default off; probe_freeze_sound_on brings the cue back.
	void SetFreezePromptSoundEnabled(bool enabled);
	// Flow-until-miss: on = the hold boundary sits late (recordTime + grace) so on-time
	// notes commit naturally and the transport plays straight through, with no per-note
	// freeze/restart; off = the freeze-per-note boundary (recordTime - compensation).
	// SetFlowLateGraceSeconds tunes how late.
	void SetFlowUntilMissEnabled(bool enabled);
	bool GetFlowUntilMissEnabled();
	void SetFlowLateGraceSeconds(float seconds);
	float GetFlowLateGraceSeconds();

	bool GetChordHoldsEnabled();
	// Verbose trace: on = per-tick DETECT/BEND/CHORD-WINDOW/eval-false logs for diagnosis;
	// off (default) = event logs only. Toggled over the probe-command channel.
	void SetVerboseTrace(bool enabled);
	bool GetVerboseTrace();
	// Detection strategy per technique. Blend (default) fuses native + tier-0 + ML so each can
	// rescue the other where it is weak, and neither vetoes the other unless it is confidently
	// right (ML's veto is confidence-gated and octave-tolerant, and ML latency never stalls a
	// native accept). NativeOnly and MlOnly force a single engine for testing; set over the
	// probe-command channel (probe_detect_{chord,note,bend}_{blend,native,ml}).
	enum class DetectionStrategy { Blend = 0, NativeOnly = 1, MlOnly = 2 };
	void SetChordDetectionStrategy(DetectionStrategy strategy);
	void SetNoteDetectionStrategy(DetectionStrategy strategy);
	void SetBendDetectionStrategy(DetectionStrategy strategy);
	// Chord window slide: on = a held chord whose detection window the
	// detector clock has left is evaluated with the window slid onto the clock;
	// off = authored windows only, refusals measured but not repaired.
	void SetChordWindowSlideEnabled(bool enabled);
	bool GetChordWindowSlideEnabled();
	void SetChordTier0RescueEnabled(bool enabled);   // per-tone energy chord rescue (default off)
	bool GetChordTier0RescueEnabled();
	// Repeat-strum holds: on = bare-0x2 repeat records hold like full chords;
	// off = they play through (skipped strums count missed).
	void SetRepeatStrumHoldsEnabled(bool enabled);
	bool GetRepeatStrumHoldsEnabled();
	// Native freeze announcement: on = owned holds write the game's own frozen-on-tag
	// byte at owner+0x5E3, the flag GE_FreezeOnTag sets so native systems enter the
	// freeze presentation. Default off.
	void SetNativeFreezeFlagEnabled(bool enabled);
	bool GetNativeFreezeFlagEnabled();
	// Native chord panel: on = chord holds call the lesson engine's
	// own GE_ShowChordDisplay implementation with the chord's id, so the game
	// draws its frozen chord panel; hidden again at release.
	void SetNativeChordPanelEnabled(bool enabled);
	bool GetNativeChordPanelEnabled();
	// Timed safety release (default off): on = a no-progress hold releases after its
	// budget (15s chords / 60s singles); off = it stays held, logging its refusing state
	// each interval, and the N toggle is the release. Off by default because the release
	// reads as a false accept and masks the refusal evidence.
	void SetSafetyReleaseEnabled(bool enabled);
	// Native schedule shift (experimental, default off): on = each owned release calls the
	// game's scheduler service (0x57EB00, entry 4, op add) with the frozen span, replicating
	// the frozen-span destructor's anti-sweep compensation. Bridge: schedule-shift-on|off.
	void SetScheduleShiftEnabled(bool enabled);
	// Diagnostic: request one StartAt-core call (the game's own resume + seek-to-now +
	// speed reset, 0x4749C0 with an empty tag) on the next idle scoring tick, main-thread,
	// vtable-guarded. Bridge: native-seek-test.
	void RequestNativeSeekTest();
	// Hybrid native release (default off): on = owned releases run the StartAt core (the
	// game's own unfreeze + seek + speed reset) followed by the coordinated PlayerSong
	// restart for the music. StartAt alone leaves the PlayerSong stopped, and adding
	// 0x4729E0 (GE_HideGame's core) fades the game to black. Bridge: native-release-on|off.
	void SetNativeReleaseEnabled(bool enabled);
	// Native single-note acceptance (default on): the lesson engine's own primitive, the
	// expected pitch present in the detector's current-sounding table (+0x604/+0x6A4) above
	// the native threshold (0x01199DD4) continuously for ~0.18s. Runs ahead of the heuristic
	// recoveries; nd-accept-off disables it.
	void SetNdAcceptEnabled(bool enabled);
	// Diagnostic: arm one hold to enter the game's own frozen mode (GE_FreezeSong core
	// 0x474840: owner+0x5E2 + mode call 2,0, which creates the frozen-mode object, vtable
	// 0x011A0B70) on top of the mechanical freeze, exiting at release via the UnfreezeSong
	// core 0x474890 (native ResumeFromTag unwind + mode 0,2). Bridge: freeze-mode-test.
	void RequestFreezeModeTest();
	// Formats the chord a native note object would be judged as ("G5 [3/5/5/x/x/x]")
	// from the SNG chord template at note+0x30 (name at +0x28, per-string frets
	// at +0x04, values < 0x1A playable). Returns false when the note carries no
	// readable template. Used by the desync logs.
	bool TryDescribeSelectedChordTarget(uintptr_t record, char* buffer, size_t bufferLength);
	bool TryDescribeChordTarget(uintptr_t noteAddress, char* buffer, size_t bufferLength);
	// The chord's root note for the HUD's accept readout: from the authored name ("Dsus2" -> "D"), else the lowest tone.
	bool TryDescribeChordLabel(uintptr_t noteAddress, const int* tones, int toneCount,
		char* buffer, size_t bufferLength);
	// Formats only the selected chord's six-string fingering for the compact HUD row and
	// returns its lowest played string for the matching colour marker.
	bool TryDescribeSelectedChordFingering(
		uintptr_t record,
		char* buffer,
		size_t bufferLength,
		int& lowestPlayedString);
	// True while the loaded arrangement is bass (four-string detector table), refreshed when a
	// hold is established. The stale-marker filter needs it to pick the bass neck geometry.
	bool IsBassArrangementActive();
}
