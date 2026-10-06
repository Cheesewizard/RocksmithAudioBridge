#pragma once

// Compiled only into an explicitly requested diagnostic build.
#if defined(RSMODS_AUDIO_LIFECYCLE_TRACE)
#include "ComVTable.hpp"
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>

namespace Audio::LifecycleTrace
{
	constexpr size_t CAPACITY = 256;
	constexpr size_t TABLE_CAPACITY = 8;
	constexpr size_t START_SLOT = 10;
	constexpr size_t STOP_SLOT = 11;
	constexpr size_t RESET_SLOT = 12;
	using ClientMethod = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*);

	enum class Kind { Attach, AttachFailed, StartBegin, StartEnd, StopBegin, StopEnd, ResetBegin, ResetEnd, CaptureBind };

	struct Event
	{
		uint64_t sequence = 0;
		uint64_t tick = 0;
		DWORD thread = 0;
		Kind kind = Kind::Attach;
		void* client = nullptr;
		void* context = nullptr;
		HRESULT result = S_OK;
		std::array<void*, 8> stack{};
		USHORT depth = 0;
	};

	struct Slot
	{
		std::atomic<unsigned> state{ 0 };
		Event event;
	};

	struct ClientTable
	{
		void** table = nullptr;
		ClientMethod start = nullptr;
		ClientMethod stop = nullptr;
		ClientMethod reset = nullptr;
	};

	struct BufferStats
	{
		std::atomic<uint64_t> entered{ 0 }, returned{ 0 }, packets{ 0 }, lastTick{ 0 };
		std::atomic<HRESULT> result{ S_OK };
		std::atomic<UINT32> frames{ 0 };
	};

	inline std::array<Slot, CAPACITY> slots;
	inline std::array<ClientTable, TABLE_CAPACITY> tables;
	inline std::array<BufferStats, 2> buffers;
	inline std::atomic<size_t> tableCount{ 0 };
	inline std::atomic<uint64_t> nextSequence{ 0 }, dropped{ 0 };
	inline std::mutex tableMutex;
	inline std::ofstream output;
	inline uint64_t nextSnapshot = 0;

	inline void Record(Kind kind, void* client, void* context, HRESULT result = S_OK, bool stack = false)
	{
		const uint64_t sequence = nextSequence.fetch_add(1, std::memory_order_relaxed);
		auto& slot = slots[sequence % CAPACITY];
		unsigned empty = 0;
		if (!slot.state.compare_exchange_strong(empty, 1, std::memory_order_acquire))
		{
			dropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		auto& event = slot.event;
		event = {};
		event.sequence = sequence;
		event.tick = GetTickCount64();
		event.thread = GetCurrentThreadId();
		event.kind = kind;
		event.client = client;
		event.context = context;
		event.result = result;
		if (stack) event.depth = CaptureStackBackTrace(2, static_cast<DWORD>(event.stack.size()), event.stack.data(), nullptr);
		slot.state.store(2, std::memory_order_release);
	}

	inline ClientMethod FindMethod(IAudioClient* client, size_t slot)
	{
		void** table = ComVTable::GetVTable(client);
		const size_t count = tableCount.load(std::memory_order_acquire);
		for (size_t index = 0; index < count; ++index)
		{
			if (tables[index].table != table) continue;
			return slot == START_SLOT ? tables[index].start : slot == STOP_SLOT ? tables[index].stop : tables[index].reset;
		}
		return nullptr;
	}

	inline HRESULT Call(IAudioClient* client, size_t slot, Kind begin, Kind end, void* caller)
	{
		Record(begin, client, caller, S_OK, true);
		const auto method = FindMethod(client, slot);
		const HRESULT result = method ? method(client) : E_UNEXPECTED;
		Record(end, client, caller, result);
		return result;
	}

	__declspec(noinline) inline HRESULT STDMETHODCALLTYPE Start(IAudioClient* client)
	{
		return Call(client, START_SLOT, Kind::StartBegin, Kind::StartEnd, _ReturnAddress());
	}

	__declspec(noinline) inline HRESULT STDMETHODCALLTYPE Stop(IAudioClient* client)
	{
		return Call(client, STOP_SLOT, Kind::StopBegin, Kind::StopEnd, _ReturnAddress());
	}

	__declspec(noinline) inline HRESULT STDMETHODCALLTYPE Reset(IAudioClient* client)
	{
		return Call(client, RESET_SLOT, Kind::ResetBegin, Kind::ResetEnd, _ReturnAddress());
	}

	inline void Attach(IAudioClient* client, void* stream)
	{
		if (!client)
		{
			Record(Kind::AttachFailed, client, stream, E_POINTER);
			return;
		}
		std::lock_guard<std::mutex> guard(tableMutex);
		void** table = ComVTable::GetVTable(client);
		const size_t count = tableCount.load(std::memory_order_relaxed);
		for (size_t index = 0; index < count; ++index)
		{
			if (tables[index].table == table)
			{
				const bool complete = table[START_SLOT] == reinterpret_cast<void*>(&Start)
					&& table[STOP_SLOT] == reinterpret_cast<void*>(&Stop)
					&& table[RESET_SLOT] == reinterpret_cast<void*>(&Reset);
				Record(complete ? Kind::Attach : Kind::AttachFailed, client, stream, complete ? S_OK : E_FAIL);
				return;
			}
		}
		if (!table || !table[START_SLOT] || !table[STOP_SLOT] || !table[RESET_SLOT])
		{
			Record(Kind::AttachFailed, client, stream, E_POINTER);
			return;
		}
		if (count == TABLE_CAPACITY)
		{
			Record(Kind::AttachFailed, client, stream, E_OUTOFMEMORY);
			return;
		}
		tables[count] = { table, reinterpret_cast<ClientMethod>(table[START_SLOT]),
			reinterpret_cast<ClientMethod>(table[STOP_SLOT]), reinterpret_cast<ClientMethod>(table[RESET_SLOT]) };
		// Publish all originals before any shared-vtable slot can enter a hook.
		tableCount.store(count + 1, std::memory_order_release);
		bool patched = true;
		patched &= ComVTable::PatchSlot(client, START_SLOT, reinterpret_cast<void*>(&Start)) != nullptr;
		patched &= ComVTable::PatchSlot(client, STOP_SLOT, reinterpret_cast<void*>(&Stop)) != nullptr;
		patched &= ComVTable::PatchSlot(client, RESET_SLOT, reinterpret_cast<void*>(&Reset)) != nullptr;
		Record(patched ? Kind::Attach : Kind::AttachFailed, client, stream, patched ? S_OK : E_FAIL);
	}

	inline void BufferEnter(int route)
	{
		if (route >= 0 && route < 2) buffers[route].entered.fetch_add(1, std::memory_order_relaxed);
	}

	inline void BufferReturn(int route, HRESULT result, UINT32 frames)
	{
		if (route < 0 || route >= 2) return;
		auto& value = buffers[route];
		value.result.store(result, std::memory_order_relaxed);
		value.frames.store(frames, std::memory_order_relaxed);
		value.lastTick.store(GetTickCount64(), std::memory_order_relaxed);
		if (SUCCEEDED(result) && result != AUDCLNT_S_BUFFER_EMPTY && frames > 0)
			value.packets.fetch_add(1, std::memory_order_relaxed);
		value.returned.fetch_add(1, std::memory_order_relaxed);
	}

	inline void WriteAddress(void* address)
	{
		output << address;
		HMODULE module = nullptr;
		if (!address || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(address), &module)) return;
		char path[MAX_PATH]{};
		GetModuleFileNameA(module, path, MAX_PATH);
		output << '[' << std::filesystem::path(path).filename().string() << "+0x" << std::hex
			<< reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module) << std::dec << ']';
	}

	inline void Initialize()
	{
		char path[MAX_PATH]{};
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		const auto file = std::filesystem::path(path).parent_path()
			/ ("RSMods_audio_lifecycle_" + std::to_string(GetCurrentProcessId()) + ".txt");
		output.open(file, std::ios::out | std::ios::trunc);
		if (!output)
		{
			LOG_ERROR("[AudioTrace] Cannot open " << file.string() << std::endl);
			return;
		}
		output << "Audio lifecycle trace " << __DATE__ << ' ' << __TIME__ << " pid=" << GetCurrentProcessId()
			<< "; tick=GetTickCount64 milliseconds; begin.context=direct caller; attach.context=PortAudio stream\n";
		output << "Instrumentation can affect scheduling. No driver settings or timer resolution are changed by this tracer.\n";
		output.flush();
		LOG_INFO("[AudioTrace] Writing " << file.string() << std::endl);
	}

	inline void Poll()
	{
		if (!output.is_open() || !output) return;
		constexpr const char* names[] = { "attach", "attach-failed", "start-begin", "start-end", "stop-begin", "stop-end", "reset-begin", "reset-end", "capture-bind" };
		bool changed = false;
		for (auto& slot : slots)
		{
			if (slot.state.load(std::memory_order_acquire) != 2) continue;
			const auto& event = slot.event;
			output << "seq=" << event.sequence << " tick=" << event.tick << " tid=" << event.thread
				<< ' ' << names[static_cast<size_t>(event.kind)] << " client=" << event.client << " context=";
			WriteAddress(event.context);
			output << " hr=0x" << std::hex << static_cast<uint32_t>(event.result) << std::dec;
			for (USHORT frame = 0; frame < event.depth; ++frame)
			{
				output << " stack" << frame << '=';
				WriteAddress(event.stack[frame]);
			}
			output << '\n';
			slot.state.store(0, std::memory_order_release);
			changed = true;
		}
		const uint64_t now = GetTickCount64();
		if (now >= nextSnapshot)
		{
			nextSnapshot = now + 1000;
			for (size_t route = 0; route < buffers.size(); ++route)
			{
				auto& value = buffers[route];
				output << "tick=" << now << " route=" << route << " get-enter=" << value.entered.load()
					<< " get-return=" << value.returned.load() << " packets=" << value.packets.load()
					<< " last-return-tick=" << value.lastTick.load() << " frames=" << value.frames.load()
					<< " hr=0x" << std::hex << static_cast<uint32_t>(value.result.load()) << std::dec << '\n';
			}
			output << "trace-dropped=" << dropped.load() << '\n';
			changed = true;
		}
		if (changed) output.flush();
	}
}
#endif
