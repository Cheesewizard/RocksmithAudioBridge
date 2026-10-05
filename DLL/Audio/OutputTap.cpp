#include "stdafx.h"
#include "OutputTap.hpp"
#include "GameAudioRecorder.hpp"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/implements.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace Audio::OutputTap
{
	using namespace Microsoft::WRL;

	namespace
	{
		// The render format the game negotiated, captured at Initialize so the tap knows how to read
		// each block. Written on the game's audio thread at open, read on the same thread per block.
		struct TapFormat
		{
			std::atomic<uint32_t> channels{ 2 };
			std::atomic<uint32_t> bits{ 32 };
			std::atomic<bool> isFloat{ false };
			std::atomic<bool> valid{ false };
		};
		TapFormat g_format;

		std::mutex g_recorderMutex;
		std::shared_ptr<RecordingSession> g_session;

		// Largest packet the wet recorder accepts (its queue slots are sized for it at Start). Audio
		// blocks can be bigger (ASIO buffers of 4096 / 8192 frames) and an oversized packet ends the
		// wet take, so blocks are handed over in pieces of at most this size.
		constexpr uint32_t WET_PACKET_FRAMES = 2048;
		void SubmitWetInPackets(RecordingSession& session, const float* stereo, uint32_t frames) noexcept
		{
			for (uint32_t done = 0; done < frames; done += WET_PACKET_FRAMES)
				session.SubmitWet(stereo + static_cast<size_t>(done) * 2, (std::min)(WET_PACKET_FRAMES, frames - done));
		}
		// Lock-free mirror of the session for the read-only status getters. The overlay polls IsRecording
		// every rendered frame and the ASIO callback takes g_recorderMutex every audio block, so the
		// getters must not share that mutex with the real-time thread. Written (under g_recorderMutex)
		// only by Start/StopRecording; read with std::atomic_load.
		std::shared_ptr<RecordingSession> g_sessionView;
		std::shared_ptr<RecordingSession> SessionView() { return std::atomic_load(&g_sessionView); }
		std::atomic<bool> g_clientSeen{ false };
		std::atomic<uint64_t> g_observedFrames{ 0 };
		std::atomic<uint64_t> g_loggedFrames{ 0 };

		void StoreFormat(const WAVEFORMATEX& format)
		{
			uint32_t channels = format.nChannels ? format.nChannels : 2;
			bool isFloat = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
			if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			{
				const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
				isFloat = ext.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
			}
			g_format.channels.store(channels, std::memory_order_relaxed);
			g_format.bits.store(format.wBitsPerSample, std::memory_order_relaxed);
			g_format.isFloat.store(isFloat, std::memory_order_relaxed);
			g_format.valid.store(true, std::memory_order_release);
			LOG_INFO("(OUTPUT TAP) render format: " << format.nSamplesPerSec << " Hz, " << channels
				<< " ch, " << format.wBitsPerSample << "-bit, " << (isFloat ? "float" : "int") << std::endl);
		}

		// Copy one just-written block (read-only) to the recorder if one is armed, converting the
		// game's sample format to float stereo. Runs on the game's audio render thread; the recorder's
		// own writer thread does the disk I/O, so this stays a memcpy + convert with no blocking.
		void FeedBlock(const BYTE* data, UINT32 frames) noexcept
		{
			g_observedFrames.fetch_add(frames, std::memory_order_relaxed);
			const uint64_t total = g_observedFrames.load(std::memory_order_relaxed);
			if (total - g_loggedFrames.load(std::memory_order_relaxed) >= 48000 * 5)
			{
				g_loggedFrames.store(total, std::memory_order_relaxed);
				LOG_INFO("(OUTPUT TAP) observed " << total << " frames at the render tap" << std::endl);
			}

			std::shared_ptr<RecordingSession> session;
			{
				std::lock_guard<std::mutex> guard(g_recorderMutex);
				session = g_session;
			}
			if (!session || !data || !frames) return;

			const uint32_t channels = g_format.channels.load(std::memory_order_relaxed);
			const uint32_t bits = g_format.bits.load(std::memory_order_relaxed);
			const bool isFloat = g_format.isFloat.load(std::memory_order_relaxed);
			if (channels == 0) return;

			static thread_local std::vector<float> stereo;
			stereo.resize(static_cast<size_t>(frames) * 2);
			for (UINT32 frame = 0; frame < frames; ++frame)
			{
				float left = 0.f, right = 0.f;
				if (isFloat && bits == 32)
				{
					const float* s = reinterpret_cast<const float*>(data) + static_cast<size_t>(frame) * channels;
					left = s[0]; right = channels > 1 ? s[1] : s[0];
				}
				else if (bits == 32)
				{
					const int32_t* s = reinterpret_cast<const int32_t*>(data) + static_cast<size_t>(frame) * channels;
					left = s[0] / 2147483648.0f; right = (channels > 1 ? s[1] : s[0]) / 2147483648.0f;
				}
				else if (bits == 16)
				{
					const int16_t* s = reinterpret_cast<const int16_t*>(data) + static_cast<size_t>(frame) * channels;
					left = s[0] / 32768.0f; right = (channels > 1 ? s[1] : s[0]) / 32768.0f;
				}
				else return; // unsupported depth; drop rather than write garbage
				stereo[static_cast<size_t>(frame) * 2] = left;
				stereo[static_cast<size_t>(frame) * 2 + 1] = right;
			}
			SubmitWetInPackets(*session, stereo.data(), static_cast<uint32_t>(frames));
		}

		class TapRenderClient : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IAudioRenderClient, FtmBase>
		{
		public:
			explicit TapRenderClient(ComPtr<IAudioRenderClient> real) : real(std::move(real)) {}
			HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 framesRequested, BYTE** data) override
			{
				const HRESULT result = real->GetBuffer(framesRequested, data);
				if (SUCCEEDED(result) && data) { lastBuffer = *data; lastFrames = framesRequested; }
				else { lastBuffer = nullptr; lastFrames = 0; }
				return result;
			}
			HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 framesWritten, DWORD flags) override
			{
				if (lastBuffer && framesWritten && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))
					FeedBlock(lastBuffer, framesWritten);
				lastBuffer = nullptr;
				return real->ReleaseBuffer(framesWritten, flags);
			}
		private:
			ComPtr<IAudioRenderClient> real;
			BYTE* lastBuffer = nullptr;
			UINT32 lastFrames = 0;
		};

		class TapAudioClient : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IAudioClient3, FtmBase>
		{
		public:
			explicit TapAudioClient(ComPtr<IAudioClient3> real) : real(std::move(real)) {}
			// IAudioClient
			HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME bufferDuration,
				REFERENCE_TIME periodicity, const WAVEFORMATEX* format, LPCGUID session) override
			{
				const HRESULT result = real->Initialize(mode, flags, bufferDuration, periodicity, format, session);
				LOG_INFO("(OUTPUT TAP) client Initialize mode=" << mode << " hr=" << std::hex << result << std::dec << std::endl);
				if (SUCCEEDED(result) && format) StoreFormat(*format);
				return result;
			}
			HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override { return real->GetBufferSize(frames); }
			HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* latency) override { return real->GetStreamLatency(latency); }
			HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* padding) override { return real->GetCurrentPadding(padding); }
			HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE mode, const WAVEFORMATEX* format, WAVEFORMATEX** closest) override
			{
				const HRESULT result = real->IsFormatSupported(mode, format, closest);
				LOG_INFO("(OUTPUT TAP) client IsFormatSupported mode=" << mode << " hr=" << std::hex << result << std::dec << std::endl);
				return result;
			}
			HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override { return real->GetMixFormat(format); }
			HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* normal, REFERENCE_TIME* minimum) override { return real->GetDevicePeriod(normal, minimum); }
			HRESULT STDMETHODCALLTYPE Start() override { return real->Start(); }
			HRESULT STDMETHODCALLTYPE Stop() override { return real->Stop(); }
			HRESULT STDMETHODCALLTYPE Reset() override { return real->Reset(); }
			HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE handle) override { return real->SetEventHandle(handle); }
			HRESULT STDMETHODCALLTYPE GetService(REFIID id, void** object) override
			{
				if (id == __uuidof(IAudioRenderClient))
				{
					ComPtr<IAudioRenderClient> renderClient;
					const HRESULT result = real->GetService(id, &renderClient);
					if (FAILED(result) || !renderClient) return result;
					auto tap = Make<TapRenderClient>(renderClient);
					if (!tap) return E_OUTOFMEMORY;
					g_clientSeen.store(true, std::memory_order_release);
					LOG_INFO("(OUTPUT TAP) render client wrapped; tap is live" << std::endl);
					return tap.CopyTo(id, object);
				}
				return real->GetService(id, object);
			}
			// IAudioClient2
			HRESULT STDMETHODCALLTYPE IsOffloadCapable(AUDIO_STREAM_CATEGORY category, BOOL* capable) override { return real->IsOffloadCapable(category, capable); }
			HRESULT STDMETHODCALLTYPE SetClientProperties(const AudioClientProperties* properties) override { return real->SetClientProperties(properties); }
			HRESULT STDMETHODCALLTYPE GetBufferSizeLimits(const WAVEFORMATEX* format, BOOL eventDriven, REFERENCE_TIME* minDuration, REFERENCE_TIME* maxDuration) override { return real->GetBufferSizeLimits(format, eventDriven, minDuration, maxDuration); }
			// IAudioClient3
			HRESULT STDMETHODCALLTYPE GetSharedModeEnginePeriod(const WAVEFORMATEX* format, UINT32* defaultPeriod, UINT32* fundamentalPeriod, UINT32* minPeriod, UINT32* maxPeriod) override { return real->GetSharedModeEnginePeriod(format, defaultPeriod, fundamentalPeriod, minPeriod, maxPeriod); }
			HRESULT STDMETHODCALLTYPE GetCurrentSharedModeEnginePeriod(WAVEFORMATEX** format, UINT32* currentPeriod) override { return real->GetCurrentSharedModeEnginePeriod(format, currentPeriod); }
			HRESULT STDMETHODCALLTYPE InitializeSharedAudioStream(DWORD flags, UINT32 periodFrames, const WAVEFORMATEX* format, LPCGUID session) override
			{
				const HRESULT result = real->InitializeSharedAudioStream(flags, periodFrames, format, session);
				if (SUCCEEDED(result) && format) StoreFormat(*format);
				return result;
			}
		private:
			ComPtr<IAudioClient3> real;
		};

		class TapDevice : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMDevice, IMMEndpoint, FtmBase>
		{
		public:
			explicit TapDevice(ComPtr<IMMDevice> real) : real(std::move(real)) {}
			HRESULT STDMETHODCALLTYPE Activate(REFIID id, DWORD context, PROPVARIANT* params, void** object) override
			{
				if (!object) return E_POINTER;
				if (id == __uuidof(IAudioClient) || id == __uuidof(IAudioClient2) || id == __uuidof(IAudioClient3))
				{
					// Activate with the interface the caller actually asked for (RS_ASIO may not accept an
					// IAudioClient3 activation IID), then QI up to IAudioClient3 to wrap. If it cannot be
					// wrapped, hand back the real client untapped - the tap must never break playback.
					ComPtr<IUnknown> raw;
					const HRESULT result = real->Activate(id, context, params, reinterpret_cast<void**>(raw.GetAddressOf()));
					LOG_INFO("(OUTPUT TAP) device Activate hr=" << std::hex << result << std::dec << std::endl);
					if (FAILED(result) || !raw) return result;
					ComPtr<IAudioClient3> client;
					if (FAILED(raw.As(&client)) || !client) { *object = raw.Detach(); return S_OK; }
					auto wrapper = Make<TapAudioClient>(client);
					if (!wrapper) { *object = raw.Detach(); return S_OK; }
					return wrapper.CopyTo(id, object);
				}
				return real->Activate(id, context, params, object);
			}
			HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD access, IPropertyStore** properties) override { return real->OpenPropertyStore(access, properties); }
			HRESULT STDMETHODCALLTYPE GetId(LPWSTR* id) override { return real->GetId(id); }
			HRESULT STDMETHODCALLTYPE GetState(DWORD* state) override { return real->GetState(state); }
			HRESULT STDMETHODCALLTYPE GetDataFlow(EDataFlow* flow) override
			{
				ComPtr<IMMEndpoint> endpoint;
				if (SUCCEEDED(real.As(&endpoint))) return endpoint->GetDataFlow(flow);
				return E_NOINTERFACE;
			}
		private:
			ComPtr<IMMDevice> real;
		};

		// Wrap a device only if it is a render endpoint; capture devices pass through untouched.
		std::mutex g_deviceCacheMutex;
		std::map<std::wstring, ComPtr<IMMDevice>> g_deviceCache;   // device id -> its TapDevice wrapper

		HRESULT WrapIfRender(IMMDevice* device, IMMDevice** wrapped)
		{
			*wrapped = nullptr;
			if (!device) return E_POINTER;
			ComPtr<IMMEndpoint> endpoint;
			EDataFlow flow = eAll;
			const bool haveFlow = SUCCEEDED(ComPtr<IMMDevice>(device).As(&endpoint)) && SUCCEEDED(endpoint->GetDataFlow(&flow));
			if (!haveFlow || flow != eRender)
			{
				ComPtr<IMMDevice> passthrough(device);
				return passthrough.CopyTo(wrapped);
			}
			// Return the SAME TapDevice for a given device id every time. The game checks that its default
			// render endpoint is present in the enumerated list; if the default and the list item were
			// different wrapper objects (not COM-identical), it concludes there is no output device.
			LPWSTR idRaw = nullptr;
			std::wstring id;
			if (SUCCEEDED(device->GetId(&idRaw)) && idRaw) { id = idRaw; CoTaskMemFree(idRaw); }
			std::lock_guard<std::mutex> guard(g_deviceCacheMutex);
			auto existing = g_deviceCache.find(id);
			if (!id.empty() && existing != g_deviceCache.end() && existing->second)
				return existing->second.CopyTo(wrapped);
			auto tap = Make<TapDevice>(ComPtr<IMMDevice>(device));
			if (!tap) return E_OUTOFMEMORY;
			ComPtr<IMMDevice> tapDevice;
			tap.As(&tapDevice);
			if (!id.empty()) g_deviceCache[id] = tapDevice;
			LOG_INFO("(OUTPUT TAP) wrapped render device (id cached)" << std::endl);
			return tapDevice.CopyTo(wrapped);
		}

		class TapCollection : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMDeviceCollection, FtmBase>
		{
		public:
			std::vector<ComPtr<IMMDevice>> devices;
			HRESULT STDMETHODCALLTYPE GetCount(UINT* count) override { if (!count) return E_POINTER; *count = static_cast<UINT>(devices.size()); return S_OK; }
			HRESULT STDMETHODCALLTYPE Item(UINT index, IMMDevice** device) override
			{
				if (!device) return E_POINTER;
				*device = nullptr;
				return index < devices.size() ? devices[index].CopyTo(device) : E_INVALIDARG;
			}
		};

		class TapEnumerator : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMDeviceEnumerator, FtmBase>
		{
		public:
			explicit TapEnumerator(ComPtr<IMMDeviceEnumerator> physical) : physical(std::move(physical)) {}
			HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow flow, DWORD mask, IMMDeviceCollection** devices) override
			{
				if (!devices) return E_POINTER;
				*devices = nullptr;
				ComPtr<IMMDeviceCollection> actual;
				HRESULT result = physical->EnumAudioEndpoints(flow, mask, &actual);
				if (FAILED(result)) return result;
				auto collection = Make<TapCollection>();
				if (!collection) return E_OUTOFMEMORY;
				UINT count = 0;
				result = actual->GetCount(&count);
				for (UINT index = 0; SUCCEEDED(result) && index < count; ++index)
				{
					ComPtr<IMMDevice> device, wrapped;
					result = actual->Item(index, &device);
					if (SUCCEEDED(result)) result = WrapIfRender(device.Get(), wrapped.GetAddressOf());
					if (SUCCEEDED(result)) collection->devices.push_back(wrapped);
				}
				if (FAILED(result)) return result;
				LOG_INFO("(OUTPUT TAP) EnumAudioEndpoints flow=" << flow << " wrapped " << collection->devices.size() << " device(s)" << std::endl);
				return collection.CopyTo(devices);
			}
			HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice** device) override
			{
				if (!device) return E_POINTER;
				*device = nullptr;
				ComPtr<IMMDevice> real;
				const HRESULT result = physical->GetDefaultAudioEndpoint(flow, role, &real);
				LOG_INFO("(OUTPUT TAP) GetDefaultAudioEndpoint flow=" << flow << " role=" << role << " hr=" << std::hex << result << std::dec << std::endl);
				if (FAILED(result)) return result;
				return WrapIfRender(real.Get(), device);
			}
			HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, IMMDevice** device) override
			{
				if (!device) return E_POINTER;
				*device = nullptr;
				ComPtr<IMMDevice> real;
				const HRESULT result = physical->GetDevice(id, &real);
				if (FAILED(result)) return result;
				return WrapIfRender(real.Get(), device);
			}
			HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* client) override { return physical->RegisterEndpointNotificationCallback(client); }
			HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* client) override { return physical->UnregisterEndpointNotificationCallback(client); }
		private:
			ComPtr<IMMDeviceEnumerator> physical;
		};
	}

	HRESULT CreateTapEnumerator(IMMDeviceEnumerator* physical, IMMDeviceEnumerator** enumerator)
	{
		if (!physical || !enumerator) return E_POINTER;
		*enumerator = nullptr;
		auto wrapper = Make<TapEnumerator>(ComPtr<IMMDeviceEnumerator>(physical));
		if (!wrapper) return E_OUTOFMEMORY;
		return wrapper.CopyTo(enumerator);
	}

	namespace
	{
		// The Rocksmith Audio Bridge proxy driver hands over each output block interleaved in its ASIO
		// sample format. Convert to float stereo and feed the recorder. LSB types are little-endian
		// (x86 native); Int32LSB16/18/20/24 are 32-bit containers read as int32.
		int AsioSampleBytes(long t)
		{
			switch (t)
			{
			case 0: case 16: return 2;                                   // Int16
			case 1: case 17: return 3;                                   // Int24
			case 2: case 3: case 18: case 19:
			case 24: case 25: case 26: case 27: return 4;                // Int32 / Float32 / containers
			default: return 0;
			}
		}
		float AsioToFloat(const BYTE* p, long t)
		{
			switch (t)
			{
			case 16: return *reinterpret_cast<const int16_t*>(p) / 32768.0f;
			case 17: { int v = p[0] | (p[1] << 8) | (p[2] << 16); if (v & 0x800000) v |= ~0xFFFFFF; return v / 8388608.0f; }
			case 19: return *reinterpret_cast<const float*>(p);
			default: return *reinterpret_cast<const int32_t*>(p) / 2147483648.0f;   // Int32LSB and containers
			}
		}
		void __cdecl ProxySink(const void* data, long frames, long channels, long asioType, double) noexcept
		{
			if (!data || frames <= 0 || channels <= 0) return;
			std::shared_ptr<RecordingSession> session;
			{
				std::lock_guard<std::mutex> guard(g_recorderMutex);
				session = g_session;
			}
			if (!session) return;
			const int bytes = AsioSampleBytes(asioType);
			if (bytes == 0) return;
			g_observedFrames.fetch_add(frames, std::memory_order_relaxed);
			static thread_local std::vector<float> stereo;
			stereo.resize(static_cast<size_t>(frames) * 2);
			const BYTE* base = reinterpret_cast<const BYTE*>(data);
			for (long f = 0; f < frames; ++f)
			{
				const BYTE* fb = base + static_cast<size_t>(f) * channels * bytes;
				const float left = AsioToFloat(fb, asioType);
				stereo[static_cast<size_t>(f) * 2] = left;
				stereo[static_cast<size_t>(f) * 2 + 1] = channels > 1 ? AsioToFloat(fb + bytes, asioType) : left;
			}
			SubmitWetInPackets(*session, stereo.data(), static_cast<uint32_t>(frames));
		}
		// Arm/disarm the tap in the proxy driver. Registered only while a take runs, so with the
		// bridge off the proxy is a pure passthrough (no sink, no copy).
		void SetProxySink(bool on)
		{
			HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
			if (!proxy)
			{
				if (on) LOG_ERROR("(OUTPUT TAP) Rocksmith Audio Bridge ASIO driver not loaded; wet recording needs the proxy ASIO driver" << std::endl);
				return;
			}
			using SetSinkFn = void(__cdecl*)(void(__cdecl*)(const void*, long, long, long, double));
			auto setSink = reinterpret_cast<SetSinkFn>(GetProcAddress(proxy, "RSModsAsio_SetSink"));
			if (setSink) setSink(on ? &ProxySink : nullptr);
		}
	}

	HRESULT StartRecording(const std::wstring& directory)
	{
		const std::filesystem::path path(directory);
		if (!path.is_absolute()) return E_INVALIDARG;
		auto session = std::make_shared<RecordingSession>();
		HRESULT result = session->Start(path, WET_PACKET_FRAMES);
		if (FAILED(result)) return result;
		std::lock_guard<std::mutex> guard(g_recorderMutex);
		if (g_session)
		{
			std::wstring ignoredPath;
			uint64_t ignoredFrames = 0, ignoredStarted = 0;
			session->Stop(ignoredPath, ignoredFrames, ignoredStarted);
			return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
		}
		g_session = std::move(session);
		std::atomic_store(&g_sessionView, g_session);
		SetProxySink(true);
		LOG_INFO("(OUTPUT TAP) recording started (wet mix and dry input)" << std::endl);
		return S_OK;
	}

	HRESULT StopRecording(std::wstring& savedPath, uint64_t& frames, uint64_t& started)
	{
		frames = 0; started = 0;
		SetProxySink(false);   // stop pulling from the proxy before the recorder is torn down (no-op if dry)
		std::shared_ptr<RecordingSession> finished;
		{
			std::lock_guard<std::mutex> guard(g_recorderMutex);
			finished.swap(g_session);
			std::atomic_store(&g_sessionView, std::shared_ptr<RecordingSession>());
		}
		if (!finished) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
		finished->Stop(savedPath, frames, started);
		LOG_INFO("(OUTPUT TAP) recording stopped (wet + dry); " << frames << " frames" << std::endl);
		return finished->GetError();
	}

	// Status getters read the lock-free session view: polled per frame by the overlay and per
	// status request by the bridge, they never contend with the audio thread's per-block lock.
	bool IsRecording()
	{
		return SessionView() != nullptr;
	}

	bool IsRecordingDry()
	{
		std::lock_guard<std::mutex> guard(g_recorderMutex);
		return g_session != nullptr;
	}

	HRESULT RecordingError()
	{
		const auto session = SessionView();
		return session ? session->GetError() : S_OK;
	}

	uint64_t RecordedFrames()
	{
		const auto session = SessionView();
		return session ? session->GetFrames() : 0;
	}

	uint64_t RecordingStarted()
	{
		const auto session = SessionView();
		return session ? session->GetStarted() : 0;
	}

	bool TapClientSeen() { return g_clientSeen.load(std::memory_order_acquire); }
	bool ProxyAvailable()
	{
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		return proxy && GetProcAddress(proxy, "RSModsAsio_SetSink") != nullptr;
	}
	int ProxyOutputMode()
	{
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		if (!proxy) return 0;
		using ModeFn = int(__cdecl*)();
		auto mode = reinterpret_cast<ModeFn>(GetProcAddress(proxy, "RSModsAsio_GetOutputMode"));
		return mode ? mode() : 0;
	}
	int ProxyInputMode()
	{
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		if (!proxy) return 0;
		using ModeFn = int(__cdecl*)();
		auto mode = reinterpret_cast<ModeFn>(GetProcAddress(proxy, "RSModsAsio_GetInputMode"));
		return mode ? mode() : 0;
	}
	bool TryPromoteProxyOutput(const std::wstring& driverName)
	{
		if (driverName.empty()) return false;
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		if (!proxy) return false;
		using PromoteFn = int(__cdecl*)(const wchar_t*);
		auto promote = reinterpret_cast<PromoteFn>(GetProcAddress(proxy, "RSModsAsio_TryPromote"));
		return promote && promote(driverName.c_str()) != 0;
	}
	bool TryRebindProxyOutput(const std::wstring& driverName)
	{
		if (driverName.empty()) return false;
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		if (!proxy) return false;
		using RebindFn = int(__cdecl*)(const wchar_t*);
		auto rebind = reinterpret_cast<RebindFn>(GetProcAddress(proxy, "RSModsAsio_TryRebind"));
		return rebind && rebind(driverName.c_str()) != 0;
	}
	bool TryDemoteProxyOutput()
	{
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		if (!proxy) return false;
		using DemoteFn = int(__cdecl*)();
		auto demote = reinterpret_cast<DemoteFn>(GetProcAddress(proxy, "RSModsAsio_TryDemote"));
		return demote && demote() != 0;
	}
	// Configure the proxy's static output trim (linear gain in the first arg; the second is reserved). Returns
	// false when the proxy driver is not loaded (WASAPI output, or ASIO without the bridge). gain == 1 = off.
	bool ConfigureLimiter(float gainLinear, float reserved)
	{
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		if (!proxy) return false;
		using ConfigFn = void(__cdecl*)(float, float);
		auto fn = reinterpret_cast<ConfigFn>(GetProcAddress(proxy, "RSModsAsio_ConfigureLimiter"));
		if (!fn) return false;
		fn(gainLinear, reserved);
		return true;
	}
	// Configure the proxy's output loudness guard: a real look-ahead brickwall limiter (holds the ceiling)
	// and/or a slow loudness AGC (equalises song-to-song). Both stages toggle independently; ceiling and
	// target are linear (0..1). Returns false when the proxy driver is not loaded.
	bool ConfigureOutputGuard(bool limiterOn, float ceilingLin, bool agcOn, float targetRms)
	{
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		if (!proxy) return false;
		using GuardFn = void(__cdecl*)(int, float, int, float);
		auto fn = reinterpret_cast<GuardFn>(GetProcAddress(proxy, "RSModsAsio_ConfigureOutputGuard"));
		if (!fn) return false;
		fn(limiterOn ? 1 : 0, ceilingLin, agcOn ? 1 : 0, targetRms);
		return true;
	}
	// Arm the proxy to inject one latency probe on the next output buffers. Returns the probe length in
	// samples (0 if the proxy is not loaded), which the host regenerates as the correlation reference.
	int ArmLatencyProbe()
	{
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		if (!proxy) return 0;
		using LenFn = int(__cdecl*)();
		using ArmFn = void(__cdecl*)();
		auto len = reinterpret_cast<LenFn>(GetProcAddress(proxy, "RSModsAsio_ProbeLength"));
		auto arm = reinterpret_cast<ArmFn>(GetProcAddress(proxy, "RSModsAsio_ArmLatencyProbe"));
		if (!len || !arm) return 0;
		arm();
		return len();
	}

	// Read the proxy's per-channel output peak-hold + RMS (0..1). Enables the proxy meter on first call and
	// leaves it on (the per-buffer cost is tiny). Zeros the outputs when the proxy is not loaded.
	void ReadOutputLevels(float* peak, float* rms, int maxCh)
	{
		for (int i = 0; i < maxCh; ++i) { if (peak) peak[i] = 0.0f; if (rms) rms[i] = 0.0f; }
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		if (!proxy) return;
		static bool metering = false;
		if (!metering)
		{
			using EnableFn = void(__cdecl*)(int);
			if (auto en = reinterpret_cast<EnableFn>(GetProcAddress(proxy, "RSModsAsio_EnableMeter"))) { en(1); metering = true; }
		}
		using GetFn = int(__cdecl*)(float*, float*, int);
		if (auto get = reinterpret_cast<GetFn>(GetProcAddress(proxy, "RSModsAsio_GetOutputLevels"))) get(peak, rms, maxCh);
	}
	uint64_t ObservedFrames() { return g_observedFrames.load(std::memory_order_relaxed); }

	bool AddProxySink(ProxySinkFn sink)
	{
		if (!sink) return false;
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		if (!proxy) { LOG_ERROR("(OUTPUT TAP) alternate-device routing needs the Rocksmith Audio Bridge ASIO proxy driver, which is not loaded" << std::endl); return false; }
		using AddFn = int(__cdecl*)(void(__cdecl*)(const void*, long, long, long, double));
		auto add = reinterpret_cast<AddFn>(GetProcAddress(proxy, "RSModsAsio_AddSink"));
		if (!add) return false;
		return add(sink) >= 0;
	}

	void RemoveProxySink(ProxySinkFn sink)
	{
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		if (!proxy || !sink) return;
		using RemoveFn = void(__cdecl*)(void(__cdecl*)(const void*, long, long, long, double));
		if (auto remove = reinterpret_cast<RemoveFn>(GetProcAddress(proxy, "RSModsAsio_RemoveSink"))) remove(sink);
	}

	bool SetProxyForwardMuted(bool muted)
	{
		HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
		if (!proxy) return false;
		using MuteFn = void(__cdecl*)(int);
		auto fn = reinterpret_cast<MuteFn>(GetProcAddress(proxy, "RSModsAsio_SetForwardMuted"));
		if (!fn) return false;
		fn(muted ? 1 : 0);
		return true;
	}
}
