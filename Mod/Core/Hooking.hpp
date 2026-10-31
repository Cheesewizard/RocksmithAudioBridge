#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

namespace AudioBridge::Hooking
{
	// Detours target to detour; returns the trampoline, or nullptr. Detours 1.5's DetourFunction, falling back (and on
	// Learn & Play going straight) to the same trampoline built with direct-syscall protection changes: Learn & Play's
	// wrapper refuses VirtualProtect on game code after ~5 s each. Every Audio Bridge detour goes through here.
	PBYTE DetourFunction(PBYTE target, PBYTE detour);

	// The first `count` (at most 16) bytes at `address` as hex, for "does not match the expected build" logs; a jump
	// there (another mod's hook) is named with its target and module. "unreadable" when the memory can't be read.
	std::string DescribeCodeBytes(std::uintptr_t address, std::size_t count);
}
