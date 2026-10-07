#pragma once

// Shared state between the overlay (DLL/Audio/VstHostLink.cpp) and the VST host program
// (VstHost/Source/Main.cpp). Plain named shared memory; the layout is the same in 32 and 64 bit (only 32-bit
// fields). The VST host does the audio and owns the amp window; the overlay tells it whether the overlay is on
// screen and what the External amp page's buttons asked for.

#include <Windows.h>
#include <cstdint>

namespace VstHostControl
{
	constexpr wchar_t kMappingName[] = L"Local\\RocksmithAudioBridge.VstHost.v2";
	constexpr uint32_t kMagic = 0x56424152;   // "RABV"
	constexpr uint32_t kVersion = 2;   // v2: plugin switch inside the running host (a new mapping name, so v1 never meets it)
	constexpr int kBeatMs = 250;              // both sides bump their beat at least this often
	constexpr int kAliveMs = 1500;            // a side is gone when its beat stops this long
	constexpr int kPathChars = 520;

	struct Shared
	{
		uint32_t magic;
		uint32_t version;

		// Written by the VST host.
		volatile LONG hostPid;         // 0 = not running
		volatile LONG hostBeat;
		volatile LONG popoutShown;     // the amp window is on screen now
		char pluginName[64];           // UTF-8

		// Written by the overlay.
		volatile LONG overlayBeat;
		volatile LONG overlayVisible;  // the overlay is open

		// Written by both: the page's "Pop out amp" button and the amp window's own close button. The VST host seeds
		// it from its saved choice when it starts.
		volatile LONG popoutWanted;
		volatile LONG quitRequested;   // overlay -> VST host: close (saves the plugin's settings)

		// Plugin switch without restarting the host: the overlay writes loadPath, then bumps loadRequest. The host
		// saves the current plugin's settings, swaps the plugin in place (the audio link and the popout stay up),
		// then sets loadDone to the request it handled. A plugin that fails to load leaves the previous one running
		// and its reason in loadError.
		wchar_t loadPath[kPathChars];
		volatile LONG loadRequest;
		volatile LONG loadDone;
		volatile LONG loading;          // host: 1 while a plugin loads (its beat pauses meanwhile)
		char loadError[128];            // UTF-8, empty when the last switch worked
		wchar_t pluginPath[kPathChars]; // host: the plugin it runs now
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
