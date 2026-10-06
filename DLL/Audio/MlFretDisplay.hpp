#pragma once

#include "MlStringFretReader.hpp"
#include <cmath>

namespace MlFretDisplay
{
	inline bool ResolveExpectedPosition(MlStringFretReader::StringFret& display,
		int expectedString, int physicalFret, int expectedMidi, float minimumConfidence)
	{
		static constexpr int OPEN_MIDI[6] = { 40, 45, 50, 55, 59, 64 };
		if (expectedString < 0 || expectedString >= 6 || physicalFret < 0 || physicalFret > 24
			|| !std::isfinite(minimumConfidence) || minimumConfidence <= 0 || minimumConfidence > 1)
		{
			return false;
		}
		const int soundingFret = physicalFret + display.shift;
		if (OPEN_MIDI[expectedString] + soundingFret != expectedMidi) return false;
		float matchingConfidence = -1.0f;
		float competingConfidence = -1.0f;
		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			const float confidence = display.conf[stringIndex];
			if (display.fret[stringIndex] < 0 || display.fret[stringIndex] > 19
				|| !std::isfinite(confidence) || confidence < 0 || confidence > 1) continue;
			if (OPEN_MIDI[stringIndex] + display.fret[stringIndex] == expectedMidi)
			{
				if (confidence > matchingConfidence) matchingConfidence = confidence;
			}
			else
			{
				if (stringIndex == expectedString && confidence >= minimumConfidence) return false;
				if (confidence > competingConfidence) competingConfidence = confidence;
			}
		}
		if (matchingConfidence < minimumConfidence || matchingConfidence < competingConfidence) return false;
		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			if (display.fret[stringIndex] >= 0
				&& OPEN_MIDI[stringIndex] + display.fret[stringIndex] == expectedMidi)
			{
				display.fret[stringIndex] = -1;
				display.physFret[stringIndex] = -1;
				display.conf[stringIndex] = 0;
			}
		}
		display.fret[expectedString] = soundingFret;
		display.physFret[expectedString] = physicalFret;
		display.conf[expectedString] = matchingConfidence;
		return true;
	}
}
