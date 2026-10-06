#include "../stdafx.h"
#include "NoteByNoteHighwayRenderer.hpp"

#include "../Mods/NoteByNoteNativeScoring.hpp"
#include "../Research/ResearchBridge.hpp"

#include <cstdint>
#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <cmath>

// Non-destructive presentation focus for Note by Note. Rocksmith's presentability
// predicate is deliberately left untouched because its false path also retires scoring
// records. Visual filtering happens only after native state updates have completed.
namespace
{
	constexpr uintptr_t NATIVE_NOTEWAY_SETUP = 0x7E22F0;
	constexpr uintptr_t NATIVE_DRAW_DESCRIPTORS = 0x7A4B50;
	constexpr uintptr_t NATIVE_DRAW_GEOMETRY = 0x7A52C0;
	constexpr uintptr_t NATIVE_DRAW_SUBMIT = 0x7A6160;
	// The neck diagram grid write. Reached from eight call sites inside 0x7A5D50, two of
	// which are downstream of a path that skips the presentability predicate entirely.
	constexpr uintptr_t NATIVE_FRETBOARD_GRID_WRITE = 0x7AA140;
	// Consumes the finished per-frame fretboard buffer, which arrives in EDI. Plain RET, no
	// stack arguments. This is the only point where the completed state exists: render
	// preparation 0x7E4870 initialises the buffer at 0x7AA0B0, fills it by looping the owner
	// note vector through 0x7A5D50, and then calls this. Reading the buffer anywhere else in
	// the frame returns the previous frame's residue.
	constexpr uintptr_t NATIVE_FRETBOARD_BUFFER_CONSUME = 0x7AA1B0;
	// The sub-element placement setups of the note-visual class (vtable 0x11CF1D8): run-once
	// state machines, one per quad type, that anchor a quad on the neck. The note visual
	// (sub-element +0x8) carries its string at +0xC and fret at +0xD as bytes. All share the
	// __fastcall(subElement) shape. Sites 0x7A8CE0/0x7A8E90 place the highway visuals, so the
	// runtime site mask decides which sites target mode may act on.
	constexpr uintptr_t NECK_PLACEMENT_SITES[] =
		{ 0x7A8B10, 0x7A8CE0, 0x7A8E90, 0x7A90B0, 0x7A9290, 0x7A93C0, 0x7A94F0 };
	constexpr size_t NECK_PLACEMENT_SITE_COUNT =
		sizeof(NECK_PLACEMENT_SITES) / sizeof(NECK_PLACEMENT_SITES[0]);
	// Slot +0x5C, __thiscall(this, byte): the marker sub-element's deleting destructor, hooked
	// to evict the remembered stale-marker visual.
	constexpr uintptr_t NECK_VISIBILITY_SITE = 0x7A9220;
	// FUN_0079D070 is the per-frame marker updater: it tests whether the current time is inside
	// the note's active window via FUN_007E9ED0(visual+0x38) and, on change, rebroadcasts the
	// lit/dim bank to the marker quads. With the transport frozen, upcoming notes' windows
	// contain the frozen time, so their markers light early. The condition is the seam: its
	// detour forces the "outside window" answer for non-whitelisted visuals while a hold is
	// owned. Relocation (above) is diagnostics only, since visual position is the highway
	// position.
	constexpr uintptr_t NECK_MARKER_UPDATER = 0x79D070;
	constexpr uintptr_t NECK_WINDOW_CONDITION = 0x7E9ED0;
	// 16-bit dimension counts the consumer's own nested loop bounds against, read rather than
	// assumed to be six by twenty-six.
	constexpr uintptr_t FRETBOARD_BUFFER_STRING_COUNT = 0xD00;
	constexpr uintptr_t FRETBOARD_BUFFER_FRET_COUNT = 0xD02;
	constexpr uintptr_t RECORD_MASK = 0x00;
	constexpr uint32_t NOTE_MASK_HAMMERON = 0x00000200;
	constexpr uint32_t NOTE_MASK_PULLOFF = 0x00000400;
	constexpr uintptr_t NATIVE_NOTE_RECORD = 0x2C;
	// The SNG record's authored time, same layout the scoring controller reads.
	constexpr uintptr_t RECORD_TIME_OFFSET = 0x0C;

	using NativeNotewaySetup = void(*)(int);

	using NativeDrawDescriptors = void(*)(void*, void*, float, float);
	using NativeDrawGeometry = void(*)(void*, void*, float, float);
	using NativeDrawSubmit = void(*)(float);
	// ESI is an implicit argument, so this is only ever tail-jumped to from the naked
	// detour and never called through this type.
	using NativeGridWrite = void(*)();
	// EDI is an implicit argument, so this is only ever tail-jumped to.
	using NativeBufferConsume = void(*)();
	using NativeNeckPlacement = uint32_t(__fastcall*)(void* subElement, void* unusedEdx);

	using NativeNeckVisibility = uint32_t(__fastcall*)(void* self, void* unusedEdx, uint8_t flag);
	// From the 0x79D070 disassembly: PUSH owner, PUSH visual, CALL, RET 0x8, i.e.
	// __stdcall(visual, owner) with callee cleanup. Hooking it is unsafe (it parks the visual in
	// EDX across the condition call); calling it is safe, it loads EDX itself from the stack arg.
	using NativeMarkerUpdater = void(__stdcall*)(void* visual, void* owner);
	using NativeWindowCondition = uint16_t(__fastcall*)(void* windowStruct, void* unusedEdx, float time);

	NativeNotewaySetup originalNotewaySetup = nullptr;
	NativeNeckPlacement originalNeckPlacements[NECK_PLACEMENT_SITE_COUNT] = {};
	NativeNeckVisibility originalNeckVisibility = nullptr;
	NativeMarkerUpdater originalMarkerUpdater = nullptr;
	NativeWindowCondition originalWindowCondition = nullptr;
	NativeDrawDescriptors originalDrawDescriptors = nullptr;
	NativeDrawGeometry originalDrawGeometry = nullptr;
	NativeDrawSubmit originalDrawSubmit = nullptr;
	NativeGridWrite originalGridWrite = nullptr;
	NativeBufferConsume originalBufferConsume = nullptr;
	bool isGateInstalled = false;

	// Refreshed once per native preparation pass, then read once per note. Kept as
	// plain scalars so the per-note path is a load and a compare with no locking,
	// no bridge call, and no allocation.
	// Default for the runtime HighwayMode. Suppression is off so the upcoming highway stays
	// readable, as in Rocksmith+'s note-by-note mode; suppressing it also starves per-note state
	// (wrong bend lines and trails). True re-engages the descriptor gate at 0x7A4B50 and the D3D
	// instance filter. The frozen transport keeps the whole remaining chart resident; if frame
	// rate suffers, the lever is the noteway pool gate at 0x7E22F0 (residency), not the draw
	// gates (appearance).
	constexpr bool IS_PRESENTATION_SUPPRESSION_ENABLED = false;
	volatile long highwayMode =
		static_cast<long>(NoteByNoteHighwayRenderer::HighwayMode::Off);
	// The target's authored time, for the strike-plane rule. A note earlier than the target
	// should already have passed; with the transport frozen it parks at the plane instead.
	volatile float selectedRecordTime = 0.0f;
	volatile bool hasSelectedRecordTime = false;
	// Covers dense neighbours either side of the target (the controller's dense-successor
	// boundary is 0.270 s) without reaching notes far enough away to read.
	volatile float highwayWindowSeconds = 0.5f;
	constexpr uintptr_t RECORD_TIME = 0x0C;

	// The fretboard is gated separately from the highway.
	//
	// The near pool in 0x7E2340 holds every note inside a time window, and 0x7A5CE0 admits a
	// note to the neck diagram while currentTime is before its end, so normally notes reach the
	// fretboard one at a time. Freezing currentTime stops the window moving without collapsing
	// it, so close neighbours sit on the fretboard together. Gating the grid written by 0x7AA140
	// keeps the neck diagram to the current gesture (the target plus its published legato group)
	// while the highway shows everything.
	constexpr bool IS_FRETBOARD_SUPPRESSION_ENABLED = true;

	volatile bool isTargetActive = false;
	volatile bool isNativeHoldOwned = false;
	volatile uintptr_t selectedRecord = 0;

	// Settle hold for draw/pool suppression. A native Riff Repeater range change or the
	// coordinated PlayerSong restart behind a dense-successor advance rebuilds the note pool over
	// a few frames. Reporting a note not presentable during that churn makes the engine retire
	// (free) a note it already committed to draw, and its draw walk then faults on the freed
	// descriptor. The published epoch increments on every such restart, so draw/pool suppression
	// is disabled for a short window after an epoch change. The presentability predicate is
	// always pass-through.
	constexpr uint32_t PRESENTATION_SETTLE_FRAMES = 8;
	volatile uint32_t presentationSettleFrames = 0;
	volatile uint64_t lastSeenEpoch = 0;
	volatile bool hasLastSeenEpoch = false;
	// The epoch misses the hybrid-release rebuild (StartAt core + coordinated restart), which
	// rebuilds the note pool without incrementing it (a freed chord panel's vtable gets called).
	// So the settle also arms on the scoring target changing and on the native hold releasing:
	// any hold hand-off may retire an object the draw path still references.
	volatile uintptr_t lastSeenSelectedRecord = 0;
	volatile bool hasLastSeenSelectedRecord = false;
	volatile bool lastOwnsNativeHold = false;
	volatile bool hasLastOwnsNativeHold = false;
	// The same target expressed as the instance streams express it, for the D3D filter.
	volatile int selectedStringIndex = -1;
	volatile int selectedFret = -1;
	// The target's legato gesture, computed by the probe where the whole note vector is
	// available in time order. Fixed size and written only from the preparation pass.
	volatile uint32_t visualGroupCount = 0;
	uintptr_t visualGroupRecords[ResearchProtocol::NoteByNoteState::MaxVisualGroup] = {};
	// The same gesture as coordinates, for the grid-write gate, which is handed
	// (string, fret) and never a note pointer.
	int visualGroupStrings[ResearchProtocol::NoteByNoteState::MaxVisualGroup] = {};
	int visualGroupFrets[ResearchProtocol::NoteByNoteState::MaxVisualGroup] = {};
	uintptr_t loggedRecord = 0;
	// Chord-hold shape for the stale-marker quad filter. Chord holds clear the single-note
	// target (suppressing a chord's siblings would hide notes that must be played), so the
	// overlay's event handler stashes the SNG chord template's per-string frets here and the
	// filter keeps exactly these coordinates while the live state confirms the same chord is
	// held. Written on the scoring thread, read on the render thread under the same
	// tolerated-race rules as the single-target fields above.
	volatile bool isChordTargetActive = false;
	// isChordTargetActive while the song is actually frozen on that chord (the native hold is
	// owned). The chord target is active as soon as the chord is selected, while it still
	// scrolls in; the host finger numerals must only draw during the freeze.
	volatile bool isChordHoldFrozen = false;
	volatile int32_t chordShapeChordId = -1;
	int chordShapeFrets[6] = { -1, -1, -1, -1, -1, -1 };
	// Template fingers parallel to chordShapeFrets (1..4, or 0 when the template
	// gives none), for the host-drawn numerals. Same tolerated-race rules.
	int chordShapeFingers[6] = { 0, 0, 0, 0, 0, 0 };
	// The chord record's anchor span, where an open member's bar is drawn (-1 = unknown).
	int chordShapeAnchorFret = -1;
	int chordShapeAnchorWidth = -1;
	// Diagnostics. Distinguishes three failure modes that look identical on screen:
	// the detour never firing, the decision returning false, or suppression not
	// actually preventing the draw.
	volatile uint32_t descriptorCalls = 0;
	volatile uint32_t descriptorSuppressed = 0;
	volatile uint32_t geometryCalls = 0;
	volatile uint32_t geometrySuppressed = 0;
	volatile uint32_t submitCalls = 0;
	volatile uint32_t submitSuppressed = 0;
	template <typename T>
	bool TryRead(uintptr_t address, T& value)
	{
		if (address == 0) return false;

		__try
		{
			value = *reinterpret_cast<const T*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Returns true when this note must be hidden. Integer work only: the caller has a
	// live x87 stack (the float argument was just stored through FSTP), so this must
	// not disturb the FPU.
	enum class CountedGate { Descriptors, Geometry, Submit };

	bool __cdecl ShouldSuppressNoteCounted(void* note, CountedGate gate);
	bool __cdecl ShouldSuppressNote(void* note);
	bool __cdecl IsOutsideCurrentGesture(void* note);

	bool __cdecl ShouldSuppressDescriptors(void* note)
	{
		return ShouldSuppressNoteCounted(note, CountedGate::Descriptors);
	}

	bool __cdecl ShouldSuppressGeometry(void* note)
	{
		return ShouldSuppressNoteCounted(note, CountedGate::Geometry);
	}

	bool __cdecl ShouldSuppressSubmit(void* note)
	{
		return ShouldSuppressNoteCounted(note, CountedGate::Submit);
	}

	// Whether this note is not part of the current gesture. Shared by both gates so the
	// fretboard and the highway can never disagree about what the target is.
	bool __cdecl IsOutsideCurrentGesture(void* note)
	{
		// While a bootstrap/epoch restart settles, treat every note as part of the current
		// gesture so nothing is reported not-presentable and Rocksmith retires nothing while
		// it is rebuilding its note pool. See PRESENTATION_SETTLE_FRAMES.
		if (presentationSettleFrames > 0) return false;
		if (!isTargetActive) return false;

		const uintptr_t target = selectedRecord;
		if (target == 0) return false;

		uintptr_t record = 0;
		if (!TryRead(reinterpret_cast<uintptr_t>(note) + NATIVE_NOTE_RECORD, record))
		{
			// An unreadable note is left to Rocksmith rather than guessed at.
			return false;
		}

		if (record == target) return false;

		// Keep the target's legato continuation visible. Only the contiguous same-string run
		// published by the probe qualifies; sparing every legato note in the chart would leave
		// unrelated notes beside the target.
		const uint32_t groupCount = visualGroupCount;
		for (uint32_t i = 0; i < groupCount
			&& i < ResearchProtocol::NoteByNoteState::MaxVisualGroup; ++i)
		{
			if (visualGroupRecords[i] == record) return false;
		}

		// A chord's member notes are separate records sharing the chord's onset, so the same
		// authored time as the target means the same gesture.
		if (hasSelectedRecordTime)
		{
			float recordTime = 0.0f;
			if (TryRead(record + RECORD_TIME_OFFSET, recordTime)
				&& std::fabs(recordTime - selectedRecordTime) <= 0.002f)
			{
				return false;
			}
		}

		return true;
	}

	// Highway gates: the draw calls and the noteway pool. Off, so the player reads ahead.
	bool __cdecl ShouldSuppressNote(void* note)
	{
		const auto mode = static_cast<NoteByNoteHighwayRenderer::HighwayMode>(highwayMode);
		if (mode == NoteByNoteHighwayRenderer::HighwayMode::Off) return false;
		if (!IsOutsideCurrentGesture(note)) return false;
		if (mode == NoteByNoteHighwayRenderer::HighwayMode::All) return true;

		// NearTargetWindow. Hide non-gesture notes close in authored time to the target. The hold
		// stops the near pool's time window moving rather than collapsing it, so neighbours sit at
		// the plane indefinitely and read as notes to play. Symmetric, because an offending
		// neighbour can follow the target as well as precede it.
		if (!hasSelectedRecordTime) return false;

		uintptr_t record = 0;
		float recordTime = 0.0f;
		if (!TryRead(reinterpret_cast<uintptr_t>(note) + NATIVE_NOTE_RECORD, record)
			|| record == 0
			|| !TryRead(record + RECORD_TIME, recordTime))
		{
			// An unreadable note is left to Rocksmith rather than guessed at.
			return false;
		}
		const float window = highwayWindowSeconds;
		return std::fabs(recordTime - selectedRecordTime) <= window;
	}

	bool __cdecl ShouldSuppressNoteCounted(void* note, CountedGate gate)
	{
		const bool suppress = ShouldSuppressNote(note);
		switch (gate)
		{
		case CountedGate::Descriptors:
			++descriptorCalls;
			if (suppress) ++descriptorSuppressed;
			break;
		case CountedGate::Geometry:
			++geometryCalls;
			if (suppress) ++geometrySuppressed;
			break;
		case CountedGate::Submit:
			++submitCalls;
			if (suppress) ++submitSuppressed;
			break;
		}
		return suppress;
	}

	// The neck-diagram grid write, gated by coordinate.
	//
	// This is the visual gate for the fretboard. The presentability predicate above remains a
	// lifecycle pass-through: a false result would retire the record at 0x7A5D7A. The grid
	// write runs to completion and is corrected afterward, so future notes remain selectable.
	//
	// But 0x7A5D50 opens with a bypass:
	//
	//   007a5d5a  MOV BL,byte ptr [EDI + 0x51]
	//   007a5d5f  JNZ 0x007a5d6a          ; nonzero -> normal path, predicate runs
	//   007a5d61  CMP byte ptr [EDI + 0x52],BL
	//   007a5d64  JZ  0x007a608a          ; both zero -> skip the predicate entirely
	//
	// 0x7A608A lands before the grid writes at 0x7A60D8 and 0x7A613D, so a note whose +0x51
	// and +0x52 are both zero reaches the neck diagram without the predicate being consulted.
	// The grid hook at 0x7AA140 sits below both paths. The meaning of +0x51/+0x52 is unknown.
	//
	// Gating by coordinate rather than by record is forced by the ABI: the write receives
	// (string, fret, value, key) and no note pointer. Coordinates are also what the diagram
	// shows, so the two agree by construction.
	// Runtime-settable because the detour is installed at startup and cannot be hot-reloaded.
	//
	// Sentinel is the default: the native write still performs all of its lifecycle work, then
	// non-gesture cells are cleared to the empty value. This keeps the current gesture visible
	// without retiring future records.
	volatile long gridGateMode =
		static_cast<long>(NoteByNoteHighwayRenderer::GridGateMode::Sentinel);
	volatile uint32_t gridSentinelValue = 0;
	volatile float gridSentinelKey = 0.0f;

	// Snapshot of the cell a suppressed write is about to touch, taken before the original
	// runs and consumed immediately after it. The grid write happens on one thread inside a
	// render pass, and the detour always pairs the two, so a single slot is enough.
	// The grid object, captured from the detour because it arrives in ESI and exists nowhere
	// else. Needed to read the table, which is the only way to see a marker that is stale
	// rather than being written.
	volatile uintptr_t lastSeenGrid = 0;
	// Frames still to dump. Set on a target change so a few consecutive frames are reported,
	// which distinguishes a cell that is there every frame from one that flickers.
	volatile long fretboardDumpFramesRemaining = 0;
	// Off by default: the grid walk plus its console line costs frame rate around retargets.
	// Set to 1 to enable the dump.
	volatile long isFretboardDumpEnabled = 0;
	uintptr_t pendingGrid = 0;
	int pendingString = -1;
	int pendingFret = -1;
	uint32_t pendingValue = 0;
	float pendingKey = 0.0f;

	// The two parallel tables the write indexes, both based at the grid object:
	//   value at (string * 0x1A + fret) * 4
	//   key   at ((string + 0x1A) * 0x1A + fret) * 4
	// read directly off the index arithmetic at 0x7AA16E and 0x7AA184.
	uintptr_t GridValueCell(uintptr_t grid, int stringIndex, int fret)
	{
		return grid + (static_cast<uintptr_t>(stringIndex) * 0x1A + fret) * 4;
	}

	uintptr_t GridKeyCell(uintptr_t grid, int stringIndex, int fret)
	{
		return grid + ((static_cast<uintptr_t>(stringIndex) + 0x1A) * 0x1A + fret) * 4;
	}

	volatile uint32_t gridCalls = 0;
	volatile uint32_t gridSuppressed = 0;

	volatile uint32_t gridProbesLogged = 0;

	// grid is ESI, the object the write indexes into. The cell is read before the original runs
	// so Restore mode can put it back.
	bool __cdecl ShouldSuppressGridCell(uintptr_t grid, int stringIndex, int fret)
	{
		++gridCalls;
		if (grid != 0) lastSeenGrid = grid;
		const auto mode = static_cast<NoteByNoteHighwayRenderer::GridGateMode>(gridGateMode);
		if (mode == NoteByNoteHighwayRenderer::GridGateMode::Off) return false;
		if (!IS_FRETBOARD_SUPPRESSION_ENABLED) return false;
		if (!isTargetActive) return false;

		const int targetString = selectedStringIndex;
		const int targetFret = selectedFret;
		// Without a published target there is nothing to compare against, so the diagram is
		// left exactly as Rocksmith would draw it.
		if (targetString < 0 || targetFret < 0) return false;
		if (stringIndex == targetString && fret == targetFret) return false;

		// The target's hammer-on / pull-off run is one gesture with it and must stay visible;
		// a hammer-on follows its attack with no gap, so hiding it would be wrong in the
		// opposite direction.
		const uint32_t groupCount = visualGroupCount;
		for (uint32_t i = 0; i < groupCount
			&& i < ResearchProtocol::NoteByNoteState::MaxVisualGroup; ++i)
		{
			if (visualGroupStrings[i] == stringIndex && visualGroupFrets[i] == fret) return false;
		}

		++gridSuppressed;

		// Snapshot before the original runs, so the fix-up afterwards can put it back.
		pendingGrid = grid;
		pendingString = stringIndex;
		pendingFret = fret;
		pendingValue = 0;
		pendingKey = 0.0f;
		bool readValue = false;
		bool readKey = false;
		if (grid != 0)
		{
			readValue = TryRead(GridValueCell(grid, stringIndex, fret), pendingValue);
			readKey = TryRead(GridKeyCell(grid, stringIndex, fret), pendingKey);
			if (!readValue || !readKey)
			{
				// Nothing safe to restore, so leave the cell entirely alone.
				pendingGrid = 0;
				return false;
			}
		}

		// A few per target only: this runs inside a render path.
		if (gridProbesLogged < 4)
		{
			++gridProbesLogged;
			LOG_INFO("(NBN GRID) pre-write cell " << stringIndex << ':' << fret
				<< " value=0x" << std::hex << pendingValue << std::dec
				<< " key=" << pendingKey
				<< " readable=" << readValue << readKey
				<< " mode=" << NoteByNoteHighwayRenderer::DescribeGridGateMode(mode)
				<< " target=" << selectedStringIndex << ':' << selectedFret
				<< ". A constant key every frame means the grid is cleared, so Restore clears"
				<< " the marker; a previous note means it is not and Sentinel is needed."
				<< std::endl);
		}
		return true;
	}

	// Undoes a suppressed write, after the original has run in full.
	//
	// The original must run: 0x7AA140 performs an unconditional container operation on
	// ESI+0xD04 via 0x7AA260 before it looks at the grid, and skipping it faults the controller.
	// Every side effect happens and only the two words are put right afterwards.
	void __cdecl FinishSuppressedGridWrite()
	{
		const uintptr_t grid = pendingGrid;
		pendingGrid = 0;
		if (grid == 0 || pendingString < 0 || pendingFret < 0) return;

		const auto mode = static_cast<NoteByNoteHighwayRenderer::GridGateMode>(gridGateMode);
		uint32_t value = pendingValue;
		float key = pendingKey;
		if (mode == NoteByNoteHighwayRenderer::GridGateMode::Sentinel)
		{
			value = gridSentinelValue;
			key = gridSentinelKey;
		}

		__try
		{
			*reinterpret_cast<uint32_t*>(GridValueCell(grid, pendingString, pendingFret)) = value;
			*reinterpret_cast<float*>(GridKeyCell(grid, pendingString, pendingFret)) = key;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	// ESI carries the grid object implicitly and the four arguments are on the stack, with
	// the callee cleaning them (RET 0x10).
	//
	// The suppress path re-pushes the same four arguments and calls the original, which
	// cleans them itself, then runs the fix-up and performs its own RET 0x10. Four identical
	// `push [esp+0x10]` land the arguments in the right order because each push shifts the
	// frame by one slot. ESI survives: 0x7AA140 saves EBP, ECX and EDI and never writes ESI.
	__declspec(naked) void GridWriteDetour()
	{
		__asm
		{
			pushfd
			push eax
			push ecx
			push edx

			// Four pushes above, so the entry frame [ret][string][fret][value][key] now
			// starts at [esp+0x10]: string at 0x14, fret at 0x18.
			movzx eax, byte ptr [esp + 0x18]   // fret
			push eax
			movzx eax, byte ptr [esp + 0x18]   // string, 0x14 shifted by the push above
			push eax
			push esi                           // the grid object, an implicit argument
			call ShouldSuppressGridCell
			add esp, 0xc
			test al, al
			jnz suppress

			pop edx
			pop ecx
			pop eax
			popfd
			jmp originalGridWrite              // stack untouched; it does its own RET 0x10

		suppress:
			pop edx
			pop ecx
			pop eax
			popfd

			push dword ptr [esp + 0x10]        // key
			push dword ptr [esp + 0x10]        // value
			push dword ptr [esp + 0x10]        // fret
			push dword ptr [esp + 0x10]        // string
			call originalGridWrite             // cleans its own four arguments
			call FinishSuppressedGridWrite
			ret 0x10
		}
	}

	// The finished fretboard buffer, read where the engine consumes it.
	//
	// This read-only diagnostic confirms the completed grid after the visual correction. It
	// deliberately does not participate in note admission or retirement.
	//
	// Read-only, and dimensions read from the buffer rather than assumed.
	void __cdecl ReportFretboardBuffer(uintptr_t buffer)
	{
		if (buffer == 0 || isFretboardDumpEnabled == 0) return;
		if (fretboardDumpFramesRemaining <= 0) return;
		--fretboardDumpFramesRemaining;

		uint16_t stringCount = 0;
		uint16_t fretCount = 0;
		if (!TryRead(buffer + FRETBOARD_BUFFER_STRING_COUNT, stringCount)
			|| !TryRead(buffer + FRETBOARD_BUFFER_FRET_COUNT, fretCount))
		{
			LOG_INFO("(NBN FRETBOARD BUFFER) unreadable buffer=0x" << std::hex << buffer
				<< std::dec << "." << std::endl);
			return;
		}
		// Bound the walk against a corrupt read; the real values are expected to be 6 and 26.
		if (stringCount > 8) stringCount = 8;
		if (fretCount > 32) fretCount = 32;

		std::ostringstream occupied;
		uint32_t occupiedCount = 0;
		for (uint16_t stringIndex = 0; stringIndex < stringCount; ++stringIndex)
		{
			for (uint16_t fret = 0; fret < fretCount; ++fret)
			{
				uint32_t value = 0;
				float key = 0.0f;
				if (!TryRead(GridValueCell(buffer, stringIndex, fret), value)) continue;
				if (!TryRead(GridKeyCell(buffer, stringIndex, fret), key)) continue;
				if (value == 0) continue;
				if (occupiedCount < 16)
				{
					occupied << ' ' << stringIndex << ':' << fret
						<< "=0x" << std::hex << value << std::dec
						<< '/' << std::fixed << std::setprecision(3) << key;
				}
				++occupiedCount;
			}
		}

		LOG_INFO("(NBN FRETBOARD BUFFER) occupied=" << occupiedCount
			<< " target=" << selectedStringIndex << ':' << selectedFret
			<< " group=" << visualGroupCount
			<< " dims=" << stringCount << 'x' << fretCount
			<< " buffer=0x" << std::hex << buffer << std::dec
			<< " |" << occupied.str()
			<< ". Any occupied cell that is not the target or its group is what draws the extra"
			<< " marker; if only the target is here, the diagram is not drawn from this buffer."
			<< std::endl);
	}

	// EDI carries the buffer implicitly. Plain RET, no stack arguments, so the detour reports
	// and then tail-jumps with the stack untouched.
	__declspec(naked) void BufferConsumeDetour()
	{
		__asm
		{
			pushfd
			push eax
			push ecx
			push edx

			push edi                       // the buffer
			call ReportFretboardBuffer
			add esp, 4

			pop edx
			pop ecx
			pop eax
			popfd
			jmp originalBufferConsume
		}
	}

	// Noteway (scrolling highway) gate.
	//
	// The fretboard gate above controls 0x7AA140's six-by-twenty-six string/fret grid,
	// which is the neck diagram, not the highway. The highway is maintained by
	// 0x7E2340, which bands each note into one of three object pools by comparing the
	// note's time against the current time:
	//
	//   state = 2;
	//   if (note+0x38 < time + scale*near || note+0x180 == 0) state = 0;
	//   else if (note+0x38 < time + scale*far)                state = 1;
	//   if (note+0x178 != state) {
	//       if (note+0x178 != -1) teardown(0x7E2210);   // sets note+0x178/+0x17C = -1
	//       setup(0x7E22F0, state);                     // allocates from the pool
	//   }
	//
	// Because Note by Note freezes the transport, "time" never advances, so no note is
	// ever rebanded and nothing is torn down: the whole remaining chart stays resident
	// and is drawn every frame. That is the packed highway, the lingering previous
	// note, and the frame rate, all from one cause.
	//
	// Suppressing setup is therefore the entire fix. The engine's own teardown has
	// already run and left note+0x178 at -1, which is its "no noteway object" state, so
	// skipping setup leaves the note undrawn. On later frames teardown is skipped too
	// (already -1), leaving only two compares per note. Letting a note through restores
	// it normally, so nothing needs restoring by hand.
	//
	// ABI read from the image, not inferred:
	//   007e22f4  8B 5D 08        MOV EBX,[EBP+8]        ; arg: pool state
	//   007e231b  8B 06           MOV EAX,[ESI]          ; note = *ESI
	//   007e231d  89 98 78 01..   MOV [EAX+0x178],EBX
	//   007e2337  C2 04 00        RET 0x4
	// and its call site, which establishes the register inputs:
	//   007e24a4  56              PUSH ESI               ; state
	//   007e24a5  8D 75 FC        LEA ESI,[EBP-0x4]      ; ESI = &notePtr
	//   007e24a8  E8 43 FE FF FF  CALL 0x007e22f0        ; EAX = owner
	//
	// So: owner in EAX, &notePtr in ESI, state at [ESP+4], callee pops 4.
	__declspec(naked) void NotewaySetupDetour()
	{
		__asm
		{
			pushfd
			push ecx
			push edx
			push eax                    // preserve the owner

			mov eax, [esi]              // note = *ESI
			push eax
			call ShouldSuppressNote
			add esp, 4
			test al, al
			jnz suppressNoteway

			pop eax
			pop edx
			pop ecx
			popfd
			jmp originalNotewaySetup    // ESI and the stack are untouched

		suppressNoteway:
			pop eax
			pop edx
			pop ecx
			popfd
			ret 4                       // skip setup; note stays at -1 and is not drawn
		}
	}

	// Highway drawing gate.
	//
	// 0x7A49A0 -> 0x7A4A40 does four things per note:
	//
	//   007a4a57  CALL 0x7A4AA0     ; per-note state
	//   007a4a6e  CALL 0x7A4B50     ; per-note state / geometry
	//   007a4a85  CALL 0x7A52C0     ; push ESI=owner, push EDI=note, 2 floats
	//   007a4a93  CALL 0x7A6160     ; note in EAX, one float
	//
	// Despite their names, 0x7A52C0 and 0x7A6160 are not the highway draw path: suppressing
	// them leaves the notes on screen. 0x7A4B50 iterates the note's presentation descriptors
	// and is the call that hides a note when gated. 0x7A4AA0 is left running; skipping it as
	// well starves per-note state (grey notes, wrong trail lengths, flicker on reappearance).
	//
	// ABIs read from the image:
	//   0x7A4B50: note at [ESP+4] (007a4b5f MOV EDI,[EBP+8]; 007a4b62 MOV EAX,[EDI+0x148]
	//             then 007a4b68 CMP EAX,[EDI+0x14C] -- the presentation descriptors)
	//             ends 007a5284 C2 10 00  RET 0x10, the function's only return
	//   0x7A52C0: note at [ESP+4] (007a52cd MOV EBX,[EBP+8]; 007a52d0 MOV EAX,[EBX+0x2c])
	//             ends 007a56e6 C2 10 00  RET 0x10
	//   0x7A6160: note in EAX (007a6164 MOV EDI,EAX)
	//             ends 007a619f C2 04 00  RET 0x4
	//
	// The 0x7A4A40 call site establishes the argument order for both RET 0x10 callees
	// (007a4a6c PUSH ESI; 007a4a6d PUSH EDI; 007a4a6e CALL 0x7A4B50), so note is the
	// first stack argument and owner the second, with the two floats above them.
	//
	// 0x7A4AA0 is deliberately not gated. Its note arrives in EDI (007a4aa4 MOV ESI,[EDI+0x154];
	// note+0x154 is the NoteVfx controller) and its single return is RET 0xC at 0x7A4B41.
	// Gating it does not remove sustain or bend trails, and it starves per-note visual state so
	// notes revealed later render grey. 0x7A4AA0 and 0x7A4B50 populate per-note visual state
	// rather than drawing, so suppressing them cannot remove geometry already built. Hiding a
	// note belongs at the D3D instance layer, where filtering is non-destructive.

	__declspec(naked) void DrawDescriptorsDetour()
	{
		__asm
		{
			pushfd
			push eax
			push ecx
			push edx

			mov eax, [esp + 20]         // 16 bytes pushed + 4 byte return address
			push eax
			call ShouldSuppressDescriptors
			add esp, 4
			test al, al
			jnz suppressDescriptors

			pop edx
			pop ecx
			pop eax
			popfd
			jmp originalDrawDescriptors

		suppressDescriptors:
			pop edx
			pop ecx
			pop eax
			popfd
			ret 0x10
		}
	}

	__declspec(naked) void DrawGeometryDetour()
	{
		__asm
		{
			pushfd
			push eax
			push ecx
			push edx

			mov eax, [esp + 20]         // 16 bytes pushed + 4 byte return address
			push eax
			call ShouldSuppressGeometry
			add esp, 4
			test al, al
			jnz suppressGeometry

			pop edx
			pop ecx
			pop eax
			popfd
			jmp originalDrawGeometry

		suppressGeometry:
			pop edx
			pop ecx
			pop eax
			popfd
			ret 0x10
		}
	}

	__declspec(naked) void DrawSubmitDetour()
	{
		__asm
		{
			pushfd
			push ecx
			push edx
			push eax                    // note arrives in EAX

			push eax
			call ShouldSuppressSubmit
			add esp, 4
			test al, al
			jnz suppressSubmit

			pop eax
			pop edx
			pop ecx
			popfd
			jmp originalDrawSubmit

		suppressSubmit:
			pop eax
			pop edx
			pop ecx
			popfd
			ret 4
		}
	}
}

namespace
{
	// The placement experiments are disabled by default. They alter shared highway state and
	// must never coexist with the stopped-preview marker filter.
	volatile long neckPlacementMode = 0;
	volatile long neckPlacementLogBudget = 40;
	volatile long neckPlacementRelocated = 0;
	// Which sites target mode may act on, as a bitmask over NECK_PLACEMENT_SITES indices.
	// Defaults to none. Sites 1 and 2 (0x7A8CE0/0x7A8E90) are the highway and must stay out
	// of the mask.
	volatile long neckPlacementSiteMask = 0;

	// Fade-path instrumentation for the fretboard marker's lit state. Armed by the bridge
	// (-FadeDry, budget 150, re-armable), independent of the placement mode. Logs sites 2
	// (color step: which paths run and with what flag/state), 4 (fade batch list sizes) and 5
	// (fade controller: the (ctx+0xC)+0x68/+0x6C/+0x70 fade config against the element's
	// string/fret), tagged (NBN FADE).
	volatile long fadeDryLogBudget = 0;
	struct FadeDrySample
	{
		uintptr_t parent;
		uintptr_t sub;
		int stringIndex;
		int fret;
		int fadeEnabled;
		float fadeFrom;
		float fadeTo;
		float stepFrom;
		float stepTo;
		int markerPath;
		uint32_t colorState;
		int colorFlag;
		float colorFade;
		int listA;
		int listB;
	};

	// SEH-only gather; every field read is speculative (the ctx layouts differ per
	// site and the pointers can be mid-teardown). POD locals only.
	bool TryGatherFadeDry(size_t site, uintptr_t ctx, FadeDrySample* out) noexcept
	{
		__try
		{
			out->parent = *reinterpret_cast<volatile uintptr_t*>(ctx + 0x8);
			out->sub = *reinterpret_cast<volatile uintptr_t*>(ctx + 0xC);
			out->stringIndex = -1;
			out->fret = -1;
			if (out->parent >= 0x10000 && (out->parent & 3) == 0)
			{
				out->stringIndex = *reinterpret_cast<volatile uint8_t*>(out->parent + 0xC);
				out->fret = *reinterpret_cast<volatile uint8_t*>(out->parent + 0xD);
			}
			if (site == 5)
			{
				if (out->sub < 0x10000 || (out->sub & 3) != 0) return false;
				out->fadeEnabled = *reinterpret_cast<volatile uint8_t*>(out->sub + 0x68);
				out->fadeFrom = *reinterpret_cast<volatile float*>(out->sub + 0x6C);
				out->fadeTo = *reinterpret_cast<volatile float*>(out->sub + 0x70);
				out->stepFrom = *reinterpret_cast<volatile float*>(ctx + 0x14);
				out->stepTo = *reinterpret_cast<volatile float*>(ctx + 0x18);
			}
			else if (site == 4)
			{
				const auto beginA = *reinterpret_cast<volatile uintptr_t*>(ctx + 0x14);
				const auto endA = *reinterpret_cast<volatile uintptr_t*>(ctx + 0x18);
				const auto beginB = *reinterpret_cast<volatile uintptr_t*>(ctx + 0x20);
				const auto endB = *reinterpret_cast<volatile uintptr_t*>(ctx + 0x24);
				out->listA = static_cast<int>((endA - beginA) / 24);
				out->listB = static_cast<int>((endB - beginB) / 24);
			}
			else if (site == 2)
			{
				out->markerPath = *reinterpret_cast<volatile uint8_t*>(ctx + 0x28);
				out->colorState = *reinterpret_cast<volatile uint32_t*>(ctx + 0x24);
				out->colorFade = *reinterpret_cast<volatile float*>(ctx + 0x2C);
				out->colorFlag = -1;
				if (out->sub >= 0x10000 && (out->sub & 3) == 0)
				{
					out->colorFlag = *reinterpret_cast<volatile uint8_t*>(out->sub + 0x50);
				}
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Native note progression clears the element state bytes at sub+0x50/0x51 when a note
	// passes; clearing them on a stale element removes its neck marker. The expiry policy lives
	// in the reloadable probe (ObserveNeckPlacementStep in ResearchProtocol.hpp) because
	// host-side writes to those bytes fault the scoring commit and policy changes must not need
	// a game restart. The host only forwards the placement steps.

	void LogFadeDry(size_t site, void* stepCtx, uint32_t result)
	{
		FadeDrySample s = {};
		if (!TryGatherFadeDry(site, reinterpret_cast<uintptr_t>(stepCtx), &s)) return;
		std::ostringstream line;
		line << "(NBN FADE) site=0x" << std::hex << NECK_PLACEMENT_SITES[site]
			<< " ctx=0x" << reinterpret_cast<uintptr_t>(stepCtx)
			<< " parent=0x" << s.parent << " sub=0x" << s.sub << std::dec
			<< " string=" << s.stringIndex << " fret=" << s.fret
			<< " ret=" << result << std::fixed << std::setprecision(3);
		if (site == 5)
		{
			line << " fadeEn=" << s.fadeEnabled
				<< " fadeFrom=" << s.fadeFrom << " fadeTo=" << s.fadeTo
				<< " curFrom=" << s.stepFrom << " curTo=" << s.stepTo;
		}
		else if (site == 4)
		{
			line << " listA=" << s.listA << " listB=" << s.listB;
		}
		else if (site == 2)
		{
			line << " markerPath=" << s.markerPath << " state=" << s.colorState
				<< " flag=" << s.colorFlag << " fade=" << s.colorFade;
		}
		LOG_INFO(line.str() << std::endl);
	}

	uint32_t HandleNeckPlacement(size_t site, void* subElement, uint32_t result)
	{
		const long mode = neckPlacementMode;
		if ((site == 2 || site == 4 || site == 5) && subElement != nullptr
			&& fadeDryLogBudget > 0 && InterlockedDecrement(&fadeDryLogBudget) >= 0)
		{
			LogFadeDry(site, subElement, result);
		}
		if ((site == 2 || site == 4 || site == 5) && subElement != nullptr)
		{
			ResearchBridge::DispatchNeckPlacementStep(
				static_cast<uint32_t>(site),
				subElement);
		}
		if (mode == 0 || result != 2 || subElement == nullptr) return result;

		const auto parent = *reinterpret_cast<uintptr_t*>(
			reinterpret_cast<uintptr_t>(subElement) + 0x8);
		if (parent == 0) return result;
		const int stringIndex = *reinterpret_cast<uint8_t*>(parent + 0xC);
		const int fret = *reinterpret_cast<uint8_t*>(parent + 0xD);

		ResearchProtocol::NoteByNoteState state;
		const bool hasState = ResearchBridge::TryGetNoteByNoteState(state);

		if (mode == 1)
		{
			if (InterlockedDecrement(&neckPlacementLogBudget) >= 0)
			{
				LOG_INFO("(NBN PLACEMENT) site=0x" << std::hex << NECK_PLACEMENT_SITES[site]
					<< " placed string=" << std::dec << stringIndex
					<< " fret=" << fret
					<< " visual=0x" << std::hex << parent
					<< " sub=0x" << reinterpret_cast<uintptr_t>(subElement) << std::dec
					<< std::fixed << std::setprecision(3)
					<< " pos{" << *reinterpret_cast<float*>(parent + 0x14)
					<< "," << *reinterpret_cast<float*>(parent + 0x18)
					<< "," << *reinterpret_cast<float*>(parent + 0x1C) << "}"
					<< " hold=" << (hasState && state.ownsNativeHold != 0)
					<< std::endl);
			}
			return result;
		}

		if (!hasState || state.ownsNativeHold == 0) return result;
		if (((neckPlacementSiteMask >> site) & 1) == 0) return result;

		bool isWhitelisted = stringIndex == state.visualString && fret == state.visualFret;
		for (uint32_t i = 0; !isWhitelisted && i < state.visualGroupCount
			&& i < ResearchProtocol::NoteByNoteState::MaxVisualGroup; ++i)
		{
			isWhitelisted = stringIndex == state.visualGroupStrings[i]
				&& fret == state.visualGroupFrets[i];
		}
		if (isWhitelisted) return result;

		*reinterpret_cast<float*>(parent + 0x18) = -1000000.0f;
		const auto total = InterlockedIncrement(&neckPlacementRelocated);
		if (total == 1 || total % 100 == 0)
		{
			LOG_INFO("(NBN PLACEMENT) relocated off-screen: " << total
				<< " (site=0x" << std::hex << NECK_PLACEMENT_SITES[site] << std::dec
				<< " string=" << stringIndex << " fret=" << fret << ")" << std::endl);
		}
		return result;
	}

	template <size_t Site>
	uint32_t __fastcall NeckPlacementDetour(void* subElement, void* unusedEdx)
	{
		// The color step (site 2) applies its marker color one-shot per context, consuming
		// ctx+0x2C, so a probe policy that overrides the baked color state at ctx+0x24 must
		// see the context before the native call. The post-call forward below stays for the
		// read-only observers that need the return value.
		if (Site == 2 && subElement != nullptr)
		{
			ResearchBridge::DispatchNeckPlacementStep(
				static_cast<uint32_t>(Site) | ResearchProtocol::NECK_PLACEMENT_STEP_PRE,
				subElement);
		}
		const auto result = originalNeckPlacements[Site](subElement, unusedEdx);
		return HandleNeckPlacement(Site, subElement, result);
	}

	constexpr NativeNeckPlacement NECK_PLACEMENT_DETOURS[NECK_PLACEMENT_SITE_COUNT] =
	{
		&NeckPlacementDetour<0>,
		&NeckPlacementDetour<1>,
		&NeckPlacementDetour<2>,
		&NeckPlacementDetour<3>,
		&NeckPlacementDetour<4>,
		&NeckPlacementDetour<5>,
		&NeckPlacementDetour<6>,
	};

	// Mode 3 (window): the window-condition override, identifying notes by time. The updater
	// stays unhooked (hooking its custom convention crashes); the condition alone decides. The
	// controller state names the onset exactly for the target (selectedRecordTime) and, via the
	// group record pointers (onset at record+0x0C), for the legato group. A note whose window
	// contains the frozen time but whose onset is not one of those is read-ahead: report it
	// outside its window and the game dims its marker.
	volatile long windowForcedCount = 0;
	volatile long windowLogBudget = 30;
	// Budget for the updater-visual vtable log, refreshed on every window-mode arm.
	volatile long vtableLogBudget = 48;
	constexpr float ONSET_TOLERANCE_SECONDS = 0.002f;

	// Stale-marker dim. A played note leaves the spawner's per-frame update set, so its marker
	// keeps the last broadcast state while the transport is frozen. The updater FUN_0079D070
	// keeps the note visual in EDX across the condition call, so the naked thunk passes EDX
	// through and the helper remembers the target's visual. When the target advances, one
	// synthetic call of the native updater on that visual makes the game re-ask the condition,
	// now answered "outside", and the game's own broadcast loop dims the marker.
	// Single writer (the game's update thread); volatile is for the destructor eviction.
	volatile uintptr_t staleCandidateVisual = 0;
	uintptr_t staleCandidateOwner = 0;
	float staleCandidateOnset = 0.0f;
	// Identity word for the remembered visual. A freed allocation reused by a rebuilt visual
	// can carry a matching onset at +0x38, so the vtable (every updater visual shares 0x11CF1D8)
	// is captured with the pointer and must still match at dim time. Destructor eviction alone
	// is not enough: it is a probe-requested hook, so a probe reload opens a gap where a dying
	// visual is not evicted and a dim could land on an unrelated element.
	uintptr_t staleCandidateVtable = 0;
	constexpr uintptr_t UPDATER_VISUAL_VTABLE = 0x11CF1D8;
	volatile long syntheticDimActive = 0;
	volatile long staleDimLogBudget = 20;

	// Past-onset dim list. The dense-advance coordinated rebuild destroys the remembered
	// previous-target visual before its deferred dim can fire, which would leave advanced-past
	// markers lit. The game re-creates and re-queries visuals for played notes in the rebuilt
	// pool, so any identity-passing updater visual whose onset lies behind the target is a stale
	// candidate, caught on its next natural query. Each onset dims once; the list resets when the
	// target moves backward (section wrap or re-arm); one slot is in flight at a time, and the
	// deferred-fire path with its onset+vtable revalidation performs the dim.
	constexpr size_t STALE_DIM_ONSET_CAPACITY = 48;
	float staleDimOnsets[STALE_DIM_ONSET_CAPACITY] = {};
	size_t staleDimOnsetCount = 0;
	float staleDimLastTarget = -1.0f;

	bool IsStaleDimmedOnset(float time, float tolerance)
	{
		for (size_t index = 0; index < staleDimOnsetCount; ++index)
		{
			if (std::fabs(staleDimOnsets[index] - time) <= tolerance) return true;
		}
		return false;
	}

	// Upcoming-marker dim. A note whose approach window opens during a frozen hold reveals a
	// lit fretboard marker beside the target. The lever is the stale-marker mechanism above: a
	// synthetic native-updater call whose condition answer comes back outside.
	//
	// The condition is a section check, not an approach window, so every in-section upcoming
	// note answers inside. Each upcoming note's marker is dimmed once per hold: the synthetic
	// call's own query is the forced answer that triggers the broadcast, and dimmed onsets stay
	// forced outside for the rest of the hold.
	constexpr size_t UPCOMING_DIM_CAPACITY = 16;
	float upcomingDimOnsets[UPCOMING_DIM_CAPACITY] = {};
	size_t upcomingDimCount = 0;
	float upcomingDimHoldOnset = -1.0f;
	uintptr_t upcomingCandidateVisual = 0;
	uintptr_t upcomingCandidateOwner = 0;
	float upcomingCandidateOnset = 0.0f;
	uintptr_t upcomingCandidateVtable = 0;  // same identity hardening as the stale candidate
	volatile long upcomingDimLogBudget = 24;
	// Runtime toggle for the dim (updim-on|off). Off by default: upcoming notes keep their
	// colour while frozen, leaving the double markers it would mask visible to be fixed
	// natively.
	volatile bool isUpcomingDimEnabled = false;

	// There is no reveal signal at the color-step seam: site 2 only sees pending contexts, and
	// the element state bytes read (0,1) for both showing and hidden markers. elem+0x04 is the
	// note onset in the condition's timebase. A coloured-highway version needs the draw-event
	// route: a non-target marker draw during a steady hold is the true reveal.

	bool IsUpcomingDimmedOnset(float time, float tolerance)
	{
		for (size_t index = 0; index < upcomingDimCount; ++index)
		{
			if (std::fabs(upcomingDimOnsets[index] - time) <= tolerance) return true;
		}
		return false;
	}

	bool TryReadRecordOnset(uintptr_t record, float* out) noexcept
	{
		if (record < 0x10000 || (record & 3) != 0) return false;
		__try
		{
			*out = *reinterpret_cast<volatile float*>(record + 0x0C);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// The target's legato-group onsets must stay lit: they are played as part of the
	// target's gesture.
	bool IsGroupOnset(const ResearchProtocol::NoteByNoteState& state, float time, float tolerance)
	{
		for (uint32_t index = 0; index < state.visualGroupCount
			&& index < ResearchProtocol::NoteByNoteState::MaxVisualGroup; ++index)
		{
			float onset = 0.0f;
			if (TryReadRecordOnset(state.visualGroupRecords[index], &onset)
				&& std::fabs(onset - time) <= tolerance)
			{
				return true;
			}
		}
		return false;
	}

	// SEH-guarded reads and the guarded native call live in their own functions:
	// C++ objects and __try cannot share a frame (C2712). The visual pointer can be
	// garbage (initializer call sites do not populate EDX) or freed (note despawned
	// between holds), so every dereference of a remembered pointer is guarded.
	bool TryReadVisualOnset(uintptr_t visual, float* out) noexcept
	{
		if (visual < 0x10000 || (visual & 3) != 0) return false;
		__try
		{
			*out = *reinterpret_cast<volatile float*>(visual + 0x38);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// The visual's vtable pointer, for telling fretboard string markers apart from
	// highway note visuals (the type split the window whitelist needs).
	bool TryReadVisualVtable(uintptr_t visual, uintptr_t* out) noexcept
	{
		if (visual < 0x10000 || (visual & 3) != 0) return false;
		__try
		{
			*out = *reinterpret_cast<volatile uintptr_t*>(visual);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool TrySyntheticDim(uintptr_t visual, uintptr_t owner) noexcept
	{
		__try
		{
			reinterpret_cast<NativeMarkerUpdater>(NECK_MARKER_UPDATER)(
				reinterpret_cast<void*>(visual), reinterpret_cast<void*>(owner));
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// The decision logic, ABI-free: called by the naked thunk below with everything
	// already computed. Returns the final answer for AX. edxVisual is the caller's EDX:
	// only meaningful when the caller is the per-frame updater (which keeps the note
	// visual there); initializer call sites leave garbage, so it is never dereferenced
	// without the guarded read.
	// The per-frame marker updater's return site (the caller at 0x79D092 that keeps
	// the note visual in EDX). Queries returning there drive the 3D fretboard string
	// markers; every other return site is one of the four rebuild initializers.
	constexpr uint32_t WINDOW_UPDATER_RETURN = 0x0079D092;

	uint16_t __cdecl WindowConditionHelper(void* windowStruct, float time, uint32_t native,
		void* edxVisual, uint32_t returnAddress)
	{
		const auto result = static_cast<uint16_t>(native);
		const long placementMode = neckPlacementMode;
		if (placementMode != 3 || windowStruct == nullptr)
		{
			return result;
		}

		ResearchProtocol::NoteByNoteState state;
		if (!ResearchBridge::TryGetNoteByNoteState(state) || state.ownsNativeHold == 0)
		{
			// The stale-marker capture survives ownership gaps: a safety release drops ownership
			// between holds, and clearing here would leave the played note's marker lit with nothing to
			// reconcile. The destructor detour evicts it if the visual dies, and the reconcile
			// revalidates the onset (and drops on backward motion) before dimming.
			return result;
		}
		// Forcing spans every phase of an owned hold; handing the transition phases back to native
		// lets upcoming markers flip lit for several frames between holds. A practice-loop restart
		// releases the hold, so the ownsNativeHold gate above restores native answers there. If
		// dense-advance rebuilds glitch under forcing, exclude the rebuild phases specifically.
		const bool isSteadyHold =
			state.gatePhase == ResearchProtocol::GatePhase::Holding;
		const bool isUpdaterCall = returnAddress == WINDOW_UPDATER_RETURN;

		// Stale-marker capture and dim: steady holds only. The synthetic updater call must never
		// run mid-rebuild (the remembered visual may be mid-destruction), and the capture is only
		// meaningful while the target is stable. Skipped while the synthetic updater call below is
		// on the stack (it re-enters this helper via the game's own condition call).
		// Order matters: the reconcile runs before the capture, because the first query of a new
		// hold is usually the new target itself, and capturing first would overwrite the remembered
		// previous-target visual before it could be dimmed.
		if (isSteadyHold && syntheticDimActive == 0)
		{
			// Past-onset list lifecycle: a target moving meaningfully backward is a
			// section wrap or re-arm - the old onsets belong to a retired pool.
			if (state.selectedRecordTime < staleDimLastTarget - 1.0f)
			{
				staleDimOnsetCount = 0;
			}
			staleDimLastTarget = state.selectedRecordTime;

			if (staleCandidateVisual != 0
				&& std::fabs(staleCandidateOnset - state.selectedRecordTime)
					> ONSET_TOLERANCE_SECONDS)
			{
				// The remembered visual belongs to a previous target: deal with it exactly
				// once. Forward motion dims it; backward motion is a section wrap where
				// the remembered pointer is stale by construction - drop it.
				const uintptr_t visual = staleCandidateVisual;
				const uintptr_t owner = staleCandidateOwner;
				const float onset = staleCandidateOnset;
				const uintptr_t capturedVtable = staleCandidateVtable;
				staleCandidateVisual = 0;
				bool dimmed = false;
				float onsetNow = 0.0f;
				uintptr_t vtableNow = 0;
				if (state.selectedRecordTime > onset + ONSET_TOLERANCE_SECONDS
					&& TryReadVisualOnset(visual, &onsetNow)
					&& std::fabs(onsetNow - onset) <= ONSET_TOLERANCE_SECONDS
					// Identity: the pointer must still name the SAME KIND of object it
					// was captured as. A reused allocation with a coincidentally matching
					// onset float must never receive the synthetic updater call.
					&& TryReadVisualVtable(visual, &vtableNow)
					&& vtableNow == capturedVtable)
				{
					InterlockedExchange(&syntheticDimActive, 1);
					dimmed = TrySyntheticDim(visual, owner);
					InterlockedExchange(&syntheticDimActive, 0);
				}
				if (dimmed && staleDimOnsetCount < STALE_DIM_ONSET_CAPACITY)
				{
					// Dimmed once; the past-onset capture below must not re-claim it.
					staleDimOnsets[staleDimOnsetCount++] = onset;
				}
				if (InterlockedDecrement(&staleDimLogBudget) >= 0)
				{
					LOG_INFO("(NBN PLACEMENT) stale marker "
						<< (dimmed ? "dimmed" : "dropped")
						<< " visual=0x" << std::hex << visual << std::dec
						<< std::fixed << std::setprecision(3)
						<< " onset=" << onset
						<< " newTarget=" << state.selectedRecordTime << std::endl);
				}
			}
			if (std::fabs(time - state.selectedRecordTime) <= ONSET_TOLERANCE_SECONDS)
			{
				// A query for the CURRENT target: if EDX carries a visual whose onset
				// agrees, this is the updater and EDX names the target's visual. The
				// vtable must be the updater-visual class at capture time - anything
				// else is an initializer's garbage EDX that happened to be readable.
				// Never displace a pending PAST-onset candidate (it fires next query;
				// the target's own capture repeats every frame and loses nothing).
				const bool slotHoldsPastCandidate = staleCandidateVisual != 0
					&& std::fabs(staleCandidateOnset - state.selectedRecordTime)
						> ONSET_TOLERANCE_SECONDS;
				float edxOnset = 0.0f;
				uintptr_t edxVtable = 0;
				if (!slotHoldsPastCandidate
					&& TryReadVisualOnset(reinterpret_cast<uintptr_t>(edxVisual), &edxOnset)
					&& std::fabs(edxOnset - time) <= ONSET_TOLERANCE_SECONDS
					&& TryReadVisualVtable(reinterpret_cast<uintptr_t>(edxVisual), &edxVtable)
					&& edxVtable == UPDATER_VISUAL_VTABLE)
				{
					staleCandidateVisual = reinterpret_cast<uintptr_t>(edxVisual);
					staleCandidateOwner = reinterpret_cast<uintptr_t>(windowStruct) - 0x3B0;
					staleCandidateOnset = time;
					staleCandidateVtable = edxVtable;
				}
			}
			else if (isUpdaterCall
				&& time < state.selectedRecordTime - ONSET_TOLERANCE_SECONDS
				&& staleDimOnsetCount < STALE_DIM_ONSET_CAPACITY
				&& !IsStaleDimmedOnset(time, ONSET_TOLERANCE_SECONDS)
				&& !IsGroupOnset(state, time, ONSET_TOLERANCE_SECONDS))
			{
				// A query for a past note: the rebuild-survivor path. The visual the per-advance mechanism
				// remembered died in the rebuild, so capture this live visual as the stale candidate; the
				// deferred fire above dims it on the next query with full onset+vtable revalidation. One in
				// flight at a time; each onset dims once per section.
				const bool slotFree = staleCandidateVisual == 0
					|| std::fabs(staleCandidateOnset - state.selectedRecordTime)
						<= ONSET_TOLERANCE_SECONDS;
				float edxOnset = 0.0f;
				uintptr_t edxVtable = 0;
				if (slotFree
					&& TryReadVisualOnset(reinterpret_cast<uintptr_t>(edxVisual), &edxOnset)
					&& std::fabs(edxOnset - time) <= ONSET_TOLERANCE_SECONDS
					&& TryReadVisualVtable(reinterpret_cast<uintptr_t>(edxVisual), &edxVtable)
					&& edxVtable == UPDATER_VISUAL_VTABLE)
				{
					staleCandidateVisual = reinterpret_cast<uintptr_t>(edxVisual);
					staleCandidateOwner = reinterpret_cast<uintptr_t>(windowStruct) - 0x3B0;
					staleCandidateOnset = time;
					staleCandidateVtable = edxVtable;
				}
			}

			// Upcoming-marker dim. Per-hold state: the forced list belongs to one target and resets
			// when the target advances, so a dimmed note that becomes the target gets natural answers
			// again.
			if (std::fabs(upcomingDimHoldOnset - state.selectedRecordTime)
				> ONSET_TOLERANCE_SECONDS)
			{
				upcomingDimHoldOnset = state.selectedRecordTime;
				upcomingDimCount = 0;
				upcomingCandidateVisual = 0;
			}
			if (upcomingCandidateVisual != 0)
			{
				// Deferred from the query that captured it, exactly like the stale dim:
				// never issue the synthetic call from inside that note's own updater call.
				const uintptr_t visual = upcomingCandidateVisual;
				const uintptr_t owner = upcomingCandidateOwner;
				const float onset = upcomingCandidateOnset;
				const uintptr_t capturedVtable = upcomingCandidateVtable;
				upcomingCandidateVisual = 0;
				float onsetNow = 0.0f;
				uintptr_t vtableNow = 0;
				if (isUpcomingDimEnabled
					&& onset > state.selectedRecordTime + ONSET_TOLERANCE_SECONDS
					&& upcomingDimCount < UPCOMING_DIM_CAPACITY
					&& TryReadVisualOnset(visual, &onsetNow)
					&& std::fabs(onsetNow - onset) <= ONSET_TOLERANCE_SECONDS
					&& TryReadVisualVtable(visual, &vtableNow)
					&& vtableNow == capturedVtable)
				{
					// List FIRST: the synthetic call's own condition query is forced
					// outside by the list rule below, which is what makes the game dim
					// its marker; the same rule then keeps it dim for the hold.
					upcomingDimOnsets[upcomingDimCount++] = onset;
					InterlockedExchange(&syntheticDimActive, 1);
					const bool dimmed = TrySyntheticDim(visual, owner);
					InterlockedExchange(&syntheticDimActive, 0);
					if (!dimmed && upcomingDimCount > 0)
					{
						--upcomingDimCount;
					}
					if (InterlockedDecrement(&upcomingDimLogBudget) >= 0)
					{
						LOG_INFO("(NBN UPDIM) upcoming marker "
							<< (dimmed ? "dimmed" : "call failed")
							<< " visual=0x" << std::hex << visual << std::dec
							<< std::fixed << std::setprecision(3)
							<< " onset=" << onset
							<< " target=" << state.selectedRecordTime
							<< " held=" << upcomingDimCount << std::endl);
					}
				}
			}
			else if (isUpdaterCall
				&& time > state.selectedRecordTime + ONSET_TOLERANCE_SECONDS
				&& result == 0
				&& upcomingDimCount < UPCOMING_DIM_CAPACITY
				&& !IsUpcomingDimmedOnset(time, ONSET_TOLERANCE_SECONDS)
				&& !IsGroupOnset(state, time, ONSET_TOLERANCE_SECONDS))
			{
				// Every upcoming in-section note's marker dims and stays dim, at the cost of grey upcoming
				// highway faces. Capture the visual for one synthetic dim on a later query.
				float edxOnset = 0.0f;
				uintptr_t edxVtable = 0;
				if (TryReadVisualOnset(reinterpret_cast<uintptr_t>(edxVisual), &edxOnset)
					&& std::fabs(edxOnset - time) <= ONSET_TOLERANCE_SECONDS
					&& TryReadVisualVtable(reinterpret_cast<uintptr_t>(edxVisual), &edxVtable)
					&& edxVtable == UPDATER_VISUAL_VTABLE)
				{
					upcomingCandidateVisual = reinterpret_cast<uintptr_t>(edxVisual);
					upcomingCandidateOwner = reinterpret_cast<uintptr_t>(windowStruct) - 0x3B0;
					upcomingCandidateOnset = time;
					upcomingCandidateVtable = edxVtable;
				}
			}
		}

		// The struct is the section window (bounds equal to the practice section) and the float
		// argument is the note's onset, so the note's identity is the time argument itself.
		const float start = *reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(windowStruct) + 0x14);
		const float end = *reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(windowStruct) + 0x1C);
		(void)start; (void)end;

		// Whitelist: target-only answers grey every approaching highway note, and splitting by
		// caller re-greys the highway once a hold latches, because the updater's answer lands in the
		// flag byte at visual+0x50 that the color step reads for both surfaces (the surfaces differ
		// only by the color step's ctx+0x28, not by the visual). Mode 3 therefore stays
		// time-directional.
		if (isUpdaterCall && InterlockedDecrement(&vtableLogBudget) >= 0)
		{
			uintptr_t vtable = 0;
			float visualOnset = -1.0f;
			const bool hasVtable = TryReadVisualVtable(
				reinterpret_cast<uintptr_t>(edxVisual), &vtable);
			TryReadVisualOnset(reinterpret_cast<uintptr_t>(edxVisual), &visualOnset);
			LOG_INFO("(NBN PLACEMENT) updater visual vtable=0x" << std::hex << vtable
				<< (hasVtable ? "" : " (unreadable)")
				<< " visual=0x" << reinterpret_cast<uintptr_t>(edxVisual) << std::dec
				<< std::fixed << std::setprecision(3)
				<< " visualOnset=" << visualOnset
				<< " queryTime=" << time
				<< " target=" << state.selectedRecordTime
				<< " native=" << result << std::endl);
		}
		// Dimmed onsets stay forced outside for the rest of the hold, since the marker relights on
		// the next natural inside answer. Strictly upcoming: the current target's onset never
		// matches, so a retarget onto a dimmed note returns to natural answers.
		if (isUpdaterCall
			&& time > state.selectedRecordTime + ONSET_TOLERANCE_SECONDS
			&& IsUpcomingDimmedOnset(time, ONSET_TOLERANCE_SECONDS))
		{
			return 1;
		}

		// Directional: upcoming notes (target included) keep their native "inside" answer; played
		// notes report outside and dim. This preserves the coloured highway.
		const bool isWhitelisted =
			time > state.selectedRecordTime - ONSET_TOLERANCE_SECONDS;

		if (InterlockedDecrement(&windowLogBudget) >= 0)
		{
			LOG_INFO("(NBN PLACEMENT) window check struct=0x" << std::hex
				<< reinterpret_cast<uintptr_t>(windowStruct) << std::dec
				<< std::fixed << std::setprecision(3)
				<< " start=" << start << " end=" << end << " time=" << time
				<< " native=" << result
				<< " targetTime=" << state.selectedRecordTime
				<< (isWhitelisted ? " WHITELISTED" : " forced-outside") << std::endl);
		}

		if (isWhitelisted || result != 0) return result;

		const auto total = InterlockedIncrement(&windowForcedCount);
		if (total == 1 || total % 500 == 0)
		{
			LOG_INFO("(NBN PLACEMENT) window forced outside: " << total
				<< std::fixed << std::setprecision(3) << " (last start=" << start << ")"
				<< std::endl);
		}
		return 1;
	}

	// 0x7E9ED0's caller at 0x79D092 writes through EDX immediately after the call, so the
	// condition must preserve EDX, which no compiler-generated wrapper guarantees. This thunk
	// saves ECX/EDX, runs the original (thiscall: ECX this, one stack float, callee pops), hands
	// the decision to the cdecl helper above, including the saved EDX (the note visual for
	// updater calls), restores the saved registers exactly, and returns with ret 4.
	__declspec(naked) void WindowConditionNakedDetour()
	{
		__asm
		{
			push ecx                        // saved this
			push edx                        // saved side-channel EDX
			mov eax, [esp + 12]             // the float argument
			push eax
			call originalWindowCondition    // thiscall: consumes the pushed float
			movzx eax, ax
			mov edx, [esp]                  // saved side-channel EDX (scratch; restored below)
			mov ecx, [esp + 8]              // the caller's return address (ECX is scratch here;
			                                // the real ECX is restored from [esp+4] at the end)
			push ecx                        // returnAddress: [0]=ra [4]=edx [8]=ecx [12]=ret [16]=float
			push edx                        // edxVisual:     [0]=edxV [4]=ra [8]=edx [12]=ecx [16]=ret [20]=float
			push eax                        // native:        [0]=native [4]=edxV [8]=ra [12]=edx [16]=ecx [20]=ret [24]=float
			mov eax, [esp + 24]             // the float argument again
			push eax                        // time:          [0]=time [4]=native [8]=edxV [12]=ra [16]=edx [20]=ecx [24]=ret [28]=float
			mov eax, [esp + 20]             // saved this
			push eax                        // windowStruct
			call WindowConditionHelper      // cdecl: caller cleans
			add esp, 20
			pop edx                         // restore the side-channel exactly
			pop ecx
			ret 4
		}
	}

	// The __thiscall(this, byte) slot at 0x7A9220. A __fastcall detour with a dummy EDX
	// parameter matches the thiscall ABI (ECX this, stack argument, callee pops).
	uint32_t __fastcall NeckVisibilityDetour(void* self, void* unusedEdx, uint8_t flag)
	{
		// 0x7A9220 is the marker sub-element's deleting destructor (vtable swap, resource releases,
		// optional free), so it is the eviction signal for the stale-marker capture: if the dying
		// sub-element belongs to the remembered visual, forget it.
		if (staleCandidateVisual != 0 && self != nullptr)
		{
			const auto dyingParent = *reinterpret_cast<uintptr_t*>(
				reinterpret_cast<uintptr_t>(self) + 0x8);
			if (dyingParent == staleCandidateVisual
				|| reinterpret_cast<uintptr_t>(self) == staleCandidateVisual)
			{
				staleCandidateVisual = 0;
			}
		}
		if (neckPlacementMode == 1 && InterlockedDecrement(&neckPlacementLogBudget) >= 0)
		{
			const auto parent = self == nullptr ? 0 : *reinterpret_cast<uintptr_t*>(
				reinterpret_cast<uintptr_t>(self) + 0x8);
			LOG_INFO("(NBN PLACEMENT) site=0x7a9220 flag=" << static_cast<int>(flag)
				<< " self=0x" << std::hex << reinterpret_cast<uintptr_t>(self)
				<< " parent=0x" << parent << std::dec
				<< (parent != 0
					? " string=" + std::to_string(*reinterpret_cast<uint8_t*>(parent + 0xC))
						+ " fret=" + std::to_string(*reinterpret_cast<uint8_t*>(parent + 0xD))
					: "")
				<< std::endl);
		}
		return originalNeckVisibility(self, unusedEdx, flag);
	}
}

void NoteByNoteHighwayRenderer::EvictRememberedDimCandidates()
{
	// Called on every NBN teardown (disable, probe reload, probe unload). The remembered
	// visuals are only revalidated by onset + vtable; across a teardown the destructor
	// detour that normally evicts a dying visual may be unhooked (it is probe-requested),
	// so a pointer captured before the teardown must never survive into the next hold.
	staleCandidateVisual = 0;
	staleCandidateOwner = 0;
	staleCandidateOnset = 0.0f;
	staleCandidateVtable = 0;
	upcomingCandidateVisual = 0;
	upcomingCandidateOwner = 0;
	upcomingCandidateOnset = 0.0f;
	upcomingCandidateVtable = 0;
	upcomingDimCount = 0;
	staleDimOnsetCount = 0;
	staleDimLastTarget = -1.0f;
	upcomingDimHoldOnset = -1.0f;
}

void NoteByNoteHighwayRenderer::SetNeckPlacementMode(long mode)
{
	if (mode < 0 || mode > 3)
	{
		LOG_ERROR("(NBN PLACEMENT) Invalid neck placement mode " << mode << "." << std::endl);
		return;
	}
	if (mode == 1) InterlockedExchange(&neckPlacementLogBudget, 40);
	if (mode == 2) InterlockedExchange(&neckPlacementRelocated, 0);
	if (mode == 3)
	{
		InterlockedExchange(&windowLogBudget, 30);
		InterlockedExchange(&windowForcedCount, 0);
		InterlockedExchange(&staleDimLogBudget, 20);
		InterlockedExchange(&vtableLogBudget, 48);
		staleCandidateVisual = 0;
	}
	InterlockedExchange(&neckPlacementMode, mode);
	LOG_INFO("(NBN PLACEMENT) Neck placement mode set to " << mode
		<< " (0 off, 1 dry, 2 target, 3 window), siteMask=0x" << std::hex
		<< neckPlacementSiteMask << std::dec << "." << std::endl);
}

long NoteByNoteHighwayRenderer::GetNeckPlacementMode()
{
	return neckPlacementMode;
}

void NoteByNoteHighwayRenderer::ArmFadeDryLog()
{
	InterlockedExchange(&fadeDryLogBudget, 150);
	LOG_INFO("(NBN FADE) armed: 150 lines over sites {0x7A8E90, 0x7A9290, 0x7A93C0}."
		<< std::endl);
}

void NoteByNoteHighwayRenderer::SetNeckPlacementSiteMask(long mask)
{
	InterlockedExchange(&neckPlacementSiteMask, mask);
	LOG_INFO("(NBN PLACEMENT) Site mask set to 0x" << std::hex << mask << std::dec
		<< " over {0x7A8B10, 0x7A8CE0, 0x7A8E90, 0x7A90B0, 0x7A9290, 0x7A93C0, 0x7A94F0}."
		<< std::endl);
}

long NoteByNoteHighwayRenderer::GetNeckPlacementSiteMask()
{
	return neckPlacementSiteMask;
}

bool NoteByNoteHighwayRenderer::InstallPresentationGate()
{
	if (isGateInstalled) return true;

	originalNotewaySetup = reinterpret_cast<NativeNotewaySetup>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_NOTEWAY_SETUP),
		reinterpret_cast<PBYTE>(&NotewaySetupDetour)));
	if (originalNotewaySetup == nullptr)
	{
		LOG_ERROR("(NBN PRESENTATION) Installing the noteway setup detour at 0x7E22F0 failed;"
			<< " the scrolling highway will render unmodified." << std::endl);
		return false;
	}

	originalDrawDescriptors = reinterpret_cast<NativeDrawDescriptors>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_DRAW_DESCRIPTORS),
		reinterpret_cast<PBYTE>(&DrawDescriptorsDetour)));
	if (originalDrawDescriptors == nullptr)
	{
		LOG_ERROR("(NBN PRESENTATION) Installing the descriptor draw detour at 0x7A4B50 failed;"
			<< " the scrolling highway will render unmodified." << std::endl);
		return false;
	}

	originalDrawGeometry = reinterpret_cast<NativeDrawGeometry>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_DRAW_GEOMETRY),
		reinterpret_cast<PBYTE>(&DrawGeometryDetour)));
	originalDrawSubmit = reinterpret_cast<NativeDrawSubmit>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_DRAW_SUBMIT),
		reinterpret_cast<PBYTE>(&DrawSubmitDetour)));
	if (originalDrawGeometry == nullptr || originalDrawSubmit == nullptr)
	{
		LOG_ERROR("(NBN PRESENTATION) Installing the highway draw detours failed"
			<< " (geometry=" << (originalDrawGeometry != nullptr)
			<< ", submit=" << (originalDrawSubmit != nullptr)
			<< "); the scrolling highway will render unmodified." << std::endl);
		return false;
	}

	// Where the finished per-frame fretboard buffer is consumed, which is the only valid point
	// to read it.
	originalBufferConsume = reinterpret_cast<NativeBufferConsume>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_FRETBOARD_BUFFER_CONSUME),
		reinterpret_cast<PBYTE>(&BufferConsumeDetour)));
	if (originalBufferConsume == nullptr)
	{
		LOG_ERROR("(NBN PRESENTATION) Installing the fretboard buffer detour at 0x7AA1B0"
			<< " failed; the finished neck-diagram state cannot be read." << std::endl);
		return false;
	}

	// Below both paths into the neck diagram, including the one that skips the predicate.
	originalGridWrite = reinterpret_cast<NativeGridWrite>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_FRETBOARD_GRID_WRITE),
		reinterpret_cast<PBYTE>(&GridWriteDetour)));
	if (originalGridWrite == nullptr)
	{
		LOG_ERROR("(NBN PRESENTATION) Installing the neck-diagram grid detour at 0x7AA140"
			<< " failed; fretboard-only visual suppression is unavailable, while note lifecycle"
			<< " remains protected by the predicate pass-through." << std::endl);
		return false;
	}

	for (size_t site = 0; site < NECK_PLACEMENT_SITE_COUNT; ++site)
	{
		originalNeckPlacements[site] = reinterpret_cast<NativeNeckPlacement>(DetourFunction(
			reinterpret_cast<PBYTE>(NECK_PLACEMENT_SITES[site]),
			reinterpret_cast<PBYTE>(NECK_PLACEMENT_DETOURS[site])));
		if (originalNeckPlacements[site] == nullptr)
		{
			LOG_ERROR("(NBN PLACEMENT) Installing the placement detour at 0x" << std::hex
				<< NECK_PLACEMENT_SITES[site] << std::dec
				<< " failed; read-ahead markers cannot be relocated." << std::endl);
			return false;
		}
	}
	originalNeckVisibility = reinterpret_cast<NativeNeckVisibility>(DetourFunction(
		reinterpret_cast<PBYTE>(NECK_VISIBILITY_SITE),
		reinterpret_cast<PBYTE>(&NeckVisibilityDetour)));
	if (originalNeckVisibility == nullptr)
	{
		LOG_ERROR("(NBN PLACEMENT) Installing the visibility detour at 0x7A9220 failed."
			<< std::endl);
		return false;
	}

	// The FUN_0079D070 updater stays unhooked: its custom convention (stack parameter plus EDX
	// side-channel) crashes on song load when hooked. Only the condition is hooked; its
	// __thiscall(this, float) contract is verified, and notes are identified by onset time.
	(void)&originalMarkerUpdater;
	originalWindowCondition = reinterpret_cast<NativeWindowCondition>(DetourFunction(
		reinterpret_cast<PBYTE>(NECK_WINDOW_CONDITION),
		reinterpret_cast<PBYTE>(&WindowConditionNakedDetour)));
	if (originalWindowCondition == nullptr)
	{
		LOG_ERROR("(NBN PLACEMENT) Installing the window-condition detour at 0x7E9ED0"
			<< " failed; mode window is unavailable." << std::endl);
		return false;
	}

	isGateInstalled = true;
	LOG_INFO("(NBN PRESENTATION) Rocksmith's presentability predicate is untouched;"
		<< " fretboard visual grid installed at 0x7AA140,"
		<< " highway pools at"
		<< " 0x7E22F0, descriptors at 0x7A4B50, and highway draw at 0x7A52C0/0x7A6160."
		<< " 0x7A4AA0 is left running on purpose: gating it changed nothing visually and"
		<< " starved per-note colour state. Non-selected notes are only skipped at draw"
		<< " time; legato notes in the published group stay visible." << std::endl);
	return true;
}

void NoteByNoteHighwayRenderer::RefreshSelectedTarget(void* owner)
{
	ResearchProtocol::NoteByNoteState state;
	const bool haveState = NoteByNoteNativeScoring::TryGetResearchState(state);

	// Track native range/PlayerSong restarts so the presentation grid can settle after a
	// target transition. The presentability predicate itself is always pass-through and
	// therefore never retires records during this transition.
	if (haveState)
	{
		const bool ownsHold = state.ownsNativeHold != 0;
		const bool epochChanged = hasLastSeenEpoch && state.epoch != lastSeenEpoch;
		const bool recordChanged = hasLastSeenSelectedRecord
			&& state.selectedRecord != lastSeenSelectedRecord;
		const bool holdReleased = hasLastOwnsNativeHold && lastOwnsNativeHold && !ownsHold;
		if (epochChanged || recordChanged || holdReleased)
		{
			presentationSettleFrames = PRESENTATION_SETTLE_FRAMES;
			LOG_INFO("(NBN PRESENTATION) Hold transition (epoch " << lastSeenEpoch << "->"
				<< state.epoch << (recordChanged ? ", record changed" : "")
				<< (holdReleased ? ", hold released" : "")
				<< "); suspending highway draw suppression for " << PRESENTATION_SETTLE_FRAMES
				<< " frames while Rocksmith rebuilds the note pool."
				<< std::endl);
		}
		lastSeenEpoch = state.epoch;
		hasLastSeenEpoch = true;
		lastSeenSelectedRecord = state.selectedRecord;
		hasLastSeenSelectedRecord = true;
		lastOwnsNativeHold = ownsHold;
		hasLastOwnsNativeHold = true;
	}
	if (presentationSettleFrames > 0)
	{
		--presentationSettleFrames;
	}

	if (!haveState
		|| state.isInitialized == 0
		|| state.gatePhase == ResearchProtocol::GatePhase::Idle
		|| state.trackedOwner != reinterpret_cast<uintptr_t>(owner)
		|| state.visualRecord == 0)
	{
		ClearSelectedTarget();
		return;
	}
	if (state.visualChordId != -1)
	{
		// Chords fall through to stock rendering for the single-target consumers: a chord is
		// several notes sharing one target, and suppressing its siblings would hide notes that
		// still have to be played. The quad filter is the exception: it activates on the stashed
		// chord shape (matched by chordId, fail-open on any mismatch) so future-note markers stop
		// painting during chord holds too.
		ClearSelectedTarget();
		isChordTargetActive = chordShapeChordId == state.visualChordId;
		isChordHoldFrozen = isChordTargetActive && state.ownsNativeHold != 0;
		return;
	}

	uint32_t groupCount = state.visualGroupCount;
	if (groupCount > ResearchProtocol::NoteByNoteState::MaxVisualGroup)
	{
		groupCount = ResearchProtocol::NoteByNoteState::MaxVisualGroup;
	}
	for (uint32_t i = 0; i < groupCount; ++i)
	{
		visualGroupRecords[i] = state.visualGroupRecords[i];
		visualGroupStrings[i] = state.visualGroupStrings[i];
		visualGroupFrets[i] = state.visualGroupFrets[i];
	}
	visualGroupCount = groupCount;
	selectedRecord = state.visualRecord;
	selectedStringIndex = state.visualString;
	selectedFret = state.visualFret;
	selectedRecordTime = state.selectedRecordTime;
	hasSelectedRecordTime = true;
	isNativeHoldOwned = state.ownsNativeHold != 0;
	isChordTargetActive = false;
	isTargetActive = true;

	if (loggedRecord != state.visualRecord)
	{
		loggedRecord = state.visualRecord;
		// The grid write is the sole fretboard visual gate; the predicate is intentionally a
		// lifecycle pass-through.
		LOG_INFO("(NBN FRETBOARD) Grid " << gridSuppressed << "/" << gridCalls
			<< " suppressed; predicate pass-through, since last target ("
			<< selectedStringIndex << ':' << selectedFret
			<< " +" << visualGroupCount << " group)." << std::endl);
		gridCalls = 0;
		gridSuppressed = 0;
		gridProbesLogged = 0;
		// A few consecutive frames, so a steady cell is
		// distinguishable from a flickering one.
		fretboardDumpFramesRemaining = 3;


		LOG_INFO("(NBN PRESENTATION) Draw gate since last target: descriptors "
			<< descriptorSuppressed << "/" << descriptorCalls
			<< " suppressed, geometry " << geometrySuppressed << "/" << geometryCalls
			<< " suppressed, submit " << submitSuppressed << "/" << submitCalls
			<< " suppressed, group=" << visualGroupCount << "." << std::endl);
		descriptorCalls = 0;
		descriptorSuppressed = 0;
		geometryCalls = 0;
		geometrySuppressed = 0;
		submitCalls = 0;
		submitSuppressed = 0;

		LOG_INFO("(NBN PRESENTATION) Focusing the fretboard on record=0x" << std::hex
			<< state.visualRecord << std::dec
			<< " (" << state.visualString << ':' << state.visualFret << ")."
			<< " Non-gesture grid cells are cleared after their native writes; all records"
			<< " remain presentable and selectable." << std::endl);
	}
}

void NoteByNoteHighwayRenderer::ClearSelectedTarget()
{
	isTargetActive = false;
	isChordTargetActive = false;
	isChordHoldFrozen = false;
	isNativeHoldOwned = false;
	selectedRecord = 0;
	selectedStringIndex = -1;
	selectedFret = -1;
	visualGroupCount = 0;
	loggedRecord = 0;
}

void NoteByNoteHighwayRenderer::SetChordTargetShape(
	int32_t chordId,
	const int* frets,
	const int* fingers,
	int anchorFret,
	int anchorWidth)
{
	if (frets == nullptr) return;
	for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
	{
		chordShapeFrets[stringIndex] = frets[stringIndex];
		chordShapeFingers[stringIndex] = fingers != nullptr ? fingers[stringIndex] : 0;
	}
	chordShapeAnchorFret = anchorFret;
	chordShapeAnchorWidth = anchorWidth;
	chordShapeChordId = chordId;
}

bool NoteByNoteHighwayRenderer::TryGetChordFingerForCoordinate(
	int stringIndex,
	int fret,
	int& finger)
{
	if (!isChordTargetActive) return false;
	if (stringIndex < 0 || stringIndex >= 6) return false;
	if (chordShapeFrets[stringIndex] != fret) return false;
	const int candidate = chordShapeFingers[stringIndex];
	if (candidate < 1 || candidate > 4) return false;
	finger = candidate;
	return true;
}

bool NoteByNoteHighwayRenderer::IsChordHoldActive()
{
	return isChordTargetActive;
}

bool NoteByNoteHighwayRenderer::IsChordHoldFrozen()
{
	return isChordHoldFrozen;
}

bool NoteByNoteHighwayRenderer::TryGetMarkerKeepCoordinates(
	int (&strings)[MaxMarkerKeep],
	int (&frets)[MaxMarkerKeep],
	int& count)
{
	count = 0;
	if (isTargetActive)
	{
		const int targetString = selectedStringIndex;
		const int targetFret = selectedFret;
		if (targetString < 0 || targetFret < 0) return false;
		strings[count] = targetString;
		frets[count] = targetFret;
		++count;
		// The legato group rides along so a hammer-on run's markers survive the
		// filter the same way they survive the fretboard suppression.
		uint32_t groupCount = visualGroupCount;
		if (groupCount > ResearchProtocol::NoteByNoteState::MaxVisualGroup)
		{
			groupCount = ResearchProtocol::NoteByNoteState::MaxVisualGroup;
		}
		for (uint32_t index = 0; index < groupCount && count < MaxMarkerKeep; ++index)
		{
			strings[count] = visualGroupStrings[index];
			frets[count] = visualGroupFrets[index];
			++count;
		}
		return count > 0;
	}
	if (isChordTargetActive)
	{
		int lowestFretted = 25;
		int highestFretted = 0;
		for (int stringIndex = 0; stringIndex < 6 && count < MaxMarkerKeep; ++stringIndex)
		{
			const int fret = chordShapeFrets[stringIndex];
			if (fret < 1 || fret > 24) continue;
			strings[count] = stringIndex;
			frets[count] = fret;
			++count;
			lowestFretted = (std::min)(lowestFretted, fret);
			highestFretted = (std::max)(highestFretted, fret);
		}
		// An open member is drawn as a bar across the chord's frame, so its marker sits on its
		// string at a fret inside the anchor span, not at fret 0 (the decoder has no fret 0).
		// Keeping only fretted members hid every open note of a mixed chord. Each open member
		// keeps its string across the record's anchor span; without an anchor, across the
		// fretted members' span plus one fret either side.
		int spanFirst = chordShapeAnchorFret;
		int spanLast = chordShapeAnchorFret + chordShapeAnchorWidth - 1;
		if (chordShapeAnchorFret < 1 || chordShapeAnchorWidth < 1 || chordShapeAnchorWidth > 8)
		{
			spanFirst = highestFretted > 0 ? lowestFretted - 1 : 1;
			spanLast = highestFretted > 0 ? highestFretted + 1 : 4;
		}
		spanFirst = (std::max)(spanFirst, 1);
		spanLast = (std::min)(spanLast, 24);
		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			if (chordShapeFrets[stringIndex] != 0) continue;
			for (int fret = spanFirst; fret <= spanLast && count < MaxMarkerKeep; ++fret)
			{
				strings[count] = stringIndex;
				frets[count] = fret;
				++count;
			}
		}
		// An all-open chord has no fretted member, and every filter treats an empty keep set as
		// "stand down", which lets the next chord's markers draw during the hold. Publish one
		// coordinate no marker can decode to, so the filters stay live and hide every non-member
		// marker.
		if (count == 0)
		{
			strings[count] = -2;
			frets[count] = -2;
			++count;
		}
		return true;
	}
	return false;
}

bool NoteByNoteHighwayRenderer::TryGetSelectedTargetForDiagnostics(int& stringIndex, int& fret)
{
	if (!isTargetActive) return false;

	const int candidateString = selectedStringIndex;
	const int candidateFret = selectedFret;
	if (candidateString < 0 || candidateFret < 0) return false;

	stringIndex = candidateString;
	fret = candidateFret;
	return true;
}

bool NoteByNoteHighwayRenderer::TryGetSelectedPresentation(int& stringIndex, int& fret)
{
	// Returning false leaves the instance filter dormant, so it never locks a vertex buffer.
	if (static_cast<NoteByNoteHighwayRenderer::HighwayMode>(highwayMode)
		== NoteByNoteHighwayRenderer::HighwayMode::Off)
	{
		return false;
	}
	if (!isTargetActive) return false;

	const int candidateString = selectedStringIndex;
	const int candidateFret = selectedFret;
	if (candidateString < 0 || candidateFret < 0) return false;

	stringIndex = candidateString;
	fret = candidateFret;
	return true;
}

bool NoteByNoteHighwayRenderer::HasSelectedPresentation()
{
	int stringIndex = -1;
	int fret = -1;
	return TryGetSelectedPresentation(stringIndex, fret);
}

const char* NoteByNoteHighwayRenderer::DescribeGridGateMode(GridGateMode mode)
{
	switch (mode)
	{
	case GridGateMode::Off: return "off";
	case GridGateMode::Restore: return "restore";
	case GridGateMode::Sentinel: return "sentinel";
	case GridGateMode::SkipKnownBad: return "skip-known-bad";
	}
	return "unknown";
}

void NoteByNoteHighwayRenderer::SetGridGateMode(GridGateMode mode)
{
	// Skip is refused: skipping 0x7AA140 also skips its container operation, which faults the
	// controller.
	if (mode == GridGateMode::SkipKnownBad)
	{
		LOG_ERROR("(NBN GRID) Refusing grid gate mode 'skip': skipping 0x7AA140 also skips its"
			<< " unconditional container operation on ESI+0xD04, which faulted the controller"
			<< " with \"the selected native record expired before the hold could be"
			<< " established\". Use restore or sentinel." << std::endl);
		return;
	}

	gridGateMode = static_cast<long>(mode);
	LOG_INFO("(NBN GRID) Grid gate mode set to " << DescribeGridGateMode(mode)
		<< ". The neck diagram shows the target plus its published legato group; every other"
		<< " cell is corrected after the original write runs." << std::endl);
}

NoteByNoteHighwayRenderer::GridGateMode NoteByNoteHighwayRenderer::GetGridGateMode()
{
	return static_cast<GridGateMode>(gridGateMode);
}

void NoteByNoteHighwayRenderer::SetGridSentinel(uint32_t valueWord, float keyWord)
{
	gridSentinelValue = valueWord;
	gridSentinelKey = keyWord;
	LOG_INFO("(NBN GRID) Sentinel set to value=0x" << std::hex << valueWord << std::dec
		<< " key=" << keyWord << ". Used only in sentinel mode." << std::endl);
}

void NoteByNoteHighwayRenderer::GetGridSentinel(uint32_t& valueWord, float& keyWord)
{
	valueWord = gridSentinelValue;
	keyWord = gridSentinelKey;
}

const char* NoteByNoteHighwayRenderer::DescribeHighwayMode(HighwayMode mode)
{
	switch (mode)
	{
	case HighwayMode::Off: return "off";
	case HighwayMode::All: return "all";
	case HighwayMode::NearTargetWindow: return "near-target";
	}
	return "unknown";
}

void NoteByNoteHighwayRenderer::SetUpcomingDimEnabled(bool enabled)
{
	isUpcomingDimEnabled = enabled;
	LOG_INFO("(NBN UPDIM) Upcoming-marker dim "
		<< (enabled ? "enabled: upcoming notes gray while frozen."
			: "disabled: upcoming notes keep their colour while frozen.")
		<< std::endl);
}

bool NoteByNoteHighwayRenderer::GetUpcomingDimEnabled()
{
	return isUpcomingDimEnabled;
}

void NoteByNoteHighwayRenderer::SetHighwayMode(HighwayMode mode)
{
	highwayMode = static_cast<long>(mode);
	LOG_INFO("(NBN HIGHWAY) Highway mode set to " << DescribeHighwayMode(mode)
		<< ". off leaves the highway stock; all hides every note outside the gesture and"
		<< " removes the read-ahead; near-target hides only non-gesture notes within "
		<< highwayWindowSeconds << "s of the target either side, which are the ones the"
		<< " frozen window parks at the plane." << std::endl);
}

NoteByNoteHighwayRenderer::HighwayMode NoteByNoteHighwayRenderer::GetHighwayMode()
{
	return static_cast<HighwayMode>(highwayMode);
}

void NoteByNoteHighwayRenderer::SetHighwayWindowSeconds(float seconds)
{
	// Bounded so a bad value cannot hide the whole chart or silently do nothing.
	if (!(seconds >= 0.0f) || seconds > 5.0f)
	{
		LOG_ERROR("(NBN HIGHWAY) Refusing a highway window of " << seconds
			<< "s; it must be between 0 and 5." << std::endl);
		return;
	}
	highwayWindowSeconds = seconds;
	LOG_INFO("(NBN HIGHWAY) Highway window set to " << seconds
		<< "s either side of the target." << std::endl);
}

float NoteByNoteHighwayRenderer::GetHighwayWindowSeconds()
{
	return highwayWindowSeconds;
}
