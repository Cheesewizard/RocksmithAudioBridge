#pragma once

#include <cmath>
#include <cstdint>

namespace NoteByNote
{
	inline bool ConfirmMlChordStrings(
		const int* expectedMidiByString,
		const int* observedMidiByString,
		const float* confidenceByString,
		float minimumConfidence,
		uint8_t& requiredStringMask,
		uint8_t& matchedStringMask)
	{
		requiredStringMask = 0;
		matchedStringMask = 0;
		if (expectedMidiByString == nullptr || observedMidiByString == nullptr
			|| confidenceByString == nullptr || !std::isfinite(minimumConfidence)
			|| minimumConfidence <= 0.0f || minimumConfidence > 1.0f)
		{
			return false;
		}

		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			const int expectedMidi = expectedMidiByString[stringIndex];
			if (expectedMidi < 0) continue;
			if (expectedMidi > 127) return false;

			const auto stringBit = static_cast<uint8_t>(1u << stringIndex);
			requiredStringMask |= stringBit;
			const float confidence = confidenceByString[stringIndex];
			if (observedMidiByString[stringIndex] == expectedMidi
				&& std::isfinite(confidence) && confidence >= minimumConfidence)
			{
				matchedStringMask |= stringBit;
			}
		}

		return requiredStringMask != 0 && matchedStringMask == requiredStringMask;
	}
}
