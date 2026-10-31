#pragma once

#include <Windows.h>

#include <string>

namespace Audio::SongShift
{
	// What the Wwise music hook needs from whoever owns Speaker Mode (the 4.x DLL's DropPedal, or the framework mod).
	// IsEnabled, GetShiftSemitones and Disable also run on Wwise's codec and decoder threads: lock-free, no logging.
	class ISpeakerModeController
	{
	public:
		virtual ~ISpeakerModeController() = default;

		virtual bool IsEnabled() const = 0;
		// The player's pedal shift; the song moves the opposite way.
		virtual int GetShiftSemitones() const = 0;
		virtual void Disable() = 0;
		// MainThread: reads the selected chart's tuning and sets the shift from it. True once it has.
		virtual bool TrySynchronizeTarget(const std::string& songKey) = 0;
	};

	// Detours target to detour and returns the trampoline, or nullptr. Each build passes its own (Learn & Play needs a
	// fallback Detours alone doesn't have).
	using DetourFunction_t = PBYTE(*)(PBYTE target, PBYTE detour);
}
