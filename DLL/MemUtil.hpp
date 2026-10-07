#pragma once

#include "winternl.h"

namespace MemUtil {
	bool bCompare(const BYTE* pData, const byte* bMask, const char* szMask);
	bool PatchAdr(VersioningStruct<uintptr_t>& address, LPVOID changeToMake, size_t len, bool addBaseHandle = false);
	bool PatchAdr(LPVOID address, LPVOID changeToMake, size_t len);
	bool PatchAdr(uintptr_t address, LPVOID changeToMake, size_t len, bool addBaseHandle);
	bool PlaceHook(VersioningStruct<uintptr_t>& hookSpot, void* ourFunct, int len, bool addBaseHandle = false);
	bool PlaceHook(void* hookSpot, void* ourFunct, int len);
	PBYTE TrampHook(PBYTE src, PBYTE dst, unsigned int len);
	bool IsBadReadPtr(void* pointer);
	uintptr_t FindDMAAddy(uintptr_t ptr, const std::vector<unsigned int>& offsets, bool safe = false);
	uintptr_t ReadPtr(uintptr_t adr);
	template <typename T>
	bool SetStaticValue(uintptr_t staticValue, T data, unsigned int lengthOfData);

	template <typename T>
	T FindPattern(uint32_t address, size_t size, PBYTE pattern, char* mask);

	template <typename T>
	T ReadValue(uintptr_t adr, bool addBaseHandle);

	NTSTATUS HookedVirtualProtect(LPVOID address, SIZE_T len, ULONG newProtection, ULONG& oldProtection);
	NTSTATUS HookedQueryVirtualMemory(LPVOID address, PMEMORY_BASIC_INFORMATION memoryBuffer, SIZE_T dwLength);

	uint32_t GetTextSectionAddress();
	uint32_t GetTextSectionLength();
	void CheckMemoryProtection(void* address);

	// The first `count` bytes at `address` as hex, for logging a failed prologue check. A leading E9 (jmp rel32) or
	// FF 25 (jmp [abs]) means another hook already sits there, so the jump target's module is named too.
	std::string DescribeCodeBytes(uintptr_t address, size_t count);
};

template <typename T>
/// <summary>
/// Scans memory chunk for a pattern.
/// </summary>
/// <typeparam name="T"> - Type of value to return</typeparam>
/// <param name="address"> - Address to start the search at.</param>
/// <param name="size"> - Size of search.</param>
/// <param name="pattern"> - Pattern to look for.</param>
/// <param name="mask"> - Mask of what bytes we know (notated with an "x") and what bytes we dont (notated with a "?").</param>
/// <returns>Value if found or NULL if not.</returns>
T MemUtil::FindPattern(uint32_t address, size_t size, PBYTE pattern, char* mask) {
	for (uint32_t i = 0; i < size; i++)
		if (bCompare((PBYTE)(address + i), pattern, mask))
			return (T)(address + i);

	return NULL;
}

template <typename T>
/// <summary>
/// Sets a static value in the executable, utilizing VirtualProtect.
/// </summary>
/// <typeparam name="T"> - Type of the data.</typeparam>
/// <param name="staticValue"> - Address of the static value.</param>
/// <param name="data"> - Data we should change the staticValue to.</param>
/// <param name="lengthOfData"> - Length of the data (needed for VirtualProtect).</param>
/// <returns>Successfully able to set the value.</returns>

bool MemUtil::SetStaticValue(uintptr_t staticValue, T data, unsigned int lengthOfData) {
	DWORD dwOldProt, dwDummy;

	// Change memory protection to allow writing
	NTSTATUS status = HookedVirtualProtect((LPVOID)staticValue, lengthOfData, PAGE_EXECUTE_READWRITE, dwOldProt);
	if (!NT_SUCCESS(status)) {
		return false;
	}

	// Write the data
	*(T*)staticValue = data;

	// Restore original protection
	status = HookedVirtualProtect((LPVOID)staticValue, lengthOfData, dwOldProt, dwDummy);
	if (!NT_SUCCESS(status)) {
		return false;
	}

	return true;
}

template <typename T>
T MemUtil::ReadValue(uintptr_t address, bool addBaseHandle) {
	if (address == NULL)
		return NULL;

	uintptr_t addr = address + (addBaseHandle ? Offsets::baseHandle : 0);

	return *(T*)addr;
}