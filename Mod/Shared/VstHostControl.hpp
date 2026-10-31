#pragma once

// Shared state between the overlay (DLL/ToneDesigner/VstHostLink.cpp) and the VST host program
// (VstHost/Source/Main.cpp). Plain named shared memory; the layout is the same in 32 and 64 bit (only 32-bit
// fields). The VST host does the audio and owns the amp window; the overlay tells it whether the overlay is on
// screen, and sends the Tone designer page's commands (one at a time, see Command).

#include <Windows.h>
#include <cstdint>

namespace VstHostControl
{
	// v10: undo and redo (Command::Undo, Redo, undoLabel, redoLabel) and the Inspector's plugin parameters (inspectSlot,
	// Command::SetParam, params). v9: the overlay key (overlayKey). v8: the chain's own delay (chainLatencySamples). v7: standby (off with the plugins kept loaded). v6: spare plugins kept loaded (preloading, sparePaths, preloadAllowed). v5: one window per plugin (openWindows, HideSlot). v4: slot paths, chain bypass and the Tone designer page's
	// commands. v3: plugin chains, tones, levels. A new mapping name per version, so an old host and a new overlay
	// never meet.
	constexpr wchar_t kMappingName[] = L"Local\\RocksmithAudioBridge.VstHost.v10";
	constexpr uint32_t kMagic = 0x56424152;   // "RABV"
	constexpr uint32_t kVersion = 10;
	constexpr int kBeatMs = 250;              // both sides bump their beat at least this often
	constexpr int kAliveMs = 1500;            // a side is gone when its beat stops this long
	constexpr int kPathChars = 520;
	constexpr int kMaxSlots = 8;
	constexpr int kNameChars = 64;
	constexpr int kMaxSpares = 16;
	constexpr int kMaxParams = 48;            // the Inspector shows this many of the inspected plugin's parameters
	constexpr int kParamTextChars = 32;
	constexpr int kParamLabelChars = 16;

	// Overlay -> host. The overlay writes op/arg/text, then bumps commandSeq; the host acts and sets commandDone
	// to the sequence it handled. Paths and tone names are in text.
	enum class Command : LONG
	{
		None = 0,
		AddPlugin = 1,      // text = plugin path; arg = 0 appends it, arg = n inserts it at slot n - 1
		ReplaceSlot = 2,    // arg = slot, text = plugin path
		RemoveSlot = 3,     // arg = slot
		MoveSlot = 4,       // arg = slot, arg2 = -1 (earlier) or +1 (later)
		SetBypass = 5,      // arg = slot, arg2 = 0/1
		ShowSlot = 6,       // arg = slot: open its plugin window (or bring it to the front)
		LoadTone = 7,       // text = tone name
		SaveTone = 8,       // text = tone name
		DeleteTone = 9,     // text = tone name
		SetLevels = 10,     // arg = input trim, arg2 = output trim, tenths of a dB
		ClearChain = 11,
		SetMuted = 12,      // arg = 0/1: the chain plays silence (mute while tuning)
		DuplicateSlot = 13, // arg = slot; the copy, with the same settings, goes right after it
		SetChainBypass = 14,// arg = 0/1: every plugin skipped (clean guitar); each slot keeps its own bypass
		RenameTone = 15,    // text = old name, a newline, new name
		HideSlot = 16,      // arg = slot: close its plugin window
		PrepareTone = 17,   // text = tone name: load the plugins it needs as spares; the chain plays on unchanged
		LoadSpare = 18,     // text = plugin path: load it now as a spare and keep it while the host runs
		UnloadSpare = 19,   // text = plugin path: unload its spares (not the chain's)
		SetStandby = 20,    // arg = 1: off, the game's amp plays and the plugins stay loaded; 0: on again
		ReloadPlugin = 21,  // text = plugin path: unload every copy (chain and spares), load it again from scratch
		                    // (an older host ignores it; the layout is unchanged, so no new version)
		Undo = 22,          // the last chain change taken back (undoLabel names it); nothing when there is none
		Redo = 23,          // the last change taken back done again (redoLabel)
		SetParam = 24,      // arg = slot, arg2 = parameter index (Param::index), commandValue = normalized 0..1,
		                    // commandPhase = ParamPhase: a knob drag is one Begin, any number of Change, one End
	};
	// ClearChain's arg: 0 clears the board as one undo step ("clear board"); 1 starts a new tone (the history goes).

	// SetParam's gesture. A drag is one undo step and one automation gesture for the plugin (beginChangeGesture ..
	// endChangeGesture); Set is a single edit (a click, a typed value) with a gesture and an undo step of its own.
	enum class ParamPhase : LONG
	{
		Set = 0,
		Begin = 1,
		Change = 2,
		End = 3,
	};

	// One parameter of the inspected plugin (host -> overlay).
	struct Param
	{
		volatile LONG index;               // the plugin's own parameter index, what SetParam takes
		float value;                       // normalized 0..1
		volatile LONG steps;               // 0 = continuous; else how many values it takes (2 = a switch)
		volatile LONG flags;               // kParamDiscrete, kParamBoolean
		char name[kNameChars];             // UTF-8
		char text[kParamTextChars];        // UTF-8, the value as the plugin shows it ("-6.0 dB" without the label)
		char label[kParamLabelChars];      // UTF-8, its unit ("dB", "%", "Hz"); may be empty
	};
	constexpr LONG kParamDiscrete = 1;
	constexpr LONG kParamBoolean = 2;

	struct Slot
	{
		char name[kNameChars];   // UTF-8
		volatile LONG bypass;
	};

	struct Shared
	{
		uint32_t magic;
		uint32_t version;

		// Written by the VST host.
		volatile LONG hostPid;         // 0 = not running
		volatile LONG hostBeat;
		volatile LONG popoutShown;     // the amp window is on screen now
		volatile LONG loading;         // 1 while plugins load (the beat pauses meanwhile)
		char loadError[128];           // UTF-8, the last command's problem; empty when it worked
		volatile LONG stateSeq;        // bumped after the fields below change
		volatile LONG slotCount;
		Slot slots[kMaxSlots];
		volatile LONG shownSlot;       // whose editor the amp window shows, -1 = none
		char toneName[kNameChars];     // UTF-8, the tone last loaded or saved; empty = unsaved
		volatile LONG toneModified;    // changed since that tone was loaded or saved
		volatile LONG inputTenths;     // level trims, tenths of a dB
		volatile LONG outputTenths;
		volatile LONG inputPeak;       // peak since the overlay last read it, thousandths of full scale
		volatile LONG outputPeak;

		// Written by the overlay.
		volatile LONG overlayBeat;
		volatile LONG overlayVisible;  // the overlay is open
		volatile LONG commandSeq;
		volatile LONG commandOp;       // Command
		volatile LONG commandArg;
		volatile LONG commandArg2;
		wchar_t commandText[kPathChars];

		// Written by the host.
		volatile LONG commandDone;

		// "Show plugin windows": the open plugin windows are on screen while this is on and the overlay is open.
		// Written by the page; the VST host seeds it from its saved choice when it starts.
		volatile LONG popoutWanted;
		volatile LONG quitRequested;   // overlay -> VST host: close (saves the chain)

		// Written by the host (v4).
		volatile LONG chainBypassed;   // SetChainBypass is on
		wchar_t slotPaths[kMaxSlots][kPathChars];   // each slot's plugin file
		wchar_t failedPath[kPathChars];             // the last plugin that did not load (Add or Replace)

		// Written by the host (v5).
		volatile LONG openWindows;     // bit per slot: that plugin's window is open
		// While `loading`: which plugin is loading now (UTF-8, its file name) and how far a tone has got (step 1..total).
		char loadingText[kNameChars];
		volatile LONG loadingStep;
		volatile LONG loadingTotal;

		// Written by the host (v6): plugins kept loaded outside the chain, so tones that use them switch at once
		// (VstHost Chain.hpp, spares). Starred plugins can be loaded in the background, one at a time.
		volatile LONG preloading;      // 1 while one loads that way (the beat pauses meanwhile)
		char preloadText[kNameChars];  // which (UTF-8, its file name)
		volatile LONG spareCount;
		wchar_t sparePaths[kMaxSpares][kPathChars];
		// Written by the overlay (v6).
		volatile LONG preloadAllowed;  // 0 while a song plays (a plugin starting up could cost late buffers) or the player is using the page
		// Written by the host (v7).
		volatile LONG standby;         // off: the amp link is released, the plugins stay loaded (Command::SetStandby)
		// Written by the host (v8): the delay the chain's plugins report (VST3 latency), summed over the slots that
		// play (none while the chain is bypassed), in samples at the stream's rate. The latency readout adds it live
		// to what was measured, so a new chain or a plugin changing its own delay (oversampling) needs no new
		// measurement. Kept current every beat.
		volatile LONG chainLatencySamples;
		// Written by the overlay (v9): the virtual key that opens and closes the overlay ([Keybinds] OverlayKey). The
		// host hands it back to the game when a plugin window has the keyboard. 0 until the overlay writes it.
		volatile LONG overlayKey;

		// Written by the overlay (v10): whose parameters the Inspector shows, -1 = none.
		volatile LONG inspectSlot;
		// The value and gesture phase of Command::SetParam, written with the command's other fields.
		float commandValue;
		volatile LONG commandPhase;    // ParamPhase

		// Written by the host (v10), with the chain (stateSeq): what Undo and Redo would do, for the Edit menu
		// ("remove AmpliTube 5"). UTF-8; empty when there is nothing to undo or redo.
		char undoLabel[kNameChars];
		char redoLabel[kNameChars];
		// The inspected slot's parameters, under a sequence lock: the host makes paramSeq odd, writes, then even
		// again; the overlay copies only while it is even and unchanged across the copy. Refreshed when a value
		// changes (the page, the plugin's own window, automation) and at least every kBeatMs.
		volatile LONG paramSeq;
		volatile LONG paramSlot;       // the slot they belong to, -1 = none (no slot inspected, or it is empty)
		volatile LONG paramTotal;      // how many parameters the plugin has (only the first kMaxParams are listed)
		volatile LONG paramCount;
		Param params[kMaxParams];
	};

	// Creates or opens the block. Returns nullptr on failure; `mapping` must stay open while it is used.
	inline Shared* Open(HANDLE& mapping)
	{
		mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), kMappingName);
		if (!mapping) return nullptr;
		auto* shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
		if (!shared) { CloseHandle(mapping); mapping = nullptr; return nullptr; }
		if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&shared->magic), static_cast<LONG>(kMagic), 0) == 0)
			shared->version = kVersion;
		return shared;
	}
}
