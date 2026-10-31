#include "Hooking.hpp"

#include "GameVersion.hpp"
#include "MemUtil.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace AudioBridge::Hooking
{
	namespace
	{
		// Learn & Play: Detours' VirtualProtect on game code is refused by the exe's DRM wrapper, after ~5 s each.
		// Ported from 4.x's MemUtil::DetourFunction fallback: the same trampoline Detours builds (whole instructions
		// covering the 5-byte jump, copied by Detours' instruction copier so relative branches are re-aimed, then a jump
		// back), with protection changed through upstream's direct-syscall MemUtil::HookedVirtualProtect.
		PBYTE DetourWithSyscallProtect(PBYTE target, PBYTE detour)
		{
			constexpr std::size_t JMP_SIZE = 5;
			constexpr std::size_t TRAMPOLINE_SIZE = 64;
			PBYTE trampoline = static_cast<PBYTE>(VirtualAlloc(nullptr, TRAMPOLINE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (trampoline == nullptr) return nullptr;

			PBYTE source = target;
			PBYTE copy = trampoline;
			while (static_cast<std::size_t>(source - target) < JMP_SIZE)
			{
				// A return inside the stolen bytes means the function is too short to hold the jump.
				if (*source == 0xC3 || *source == 0xC2)
				{
					VirtualFree(trampoline, 0, MEM_RELEASE);
					return nullptr;
				}
				PBYTE branchTarget = nullptr;
				PBYTE next = DetourCopyInstruction(copy, source, &branchTarget);
				// A branch back into the bytes being replaced cannot be relocated.
				if (next == nullptr || next <= source || (branchTarget > target && branchTarget < target + JMP_SIZE))
				{
					VirtualFree(trampoline, 0, MEM_RELEASE);
					return nullptr;
				}
				copy += next - source;
				source = next;
			}

			const std::size_t stolen = static_cast<std::size_t>(source - target);
			if (copy + JMP_SIZE > trampoline + TRAMPOLINE_SIZE)
			{
				VirtualFree(trampoline, 0, MEM_RELEASE);
				return nullptr;
			}
			copy[0] = 0xE9;
			*reinterpret_cast<std::int32_t*>(copy + 1) = static_cast<std::int32_t>(source - (copy + JMP_SIZE));
			FlushInstructionCache(GetCurrentProcess(), trampoline, TRAMPOLINE_SIZE);

			ULONG oldProtection = 0;
			ULONG unused = 0;
			if (!NT_SUCCESS(MemUtil::HookedVirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, oldProtection)))
			{
				VirtualFree(trampoline, 0, MEM_RELEASE);
				return nullptr;
			}
			target[0] = 0xE9;
			*reinterpret_cast<std::int32_t*>(target + 1) = static_cast<std::int32_t>(detour - (target + JMP_SIZE));
			for (std::size_t i = JMP_SIZE; i < stolen; i++) target[i] = 0xCC;   // same padding Detours leaves
			FlushInstructionCache(GetCurrentProcess(), target, stolen);
			MemUtil::HookedVirtualProtect(target, stolen, oldProtection, unused);
			return trampoline;
		}
	}

	// September 2022 (and any other exe): Detours, as 4.x. Learn & Play goes straight to the syscall path, as 4.x did:
	// Detours' refused VirtualProtect calls would cost ~5 s each, which pushed the cable install past the game's own
	// audio start there ("No audio output device").
	PBYTE DetourFunction(PBYTE target, PBYTE detour)
	{
		if (target == nullptr || detour == nullptr) return nullptr;
		if (!GameVersion::IsLearnAndPlay())
		{
			if (PBYTE trampoline = ::DetourFunction(target, detour)) return trampoline;
		}
		return DetourWithSyscallProtect(target, detour);
	}

	namespace
	{
		bool CopyCodeBytes(std::uintptr_t address, BYTE* out, std::size_t count)
		{
			__try
			{
				std::memcpy(out, reinterpret_cast<const void*>(address), count);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		std::string ModuleNameFor(std::uintptr_t address)
		{
			HMODULE module = nullptr;
			char path[MAX_PATH] = {};
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(address), &module)
				|| GetModuleFileNameA(module, path, MAX_PATH) == 0)
				return "no module (allocated memory)";
			const char* name = std::strrchr(path, '\\');
			return name ? name + 1 : path;
		}
	}

	// Ported from 4.x's MemUtil::DescribeCodeBytes (upstream's MemUtil has none).
	std::string DescribeCodeBytes(std::uintptr_t address, std::size_t count)
	{
		BYTE bytes[16] = {};
		count = (std::min)(count, sizeof(bytes));
		if (!CopyCodeBytes(address, bytes, count)) return "unreadable";

		std::ostringstream text;
		text << std::hex << std::uppercase << std::setfill('0');
		for (std::size_t i = 0; i < count; ++i)
			text << (i == 0 ? "" : " ") << std::setw(2) << static_cast<int>(bytes[i]);

		std::uintptr_t target = 0;
		if (count >= 5 && bytes[0] == 0xE9)
		{
			std::int32_t relative = 0;
			std::memcpy(&relative, bytes + 1, sizeof(relative));
			target = address + 5 + relative;
		}
		else if (count >= 6 && bytes[0] == 0xFF && bytes[1] == 0x25)
		{
			std::uint32_t slot = 0;
			std::memcpy(&slot, bytes + 2, sizeof(slot));
			if (!CopyCodeBytes(slot, reinterpret_cast<BYTE*>(&target), sizeof(target))) target = 0;
		}
		if (target != 0)
			text << " (already hooked: jumps to 0x" << std::setw(8) << target << " in " << ModuleNameFor(target) << ")";
		return text.str();
	}
}
