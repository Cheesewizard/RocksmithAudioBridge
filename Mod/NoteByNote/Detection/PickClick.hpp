#pragma once

#include <cmath>
#include <cstdint>

// Pick click. A picked note starts with a short broadband transient: the pick hitting the
// string. A slide into the note does not; the string keeps ringing and only its pitch moves.
// The pick buffer's pitch test cannot tell them apart (after a refused wrong-fret pick, the
// onset detector can fire on the slide and the target pitch passes as a fresh pick). This
// measures the level just before and just after an attack, both plain and first-differenced
// (the difference weights high frequencies, where the click lives).
namespace NoteByNote
{
	constexpr double PICK_CLICK_PRE_START_SECONDS = 0.025;   // pre window [attack-25 ms, attack-5 ms)
	constexpr double PICK_CLICK_PRE_END_SECONDS = 0.005;
	constexpr double PICK_CLICK_POST_SECONDS = 0.020;        // post window [attack, attack+20 ms)

	struct PickClickReading
	{
		float preDb = -160.0f;
		float postDb = -160.0f;
		float preHfDb = -160.0f;
		float postHfDb = -160.0f;
		float riseDb() const { return postDb - preDb; }
		float hfRiseDb() const { return postHfDb - preHfDb; }
	};

	// Slide-ins read hfRise -5..+5 dB and rise -7..+2 dB; real picks read 12-40 dB. A pick needs
	// 6 dB on either measure. Set false to go back to pitch-only confirmation.
	constexpr bool PICK_CLICK_REQUIRED = true;
	constexpr float PICK_CLICK_MIN_RISE_DB = 6.0f;

	inline bool HasPickClick(const PickClickReading& reading)
	{
		return reading.hfRiseDb() >= PICK_CLICK_MIN_RISE_DB || reading.riseDb() >= PICK_CLICK_MIN_RISE_DB;
	}

	inline uint32_t PickClickPreSamples(uint32_t sampleRate)
	{
		return static_cast<uint32_t>(sampleRate * PICK_CLICK_PRE_START_SECONDS) + 1;
	}

	inline uint32_t PickClickPostSamples(uint32_t sampleRate)
	{
		return static_cast<uint32_t>(sampleRate * PICK_CLICK_POST_SECONDS);
	}

	// samples[attackOffset] is the attack sample. Needs PickClickPreSamples before it and
	// PickClickPostSamples after it.
	inline bool MeasurePickClick(const float* samples, uint32_t sampleCount, uint32_t attackOffset,
		uint32_t sampleRate, PickClickReading& out)
	{
		out = {};
		if (!samples || sampleRate < 8000 || sampleRate > 96000) return false;
		const uint32_t preStart = static_cast<uint32_t>(sampleRate * PICK_CLICK_PRE_START_SECONDS);
		const uint32_t preEnd = static_cast<uint32_t>(sampleRate * PICK_CLICK_PRE_END_SECONDS);
		const uint32_t post = PickClickPostSamples(sampleRate);
		if (attackOffset < preStart + 1 || attackOffset + post > sampleCount || post < 16) return false;
		auto measure = [&](uint32_t begin, uint32_t end, float& levelDb, float& hfDb)
		{
			double energy = 0.0, hfEnergy = 0.0;
			for (uint32_t i = begin; i < end; ++i)
			{
				const double value = samples[i];
				const double difference = value - samples[i - 1];
				if (!std::isfinite(value)) return false;
				energy += value * value;
				hfEnergy += difference * difference;
			}
			const double count = static_cast<double>(end - begin);
			levelDb = static_cast<float>(10.0 * std::log10(energy / count + 1e-16));
			hfDb = static_cast<float>(10.0 * std::log10(hfEnergy / count + 1e-16));
			return true;
		};
		return measure(attackOffset - preStart, attackOffset - preEnd, out.preDb, out.preHfDb)
			&& measure(attackOffset, attackOffset + post, out.postDb, out.postHfDb);
	}
}
