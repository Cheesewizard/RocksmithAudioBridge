#pragma once

#include <windows.h>
#include <cstdint>

// Fixed game addresses for the two supported Rocksmith 2014 builds.
//
// September 2022 Remastered has no ASLR, so its addresses are absolute VAs. Learn & Play December 2024 is relocated
// on every launch, so its addresses are RVAs added to the running module base. Every other exe gets the September
// value, which is exactly what the code did before Learn & Play support.
//
// The build is read from the CheckSum field of the main module's in-memory PE header (the same value
// VersioningStruct matches), not from the exe file. That keeps these safe in namespace-scope initialisers, which run
// under the loader lock, and needs no other translation unit to be initialised first.
//
// Header-only so the Note by Note probe, which compiles host sources without VersioningStruct.cpp, links too.
// Learn & Play values were ported from the September build's addresses and checked against the Learn & Play exe.
namespace GameAddressDetail
{
	inline bool IsLearnAndPlayDecember2024()
	{
		const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
		constexpr DWORD LEARN_AND_PLAY_DECEMBER_2024_CHECKSUM = 0x0176EC34;
		return nt->OptionalHeader.CheckSum == LEARN_AND_PLAY_DECEMBER_2024_CHECKSUM;
	}
}

// Absolute address of a game location in the running build.
inline uintptr_t GameAddress(uintptr_t septemberVa, uintptr_t learnAndPlayRva)
{
	return GameAddressDetail::IsLearnAndPlayDecember2024()
		? reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + learnAndPlayRva
		: septemberVa;
}

// The same choice for code that already works in RVAs (so a relocated September exe still resolves): the RVA for the
// running build, to be added to the module base by the caller.
inline uintptr_t GameRva(uintptr_t septemberRva, uintptr_t learnAndPlayRva)
{
	return GameAddressDetail::IsLearnAndPlayDecember2024() ? learnAndPlayRva : septemberRva;
}
