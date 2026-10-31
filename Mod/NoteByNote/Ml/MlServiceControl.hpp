#pragma once

#include <windows.h>
#include <cstdint>

namespace MlServiceControl
{
	enum class State : uint32_t { Starting = 1, Waiting = 3, Stopped = 4, Failed = 5, Restarting = 6 };

	// Versioned cross-bitness status; only the game writes, under the sequence lock.
	struct Status
	{
		uint32_t version;
		volatile LONG sequence;
		uint64_t tick;
		uint32_t processId;
		State state;
		uint32_t error;
		uint32_t reserved;
	};
	static_assert(sizeof(Status) == 32, "ML control ABI must remain 32 bytes");
}
