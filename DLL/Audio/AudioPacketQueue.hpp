#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace Audio
{
	// One producer and one consumer. Storage is allocated before either starts.
	class AudioPacketQueue
	{
	public:
		void Initialize(uint32_t packetBytes, uint32_t packetCount)
		{
			if (packetBytes == 0 || packetCount < 2) throw std::invalid_argument("Invalid audio queue capacity");
			bytesPerPacket = packetBytes;
			capacity = packetCount;
			storage.resize(static_cast<size_t>(packetBytes) * packetCount);
			lengths.resize(packetCount);
			Reset();
		}

		uint8_t* BeginWrite() noexcept
		{
			const uint32_t next = written.load(std::memory_order_relaxed);
			if (next - read.load(std::memory_order_acquire) >= capacity) return nullptr;
			return storage.data() + static_cast<size_t>(next % capacity) * bytesPerPacket;
		}

		void CommitWrite(uint32_t bytes) noexcept
		{
			const uint32_t next = written.load(std::memory_order_relaxed);
			lengths[next % capacity] = bytes;
			written.store(next + 1, std::memory_order_release);
		}

		const uint8_t* BeginRead(uint32_t& bytes) const noexcept
		{
			const uint32_t next = read.load(std::memory_order_relaxed);
			if (next == written.load(std::memory_order_acquire)) return nullptr;
			bytes = lengths[next % capacity];
			return storage.data() + static_cast<size_t>(next % capacity) * bytesPerPacket;
		}

		void CommitRead() noexcept
		{
			read.fetch_add(1, std::memory_order_release);
		}

		uint32_t Count() const noexcept
		{
			return written.load(std::memory_order_acquire) - read.load(std::memory_order_acquire);
		}

		// Only while both sides are stopped.
		void Reset() noexcept
		{
			written.store(0, std::memory_order_relaxed);
			read.store(0, std::memory_order_relaxed);
		}

	private:
		std::atomic<uint32_t> written{ 0 };
		std::atomic<uint32_t> read{ 0 };
		uint32_t bytesPerPacket = 0;
		uint32_t capacity = 0;
		std::vector<uint8_t> storage;
		std::vector<uint32_t> lengths;
	};
}
