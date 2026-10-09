#pragma once

#include <cstdint>

// Guitar or bass, from the game's own note detector. Both instruments use the Player 1 detection slot *(0x0135F57C)+0x10; +0x14 is
// player 2, not bass. The detector's open-string table at detector+0x134C (int16[6], MIDI in the
// detector's tuning frame) holds four notes and two zeros on bass ({28,33,38,43,0,0} for E
// standard) and six notes on guitar. The Note by Note probe and the host's ML exporter both use
// this, so the note decision and the ML model always agree on the instrument.
namespace ArrangementInstrument
{
	constexpr uintptr_t DETECTION_ROOT = 0x0135F57C;
	constexpr uintptr_t PLAYER_ONE_ARRANGEMENT = 0x10;
	constexpr uintptr_t ARRANGEMENT_ENGINE = 0x08;
	constexpr uintptr_t ENGINE_DETECTOR = 0x04;
	constexpr uintptr_t DETECTOR_OPEN_STRING_MIDI = 0x134C;

	enum class Kind : int32_t { Guitar = 0, Bass = 1 };

	// The two unused slots carry the tuning offset too: 0 in E standard, -1 in Eb ({27,32,37,42,-1,-1}),
	// so an == 0 test would read a detuned bass as a guitar. Unused means "not a note": zero or below.
	inline bool IsBassOpenStringTable(const int16_t* open)
	{
		for (int index = 0; index < 4; ++index)
		{
			if (open[index] <= 0 || open[index] >= 128) return false;
		}
		return open[4] <= 0 && open[5] <= 0;
	}

	// Open-string MIDI (E standard) the ML model's string slots stand for. Bass uses slots 0..3.
	inline const int* OpenStringMidi(Kind kind)
	{
		static const int guitar[6] = { 40, 45, 50, 55, 59, 64 };
		static const int bass[6] = { 28, 33, 38, 43, -1, -1 };
		return kind == Kind::Bass ? bass : guitar;
	}

	inline int StringCount(Kind kind) { return kind == Kind::Bass ? 4 : 6; }

	// Reads the live detector's open-string table and reports whether it is a bass table. For
	// callers that cannot reach the controller's cached flag (the host's D3D marker filter keeps
	// working while a hot-loaded probe owns the controller). False when anything is unreadable.
	inline bool TryReadIsBass(bool& isBass)
	{
		__try
		{
			const auto root = *reinterpret_cast<const volatile uintptr_t*>(DETECTION_ROOT);
			if (root == 0) return false;
			const auto arrangement = *reinterpret_cast<const volatile uintptr_t*>(root + PLAYER_ONE_ARRANGEMENT);
			if (arrangement == 0) return false;
			const auto engine = *reinterpret_cast<const volatile uintptr_t*>(arrangement + ARRANGEMENT_ENGINE);
			if (engine == 0) return false;
			const auto detector = *reinterpret_cast<const volatile uintptr_t*>(engine + ENGINE_DETECTOR);
			if (detector == 0) return false;
			int16_t open[6];
			for (int index = 0; index < 6; ++index)
			{
				open[index] = *reinterpret_cast<const volatile int16_t*>(
					detector + DETECTOR_OPEN_STRING_MIDI + index * sizeof(int16_t));
			}
			isBass = IsBassOpenStringTable(open);
			return true;
		}
		__except (1)
		{
			return false;
		}
	}
}
