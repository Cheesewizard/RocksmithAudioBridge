#include "stdafx.h"
#include "DeviceVolume.hpp"
#include "ExclusiveOutputDevice.hpp"

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Audio::DeviceVolume
{
	namespace
	{
		std::mutex g_mutex;
		std::condition_variable g_wake;
		std::wstring g_watched;          // Windows id (no "excl:")
		bool g_exclusive = false;
		State g_state;
		bool g_setLevel = false, g_setMute = false;
		float g_pendingLevel = 1.0f;
		bool g_pendingMute = false;
		std::atomic<float> g_softwareGain{ 1.0f };
		std::once_flag g_started;

		void Publish()
		{
			// Under g_mutex.
			const bool apply = g_exclusive && g_state.known && !g_state.hardware;
			g_softwareGain.store(apply ? (g_state.muted ? 0.0f : g_state.level) : 1.0f);
		}

		void Run()
		{
			const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
			if (SUCCEEDED(com)) CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
			Microsoft::WRL::ComPtr<IAudioEndpointVolume> volume;
			std::wstring open;
			bool hardware = false;
			for (;;)
			{
				std::wstring id;
				bool setLevel = false, setMute = false;
				float level = 1.0f;
				bool mute = false;
				{
					std::unique_lock<std::mutex> lock(g_mutex);
					g_wake.wait_for(lock, std::chrono::milliseconds(500));
					id = g_watched;
					setLevel = g_setLevel; level = g_pendingLevel; g_setLevel = false;
					setMute = g_setMute; mute = g_pendingMute; g_setMute = false;
				}
				if (!enumerator) continue;
				if (id != open)
				{
					volume.Reset();
					open = id;
					hardware = false;
					Microsoft::WRL::ComPtr<IMMDevice> device;
					if (!id.empty() && SUCCEEDED(enumerator->GetDevice(id.c_str(), &device))
						&& SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(volume.GetAddressOf()))))
					{
						DWORD support = 0;
						hardware = SUCCEEDED(volume->QueryHardwareSupport(&support)) && (support & ENDPOINT_HARDWARE_SUPPORT_VOLUME);
					}
					std::lock_guard<std::mutex> lock(g_mutex);
					g_state = State{};
					Publish();
				}
				if (!volume) continue;
				if (setLevel) volume->SetMasterVolumeLevelScalar((std::clamp)(level, 0.0f, 1.0f), nullptr);
				if (setMute) volume->SetMute(mute ? TRUE : FALSE, nullptr);
				float now = 1.0f;
				BOOL muted = FALSE;
				const bool read = SUCCEEDED(volume->GetMasterVolumeLevelScalar(&now)) && SUCCEEDED(volume->GetMute(&muted));
				std::lock_guard<std::mutex> lock(g_mutex);
				if (open != g_watched) continue;
				g_state.known = read;
				g_state.level = now;
				g_state.muted = muted != FALSE;
				g_state.hardware = hardware;
				Publish();
			}
		}

		void Start() { std::call_once(g_started, [] { std::thread(Run).detach(); }); }
	}

	void Watch(const std::wstring& endpointId, bool exclusive)
	{
		Start();
		const std::wstring id = SharedOutput::ExclusiveOutputDevice::WindowsId(endpointId);
		std::lock_guard<std::mutex> lock(g_mutex);
		if (id == g_watched && exclusive == g_exclusive) return;
		g_watched = id;
		g_exclusive = exclusive;
		Publish();
		g_wake.notify_one();
	}

	State Get()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_state;
	}

	void Set(float level)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_pendingLevel = level;
		g_setLevel = true;
		g_state.level = level;   // the slider follows at once
		Publish();
		g_wake.notify_one();
	}

	void SetMute(bool muted)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_pendingMute = muted;
		g_setMute = true;
		g_state.muted = muted;
		Publish();
		g_wake.notify_one();
	}

	float SoftwareGain() { return g_softwareGain.load(std::memory_order_relaxed); }
}
