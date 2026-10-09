#pragma once

namespace NoteByNoteMenu
{
	void Initialize();
	void Poll();
	struct Diagnostics
	{
		bool hooked = false;
		bool controllerCaptured = false;
		unsigned long long focusedReads = 0;
		unsigned long long repairs = 0;
		unsigned long long toggles = 0;
		int lastRowValue = -1;
		bool lastToggleAccepted = false;
		unsigned long long stageBuilds = 0;
		int lastChildCount = 0;
		int lastRowSortOrderFrom = -1;
		int lastRowSortOrderTo = -1;
		int lastRowPreset = -1;
		std::string builderListing;
		unsigned long lastBuilderController = 0;
		int lastBuilderCounter = -1;
		unsigned long long probes = 0;
		unsigned long probeContainer = 0;
		unsigned long probeRangeBegin = 0;
		unsigned long probeRangeEnd = 0;
		int probeChildCount = 0;
		std::string probeListing;
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
	void RequestRowProbe();
	bool IsMenuRowMissing();
}
