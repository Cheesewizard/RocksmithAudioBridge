#pragma once

// Note by Note presentation gate.
//
// Rocksmith decides whether a note is drawn in exactly one place: the presentability
// predicate at 0x7A5CE0, whose only caller is 0x7A5D50 at 0x7A5D71. Its decompiled
// body reduces to "currentTime < noteEndTime". When it returns zero the engine calls
// its own retirement path 0x7A61F0 and never admits the note to the string/fret
// presentation grid, so nothing downstream constructs or draws it.
//
// While Note by Note owns a hold the controller freezes the transport, so currentTime
// stops advancing and no note can fail that test. The whole remaining chart stays
// presentable and is redrawn every frame, which keeps the future highway visible during
// a hold and also costs frame rate (the scoring update is frame-coupled, so its tick
// drops with it). A lower tick rate does not starve the onset detector: it reports the
// pitch of the analysis ring's current frame on every call, and nothing about a pluck
// expires between calls.
//
// The predicate also owns note retirement, so it must remain a pass-through for Note by
// Note. Fretboard-only suppression is applied after the native grid write completes,
// preserving future records for the selector while clearing non-gesture cells visually.
namespace NoteByNoteHighwayRenderer
{
	// Installs the non-destructive presentation hooks. Safe to call once at startup.
	bool InstallPresentationGate();

	// Refreshes the cached target once per native preparation pass.
	void RefreshSelectedTarget(void* owner);

	// Drops the cached target so the highway immediately returns to stock.
	void ClearSelectedTarget();

	// The selected target expressed the way the D3D instance streams express it, so the
	// instance filter can decide without reaching back into native note memory.
	//
	// Engine-side per-note gating does not work: 0x7A4AA0 and 0x7A4B50 populate per-note
	// visual state rather than drawing, so suppressing them cannot remove geometry that is
	// already built and leaves notes grey when they are later revealed. Filtering instances
	// out of the draw is non-destructive and leaves every note's state fully maintained.
	//
	// Returns false when no single-note target is active, in which case the highway must be
	// left exactly as Rocksmith submitted it.
	bool TryGetSelectedPresentation(int& stringIndex, int& fret);
	// Same predicate without the coordinates: the draw hook's cheap gate for whether the
	// instance filter could act on this draw at all, so it never issues a device query
	// while Note by Note holds nothing.
	bool HasSelectedPresentation();

	// The selected target regardless of whether suppression is enabled. With suppression
	// off TryGetSelectedPresentation reports nothing, but diagnostics and target marking
	// still need to know which note is selected.
	bool TryGetSelectedTargetForDiagnostics(int& stringIndex, int& fret);

	// Chord-hold shape for the stale-marker quad filter: the SNG chord template's
	// per-string frets (playable fret or -1), stashed by the overlay's event handler
	// which already reads the template for the target text. Matched by chordId against
	// the live state before use; a mismatch simply leaves the filter dormant.
	// fingers (1..4 per fretted string, or 0/absent) rides along for the host-drawn
	// finger numerals; nullptr means no fingering.
	void SetChordTargetShape(int32_t chordId, const int* frets, const int* fingers = nullptr);

	// The template finger (1..4) for one fretted chord member, valid only while a
	// chord hold drives the keep set. The game's numeral glyphs are screen-space quads
	// that never repaint on frozen retargets, so the host draws its own numerals from this.
	bool TryGetChordFingerForCoordinate(int stringIndex, int fret, int& finger);

	// The complete coordinate set the stale-marker quad filter must keep: the single
	// target plus its legato group during note holds, or the fretted chord members
	// during chord holds. Returns false (filter dormant) when neither is active.
	bool TryGetMarkerKeepCoordinates(int (&strings)[8], int (&frets)[8], int& count);

	// True while a chord hold drives the keep set. The fingering-panel layer paints
	// the WRONG chord's fingering during frozen chord holds, so the overlay carries the
	// authoritative fingering.
	bool IsChordHoldActive();

	// How the neck-diagram grid gate at 0x7AA140 treats a cell outside the current gesture.
	//
	// Runtime-settable because the detour is installed at startup and cannot be hot-reloaded.
	// Which mode is correct depends on whether the grid is cleared between frames, which the
	// (NBN GRID) pre-write line reports.
	enum class GridGateMode
	{
		// No suppression. The diagram behaves exactly as stock.
		Off = 0,
		// Run the original in full, then put the two cells back as they were. Correct if the
		// grid is cleared between frames, because the restored state is then the cleared one.
		Restore = 1,
		// Run the original in full, then overwrite the two cells with a configured sentinel.
		// Needed if nothing clears the grid, because restoring would preserve a stale note.
		Sentinel = 2,
		// Skip the original entirely. Known bad: 0x7AA140 also performs an unconditional
		// container operation on ESI+0xD04 via 0x7AA260, and skipping it expires the selected
		// native record before the hold can be established.
		SkipKnownBad = 3
	};

	void SetGridGateMode(GridGateMode mode);
	GridGateMode GetGridGateMode();

	// The neck-placement diagnostics are disabled by default and must remain off during
	// stopped-preview filtering.
	void SetNeckPlacementMode(long mode);

	// Drop every remembered dim-candidate visual pointer. Must be called on each NBN
	// teardown path (disable, probe reload, probe unload): the destructor-detour
	// eviction is probe-requested and can be unhooked mid-teardown, and a stale
	// pointer that survives can hand the synthetic dim to a reused allocation
	// (persistent neck elements dimmed for the rest of the game session).
	void EvictRememberedDimCandidates();
	long GetNeckPlacementMode();
	void SetNeckPlacementSiteMask(long mask);
	long GetNeckPlacementSiteMask();
	// Grants a bounded (NBN FADE) log budget over the fade pipeline, independent of the
	// placement mode. Re-arm by calling again; zero steady-state cost once spent.
	void ArmFadeDryLog();
	const char* DescribeGridGateMode(GridGateMode mode);

	// The words written to the value and key cells in Sentinel mode. Defaults are the
	// zero/lowest-priority pair; the (NBN GRID) line reports what an untouched cell actually
	// holds.
	void SetGridSentinel(uint32_t valueWord, float keyWord);
	void GetGridSentinel(uint32_t& valueWord, float& keyWord);

	// How the scrolling highway treats notes that are not the current gesture.
	//
	// A double marker during a hold is highway geometry, not a neck-diagram fault (the
	// fretboard buffer consumed at 0x7AA1B0 holds only the target's cell). With the transport
	// frozen a neighbouring note scrolls to the strike plane and parks there, reading as a
	// second note to play. Hiding the whole highway removes the read-ahead, so the targeted
	// form keeps the highway and hides only what has arrived near the plane and is not the
	// gesture.
	enum class HighwayMode
	{
		// Highway exactly as Rocksmith submits it.
		Off = 0,
		// Hide every note that is not the target or its published legato group. Removes the
		// read-ahead; useful as a diagnostic.
		All = 1,
		// Hide only non-gesture notes within a time window either side of the target, which
		// are the ones the frozen window parks close enough to the plane to be mistaken for
		// something to play. Notes further out stay visible, so the read-ahead survives.
		//
		// The window has to be symmetric: the offending marker can sit after the target as
		// well as before it.
		NearTargetWindow = 2
	};

	void SetHighwayMode(HighwayMode mode);
	HighwayMode GetHighwayMode();
	const char* DescribeHighwayMode(HighwayMode mode);

	// Upcoming-marker dim: while a hold is frozen, upcoming in-section markers are
	// force-dimmed so notes that cannot be played yet do not look playable. Off keeps
	// upcoming notes in colour at the risk of a double marker; on dims them.
	void SetUpcomingDimEnabled(bool enabled);
	bool GetUpcomingDimEnabled();

	// Half-width of the NearTargetWindow, in seconds of authored time. Runtime-settable
	// because the right value is a feel judgement. The controller's dense-successor boundary
	// is 0.270, so the useful range is around a quarter to half a second.
	void SetHighwayWindowSeconds(float seconds);
	float GetHighwayWindowSeconds();
}
