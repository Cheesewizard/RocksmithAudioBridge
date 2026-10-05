#pragma once

namespace NoteByNoteMenu
{
	void Initialize();

	// Once per presented frame from the EndScene seam (the game's UI thread): reads the
	// NOTE BY NOTE row while it is focused and repairs it by name while it is not.
	void Poll();

	// What the rocker wiring last saw, for the bridge 'status' reply.
	struct Diagnostics
	{
		bool hooked = false;
		bool controllerCaptured = false;
		unsigned long long focusedReads = 0;
		unsigned long long repairs = 0;
		unsigned long long toggles = 0;
		int lastRowValue = -1;
		bool lastToggleAccepted = false;
		// Stage-builder post-hook: how many rows the screen had, and where our row was moved.
		unsigned long long stageBuilds = 0;
		int lastChildCount = 0;
		int lastRowSortOrderFrom = -1;
		int lastRowSortOrderTo = -1;
		// Builder preset of the rebuilt row to the live state: bitmask 1 = InitialValue
		// written, 2 = Value written, 0 = no member found, -1 = faulted.
		int lastRowPreset = -1;
		// Every child the builder post-hook enumerated, "ID:SortOrder" per child.
		std::string builderListing;
		unsigned long lastBuilderController = 0;
		int lastBuilderCounter = -1;
		// Row probe results.
		unsigned long long probes = 0;
		unsigned long probeContainer = 0;
		unsigned long probeRangeBegin = 0;
		unsigned long probeRangeEnd = 0;
		int probeChildCount = 0;
		std::string probeListing;
		// Diagnostic: the container's vtable and its slot-0x74 (the enumerate method) as seen
		// at the builder detour (right after the game built its rows) and at the UI-thread probe
		// (fully realized). If these differ, the +0x1C4 container was replaced after realization.
		// Same three, sampled BEFORE the builder ran (the container the game enumerates).
		unsigned long preBuilderContainer = 0;
		unsigned long preBuilderVtable = 0;
		unsigned long preBuilderSlot74 = 0;
		unsigned long builderContainer = 0;
		unsigned long builderVtable = 0;
		unsigned long builderSlot74 = 0;
		unsigned long probeVtable = 0;
		unsigned long probeSlot74 = 0;
	};
	Diagnostics GetDiagnostics();

	// Ask the UI thread to enumerate the current screen's row container on its next poll;
	// the result lands in Diagnostics::probe* (bridge command probe_menu_rows, read twice).
	void RequestRowProbe();

	// True once Riff Repeater Advanced Settings was built without the NOTE BY NOTE row (cache.psarc reset or never
	// patched): Note by Note cannot be switched on until the installer's Reinstall / Repair adds it back.
	bool IsMenuRowMissing();
}
