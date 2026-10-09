#pragma once

#include <cmath>
#include <cstdint>

namespace NoteByNote
{
	// The mod's HFC attack stream fires on a fret-hand slam onto still-ringing strings exactly
	// as it does on a real strum (re-fretting a ringing chord onto the next shape). Level,
	// per-tone excitation and the HFC transient cannot tell the two apart; the loudness envelope
	// around the attack can: a fret-hand slam damps the ring in the run-up (-24 -> -39 dB) and
	// then excites weakly (+10 dB), while a real strum from a quiet state rises far more
	// (+27..+34 dB) and a quiet re-strum over an undamped ring shows no drop. Constants are in the
	// raw-RMS domain; the (NBN CHORD ATTACK) envelope log line reports the measured values.
	struct ChordAttackEnvelope
	{
		double peakBeforeDb = 0.0;
		double floorBeforeDb = 0.0;
		double afterDb = 0.0;
	};

	// Loudest 10 ms RMS hop in [-200 ms, -60 ms), quietest in [-60 ms, 0), loudest in [0, +40 ms).
	// Needs 200 ms before and 40 ms after the attack; returns false when the window is not covered
	// (behaviour then falls back to the unchanged accept path).
	inline bool MeasureChordAttackEnvelope(const float* samples, uint32_t attackOffset, uint32_t count,
		uint32_t rate, ChordAttackEnvelope& out)
	{
		if (samples == nullptr || rate < 8000) return false;
		const uint32_t hop = rate / 100;              // 10 ms
		const uint32_t pre = (rate / 5);              // 200 ms
		const uint32_t guard = (rate * 3) / 50;       // 60 ms
		const uint32_t post = (rate / 25);            // 40 ms
		if (hop < 2 || attackOffset < pre || attackOffset + post > count) return false;

		auto hopRmsDb = [&](uint32_t startIndex)
		{
			double sum = 0.0;
			for (uint32_t i = 0; i < hop; ++i)
			{
				const double s = samples[startIndex + i];
				sum += s * s;
			}
			const double rms = std::sqrt(sum / hop);
			return 20.0 * std::log10(rms > 1e-9 ? rms : 1e-9);
		};

		double peakBefore = -300.0, floorBefore = 300.0, after = -300.0;
		for (uint32_t p = attackOffset - pre; p + hop <= attackOffset - guard; p += hop)
		{
			const double db = hopRmsDb(p);
			if (db > peakBefore) peakBefore = db;
		}
		for (uint32_t p = attackOffset - guard; p + hop <= attackOffset; p += hop)
		{
			const double db = hopRmsDb(p);
			if (db < floorBefore) floorBefore = db;
		}
		for (uint32_t p = attackOffset; p + hop <= attackOffset + post; p += hop)
		{
			const double db = hopRmsDb(p);
			if (db > after) after = db;
		}
		out.peakBeforeDb = peakBefore;
		out.floorBeforeDb = floorBefore;
		out.afterDb = after;
		return true;
	}

	// Natural ring decay over the 200 ms run-up (a re-strum over a decaying chord) reads about
	// 9-10 dB; a fret-hand slam damps about 15 dB. 13 separates a hard mute from decay.
	constexpr double CHORD_HAMMER_DAMP_DB = 13.0;     // ring dropped this much in the run-up
	constexpr double CHORD_HAMMER_MAX_RISE_DB = 15.0; // and re-excited less than this

	inline bool LooksLikeFretHandHammer(const ChordAttackEnvelope& e)
	{
		return e.peakBeforeDb - e.floorBeforeDb >= CHORD_HAMMER_DAMP_DB
			&& e.afterDb - e.floorBeforeDb < CHORD_HAMMER_MAX_RISE_DB;
	}

	// A strum lifts the level above the run-up; a slide from a wrong shape onto the chord does
	// not (the strings keep ringing, only the pitch moves), yet the HFC stream fires on it and the
	// chord then matches (slides read -4.9..0.0 dB after-peakBefore; genuine strums +6.1..+90 dB).
	// Set false to disable.
	// A palm mute then a strum rises little over the pre-mute ring but a lot over the muted floor,
	// so a strum needs EITHER rise; a slide has neither (slides: <= +2.6 dB over the floor).
	constexpr bool CHORD_STRUM_RISE_REQUIRED = true;
	constexpr double CHORD_STRUM_MIN_RISE_DB = 3.0;           // over the run-up peak
	constexpr double CHORD_STRUM_MIN_FLOOR_RISE_DB = 6.0;     // over the quietest 10 ms before the attack

	inline bool LacksStrumRise(const ChordAttackEnvelope& e)
	{
		return CHORD_STRUM_RISE_REQUIRED
			&& e.afterDb - e.peakBeforeDb < CHORD_STRUM_MIN_RISE_DB
			&& e.afterDb - e.floorBeforeDb < CHORD_STRUM_MIN_FLOOR_RISE_DB;
	}
}
