#pragma once

// Shared state between the overlay's Tone designer page (DLL/ToneDesigner/StompboxLink.cpp) and the VST host's
// Stompbox engine (VstHost/Source/StompboxService.cpp). Plain named shared memory, separate from VstHostControl so
// the VST rig's contract is untouched; the layout is the same in 32 and 64 bit (only 32-bit fields and chars).
//
// The overlay says whether the Tone designer plays (`wanted`) and sends one Stompbox command line at a time
// (Stompbox's own text protocol: "SetChain Main NoiseGate NAM Cabinet", "SetParam NAM Model \"Crunch.nam\"",
// "SavePreset Classic", ...). The host runs it on the engine and republishes what the page shows: the catalogue of
// pedals (once per engine load), the current program (after every command) and the preset list.
//
// Text blocks use a sequence lock: the host makes the sequence odd, writes, then makes it even again; a reader copies
// only while it is even and unchanged across the copy.

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace StompboxControl
{
	constexpr wchar_t MAPPING_NAME[] = L"Local\\RocksmithAudioBridge.Stompbox.v1";
	constexpr uint32_t MAGIC = 0x58425453;   // "STBX"
	constexpr uint32_t VERSION = 1;
	constexpr int COMMAND_CHARS = 4096;
	constexpr int CATALOG_CHARS = 262144;     // every pedal with its parameters; File parameters list their files
	constexpr int PROGRAM_CHARS = 65536;
	constexpr int PRESET_CHARS = 32768;
	constexpr int ERROR_CHARS = 256;
	constexpr char CHAIN_NAME[] = "Main";     // the one chain the board edits (SetGlobalChain in StompboxService)

	enum class EngineState : LONG { Off = 0, Loading = 1, Ready = 2, Failed = 3 };

	// Catalogue lines, tab separated (text fields never contain tabs or line breaks):
	//   P <name> <background #rrggbb> <foreground #rrggbb> <description>
	//   A <parameter> <type 0-6, StompParameterType> <min> <max> <default> <range power> <display format> <advanced 0/1> <output 0/1> <description>
	//   E <choice>   (one per Enum or File choice of the parameter above)

	struct Shared
	{
		uint32_t magic;
		uint32_t version;

		// Written by the overlay.
		volatile LONG wanted;          // 1: the Tone designer plays (the host loads Stompbox and routes the stream to it)
		volatile LONG commandSeq;      // bumped after `command` is written
		char command[COMMAND_CHARS];   // UTF-8, one Stompbox command line

		// Written by the host.
		volatile LONG hostPid;
		volatile LONG engineState;     // EngineState
		volatile LONG playing;         // the stream goes through Stompbox now
		volatile LONG presetLoading;   // Stompbox is loading a preset or a model on its own thread
		volatile LONG commandDone;     // the commandSeq the host has run
		char error[ERROR_CHARS];       // UTF-8, the last problem; empty when the last command worked
		volatile LONG catalogSeq;
		char catalog[CATALOG_CHARS];
		volatile LONG programSeq;
		char program[PROGRAM_CHARS];   // Stompbox's DumpProgram text
		volatile LONG presetSeq;
		char presets[PRESET_CHARS];    // preset names, one per line; the first line is the current preset
	};

	inline LONG Load(volatile LONG* value) { return InterlockedCompareExchange(value, 0, 0); }

	// Creates or opens the block. Returns nullptr on failure; `mapping` must stay open while it is used.
	inline Shared* Open(HANDLE& mapping)
	{
		mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), MAPPING_NAME);
		if (!mapping) return nullptr;
		auto* shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
		if (!shared) { CloseHandle(mapping); mapping = nullptr; return nullptr; }
		if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&shared->magic), static_cast<LONG>(MAGIC), 0) == 0)
			shared->version = VERSION;
		return shared;
	}

	// Host side. False when the text does not fit (nothing is written then).
	inline bool WriteText(volatile LONG* sequence, char* destination, size_t capacity, const std::string& text)
	{
		if (text.size() + 1 > capacity) return false;
		InterlockedIncrement(sequence);   // odd: being written
		std::memcpy(destination, text.c_str(), text.size() + 1);
		InterlockedIncrement(sequence);   // even: complete
		return true;
	}

	// Overlay side. True with `text` when a complete copy newer than `seen` was read; `seen` is updated.
	inline bool ReadText(volatile LONG* sequence, const char* source, size_t capacity, LONG& seen, std::string& text)
	{
		const LONG before = Load(sequence);
		if (before == seen || (before & 1) != 0) return false;
		std::string copy(source, strnlen(source, capacity));
		MemoryBarrier();
		if (Load(sequence) != before) return false;
		text = std::move(copy);
		seen = before;
		return true;
	}

	// One word of a command line, quoted the way Stompbox reads it (std::quoted).
	inline std::string Quote(const std::string& word)
	{
		std::string quoted = "\"";
		for (const char c : word)
		{
			if (c == '"' || c == '\\') quoted += '\\';
			quoted += c;
		}
		return quoted + "\"";
	}
}
