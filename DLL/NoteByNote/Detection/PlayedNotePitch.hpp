#pragma once

namespace NoteByNote
{
	// Bass arrangements chart strings 0..3 (E1 A1 D2 G2). The game's own detector says which
	// instrument is loaded: its open-string table (detector+0x134C) holds four notes and two
	// zeros on bass, see IsBassArrangement in NoteByNoteNativeScoring.cpp.
	inline bool TryResolvePlayedNotePitch(int stringIndex, int fret, int tuningOffset, int inputShift, int& midi,
		bool bass = false)
	{
		constexpr int OPEN_MIDI[] = { 40, 45, 50, 55, 59, 64 };
		constexpr int BASS_OPEN_MIDI[] = { 28, 33, 38, 43 };
		midi = -1;
		const int stringCount = bass ? 4 : 6;
		if (stringIndex < 0 || stringIndex >= stringCount || fret < 0 || fret >= 26
			|| tuningOffset < -12 || tuningOffset > 12 || inputShift < -48 || inputShift > 48) return false;
		const int resolved = (bass ? BASS_OPEN_MIDI[stringIndex] : OPEN_MIDI[stringIndex])
			+ tuningOffset + fret + inputShift;
		if (resolved < 0 || resolved > 127) return false;
		midi = resolved;
		return true;
	}
}
