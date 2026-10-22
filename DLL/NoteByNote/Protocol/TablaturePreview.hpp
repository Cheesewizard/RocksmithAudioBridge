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

	struct PreviewPosition
	{
		int32_t stringIndex = -1;
		int32_t fret = -1;
		uint32_t techniques = 0;
		int32_t bendSemitones = 0;
		int32_t slideTargetFret = -1;
	};

	struct PreviewEvent
	{
		uint64_t id = 0;
		float time = 0.0f;
		uint32_t positionCount = 0;
		PreviewPosition positions[6] = {};
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
		// The game's current loop / practice range (Riff Repeater), in song
		// seconds; valid only when loopValid is 1. Refreshed every frame.
		uint8_t loopValid = 0;
		float loopStart = 0.0f;
		float loopEnd = 0.0f;
		// Start times of the arrangement's sections (the Riff Repeater sections).
		static constexpr uint32_t MaxSections = 256;
		uint32_t sectionCount = 0;
		float sectionStarts[MaxSections] = {};
		// Bar lines: downbeat times and their measure numbers (from 0), in order.
		static constexpr uint32_t MaxBars = 1024;
		uint32_t barCount = 0;
		float barStarts[MaxBars] = {};
		int32_t barNumbers[MaxBars] = {};
		char error[160] = {};
	};
}
