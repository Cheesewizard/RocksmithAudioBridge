#include "SplitInput.hpp"
#include <algorithm>
#include <cstring>
#include <thread>
#include <chrono>

namespace AsioProxy
{
	std::atomic<SplitInput*> SplitInput::s_instances[SplitInput::kMaxDevices]{};
	ASIOCallbacks SplitInput::s_callbacks[SplitInput::kMaxDevices] = {
		{ &SplitInput::OnBufferSwitch<0>, &SplitInput::OnSampleRateDidChange, &SplitInput::OnAsioMessage<0>, &SplitInput::OnBufferSwitchTimeInfo<0> },
		{ &SplitInput::OnBufferSwitch<1>, &SplitInput::OnSampleRateDidChange, &SplitInput::OnAsioMessage<1>, &SplitInput::OnBufferSwitchTimeInfo<1> },
		{ &SplitInput::OnBufferSwitch<2>, &SplitInput::OnSampleRateDidChange, &SplitInput::OnAsioMessage<2>, &SplitInput::OnBufferSwitchTimeInfo<2> },
	};

	namespace
	{
		constexpr long kSampleRate = 48000;         // Rocksmith's engine rate; the output interface runs at it too
		constexpr long kMaxHostFrames = 8192;       // largest block the bridge's callback reads at once

		// asioMessage selectors (Steinberg SDK values).
		constexpr long kAsioSelectorSupported = 1, kAsioEngineVersion = 2, kAsioResetRequest = 3, kAsioSupportsTimeInfo = 7;
	}

	bool SplitInput::Open(const std::wstring& driverName, const std::wstring& dllPath, const CLSID& clsid, void* sysHandle,
		const std::vector<long>& channels, LogFn log, std::string& failure)
	{
		if (m_quarantined.load(std::memory_order_acquire)) { failure = "a previous driver is still stuck on this slot"; return false; }
		Close();
		m_log = log;
		m_driverName = driverName;
		auto fail = [&](const std::string& why) { failure = why; if (m_log) m_log("SplitInput: '%ls': %s", driverName.c_str(), why.c_str()); Release(); return false; };
		if (channels.empty() || channels.size() > kMaxChannels) return fail("no input channels to capture");
		for (size_t set = 0; set < kMaxDevices && m_slotIndex < 0; ++set)
		{
			SplitInput* expected = nullptr;
			if (s_instances[set].compare_exchange_strong(expected, this)) m_slotIndex = static_cast<int>(set);
		}
		if (m_slotIndex < 0) return fail("three input interfaces are open already");

		m_module = LoadLibraryW(dllPath.c_str());
		if (!m_module) return fail("its driver file would not load, Windows error " + std::to_string(GetLastError()));
		// Pinned, as the bridge pins the output driver (AsioProxyDriver LoadReal): a driver thread that outlives
		// Release() must never find its code unmapped.
		HMODULE pinned = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(m_module), &pinned);
		using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
		auto getClassObject = reinterpret_cast<GetClassObject>(GetProcAddress(m_module, "DllGetClassObject"));
		IClassFactory* factory = nullptr;
		if (!getClassObject || FAILED(getClassObject(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory))) || !factory)
			return fail("its driver file is not a working ASIO driver");
		const HRESULT created = factory->CreateInstance(nullptr, clsid, reinterpret_cast<void**>(&m_driver));
		factory->Release();
		if (FAILED(created) || !m_driver) { m_driver = nullptr; return fail("the driver could not be created"); }
		if (!m_driver->init(sysHandle))
		{
			char message[128]{};
			m_driver->getErrorMessage(message);
			return fail(message[0] ? std::string("it refused to start: ") + message : std::string("it refused to start"));
		}
		if (m_driver->canSampleRate(kSampleRate) != 0) return fail("it cannot run at 48 kHz");
		m_driver->setSampleRate(kSampleRate);

		long inputs = 0, outputs = 0;
		m_driver->getChannels(&inputs, &outputs);
		{
			std::lock_guard<std::mutex> lock(m_namesMutex);
			m_names.clear();
			for (long channel = 0; channel < (std::min)(inputs, 256L); ++channel)
			{
				ASIOChannelInfo info{};
				info.channel = channel;
				info.isInput = 1;
				const bool named = m_driver->getChannelInfo(&info) == 0;
				info.name[sizeof(info.name) - 1] = '\0';
				m_names.push_back(named && info.name[0] ? std::string(info.name) : "In " + std::to_string(channel + 1));
			}
		}
		for (long channel : channels)
		{
			const long wanted = inputs > 0 ? (channel >= 0 && channel < inputs ? channel : channel % inputs) : channel;
			ASIOChannelInfo info{};
			info.channel = wanted;
			info.isInput = 1;
			if (m_driver->getChannelInfo(&info) != 0) return fail("it would not report the format of input " + std::to_string(wanted + 1));
			ASIOBufferInfo buffer{};
			buffer.isInput = 1;
			buffer.channelNum = wanted;
			m_infos.push_back(buffer);
			m_types.push_back(info.type);
		}

		long minimum = 0, maximum = 0, preferred = 0, granularity = 0;
		m_driver->getBufferSize(&minimum, &maximum, &preferred, &granularity);
		m_bufferSize = preferred > 0 ? preferred : (minimum > 0 ? minimum : 256);
		// One callback's block is pushed as one packet; VirtualCableCapture takes up to 4096 frames per packet.
		if (m_bufferSize > 4096) return fail("its buffer size (" + std::to_string(m_bufferSize) + ") is too large; set it to 4096 or less in its control panel");
		m_feed.assign(static_cast<size_t>(m_bufferSize), 0.0f);
		m_stereo.assign(static_cast<size_t>(kMaxHostFrames) * 2, 0);
		m_slotBlock.assign(m_infos.size(), std::vector<int32_t>(static_cast<size_t>(kMaxHostFrames), 0));
		m_slotRead.assign(m_infos.size(), 0);
		for (size_t slot = 0; slot < m_infos.size(); ++slot)
		{
			auto ring = std::make_unique<VirtualCableCapture>();
			ring->StartFed();
			m_rings.push_back(std::move(ring));
		}
		if (m_driver->createBuffers(m_infos.data(), static_cast<long>(m_infos.size()), m_bufferSize, &s_callbacks[m_slotIndex]) != 0)
			return fail("it refused a " + std::to_string(m_bufferSize) + " frame input buffer");
		m_capturing.store(true, std::memory_order_release);
		if (m_driver->start() != 0)
		{
			m_capturing.store(false, std::memory_order_release);
			m_driver->disposeBuffers();
			return fail("it would not start");
		}
		m_open.store(true, std::memory_order_release);
		m_openedTick.store(GetTickCount64(), std::memory_order_release);
		if (m_log) m_log("SplitInput: '%ls' open for %zu input channel(s), %ld frame buffers at 48 kHz", driverName.c_str(), m_infos.size(), m_bufferSize);
		return true;
	}

	bool SplitInput::Close()
	{
		if (m_quarantined.load(std::memory_order_acquire)) return false;
		if (!m_driver && !m_module && m_slotIndex < 0) return true;
		const bool wasOpen = m_open.exchange(false, std::memory_order_seq_cst);
		// seq_cst on both sides (here and Capture's in-flight count) so a callback either sees capturing off or is
		// counted by the wait below; with release/acquire the store and the load could pass each other.
		m_capturing.store(false, std::memory_order_seq_cst);
		if (m_driver && wasOpen)
		{
			m_driver->stop();
			// The driver's thread may be inside Capture: wait it out before the rings and buffers go away.
			int waited = 0;
			for (; waited < 2000 && m_callbacksInFlight.load(std::memory_order_seq_cst) != 0; ++waited)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			if (m_callbacksInFlight.load(std::memory_order_seq_cst) != 0)
			{
				// Wedged inside its callback (2026-10-11: Realtek ASIO and ASIO4ALL fighting over one sound chip ended in
				// heap corruption when the rings were freed under a running callback). Quarantine like the output
				// driver: leave the driver, its buffers and the rings allocated for good and only detach this object.
				if (m_log) m_log("SplitInput: '%ls' did not leave its callback; left running detached", m_driverName.c_str());
				Quarantine();
				return false;
			}
			m_driver->disposeBuffers();
			if (m_log) m_log("SplitInput: '%ls' closed", m_driverName.c_str());
		}
		Release();
		return true;
	}

	void SplitInput::Quarantine()
	{
		// Nothing is freed or moved: a callback already past the m_capturing check still reads the members through
		// `this`. The object is marked and its owner abandons it (never deletes it); its callback set stays claimed so a
		// later interface never receives this driver's stray callbacks.
		m_quarantined.store(true, std::memory_order_release);
	}

	void SplitInput::Release()
	{
		if (m_driver) { m_driver->Release(); m_driver = nullptr; }
		if (m_module) { FreeLibrary(m_module); m_module = nullptr; }
		{
			std::lock_guard<std::mutex> lock(m_stateMutex);
			for (auto& ring : m_rings) ring->Stop();
			m_rings.clear();
		}
		m_infos.clear();
		m_types.clear();
		m_slotBlock.clear();
		m_slotRead.clear();
		if (m_slotIndex >= 0)
		{
			SplitInput* self = this;
			s_instances[m_slotIndex].compare_exchange_strong(self, nullptr);
			m_slotIndex = -1;
		}
	}

	void SplitInput::Read(size_t slot, int32_t* mono, uint32_t frames)
	{
		if (!mono || !frames) return;
		if (!m_open.load(std::memory_order_acquire) || slot >= m_rings.size() || frames > static_cast<uint32_t>(kMaxHostFrames))
		{
			std::memset(mono, 0, static_cast<size_t>(frames) * sizeof(int32_t));
			return;
		}
		if (slot >= m_slotBlock.size()) { std::memset(mono, 0, static_cast<size_t>(frames) * sizeof(int32_t)); return; }
		std::vector<int32_t>& block = m_slotBlock[slot];
		if (!m_slotRead[slot])
		{
			m_rings[slot]->Read(m_stereo.data(), frames);
			for (uint32_t frame = 0; frame < frames; ++frame) block[frame] = m_stereo[static_cast<size_t>(frame) * 2];
			m_slotRead[slot] = 1;
		}
		std::memcpy(mono, block.data(), static_cast<size_t>(frames) * sizeof(int32_t));
	}

	VirtualCableCapture::Stats SplitInput::Stats(size_t slot) const
	{
		std::lock_guard<std::mutex> lock(m_stateMutex);
		return slot < m_rings.size() && m_open.load(std::memory_order_acquire) ? m_rings[slot]->GetStats() : VirtualCableCapture::Stats{};
	}

	std::vector<std::string> SplitInput::ChannelNames() const
	{
		std::lock_guard<std::mutex> lock(m_namesMutex);
		return m_names;
	}

	// ---- the input driver's callbacks (the per-set trampolines are in the header) ------------------------------
	long SplitInput::HandleMessage(SplitInput* self, long selector, long value)
	{
		switch (selector)
		{
		case kAsioSelectorSupported: return value == kAsioSelectorSupported || value == kAsioEngineVersion || value == kAsioResetRequest ? 1 : 0;
		case kAsioEngineVersion: return 2;
		case kAsioResetRequest:
			// The input interface wants a restart (unplugged, settings changed in its panel). It keeps delivering what
			// it can; picking it again on the overlay's Guitar input card reopens it.
			if (self && self->m_log)
				self->m_log("SplitInput: '%ls' asked for a reset; pick it again on the overlay's Output page if the guitar stops", self->m_driverName.c_str());
			return 1;
		case kAsioSupportsTimeInfo: return 0;
		default: return 0;
		}
	}

	void SplitInput::OnSampleRateDidChange(ASIOSampleRate) {}

	void SplitInput::Capture(long index) noexcept
	{
		m_callbacksInFlight.fetch_add(1, std::memory_order_seq_cst);
		if (m_capturing.load(std::memory_order_seq_cst) && (index == 0 || index == 1))
		{
			for (size_t slot = 0; slot < m_infos.size() && slot < m_rings.size(); ++slot)
			{
				const auto* source = static_cast<const BYTE*>(m_infos[slot].buffers[index]);
				if (!source) continue;
				for (long frame = 0; frame < m_bufferSize; ++frame) m_feed[static_cast<size_t>(frame)] = ToFloat(source, m_types[slot], frame);
				m_rings[slot]->PushFed(m_feed.data(), static_cast<uint32_t>(m_bufferSize));
			}
		}
		m_callbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
	}

	float SplitInput::ToFloat(const BYTE* source, long type, long frame) noexcept
	{
		switch (type)
		{
		case ASIOSTInt16LSB: { int16_t s; std::memcpy(&s, source + frame * 2, 2); return s / 32768.0f; }
		case ASIOSTInt24LSB:
		{
			const BYTE* p = source + frame * 3;
			const int32_t s = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 24));
			return static_cast<float>(s / 2147483648.0);
		}
		case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
		{
			const int shift = type == ASIOSTInt32LSB16 ? 16 : type == ASIOSTInt32LSB18 ? 14 : type == ASIOSTInt32LSB20 ? 12 : 8;
			int32_t s; std::memcpy(&s, source + frame * 4, 4);
			return static_cast<float>(static_cast<int32_t>(static_cast<uint32_t>(s) << shift) / 2147483648.0);
		}
		case ASIOSTFloat32LSB: { float s; std::memcpy(&s, source + frame * 4, 4); return s == s ? s : 0.0f; }
		case ASIOSTFloat64LSB: { double s; std::memcpy(&s, source + frame * 8, 8); return s == s ? static_cast<float>(s) : 0.0f; }
		case ASIOSTInt32LSB: { int32_t s; std::memcpy(&s, source + frame * 4, 4); return static_cast<float>(s / 2147483648.0); }
		default: return 0.0f;   // a big-endian or unknown format: silence rather than noise
		}
	}
}
