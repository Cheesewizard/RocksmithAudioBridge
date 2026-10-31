#pragma once

#include <Windows.h>

#include <cstdint>

// The game reads and hooks Drop Pedal's shared code (DropPedalHooks, Speaker Mode's chart sync) needs. The 4.x DLL
// (DLL/DropPedal/GameTuning.cpp) and the framework mod (Mod/DropPedal/GameTuning.cpp) each implement them on their
// own core.
namespace AudioBridge::GameTuning
{
	// Player 1's note-detection reference frequency (A440 = 440) and where it lives. False while it isn't readable.
	bool TryGetTrueTuning(float& trueTuning, std::uintptr_t& address);
	// The loaded arrangement's per-string offsets (low E first). False while no arrangement is loaded.
	bool TryGetArrangementTuning(int (&stringOffsets)[6]);
	// The open pre-song tuner's per-string offsets. A failed read gives offsets far outside -24..24 (69), which callers
	// range-check.
	bool TryGetTunerTuning(int (&stringOffsets)[6]);
	// The function that turns an arrangement's cent offset into the detection reference; 0 when this game version has
	// no known address.
	std::uintptr_t ReferenceBuilderAddress();
	// Detours target to detour; returns the trampoline, or nullptr.
	PBYTE DetourFunction(PBYTE target, PBYTE detour);
}
