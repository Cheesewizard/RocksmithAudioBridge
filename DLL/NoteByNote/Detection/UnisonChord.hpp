#pragma once

namespace NoteByNote
{
	inline bool TryGetUnisonPitch(const int* tones, int count, int& pitch)
	{
		pitch = -1;
		if (!tones || count < 2 || count > 6 || tones[0] < 0 || tones[0] > 127) return false;
		for (int index = 1; index < count; ++index)
		{
			if (tones[index] != tones[0]) return false;
		}
		pitch = tones[0];
		return true;
	}
}
