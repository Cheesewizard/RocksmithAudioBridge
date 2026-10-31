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
	// v6: undo and redo of the board (UNDO_COMMAND, REDO_COMMAND, NEW_TONE_COMMAND, undoSeq, undoLabel, redoLabel).
	// v5: the automatic input calibration (autoTrimSeq, autoTrimCentidB, autoPlayingTenths).
	// v4: the player's input trim (inputTrimCentidB).
	// v3: song tones (the game's song and tone keys, the song's assignments, its prepared tones).
	// v2: the stream's measured load (loadSeq, blockFrames, loadPermille, peakPermille).
	constexpr wchar_t MAPPING_NAME[] = L"Local\\RocksmithAudioBridge.Stompbox.v6";
	constexpr uint32_t MAGIC = 0x58425453;   // "STBX"
	constexpr uint32_t VERSION = 6;
	constexpr int COMMAND_CHARS = 4096;
	constexpr int CATALOG_CHARS = 262144;     // every pedal with its parameters; File parameters list their files
	constexpr int PROGRAM_CHARS = 65536;
	constexpr int PRESET_CHARS = 32768;
	constexpr int ERROR_CHARS = 256;
	constexpr int KEY_CHARS = 128;
	constexpr int SONG_TONE_CHARS = 16384;
	constexpr int LABEL_CHARS = 64;
	constexpr char CHAIN_NAME[] = "Main";     // the one chain the board edits (SetGlobalChain in StompboxService)

	enum class EngineState : LONG { Off = 0, Loading = 1, Ready = 2, Failed = 3 };
	// The current song's tones: none assigned, being loaded into engines of their own, or ready to switch.
	enum class SongState : LONG { None = 0, Preparing = 1, Ready = 2, Failed = 3 };

	// The page's own command (not Stompbox's): SongTone "<song key>" "<tone key>" "<saved tone>" assigns a saved tone
	// to one of a song's tones (its manifest's Tone_Base / Tone_A..D key); an empty saved tone clears it.
	constexpr char SONG_TONE_COMMAND[] = "SongTone";
	// The page's own command: EditPedal "<pedal id>" opens the host's pedal window on that pedal of the board (or
	// brings it to the front); EditPedal "" closes it. The window's edits come back as the program, like the page's.
	constexpr char EDIT_PEDAL_COMMAND[] = "EditPedal";
	// The page's own commands (v6): Undo takes back the board's last change (undoLabel names it), Redo does it again.
	// The board's changes are SetChain on the Main chain and SetParam on its pedals, from the page or the host's pedal
	// window; a change and the settings that come with it (a pedal added with its starting settings) are one step,
	// and a knob turned again and again is one step. LoadPreset and NewTone clear the history.
	constexpr char UNDO_COMMAND[] = "Undo";
	constexpr char REDO_COMMAND[] = "Redo";
	// The page's own command (v6): NewTone empties the board as a new tone; unlike SetChain Main with no pedals (the
	// board cleared, one undo step), the history goes with it.
	constexpr char NEW_TONE_COMMAND[] = "NewTone";

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

		// Written by the host once a second (v2): how long the last second's audio blocks took on this PC, in
		// thousandths of the block period (1000 = the whole period: that block was late). Bumped after the fields.
		// The median is the tone's share of real time, nearly the same at any block size, so it is what a tone is
		// judged by against the base game (480-frame blocks, CableGamePeriod.hpp). The slowest blocks are what a
		// small block size (ASIO's 128 or 64, the bonus) risks on this PC.
		volatile LONG loadSeq;
		volatile LONG blockFrames;     // the stream's block size those were measured at
		volatile LONG medianPermille;  // the typical block
		volatile LONG loadPermille;    // the 99th percentile block
		volatile LONG peakPermille;    // the slowest block

		// Written by the overlay (v3), under gameSeq (odd while it writes): the song playing (VstHostLink's song key,
		// empty outside songs) and the game's current tone key (GameTones::CurrentToneKey).
		volatile LONG gameSeq;
		char songKey[KEY_CHARS];
		char gameToneKey[KEY_CHARS];

		// Written by the host (v3).
		volatile LONG songState;       // SongState
		volatile LONG songToneSeq;
		char songTones[SONG_TONE_CHARS];   // "<tone key>\t<saved tone>" lines: the current song's assignments
		char playingTone[KEY_CHARS];   // the saved tone the song plays now; empty: the board's own tone

		// Written by the overlay (v4): the input trim before every Tone designer chain, in hundredths of a dB, set by
		// the page's input calibration so the guitar drives the captures at the level they were recorded at.
		volatile LONG inputTrimCentidB;

		// Written by the host (v5): the input trim the player's own playing calls for (InputLevel: their hardest
		// playing brought to the -12 dBFS the starter tones are balanced for), in hundredths of a dB, once it has heard
		// enough playing. Bumped after the fields when it moves by a dB or more. The page applies it while its input is
		// on Automatic.
		volatile LONG autoTrimSeq;
		volatile LONG autoTrimCentidB;
		volatile LONG autoPlayingTenths;   // the playing it is from, in tenths of a second

		// Written by the host (v6), under undoSeq (odd while it writes, WriteUndo): what Undo and Redo would do, for
		// the Edit menu ("add Delay", "Gain on Screamer"). UTF-8; empty when there is nothing to undo or redo.
		volatile LONG undoSeq;
		char undoLabel[LABEL_CHARS];
		char redoLabel[LABEL_CHARS];
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

	// Overlay side: the song and tone keys under gameSeq. Long keys are cut (they are compared, never shown in full).
	inline void WriteGame(Shared& shared, const std::string& songKey, const std::string& toneKey)
	{
		InterlockedIncrement(&shared.gameSeq);   // odd: being written
		strncpy_s(shared.songKey, songKey.c_str(), _TRUNCATE);
		strncpy_s(shared.gameToneKey, toneKey.c_str(), _TRUNCATE);
		InterlockedIncrement(&shared.gameSeq);
	}

	// Host side. True with both keys when a complete copy newer than `seen` was read.
	inline bool ReadGame(Shared& shared, LONG& seen, std::string& songKey, std::string& toneKey)
	{
		const LONG before = Load(&shared.gameSeq);
		if (before == seen || (before & 1) != 0) return false;
		std::string song(shared.songKey, strnlen(shared.songKey, KEY_CHARS));
		std::string tone(shared.gameToneKey, strnlen(shared.gameToneKey, KEY_CHARS));
		MemoryBarrier();
		if (Load(&shared.gameSeq) != before) return false;
		songKey = std::move(song);
		toneKey = std::move(tone);
		seen = before;
		return true;
	}

	// Host side: the undo and redo labels under undoSeq. Long labels are cut.
	inline void WriteUndo(Shared& shared, const std::string& undo, const std::string& redo)
	{
		InterlockedIncrement(&shared.undoSeq);   // odd: being written
		strncpy_s(shared.undoLabel, undo.c_str(), _TRUNCATE);
		strncpy_s(shared.redoLabel, redo.c_str(), _TRUNCATE);
		InterlockedIncrement(&shared.undoSeq);
	}

	// Overlay side. True with both labels when a complete copy newer than `seen` was read.
	inline bool ReadUndo(Shared& shared, LONG& seen, std::string& undo, std::string& redo)
	{
		const LONG before = Load(&shared.undoSeq);
		if (before == seen || (before & 1) != 0) return false;
		std::string undoCopy(shared.undoLabel, strnlen(shared.undoLabel, LABEL_CHARS));
		std::string redoCopy(shared.redoLabel, strnlen(shared.redoLabel, LABEL_CHARS));
		MemoryBarrier();
		if (Load(&shared.undoSeq) != before) return false;
		undo = std::move(undoCopy);
		redo = std::move(redoCopy);
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

	// Stompbox's own defaults suit a demo, not a guitar: a ConvoReverb starts with no dry signal at all, a Reverb
	// 50% wet, a Delay at 50% mix and feedback, a Flanger at full depth. Added at those, a pedal pushes the guitar
	// far back at once. A pedal added from the browser, or swapped in from the pedal window's picker
	// (VstHost PedalEditor), starts here instead; presets keep their own values.
	struct StartingSetting
	{
		const char* kind;
		const char* parameter;
		const char* value;
	};

	constexpr StartingSetting STARTING_SETTINGS[] = {
		{ "ConvoReverb", "Dry", "1" }, { "ConvoReverb", "Wet", "0.2" },
		{ "Reverb", "Size", "0.4" }, { "Reverb", "Decay", "3" }, { "Reverb", "Blend", "15" },
		{ "Delay", "Delay", "380" }, { "Delay", "Mix", "0.25" }, { "Delay", "FBack", "0.3" },
		{ "Chorus", "Rate", "0.8" }, { "Chorus", "Depth", "0.05" },
		{ "Flanger", "Depth", "0.5" }, { "Flanger", "FBack", "0.3" },
		{ "Phaser", "Depth", "0.3" }, { "Phaser", "FBack", "0.3" },
		{ "Vibrato", "Depth", "0.2" },
		{ "Tremolo", "Depth", "0.4" },
		{ "Boost", "Gain", "4" },
		{ "Screamer", "Drive", "0.3" }, { "Screamer", "Tone", "700" }, { "Screamer", "Level", "0" },
		{ "Compressor", "Thresh", "-24" }, { "Compressor", "Ratio", "3" }, { "Compressor", "Attack", "10" }, { "Compressor", "Release", "120" }, { "Compressor", "Blend", "0.8" },
	};
}
