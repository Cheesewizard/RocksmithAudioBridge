#pragma once

#include <algorithm>
#include <cstddef>

// The flow speed cap, shared by the in-song controller and the Riff Repeater Settings screen so
// both arrive at the same number.
//
// A pick played on the beat needs real time to confirm (low notes ~0.20 s at p87 on guitar,
// ~0.22 s on bass). Flow waits AllowanceForGap(gap) of SONG
// time past a note before freezing, and slowing the song turns that song time into more real
// time, so the fastest speed that still lets an on-the-beat pick confirm is
// min(gap, allowance) / confirmSeconds. The section's "typical tight gap" is the gap 80% of its
// notes are wider than, so one grace note does not drag the whole section down.
namespace FlowSpeedCap
{
	constexpr float PENDING_MIN_SECONDS = 0.16f;
	constexpr float PENDING_MAX_SECONDS = 0.26f;
	constexpr float NEXT_NOTE_MARGIN_SECONDS = 0.073f;   // engine compensation (0.053) + 0.02
	constexpr float CONFIRM_SECONDS_GUITAR = 0.20f;
	constexpr float CONFIRM_SECONDS_BASS = 0.22f;
	constexpr float MIN_PERCENT = 25.0f;

	// How long flow waits past a note for a pick still being confirmed, given the room before the
	// next note (non-positive gap = nothing after it).
	inline float AllowanceForGap(float gap)
	{
		if (!(gap > 0.0f)) return PENDING_MAX_SECONDS;
		return (std::max)(PENDING_MIN_SECONDS, (std::min)(PENDING_MAX_SECONDS, gap - NEXT_NOTE_MARGIN_SECONDS));
	}

	// Real speed percent (25..100) from a section's note times; sorts `times` in place. Notes of
	// one chord share a strum (gaps under 10 ms are ignored). 100 when there is too little to judge.
	inline float CapFromNoteTimes(float* times, size_t count, bool bass, float* typicalGapOut = nullptr)
	{
		if (typicalGapOut != nullptr) *typicalGapOut = 0.0f;
		if (times == nullptr || count < 4) return 100.0f;
		std::sort(times, times + count);
		size_t gapCount = 0;
		for (size_t index = 1; index < count; ++index)
		{
			const float gap = times[index] - times[index - 1];
			if (gap > 0.01f) times[gapCount++] = gap;   // reuse the buffer for the gaps
		}
		if (gapCount < 3) return 100.0f;
		std::sort(times, times + gapCount);
		const float typicalGap = times[gapCount / 5];
		if (typicalGapOut != nullptr) *typicalGapOut = typicalGap;
		const float allowance = (std::min)(typicalGap, AllowanceForGap(typicalGap));
		const float confirmSeconds = bass ? CONFIRM_SECONDS_BASS : CONFIRM_SECONDS_GUITAR;
		const float cap = static_cast<float>(static_cast<int>(allowance / confirmSeconds * 100.0f / 5.0f)) * 5.0f;
		return (std::max)(MIN_PERCENT, (std::min)(100.0f, cap));
	}
}
