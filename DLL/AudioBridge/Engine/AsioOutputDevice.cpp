#include "stdafx.h"
#include "AsioOutputDevice.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace Audio::SharedOutput
{
	std::atomic<AsioOutputDevice*> AsioOutputDevice::s_active{ nullptr };
	ASIOCallbacks AsioOutputDevice::s_callbacks = { &AsioOutputDevice::OnBufferSwitch, &AsioOutputDevice::OnSampleRateDidChange,
		&AsioOutputDevice::OnAsioMessage, &AsioOutputDevice::OnBufferSwitchTimeInfo };

	namespace
	{
		constexpr wchar_t kPrefix[] = L"asio:";
		constexpr double kSampleRate = 48000.0;
		constexpr long kMaxBuffer = 4096;   // the backend's FIFO; a bigger ASIO buffer than this is refused
		// asioMessage selectors (Steinberg SDK values).
		constexpr long kSelectorSupported = 1, kEngineVersion = 2, kResetRequest = 3, kResyncRequest = 5, kLatenciesChanged = 6,
			kSupportsTimeInfo = 7;

		std::string Narrow(const std::wstring& text)
		{
			if (text.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
			std::string out(size > 0 ? size - 1 : 0, '\0');
			if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
			return out;
		}

		// HKLM\SOFTWARE\ASIO\<name> (32-bit view: the game is 32-bit) -> its CLSID; that CLSID's InprocServer32 -> the DLL.
		bool FindDriver(const std::wstring& name, CLSID& clsid, std::wstring& dll)
		{
			HKEY key{};
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (L"SOFTWARE\\ASIO\\" + name).c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) return false;
			wchar_t text[128]{};
			DWORD size = sizeof(text) - sizeof(wchar_t), type = 0;
			const bool read = RegQueryValueExW(key, L"CLSID", nullptr, &type, reinterpret_cast<BYTE*>(text), &size) == ERROR_SUCCESS && type == REG_SZ;
			RegCloseKey(key);
			if (!read || FAILED(CLSIDFromString(text, &clsid))) return false;
			if (RegOpenKeyExW(HKEY_CLASSES_ROOT, (std::wstring(L"CLSID\\") + text + L"\\InprocServer32").c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) return false;
			wchar_t path[MAX_PATH]{};
			size = sizeof(path) - sizeof(wchar_t);
			const bool found = RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(path), &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
			RegCloseKey(key);
			if (!found) return false;
			wchar_t expanded[MAX_PATH]{};
			dll = ExpandEnvironmentStringsW(path, expanded, MAX_PATH) ? expanded : path;
			return !dll.empty();
		}
	}

	namespace AsioCrashGuard
	{
		namespace
		{
			constexpr wchar_t kKey[] = L"Software\\RSMods\\AsioProxy";
			constexpr wchar_t kValue[] = L"CableAsioOutputPending";
			std::atomic<bool> g_checked{ false };   // only the launch's first open is judged by what was left behind
		}

		void Mark(const std::wstring& driverName)
		{
			g_checked.store(true);
			RegSetKeyValueW(HKEY_CURRENT_USER, kKey, kValue, REG_SZ, driverName.c_str(), static_cast<DWORD>((driverName.size() + 1) * sizeof(wchar_t)));
		}

		void Clear() { RegDeleteKeyValueW(HKEY_CURRENT_USER, kKey, kValue); }

		bool CrashedLastTime(const std::wstring& driverName)
		{
			if (g_checked.exchange(true)) return false;
			wchar_t pending[256]{};
			DWORD size = sizeof(pending) - sizeof(wchar_t);
			if (RegGetValueW(HKEY_CURRENT_USER, kKey, kValue, RRF_RT_REG_SZ, nullptr, pending, &size) != ERROR_SUCCESS) return false;
			return _wcsicmp(pending, driverName.c_str()) == 0;
		}
	}

	bool AsioOutputDevice::IsAsioEndpoint(const std::wstring& endpointId) { return endpointId.rfind(kPrefix, 0) == 0; }
	std::wstring AsioOutputDevice::DriverName(const std::wstring& endpointId) { return IsAsioEndpoint(endpointId) ? endpointId.substr(wcslen(kPrefix)) : std::wstring(); }
	std::wstring AsioOutputDevice::EndpointOf(const std::wstring& driverName) { return kPrefix + driverName; }

	bool AsioOutputDevice::IsInstalled(const std::wstring& driverName)
	{
		CLSID clsid{};
		std::wstring dll;
		return !driverName.empty() && FindDriver(driverName, clsid, dll);
	}

	AsioOutputDevice::~AsioOutputDevice() { Close(); }

	bool AsioOutputDevice::Open(const std::wstring& driverName, Fill fill, std::string& failure)
	{
		m_name = driverName;
		m_fill = std::move(fill);
		auto fail = [&](const std::string& why) { failure = why; Close(); return false; };
		CLSID clsid{};
		std::wstring dll;
		if (!FindDriver(driverName, clsid, dll)) return fail("it is not installed for 32-bit programs");
		AsioOutputDevice* expected = nullptr;
		if (!s_active.compare_exchange_strong(expected, this)) return fail("another ASIO output is still open");

		m_module = LoadLibraryW(dll.c_str());
		if (!m_module) return fail("its driver file would not load, Windows error " + std::to_string(GetLastError()));
		// Pinned for the life of the game: a driver thread that outlives Release() must never find its code unmapped.
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
		HWND window = D3DHooks::hThisWnd ? D3DHooks::hThisWnd : GetDesktopWindow();
		if (!m_driver->init(window))
		{
			char message[128]{};
			m_driver->getErrorMessage(message);
			return fail(message[0] ? std::string("it refused to start: ") + message : std::string("it refused to start (in use by another program?)"));
		}
		if (m_driver->canSampleRate(kSampleRate) != 0) return fail("it cannot run at 48 kHz");
		m_driver->setSampleRate(kSampleRate);

		long inputs = 0, outputs = 0;
		m_driver->getChannels(&inputs, &outputs);
		if (outputs < 1) return fail("it has no outputs");
		m_outputCount = outputs >= 2 ? 2 : 1;
		for (long channel = 0; channel < m_outputCount; ++channel)
		{
			ASIOChannelInfo info{};
			info.channel = channel;
			info.isInput = 0;
			if (m_driver->getChannelInfo(&info) != 0) return fail("it would not report the format of output " + std::to_string(channel + 1));
			if (info.type < ASIOSTInt16LSB) return fail("it uses a big-endian sample format");
			m_types[channel] = info.type;
			m_infos[channel] = ASIOBufferInfo{ 0, channel, { nullptr, nullptr } };
		}

		long minimum = 0, maximum = 0, preferred = 0, granularity = 0;
		m_driver->getBufferSize(&minimum, &maximum, &preferred, &granularity);
		m_bufferSize = preferred > 0 ? preferred : (minimum > 0 ? minimum : 256);
		if (m_bufferSize > kMaxBuffer) return fail("its buffer size (" + std::to_string(m_bufferSize) + ") is too large; set it to 4096 or less in its control panel");
		m_stereo.assign(static_cast<size_t>(m_bufferSize) * 2, 0.0f);
		if (m_driver->createBuffers(m_infos, m_outputCount, m_bufferSize, &s_callbacks) != 0)
			return fail("it refused a " + std::to_string(m_bufferSize) + " sample buffer");
		m_buffersCreated = true;
		m_supportsOutputReady = m_driver->outputReady() == 0;
		long inputLatency = 0;
		m_driver->getLatencies(&inputLatency, &m_outputLatency);
		LOG_INFO("(ASIO OUTPUT) '" << Narrow(driverName) << "' open: " << m_outputCount << " output(s), " << m_bufferSize
			<< "-sample buffers at 48 kHz (its own setting; driver buffer range " << minimum << ".." << maximum << "), output latency "
			<< m_outputLatency << " samples" << std::endl);
		return true;
	}

	bool AsioOutputDevice::Start()
	{
		if (!m_driver || !m_buffersCreated) return false;
		if (m_started) return true;
		m_running.store(true, std::memory_order_seq_cst);
		if (m_driver->start() != 0) { m_running.store(false, std::memory_order_seq_cst); return false; }
		m_started = true;
		return true;
	}

	void AsioOutputDevice::Stop()
	{
		m_running.store(false, std::memory_order_seq_cst);
		if (m_driver && m_started) m_driver->stop();
		m_started = false;
	}

	bool AsioOutputDevice::Close()
	{
		Stop();
		// The driver's thread may still be inside Process: wait it out before the buffers and the driver go away.
		for (int waited = 0; waited < 2000 && m_inFlight.load(std::memory_order_seq_cst) != 0; ++waited)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (m_inFlight.load(std::memory_order_seq_cst) != 0)
		{
			LOG_ERROR("(ASIO OUTPUT) '" << Narrow(m_name) << "' did not leave its callback; left running detached" << std::endl);
			return false;   // quarantined: s_active keeps pointing here, so a stray callback still finds a valid object
		}
		if (m_driver && m_buffersCreated) m_driver->disposeBuffers();
		m_buffersCreated = false;
		if (m_driver) { m_driver->Release(); m_driver = nullptr; }
		// The module stays loaded (pinned): never FreeLibrary an ASIO driver while the game runs.
		m_module = nullptr;
		AsioOutputDevice* self = this;
		s_active.compare_exchange_strong(self, nullptr);
		return true;
	}

	void AsioOutputDevice::Process(long index) noexcept
	{
		m_inFlight.fetch_add(1, std::memory_order_seq_cst);
		if (index == 0 || index == 1)
		{
			const bool running = m_running.load(std::memory_order_seq_cst);
			std::fill(m_stereo.begin(), m_stereo.end(), 0.0f);
			if (running && m_fill) m_fill(m_stereo.data(), static_cast<uint32_t>(m_bufferSize));
			for (long channel = 0; channel < m_outputCount; ++channel)
				if (m_infos[channel].buffers[index])
					Convert(m_stereo.data(), m_outputCount == 1 ? -1 : channel, m_infos[channel].buffers[index], m_types[channel], m_bufferSize);
			if (m_supportsOutputReady && m_driver) m_driver->outputReady();
			if (running)
			{
				m_lastCallback.store(GetTickCount64(), std::memory_order_release);
				m_callbacks.fetch_add(1, std::memory_order_acq_rel);
			}
		}
		m_inFlight.fetch_sub(1, std::memory_order_seq_cst);
	}

	// channel -1: a mono interface gets the sum of both (halved, so a centred sound keeps its level).
	void AsioOutputDevice::Convert(const float* stereo, long channel, void* destination, long type, long frames) noexcept
	{
		BYTE* out = static_cast<BYTE*>(destination);
		for (long frame = 0; frame < frames; ++frame)
		{
			float value = channel < 0 ? (stereo[frame * 2] + stereo[frame * 2 + 1]) * 0.5f : stereo[frame * 2 + channel];
			value = value == value ? (std::max)(-1.0f, (std::min)(1.0f, value)) : 0.0f;
			switch (type)
			{
			case ASIOSTInt16LSB: { const int16_t s = static_cast<int16_t>(value * 32767.0f); std::memcpy(out + frame * 2, &s, 2); break; }
			case ASIOSTInt24LSB:
			{
				const int32_t s = static_cast<int32_t>(value * 8388607.0f);
				out[frame * 3] = static_cast<BYTE>(s); out[frame * 3 + 1] = static_cast<BYTE>(s >> 8); out[frame * 3 + 2] = static_cast<BYTE>(s >> 16);
				break;
			}
			case ASIOSTInt32LSB: { const int32_t s = static_cast<int32_t>(static_cast<double>(value) * 2147483647.0); std::memcpy(out + frame * 4, &s, 4); break; }
			case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
			{
				const int bits = type == ASIOSTInt32LSB16 ? 16 : type == ASIOSTInt32LSB18 ? 18 : type == ASIOSTInt32LSB20 ? 20 : 24;
				const int32_t s = static_cast<int32_t>(value * static_cast<float>((1 << (bits - 1)) - 1));
				std::memcpy(out + frame * 4, &s, 4);
				break;
			}
			case ASIOSTFloat32LSB: std::memcpy(out + frame * 4, &value, 4); break;
			case ASIOSTFloat64LSB: { const double s = value; std::memcpy(out + frame * 8, &s, 8); break; }
			default: break;   // refused at Open
			}
		}
	}

	void AsioOutputDevice::OnBufferSwitch(long index, ASIOBool)
	{
		if (AsioOutputDevice* self = s_active.load(std::memory_order_acquire)) self->Process(index);
	}

	ASIOTime* AsioOutputDevice::OnBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct)
	{
		OnBufferSwitch(index, direct);
		return params;
	}

	void AsioOutputDevice::OnSampleRateDidChange(ASIOSampleRate) {}

	long AsioOutputDevice::OnAsioMessage(long selector, long value, void*, double*)
	{
		switch (selector)
		{
		case kSelectorSupported:
			return value == kEngineVersion || value == kResetRequest || value == kResyncRequest || value == kLatenciesChanged ? 1 : 0;
		case kEngineVersion: return 2;
		case kResetRequest:
			// Its control panel changed the buffer size, or a device came or went: the backend reopens it (off this
			// driver's thread), the way the bridge's ASIO driver handles the same request.
			if (AsioOutputDevice* self = s_active.load(std::memory_order_acquire)) self->m_resetRequested.store(true);
			return 1;
		case kResyncRequest: case kLatenciesChanged: return 1;
		case kSupportsTimeInfo: return 0;
		default: return 0;
		}
	}
}
