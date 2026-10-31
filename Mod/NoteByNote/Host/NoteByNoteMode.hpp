#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace NoteByNoteProbe {
	struct NativeRenderedNote
	{
		int stringIndex = -1;
		int fret = -1;
	};

	struct NativeRenderedAttack
	{
		bool isTransition = false;
		uint64_t renderFrame = 0;
		float songTime = 0.0f;
		float longitudinalPosition = 0.0f;
		std::vector<NativeRenderedNote> notes;
	};

	struct NativeScoringNote
	{
		uintptr_t nativeNoteAddress = 0;
		uintptr_t recordAddress = 0;
		uint32_t noteMask = 0;
		uint32_t noteFlags = 0;
		uint32_t noteHash = 0;
		float authoredTime = 0.0f;
		float nativeEventTime = 0.0f;
		int stringIndex = -1;
		int fret = -1;
		int chordId = -1;
		int chordNotesId = -1;
		int phraseIterationId = -1;
		float range9C = 0.0f;
		float rangeA0 = 0.0f;
		float rangeA4 = 0.0f;
		float rangeA8 = 0.0f;
		float rangeAC = 0.0f;
		float rangeB0 = 0.0f;
		uint8_t state50 = 0;
		uint8_t state51 = 0;
		uint8_t state52 = 0;
		uint8_t stateC0 = 0;
		uint8_t stateC1 = 0;
		uint8_t stateC2 = 0;
		uint8_t stateC3 = 0;
		uint8_t state144 = 0;
		int32_t bendSemitones = 0;
	};

	struct NativeScoringFrame
	{
		uintptr_t ownerAddress = 0;
		uintptr_t noteListBeginAddress = 0;
		uintptr_t noteListEndAddress = 0;
		float updateTime = 0.0f;
		bool isAfterUpdate = false;
		bool isInPlayableRange = false;
		bool wasLastNoteHit = false;
		size_t nativeNoteSlotCount = 0;
		size_t unreadableNoteSlotCount = 0;
		std::vector<NativeScoringNote> notes;
	};

	enum class NativeExpectedAttackEventKind
	{
		CandidateChanged,
		HoldEstablished,
		ScoringStateChanged
	};

	struct NativeExpectedAttackEvent
	{
		NativeExpectedAttackEventKind kind = NativeExpectedAttackEventKind::CandidateChanged;
		uintptr_t ownerAddress = 0;
		uint64_t epoch = 0;
		float updateTime = 0.0f;
		bool isAfterUpdate = false;
		bool isInPlayableRange = false;
		bool wasLastNoteHit = false;
		bool hasPreviousNote = false;
		bool isNotePresent = true;
		NativeScoringNote previousNote;
		NativeScoringNote note;
	};

	std::string GetAutomaticTargetText();
	void HandleNativeRenderedAttack(const NativeRenderedAttack& attack);
	void HandleNativeControllerFault(const std::string& reason);
	void HandleNativeExpectedAttackEvent(const NativeExpectedAttackEvent& event);
	void Initialize();
	bool IsAutomaticEnabled();
	bool IsAutomaticEnabledFast();
	// MainThread, after each GameState::currentMenu update (outside the Loading phase): copies what the other threads
	// need into atomics and a locked copy of the menu name.
	void PublishMenuState();
	// Any thread, from the last PublishMenuState: empty / false before the first.
	std::string CurrentMenu();
	void CopyCurrentMenu(std::string& out);   // the same into `out`, reusing its buffer
	bool IsInRiffRepeaterMenusFast();
	bool IsInLasPlayingFast();   // GameState::Menus::IsInLASPlayingModes
	// In a song mode and not in the Riff Repeater menus (4.x's HUD gate: IsInSongModes && !IsInRiffRepeaterMenus).
	bool IsPracticingFast();
	// The game's scoring thread. Lock-free.
	bool IsFreezeActive();
	// The game's scoring thread: whether the scoring detour hands this update to the controller (Note by Note on, or
	// its stop not yet seen by the controller).
	bool ShouldDispatchScoringUpdate();
	// MainThread: retires Note by Note after 1.5 s outside the song and the Riff Repeater menus.
	void TickLifecycle();
	bool SetAutomaticEnabled(bool shouldEnable);
	bool ToggleAutomatic();
	// Re-arm the enabled state directly, bypassing the Riff Repeater menu gate. For restoring the player's
	// state across a controller reload, where the gate was already passed.
	void ForceRestoreAutomatic();
}
