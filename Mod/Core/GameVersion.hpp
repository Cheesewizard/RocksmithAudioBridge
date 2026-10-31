#pragma once

#include <cstdint>

// Which Rocksmith build is running, and Audio Bridge's own game addresses that upstream's Offsets doesn't carry.
namespace AudioBridge::GameVersion
{
	// The December 2024 Learn & Play exe (identified by its PE checksum); otherwise the September 2022 one.
	bool IsLearnAndPlay();

	// The running exe is exactly one of the two builds Audio Bridge's addresses are mapped for (4.x's
	// IsExactGameBuild for either): code patching fixed addresses stays off on any other exe.
	bool IsKnownBuild();

	// PortAudio's Pa_OpenStream, used by the game's input (SoundInputPortAudio).
	// 0 on an unknown build or a relocated September exe.
	std::uintptr_t PortAudioOpenStream();

	// An absolute address for the running build (4.x's GameAddress): September 2022 addresses are absolute VAs (that exe
	// isn't relocated), Learn & Play ones RVAs added to the module base. Safe in namespace-scope initialisers.
	std::uintptr_t Address(std::uintptr_t septemberVa, std::uintptr_t learnAndPlayRva);

	// An RVA for the running build (4.x's GameRva): the caller adds the module base, so a relocated September exe
	// (Mandatory ASLR) still resolves.
	std::uintptr_t Rva(std::uintptr_t septemberRva, std::uintptr_t learnAndPlayRva);
}
