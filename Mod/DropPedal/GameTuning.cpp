#include "GameTuning.hpp"

#include "MemUtil.hpp"
#include "Offsets.hpp"
#include "SongTuning.hpp"
#include "VersioningStruct.h"
#include "../Core/Hooking.hpp"

#include <cmath>

// The framework build's side of Drop Pedal's shared game access, on upstream RSMods' core. The 4.x DLL has its own
// (DLL/DropPedal/GameTuning.cpp).
namespace AudioBridge::GameTuning
{
	namespace
	{
		// Converts an arrangement's cent offset to the note-detection reference, 440 * 2^(cents / 1200), and stamps it
		// at [detection + 0x135C]. Not in upstream's offsets. September 2022, Learn & Play.
		std::uintptr_t ReferenceBuilderForThisGame()
		{
			VersioningStruct<std::uintptr_t> builder({ 0x004DCCB0, Offsets::baseHandle + 0x000DD930 });
			return builder.GetValue();
		}
	}

	bool TryGetTrueTuning(float& trueTuning, std::uintptr_t& address)
	{
		address = MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_trueTuning, Offsets::ptr_trueTuningOffsets, true);
		if (address == 0) return false;

		const float value = *reinterpret_cast<const float*>(address);
		if (!std::isfinite(value) || value <= 0.0f)
		{
			address = 0;
			return false;
		}
		trueTuning = value;
		return true;
	}

	// Upstream's SongTuning::GetCurrentTuning can't tell a failed read from E standard, so the chain is walked here.
	bool TryGetArrangementTuning(int (&stringOffsets)[6])
	{
		const std::uintptr_t address = MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_tuning, Offsets::ptr_tuningOffsets, true);
		if (address == 0) return false;

		const auto* tuning = reinterpret_cast<const Tuning*>(address);
		const byte strings[6] = { tuning->lowE, tuning->strA, tuning->strD, tuning->strG, tuning->strB, tuning->highE };
		for (int string = 0; string < 6; string++)
			stringOffsets[string] = static_cast<int>(static_cast<signed char>(strings[string]));
		return true;
	}

	// Upstream reads the open tuner through its tuner-tick hook, and returns 69 per string when it can't.
	bool TryGetTunerTuning(int (&stringOffsets)[6])
	{
		const Tuning tuning = SongTuning::GetTuningAtTuner(false);

		const byte strings[6] = { tuning.lowE, tuning.strA, tuning.strD, tuning.strG, tuning.strB, tuning.highE };
		for (int string = 0; string < 6; string++)
			stringOffsets[string] = static_cast<int>(static_cast<signed char>(strings[string]));
		return true;
	}

	std::uintptr_t ReferenceBuilderAddress()
	{
		static const std::uintptr_t address = ReferenceBuilderForThisGame();
		return address;
	}

	PBYTE DetourFunction(PBYTE target, PBYTE detour)
	{
		return AudioBridge::Hooking::DetourFunction(target, detour);
	}
}
