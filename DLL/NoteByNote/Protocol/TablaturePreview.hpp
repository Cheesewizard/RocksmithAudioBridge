#pragma once

#include <cstdint>

namespace ResearchProtocol
{
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_HAMMER_ON = 1u;
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_PULL_OFF = 2u;
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_BEND = 4u;
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_SLIDE = 8u;
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_MUTED = 16u;
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_HARMONIC = 32u;
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_TAP = 64u;
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_PINCH_HARMONIC = 128u;
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_PALM_MUTE = 256u;
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_VIBRATO = 512u;
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_TREMOLO = 1024u;
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_ACCENT = 2048u;
	// A linked continuation of the previous note on the string (the chart's
	// linkNext child): drawn as a tied note, never a target.
	constexpr uint32_t TAB_PREVIEW_TECHNIQUE_TIED = 4096u;

	struct PreviewPosition
	{
		int32_t stringIndex = -1;
		int32_t fret = -1;
		uint32_t techniques = 0;
		int32_t bendSemitones = 0;
		int32_t slideTargetFret = -1;
		// Seconds the note rings (vibrato length), 0 when not authored.
		float sustain = 0.0f;
		// Bend shape in semitones from the chart's bend curve: the pitch at the
		// attack (above 0 = pre-bend), the highest point and the last point
		// (below the peak = a release). All 0 when the note is not bent.
		float bendStart = 0.0f;
		float bendPeak = 0.0f;
		float bendEnd = 0.0f;
	};

	struct PreviewEvent
	{
		uint64_t id = 0;
		float time = 0.0f;
		uint32_t positionCount = 0;
		PreviewPosition positions[6] = {};
		// The chord's name from its template ("Em", "A5"), empty for single notes
		// and unnamed chords. Always NUL terminated.
		char chordName[20] = {};
	};

	// Which authored level each phrase iteration shows.
	constexpr uint8_t TAB_PREVIEW_LEVEL_MAXIMUM = 0;  // each phrase's 100% level
	constexpr uint8_t TAB_PREVIEW_LEVEL_CURRENT = 1;  // the level the player is on now

	struct PreviewState
	{
		uint64_t generation = 0;
		uint64_t currentTargetId = 0;
		// Changes with the song/arrangement but not with level changes, so the
		// window can keep its scroll place across a difficulty change.
		uint64_t arrangementKey = 0;
		uint32_t eventCount = 0;
		uint32_t stringCount = 6;
		int32_t tuningOffsets[6] = {};
		uint8_t available = 0;
		// In: the level mode the window wants. Out: the mode of the snapshot.
		uint8_t levelMode = TAB_PREVIEW_LEVEL_CURRENT;
		// Out, current mode only: 1 when the game's per-phrase levels were read,
		// 0 when the snapshot fell back to each phrase's 100% level.
		uint8_t currentLevelsRead = 0;
		// Out, current mode only: phrase iterations shown below their 100% level
		// (0 means the current level already is the full chart).
		uint32_t iterationsBelowMax = 0;
		// In: 1 while the tab window is drawn open (not collapsed). The controller
		// then holds every note whatever is played, bypassing detection. It lapses
		// on its own when the window stops asking (collapsed, closed, hidden,
		// menus), so a hidden window can never leave the song frozen.
		uint8_t readerHold = 0;
		// Out: 1 while a note is held because the tab is open.
		uint8_t readerHolding = 0;
		// The game's current loop / practice range (Riff Repeater), in song
		// seconds; valid only when loopValid is 1. Refreshed every frame.
		uint8_t loopValid = 0;
		float loopStart = 0.0f;
		float loopEnd = 0.0f;
		// Start times of the arrangement's sections (the Riff Repeater sections).
		static constexpr uint32_t MaxSections = 256;
		uint32_t sectionCount = 0;
		float sectionStarts[MaxSections] = {};
		// The section names ("verse", "chorus"), parallel to sectionStarts.
		char sectionNames[MaxSections][20] = {};
		// Bar lines: downbeat times and their measure numbers (from 0), in order.
		static constexpr uint32_t MaxBars = 1024;
		uint32_t barCount = 0;
		float barStarts[MaxBars] = {};
		int32_t barNumbers[MaxBars] = {};
		char error[160] = {};
	};
}
