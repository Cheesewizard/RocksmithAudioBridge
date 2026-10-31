#pragma once

#include "../../Mod/Shared/AsioInterface.h"
#include "VirtualCableCapture.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace AsioProxy
{
	// A second ASIO interface for the guitar (each input's Interface on the overlay's Routing card), while the
	// bridge stays bound to the output interface and runs on its clock.
	//
	// RS_ASIO.ini names the bridge for every section, so RS_ASIO sees one device; the split happens in here. The
	// input interface is opened as an input-only ASIO stream with its own callbacks. Each captured channel goes into
	// a VirtualCableCapture in fed mode: the same lock-free ring, adaptive margin and drift servo with a fractional
	// resampler the Real Tone Cable uses, so the guitar is pulled onto the output interface's clock (a few hundred
	// ppm of correction at most, far below audible pitch change). The bridge's own callback reads it per host input
	// channel (Read). Everything after that (mixer, recording, the amp sim) already runs on the output's clock, so it
	// needs nothing extra.
	//
	// ASIO callbacks carry no context, so each open SplitInput takes one of kMaxDevices fixed callback sets (one
	// static instance pointer per set): up to three input interfaces at once, one per player and the mic.
	// Open/Close run with the bridge's real callbacks disabled; Read runs on the bridge's audio thread, the producer
	// on the input driver's audio thread.
	class SplitInput final
	{
	public:
		using LogFn = void(*)(const char* format, ...);
		static constexpr size_t kMaxChannels = 4;
		static constexpr size_t kMaxDevices = 3;

		SplitInput() = default;
		~SplitInput() { if (!IsQuarantined()) Close(); }
		SplitInput(const SplitInput&) = delete;
		SplitInput& operator=(const SplitInput&) = delete;

		// Loads, initialises and starts the input interface for `channels` (0-based device input channels). On
		// failure returns false with a user-readable `failure` and leaves nothing open.
		bool Open(const std::wstring& driverName, const std::wstring& dllPath, const CLSID& clsid, void* sysHandle,
			const std::vector<long>& channels, LogFn log, std::string& failure, bool remote64 = false);
		// False when the driver stayed wedged in its callback: the object is quarantined and must be abandoned
		// (leaked, never deleted or reused); IsQuarantined says so.
		bool Close();
		bool IsQuarantined() const { return m_quarantined.load(std::memory_order_acquire); }
		bool IsOpen() const { return m_open.load(std::memory_order_acquire); }
		// GetTickCount64 when it last opened: a stall of the output soon after points at this interface.
		uint64_t OpenedTick() const { return m_openedTick.load(std::memory_order_acquire); }
		const std::wstring& DriverName() const { return m_driverName; }

		// Bridge audio thread, once per host block before any Read: a slot is drained from its ring only once per block,
		// so two host channels on the same input-interface channel (both players on In 2) get the same audio. Draining
		// it twice would hand each one alternate halves, played at double speed (heard as echo and garble).
		void BeginBlock() { std::fill(m_slotRead.begin(), m_slotRead.end(), static_cast<uint8_t>(0)); }
		// Bridge audio thread: `frames` mono Int32 samples of capture slot `slot` (the index in Open's `channels`).
		void Read(size_t slot, int32_t* mono, uint32_t frames);
		VirtualCableCapture::Stats Stats(size_t slot) const;
		// The input interface's own channel names, filled at Open.
		std::vector<std::string> ChannelNames() const;

	private:
		template <size_t N> static void OnBufferSwitch(long index, ASIOBool) { if (SplitInput* self = s_instances[N].load(std::memory_order_acquire)) self->Capture(index); }
		template <size_t N> static ASIOTime* OnBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct) { OnBufferSwitch<N>(index, direct); return params; }
		template <size_t N> static long OnAsioMessage(long selector, long value, void*, double*) { return HandleMessage(s_instances[N].load(std::memory_order_acquire), selector, value); }
		static long HandleMessage(SplitInput* self, long selector, long value);
		static void OnSampleRateDidChange(ASIOSampleRate rate);
		void Capture(long index) noexcept;
		static float ToFloat(const BYTE* source, long type, long frame) noexcept;
		void Release();
		void Quarantine();

		static std::atomic<SplitInput*> s_instances[kMaxDevices];
		static ASIOCallbacks s_callbacks[kMaxDevices];
		int m_slotIndex = -1;   // which callback set this instance holds while open

		std::atomic<bool> m_open{ false };
		std::atomic<bool> m_capturing{ false };
		std::atomic<uint32_t> m_callbacksInFlight{ 0 };
		std::atomic<uint64_t> m_openedTick{ 0 };
		std::atomic<bool> m_quarantined{ false };
		mutable std::mutex m_stateMutex;           // Stats (overlay thread) against Release clearing the rings
		IAsioDriver* m_driver = nullptr;
		HMODULE m_module = nullptr;
		std::wstring m_driverName;
		LogFn m_log = nullptr;
		long m_bufferSize = 0;
		std::vector<ASIOBufferInfo> m_infos;
		std::vector<long> m_types;
		std::vector<std::unique_ptr<VirtualCableCapture>> m_rings;
		std::vector<float> m_feed;                 // producer scratch, one block of one channel
		std::vector<int32_t> m_stereo;             // reader scratch: VirtualCableCapture::Read writes interleaved stereo
		std::vector<std::vector<int32_t>> m_slotBlock;   // this block's mono samples per slot
		std::vector<uint8_t> m_slotRead;                 // slot already drained this block (BeginBlock clears)
		mutable std::mutex m_namesMutex;
		std::vector<std::string> m_names;
	};
}
