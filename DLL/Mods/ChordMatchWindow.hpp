#pragma once

#include <cmath>
#include <cstdint>

namespace NoteByNote
{
	template<typename ReadTimestamp, typename MatchFrame>
	bool MatchChordAttackFrames(double newestTime, double holdTime, uint64_t attackSample,
		uint64_t currentSample, uint32_t sampleRate, int frameCount,
		ReadTimestamp readTimestamp, MatchFrame matchFrame)
	{
		if (!std::isfinite(newestTime) || !std::isfinite(holdTime)
			|| newestTime <= holdTime || sampleRate == 0 || attackSample == 0
			|| currentSample < attackSample || currentSample - attackSample > sampleRate * 3ULL / 10)
		{
			return false;
		}

		// The clocks have different origins. Compare elapsed capture time, bounded
		// by the hold latch so a previous note's frames cannot be reused.
		const double attackTime = newestTime
			- static_cast<double>(currentSample - attackSample) / sampleRate;
		for (int offset = 0; offset < frameCount && offset < 48; ++offset)
		{
			double timestamp = 0.0;
			if (!readTimestamp(offset, timestamp) || !std::isfinite(timestamp)
				|| timestamp > newestTime || timestamp <= holdTime || timestamp < attackTime)
			{
				return false;
			}
			if (matchFrame(offset)) return true;
		}
		return false;
	}
}
