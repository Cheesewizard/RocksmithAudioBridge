#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace NoteByNote
{
	namespace Tablature
	{
		enum Technique : std::uint32_t
		{
			HammerOn = 1u,
			PullOff = 2u,
			Bend = 4u,
			Slide = 8u,
			Muted = 16u,
			Harmonic = 32u,
			Tap = 64u
		};

		struct Position
		{
			int stringIndex = 0;
			int fret = 0;
			std::uint32_t techniques = 0;
			int bendSemitones = 0;
			int slideTargetFret = -1;
		};

		struct Event
		{
			std::uint64_t id = 0;
			float time = 0.0f;
			std::vector<Position> positions;
		};

		struct Snapshot
		{
			std::uint64_t generation = 0;
			std::uint64_t currentTargetId = 0;
			std::uint64_t arrangementKey = 0;
			// True when showing the player's current levels, false for 100%.
			bool showsCurrentLevels = true;
			// Current mode only: false when the game's levels could not be read
			// and the snapshot fell back to 100%.
			bool currentLevelsRead = true;
			// Current mode: phrases shown below 100% (0 = the chart already is 100%).
			std::uint32_t iterationsBelowMax = 0;
			// The game's loop / practice range in song seconds, when known.
			bool loopValid = false;
			float loopStart = 0.0f;
			float loopEnd = 0.0f;
			// Section start times, drawn as section lines.
			std::vector<float> sectionStarts;
			// Bar lines: downbeat time and measure number (from 0).
			std::vector<std::pair<float, int>> bars;
			int stringCount = 6;
			std::string tuningLabels[6];
			std::vector<Event> events;
			std::string error;
		};

		// Draws one self-contained, scrollable tablature window. The snapshot is read-only;
		// scrolling and target-follow state are presentation state owned by this function.
		// Sets *toggleLevels when the user presses the 100% / current level button and
		// *toggleDock when they press the dock button. Docked, the window spans the
		// top of the screen; its height stays the user's.
		void Draw(const Snapshot& snapshot, float textScale, bool* open, bool* toggleLevels = nullptr,
			bool docked = false, bool* toggleDock = nullptr);
	}
}
