#pragma once

namespace NoteByNote
{
	enum class ChordPitchConfirmation
	{
		None,
		Unison,
		Natural,
		NativeMatcher,
		CloseDyad,
		MlStrings,
		Tier0,
		PowerChord
	};

	struct ChordPitchDecisionInput
	{
		int toneCount = 0;
		bool isFretHandMuted = false;
		bool didBuildTarget = false;
		bool hasFreshAttack = false;
		bool isCorroborated = false;
		bool isUnison = false;
		bool rawUnisonMatches = false;
		bool naturalMatches = false;
		bool nativeMatcherMatches = false;
		bool closeDyadMatches = false;
		bool mlStringsMatch = false;
		bool isTier0Enabled = false;
		bool tier0Matches = false;
		// Power chord: the fifth confirmed in the raw audio with a fresh attack
		// (PowerChordConfirmation.hpp), and the game's vote or chord matcher agreed on this strum.
		bool powerChordRawMatches = false;
		bool nativeAgreedOnStrum = false;
	};

	inline ChordPitchConfirmation EvaluateChordPitchDecision(
		const ChordPitchDecisionInput& input)
	{
		if (input.isFretHandMuted || !input.didBuildTarget || !input.hasFreshAttack)
		{
			return ChordPitchConfirmation::None;
		}

		if (input.isUnison)
		{
			return input.rawUnisonMatches
				? ChordPitchConfirmation::Unison
				: ChordPitchConfirmation::None;
		}

		// These paths confirm every authored tone from the fresh attack itself. Do not
		// put the native sounding table in front of stronger exact evidence: it routinely
		// under-reports a real close dyad as one of two tones.
		if (input.closeDyadMatches) return ChordPitchConfirmation::CloseDyad;
		// The game's sounding table almost never lists a power chord's fifth, so the table bar
		// below is unreachable for one. The fifth confirmed in the raw audio (which a root alone
		// cannot produce) plus the game's own vote or matcher on this strum stands in for it.
		if (input.powerChordRawMatches && input.nativeAgreedOnStrum) return ChordPitchConfirmation::PowerChord;
		// ML per-string reads confirm a chord of three or more tones ahead of the sounding
		// table, but a two-note chord needs the table behind it: the model can read a ringing
		// sub-octave's second harmonic as the lower tone of a dyad and accept a single note.
		// A dyad may only take the ML path once corroborated.
		if (input.mlStringsMatch && input.toneCount >= 3) return ChordPitchConfirmation::MlStrings;
		if (!input.isCorroborated) return ChordPitchConfirmation::None;

		if (input.naturalMatches) return ChordPitchConfirmation::Natural;
		if (input.nativeMatcherMatches) return ChordPitchConfirmation::NativeMatcher;
		if (input.mlStringsMatch) return ChordPitchConfirmation::MlStrings;
		if (input.isTier0Enabled && input.tier0Matches) return ChordPitchConfirmation::Tier0;
		return ChordPitchConfirmation::None;
	}

	inline const char* GetChordPitchConfirmationName(ChordPitchConfirmation confirmation)
	{
		switch (confirmation)
		{
			case ChordPitchConfirmation::Unison: return "unison";
			case ChordPitchConfirmation::Natural: return "natural";
			case ChordPitchConfirmation::NativeMatcher: return "native-matcher";
			case ChordPitchConfirmation::CloseDyad: return "close-dyad";
			case ChordPitchConfirmation::MlStrings: return "ml-strings";
			case ChordPitchConfirmation::Tier0: return "tier0";
			case ChordPitchConfirmation::PowerChord: return "power-chord";
			default: return "none";
		}
	}

	inline int GetRequiredChordSoundingToneCount(int toneCount)
	{
		return toneCount - (toneCount >= 5 ? 1 : 0);
	}

	// Late native-matcher accept. The unvoted path's 100 ms limit guards against slides, but the
	// game's chord matcher recognises a clean strum at 90-200 ms. A plain 200 ms window lets a
	// wrong strum plus a slide pass, so the late window also requires the tones in the raw audio
	// at the strum (StrumChordPresence.hpp), which a slide does not have.
	constexpr bool LATE_NATIVE_MATCH_WITH_PRESENCE_ENABLED = true;
	constexpr double UNVOTED_CHORD_MAX_AGE_MS = 100.0;
	constexpr double LATE_NATIVE_MATCH_MAX_AGE_MS = 200.0;
	// Set false to let a dyad pass on the sounding table / vote alone.
	constexpr bool DYAD_REQUIRES_STRUM_PRESENCE = true;

	inline bool IsChordSoundingCorroborated(
		int toneCount,
		int soundingPeak,
		bool naturalMatches,
		double attackAgeMilliseconds,
		bool strumPresenceConfirmed,
		bool nativeMatcherMatches)
	{
		if (toneCount < 1 || soundingPeak < 0) return false;
		// A two-note chord needs both tones in the raw audio at the strum, voted or not. The game's
		// sounding table keeps a tone ringing from the previous chord, so picking one string would
		// pass the dyad. Costs up to ~35 ms on a fast dyad accept (the check needs 85 ms of audio
		// after the strum). Larger chords keep their paths unchanged.
		if (toneCount == 2 && DYAD_REQUIRES_STRUM_PRESENCE && !strumPresenceConfirmed) return false;
		const bool chordSounded = soundingPeak >= GetRequiredChordSoundingToneCount(toneCount);
		const int halfSounding = (toneCount + 1) / 2;
		// A native vote may compensate for the sounding table under-reporting a
		// larger chord, but it must never reduce a two-note chord to one note.
		// The vote plus half the tones must also be backed by the chord's tones in the raw
		// audio at the strum (StrumChordPresence.hpp): a ringing single note can get the game's
		// vote with 2/4 sounding. This only tightens; it never adds an accept.
		const bool votedAndHalf = toneCount >= 3
			&& naturalMatches && soundingPeak >= halfSounding && strumPresenceConfirmed;
		const bool lateNativeMatch = LATE_NATIVE_MATCH_WITH_PRESENCE_ENABLED
			&& nativeMatcherMatches && strumPresenceConfirmed
			&& attackAgeMilliseconds <= LATE_NATIVE_MATCH_MAX_AGE_MS;
		return (chordSounded && (naturalMatches || attackAgeMilliseconds <= UNVOTED_CHORD_MAX_AGE_MS
				|| lateNativeMatch))
			|| votedAndHalf;
	}
}
