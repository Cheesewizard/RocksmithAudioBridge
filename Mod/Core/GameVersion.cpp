#include "GameVersion.hpp"

#include <Windows.h>

namespace AudioBridge::GameVersion
{
	namespace
	{
		constexpr DWORD SEPTEMBER_2022_CHECKSUM = 0x00B13D7C;
		constexpr DWORD LEARN_AND_PLAY_DECEMBER_2024_CHECKSUM = 0x0176EC34;

		std::uintptr_t ModuleBase()
		{
			return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		}

		// The CheckSum field of the main module's in-memory PE header: safe under the loader lock, so namespace-scope
		// initialisers can use it.
		DWORD HeaderChecksum()
		{
			const std::uintptr_t base = ModuleBase();
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
			return nt->OptionalHeader.CheckSum;
		}
	}

	bool IsLearnAndPlay()
	{
		static const bool learnAndPlay = HeaderChecksum() == LEARN_AND_PLAY_DECEMBER_2024_CHECKSUM;
		return learnAndPlay;
	}

	bool IsKnownBuild()
	{
		static const bool known = HeaderChecksum() == SEPTEMBER_2022_CHECKSUM
			|| HeaderChecksum() == LEARN_AND_PLAY_DECEMBER_2024_CHECKSUM;
		return known;
	}

	// September 2022 addresses are absolute (that exe isn't relocated); Learn & Play ones are RVAs.
	std::uintptr_t Address(std::uintptr_t september, std::uintptr_t learnAndPlayRva)
	{
		return IsLearnAndPlay() ? ModuleBase() + learnAndPlayRva : september;
	}

	// 0 when the address can't be trusted: an unknown build, or a September exe Windows relocated (Mandatory ASLR loads
	// it away from 0x400000, so its absolute address points into other code). The cable install then leaves the game's
	// input stock instead of patching unrelated bytes.
	std::uintptr_t PortAudioOpenStream()
	{
		constexpr std::uintptr_t SEPTEMBER_IMAGE_BASE = 0x00400000;
		if (!IsKnownBuild() || (!IsLearnAndPlay() && ModuleBase() != SEPTEMBER_IMAGE_BASE)) return 0;
		return Address(0x00E70550, 0x00A6F580);
	}

	std::uintptr_t Rva(std::uintptr_t septemberRva, std::uintptr_t learnAndPlayRva)
	{
		return IsLearnAndPlay() ? learnAndPlayRva : septemberRva;
	}
}
