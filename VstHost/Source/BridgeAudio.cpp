#include "BridgeAudio.hpp"

#include "HostLog.hpp"
#include "../../Mod/Shared/AsioInterface.h"

#include <algorithm>
#include <cstring>

namespace VstHost
{
	namespace
	{
		// {7B2E5C10-9F3A-4D6B-A1C8-2E4F6A8B0D31}, the bridge driver (AsioProxyDriver.cpp).
		const CLSID kBridgeClsid = { 0x7B2E5C10, 0x9F3A, 0x4D6B, { 0xA1, 0xC8, 0x2E, 0x4F, 0x6A, 0x8B, 0x0D, 0x31 } };

		constexpr ASIOError kOk = 0;
		constexpr long kSelectorSupported = 1;
		constexpr long kEngineVersion = 2;
		constexpr long kResetRequest = 3;
		constexpr long kResyncRequest = 5;
		constexpr long kLatenciesChanged = 6;
		constexpr long kSupportsTimeInfo = 7;
		constexpr float kInt32Scale = 2147483648.0f;

		// ASIO callbacks carry no user pointer; one stream per process.
		BridgeAudio* g_active = nullptr;

		void OnBufferSwitch(long index, ASIOBool) { if (g_active) g_active->Render(index); }
		ASIOTime* OnBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool) { if (g_active) g_active->Render(index); return params; }
		void OnSampleRateChanged(ASIOSampleRate) {}
		long OnAsioMessage(long selector, long value, void*, double*)
		{
			switch (selector)
			{
			case kSelectorSupported:
				return value == kEngineVersion || value == kResetRequest || value == kResyncRequest || value == kLatenciesChanged ? 1 : 0;
			case kEngineVersion: return 2;
			case kResetRequest:
			case kResyncRequest:
				if (g_active) g_active->RequestReset();
				return 1;
			case kLatenciesChanged: return 1;
			case kSupportsTimeInfo: return 0;
			default: return 0;
			}
		}

		ASIOCallbacks g_callbacks = { OnBufferSwitch, OnSampleRateChanged, OnAsioMessage, OnBufferSwitchTimeInfo };

		std::wstring ExeDirectory()
		{
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			std::wstring dir = path;
			const size_t slash = dir.find_last_of(L"\\/");
			return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
		}
	}

	bool BridgeAudio::CreateDriver(std::string& error)
	{
		// Prefer the copy shipped next to this exe, so the host always pairs with the same build of the link;
		// fall back to the registered driver.
		const std::wstring local = ExeDirectory() + L"RocksmithAudioBridgeAsio64.dll";
		m_module = LoadLibraryW(local.c_str());
		if (m_module)
		{
			using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
			auto getClassObject = reinterpret_cast<GetClassObject>(GetProcAddress(m_module, "DllGetClassObject"));
			IClassFactory* factory = nullptr;
			if (getClassObject && SUCCEEDED(getClassObject(kBridgeClsid, IID_IClassFactory, reinterpret_cast<void**>(&factory))) && factory)
			{
				factory->CreateInstance(nullptr, kBridgeClsid, reinterpret_cast<void**>(&m_driver));
				factory->Release();
			}
			if (m_driver) { Log("driver: loaded %ls", local.c_str()); return true; }
			FreeLibrary(m_module);
			m_module = nullptr;
		}
		if (SUCCEEDED(CoCreateInstance(kBridgeClsid, nullptr, CLSCTX_INPROC_SERVER, kBridgeClsid, reinterpret_cast<void**>(&m_driver))) && m_driver)
		{
			Log("driver: loaded the registered Rocksmith Audio Bridge ASIO");
			return true;
		}
		error = "Rocksmith Audio Bridge ASIO (64-bit) is not installed. Run Repair in the Audio Bridge window.";
		return false;
	}

	bool BridgeAudio::Open(HWND notifyWindow, AudioSink* sink, std::string& error)
	{
		Close();
		m_notifyWindow = notifyWindow;
		m_sink = sink;
		if (!m_driver && !CreateDriver(error)) return false;
		if (!m_driver->init(notifyWindow))
		{
			error = "The bridge driver refused to start.";
			return false;
		}

		long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
		m_driver->getBufferSize(&minSize, &maxSize, &preferred, &granularity);
		m_frames = preferred > 0 ? static_cast<int>(preferred) : 128;

		ASIOBufferInfo infos[4]{};
		for (int ch = 0; ch < 2; ++ch)
		{
			infos[ch].isInput = 1;
			infos[ch].channelNum = ch;
			infos[2 + ch].isInput = 0;
			infos[2 + ch].channelNum = ch;
		}
		g_active = this;
		const ASIOError result = m_driver->createBuffers(infos, 4, m_frames, &g_callbacks);
		if (result != kOk)
		{
			g_active = nullptr;
			error = "The bridge driver could not create buffers (error " + std::to_string(result) + ").";
			return false;
		}
		m_buffersCreated = true;
		for (int ch = 0; ch < 2; ++ch)
			for (int half = 0; half < 2; ++half)
			{
				m_in[ch][half] = infos[ch].buffers[half];
				m_out[ch][half] = infos[2 + ch].buffers[half];
			}
		m_guitar.assign(static_cast<size_t>(m_frames), 0.0f);
		m_left.assign(static_cast<size_t>(m_frames), 0.0f);
		m_right.assign(static_cast<size_t>(m_frames), 0.0f);
		Log("audio: %d frames at 48000 Hz", m_frames);
		return true;
	}

	bool BridgeAudio::Start(std::string& error)
	{
		if (!m_buffersCreated) { error = "Audio is not open."; return false; }
		if (m_running) return true;
		const ASIOError result = m_driver->start();
		if (result != kOk) { error = "The bridge driver would not start (error " + std::to_string(result) + ")."; return false; }
		m_running = true;
		return true;
	}

	void BridgeAudio::Stop()
	{
		if (m_running && m_driver) m_driver->stop();
		m_running = false;
	}

	void BridgeAudio::Close()
	{
		Stop();
		if (m_buffersCreated && m_driver) m_driver->disposeBuffers();
		m_buffersCreated = false;
		if (g_active == this) g_active = nullptr;
		if (m_driver) { m_driver->Release(); m_driver = nullptr; }
		if (m_module) { FreeLibrary(m_module); m_module = nullptr; }
	}

	void BridgeAudio::RequestReset()
	{
		if (m_notifyWindow) PostMessageW(m_notifyWindow, kResetMessage, 0, 0);
	}

	void BridgeAudio::Render(long index)
	{
		const int half = index ? 1 : 0;
		const int frames = m_frames;
		const int32_t* in = static_cast<const int32_t*>(m_in[0][half]);
		for (int i = 0; i < frames; ++i) m_guitar[i] = static_cast<float>(in[i]) / kInt32Scale;

		if (m_sink) m_sink->Process(m_guitar.data(), m_left.data(), m_right.data(), frames);
		else { std::fill(m_left.begin(), m_left.end(), 0.0f); std::fill(m_right.begin(), m_right.end(), 0.0f); }

		const float* sources[2] = { m_left.data(), m_right.data() };
		for (int ch = 0; ch < 2; ++ch)
		{
			int32_t* out = static_cast<int32_t*>(m_out[ch][half]);
			for (int i = 0; i < frames; ++i)
			{
				const float s = std::clamp(sources[ch][i], -1.0f, 1.0f);
				out[i] = s >= 1.0f ? INT32_MAX : static_cast<int32_t>(s * kInt32Scale);
			}
		}
	}
}
