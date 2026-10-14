#include "stdafx.h"
#include "AppAudioMix.hpp"
#include "OutputRateMatcher.hpp"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace Audio::AppAudioMix
{
	namespace
	{
		constexpr wchar_t kKey[] = L"Software\\RSMods\\AsioProxy";
		constexpr uint32_t kCapacity = 48000;    // 1 s of stereo frames
		constexpr uint32_t kTarget = 960;        // ~20 ms kept in hand
		constexpr uint32_t kLookahead = 17;      // the rate matcher reads 16 frames ahead

		std::atomic<int> g_enabled{ -1 };        // -1 = not read yet
		std::atomic<bool> g_started{ false }, g_running{ false };
		std::atomic<ULONGLONG> g_lastHeard{ 0 };

		std::mutex g_mutex;                       // the FIFO; Mix only try-locks it
		std::array<float, kCapacity * 2> g_fifo{};
		uint32_t g_head = 0, g_count = 0;
		bool g_primed = false;
		uint32_t g_window = 0;
		SharedOutput::OutputRateMatcher g_matcher;
		std::array<float, 4096 * 2> g_scratch{};

		class Completion : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
			Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler>
		{
		public:
			Completion() : done(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
			~Completion() { if (done) CloseHandle(done); }
			HANDLE done;
			HRESULT result = E_FAIL;
			Microsoft::WRL::ComPtr<IAudioClient> client;
			STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override
			{
				Microsoft::WRL::ComPtr<IUnknown> unknown;
				HRESULT activated = E_FAIL;
				result = operation->GetActivateResult(&activated, &unknown);
				if (SUCCEEDED(result)) result = activated;
				if (SUCCEEDED(result)) result = unknown.As(&client);
				SetEvent(done);
				return S_OK;
			}
		};

		void Push(const float* stereo, uint32_t frames, bool silent)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			for (uint32_t f = 0; f < frames; ++f)
			{
				if (g_count == kCapacity) { g_head = (g_head + 1) % kCapacity; --g_count; }   // oldest out
				const uint32_t at = (g_head + g_count) % kCapacity;
				g_fifo[at * 2] = silent ? 0.0f : stereo[f * 2];
				g_fifo[at * 2 + 1] = silent ? 0.0f : stereo[f * 2 + 1];
				++g_count;
			}
		}

		// One capture session, until it fails or the mix is switched off. False: process loopback is not available.
		bool Capture()
		{
			AUDIOCLIENT_ACTIVATION_PARAMS params{};
			params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
			params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
			params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();   // everything but Rocksmith itself
			PROPVARIANT variant{};
			variant.vt = VT_BLOB;
			variant.blob.cbSize = sizeof(params);
			variant.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
			// Looked up at run time, not linked: this DLL loads with the game, and a missing import would stop it loading at
			// all on a Windows without it. Loaded once and kept: Run retries a capture every second.
			using ActivateFunction = HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*, IActivateAudioInterfaceAsyncOperation**);
			static const HMODULE mmdevapi = LoadLibraryW(L"Mmdevapi.dll");
			static const auto activate = mmdevapi ? reinterpret_cast<ActivateFunction>(GetProcAddress(mmdevapi, "ActivateAudioInterfaceAsync")) : nullptr;
			auto completion = Microsoft::WRL::Make<Completion>();
			Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
			HRESULT result = !activate ? E_NOTIMPL : completion ? activate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &variant, completion.Get(), &operation) : E_OUTOFMEMORY;
			if (SUCCEEDED(result) && WaitForSingleObject(completion->done, 5000) != WAIT_OBJECT_0) result = E_ABORT;
			if (SUCCEEDED(result)) result = completion->result;
			if (FAILED(result))
			{
				LOG_WARNING("(APP AUDIO) Windows will not capture other programs' sound here (HRESULT " << std::hex << result << std::dec
					<< "; needs Windows 10 2004 or later)" << std::endl);
				return false;
			}
			WAVEFORMATEX format{ WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 48000 * 8, 8, 32, 0 };
			HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			Microsoft::WRL::ComPtr<IAudioClient> client = completion->client;
			Microsoft::WRL::ComPtr<IAudioCaptureClient> capture;
			result = event ? client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, &format, nullptr) : E_OUTOFMEMORY;
			if (SUCCEEDED(result)) result = client->SetEventHandle(event);
			if (SUCCEEDED(result)) result = client->GetService(IID_PPV_ARGS(&capture));
			if (SUCCEEDED(result)) result = client->Start();
			if (FAILED(result))
			{
				LOG_WARNING("(APP AUDIO) Could not start capturing other programs' sound (HRESULT " << std::hex << result << std::dec << ")" << std::endl);
				if (event) CloseHandle(event);
				return true;   // available, failed this time: try again later
			}
			LOG_INFO("(APP AUDIO) Capturing other programs' sound (all but Rocksmith) to mix into the exclusive or ASIO output" << std::endl);
			g_running.store(true);
			while (Enabled())
			{
				WaitForSingleObject(event, 100);
				UINT32 frames = 0;
				bool failed = false;
				while (SUCCEEDED(result = capture->GetNextPacketSize(&frames)) && frames)
				{
					BYTE* data = nullptr;
					DWORD flags = 0;
					if (FAILED(result = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) { failed = true; break; }
					const float* values = reinterpret_cast<const float*>(data);
					const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
					if (!silent)
					{
						float peak = 0.0f;
						for (UINT32 i = 0; i < frames * 2; ++i) peak = (std::max)(peak, std::fabs(values[i]));
						if (peak > 0.0003f) g_lastHeard.store(GetTickCount64());
					}
					Push(values, frames, silent);
					capture->ReleaseBuffer(frames);
				}
				if (failed || FAILED(result)) break;
			}
			g_running.store(false);
			client->Stop();
			CloseHandle(event);
			return true;
		}

		void Run()
		{
			const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			for (;;)
			{
				if (!Enabled()) { Sleep(500); continue; }
				if (!Capture()) break;
				Sleep(1000);   // ended (switched off, or the capture failed): a pause before the next go
			}
			if (SUCCEEDED(com)) CoUninitialize();
		}
	}

	bool Enabled()
	{
		int enabled = g_enabled.load();
		if (enabled < 0)
		{
			DWORD value = 1, size = sizeof(value);
			RegGetValueW(HKEY_CURRENT_USER, kKey, L"MixAppAudio", RRF_RT_REG_DWORD, nullptr, &value, &size);
			enabled = value ? 1 : 0;
			g_enabled.store(enabled);
		}
		return enabled == 1;
	}

	void SetEnabled(bool enabled)
	{
		g_enabled.store(enabled ? 1 : 0);
		const DWORD value = enabled ? 1 : 0;
		RegSetKeyValueW(HKEY_CURRENT_USER, kKey, L"MixAppAudio", REG_DWORD, &value, sizeof(value));
	}

	void Start()
	{
		Enabled();   // the registry read happens here, so Mix only loads the atomic
		bool expected = false;
		if (!g_started.compare_exchange_strong(expected, true)) return;
		try { std::thread(Run).detach(); }
		catch (...) { g_started.store(false); }
	}

	void Mix(float* stereo, uint32_t frames) noexcept
	{
		if (!stereo || !frames || frames > 4096 || g_enabled.load(std::memory_order_relaxed) != 1) return;
		std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
		if (!lock.owns_lock()) return;
		if (!g_primed)
		{
			if (g_count < kTarget + frames + kLookahead) return;   // build up first
			g_head = (g_head + (g_count - kTarget - frames - kLookahead)) % kCapacity;
			g_count = kTarget + frames + kLookahead;
			g_matcher.Reseed();
			g_primed = true;
		}
		if (g_count < frames + kLookahead) { g_primed = false; g_matcher.Reseed(); return; }   // ran dry: build up again
		if (g_count > kTarget * 4)
		{
			// Far behind (a stall, or the spare device's clock much faster): drop to the target at once.
			const uint32_t excess = g_count - kTarget;
			g_head = (g_head + excess) % kCapacity;
			g_count -= excess;
			g_matcher.Reseed();
		}
		// Steer the fill to the target a few times a second (+ppm reads faster when too full).
		g_window += frames;
		if (g_window >= 12000)
		{
			g_window = 0;
			g_matcher.Measure(0.0, static_cast<double>(g_count) - kTarget);
		}
		unsigned consumed = 0;
		const unsigned produced = g_matcher.Read([](unsigned frame, unsigned channel) { return g_fifo[((g_head + frame) % kCapacity) * 2 + channel]; },
			g_count, g_scratch.data(), frames, consumed);
		for (unsigned f = 0; f < produced; ++f)
		{
			stereo[f * 2] += g_scratch[f * 2];
			stereo[f * 2 + 1] += g_scratch[f * 2 + 1];
		}
		g_head = (g_head + consumed) % kCapacity;
		g_count -= consumed;
	}

	bool Hearing() { return GetTickCount64() - g_lastHeard.load() < 3000; }
	bool Running() { return g_running.load(); }
}
