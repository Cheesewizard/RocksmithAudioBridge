#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace Audio::AsioBufferState
{
	inline std::atomic<uint64_t> format{ 0 };
	inline std::atomic<uint64_t> lastPacket{ 0 };

	inline void Configure(uint32_t frames, uint32_t sampleRate)
	{
		lastPacket.store(0);
		format.store((uint64_t(sampleRate) << 32) | frames);
	}

	inline uint64_t Read()
	{
		const auto tick = lastPacket.load();
		return tick && GetTickCount64() - tick < 3000 ? format.load() : 0;
	}
}
