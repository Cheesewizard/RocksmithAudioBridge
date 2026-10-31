#pragma once

// A picture of a plugin's own window, for the Guitar Tone page's Inspector (Mod/ToneDesigner/Ui). The VST host
// (VstHost/Source) captures the editor of the slot the Inspector shows (VstHostControl::Shared::inspectSlot) a few
// times a second and writes it here; the overlay uploads it to a texture and draws it, and a click opens the real
// window. Picture only: no input goes back. Separate from VstHostControl so the chain's contract is untouched.
//
// Plain named shared memory, the same layout in 32 and 64 bit (only 32-bit fields). One frame, under a sequence
// lock: the host makes `frameSeq` odd, writes the pixels and size, then makes it even again; the overlay copies only
// while it is even and unchanged across the copy.

#include <Windows.h>
#include <cstdint>

namespace PluginPicture
{
	constexpr wchar_t MAPPING_NAME[] = L"Local\\RocksmithAudioBridge.VstHost.Picture.v1";
	constexpr uint32_t MAGIC = 0x43495052;   // "RPIC"
	constexpr uint32_t VERSION = 1;
	// The largest picture: the editor is scaled down (aspect kept) to fit. 960 x 640 BGRA is 2.4 MB.
	constexpr int MAX_WIDTH = 960;
	constexpr int MAX_HEIGHT = 640;

	enum class State : LONG
	{
		None = 0,       // no slot inspected, or nothing captured yet
		Ready = 1,      // a frame is here
		NoEditor = 2,   // the plugin has no window of its own
		Failed = 3,     // the window could not be captured (`why` says why)
	};

	struct Shared
	{
		uint32_t magic;
		uint32_t version;

		// Written by the host.
		volatile LONG state;        // State
		volatile LONG slot;         // the chain slot the frame shows, -1 = none
		volatile LONG frameSeq;     // sequence lock over width, height, sourceWidth, sourceHeight and pixels
		volatile LONG width;        // the picture's size, at most MAX_WIDTH x MAX_HEIGHT
		volatile LONG height;
		volatile LONG sourceWidth;  // the editor window's own size, for the aspect and a "shown at N %" note
		volatile LONG sourceHeight;
		char why[128];              // UTF-8, when Failed
		uint32_t pixels[MAX_WIDTH * MAX_HEIGHT];   // BGRA (D3DFMT_A8R8G8B8 order), rows of `width`, alpha 255
	};

	// Creates or opens the block. Returns nullptr on failure; `mapping` must stay open while it is used.
	inline Shared* Open(HANDLE& mapping)
	{
		mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), MAPPING_NAME);
		if (!mapping) return nullptr;
		auto* shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
		if (!shared) { CloseHandle(mapping); mapping = nullptr; return nullptr; }
		if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&shared->magic), static_cast<LONG>(MAGIC), 0) == 0)
		{
			shared->version = VERSION;
			shared->slot = -1;
		}
		return shared;
	}
}
