#include "stdafx.h"
#include "PersistentCapture.hpp"
#include "CableInput.hpp"
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

namespace Audio::PersistentInput
{
	using namespace Microsoft::WRL;

	UINT64 CaptureTime()
	{
		LARGE_INTEGER counter{}, frequency{};
		QueryPerformanceCounter(&counter);
		QueryPerformanceFrequency(&frequency);
		return static_cast<UINT64>(counter.QuadPart / frequency.QuadPart) * 10000000
			+ static_cast<UINT64>(counter.QuadPart % frequency.QuadPart) * 10000000 / frequency.QuadPart;
	}

	bool IsCaptureFormat(const WAVEFORMATEX* format)
	{
		if (!format || format->nSamplesPerSec != SAMPLE_RATE || format->nChannels < 1 || format->nChannels > 2) return false;
		WORD tag = format->wFormatTag;
		if (tag == WAVE_FORMAT_EXTENSIBLE)
		{
			if (format->cbSize < 22) return false;
			const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
			if (extended->Samples.wValidBitsPerSample != format->wBitsPerSample) return false;
			if (extended->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
			else if (extended->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
			else return false;
		}
		return ((tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16)
			|| (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32))
			&& format->nBlockAlign == format->nChannels * format->wBitsPerSample / 8
			&& format->nAvgBytesPerSec == SAMPLE_RATE * format->nBlockAlign;
	}

	class CaptureNotifications final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMNotificationClient, FtmBase>
	{
	public:
		explicit CaptureNotifications(HANDLE changed) : changed(changed) {}
		HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { ::SetEvent(changed); return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { ::SetEvent(changed); return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { ::SetEvent(changed); return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
	private:
		HANDLE changed;
	};

	CaptureSession::CaptureSession(std::wstring endpointId, bool enableHardware, bool gatedByPlayerTwoToggle)
		: selectedEndpoint(std::move(endpointId)), enableHardware(enableHardware), gatedByPlayerTwoToggle(gatedByPlayerTwoToggle)
	{
		volume = GetCableVolume();
		if (!volume) throw std::bad_alloc();
		quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		changeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (!quitEvent || !changeEvent || !captureEvent || !timer)
		{
			CloseEvents();
			throw std::runtime_error("Could not create persistent input events");
		}
	}

	CaptureSession::~CaptureSession()
	{
		::SetEvent(quitEvent);
		if (worker.joinable()) worker.join();
		if (scheduler.joinable()) scheduler.join();
		CloseEvents();
	}

	void CaptureSession::CloseEvents()
	{
		for (HANDLE event : { quitEvent, changeEvent, captureEvent, timer, gameEvent })
		{
			if (event) CloseHandle(event);
		}
		quitEvent = changeEvent = captureEvent = timer = gameEvent = nullptr;
	}

	HRESULT CaptureSession::Initialize(const WAVEFORMATEX* format, DWORD flags, REFERENCE_TIME duration)
	{
		std::lock_guard<std::mutex> guard(controlMutex);
		if (initialized) return AUDCLNT_E_ALREADY_INITIALIZED;
		if (!IsCaptureFormat(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
		if (!(flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK)
			|| (flags & ~(AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST))) return E_INVALIDARG;
		if (duration < 0 || duration > 10000000) return E_INVALIDARG;
		const UINT64 frames = duration ? (duration * SAMPLE_RATE + 9999999) / 10000000 : 480;
		if (frames < 48 || frames > MAX_PACKET_FRAMES) return AUDCLNT_E_INVALID_DEVICE_PERIOD;
		periodFrames = static_cast<UINT32>(frames);
		gameFormat = *format;
		floatFormat = format->wBitsPerSample == 32;
		try
		{
			if (enableHardware)
			{
				scheduler = std::thread(&CaptureSession::RunClock, this);
				worker = std::thread(&CaptureSession::Run, this);
			}
		}
		catch (const std::exception&)
		{
			::SetEvent(quitEvent);
			if (scheduler.joinable()) scheduler.join();
			ResetEvent(quitEvent);
			return E_OUTOFMEMORY;
		}
		initialized = true;
		return S_OK;
	}

	HRESULT CaptureSession::SetEvent(HANDLE event)
	{
		std::lock_guard<std::mutex> control(controlMutex);
		if (!initialized) return AUDCLNT_E_NOT_INITIALIZED;
		if (!event) return E_INVALIDARG;
		std::lock_guard<std::mutex> guard(packetMutex);
		if (gameEvent) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
		if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &gameEvent, 0, FALSE, DUPLICATE_SAME_ACCESS))
			return HRESULT_FROM_WIN32(GetLastError());
		return S_OK;
	}

	HRESULT CaptureSession::Start()
	{
		std::lock_guard<std::mutex> control(controlMutex);
		if (!initialized) return AUDCLNT_E_NOT_INITIALIZED;
		std::lock_guard<std::mutex> guard(packetMutex);
		if (!gameEvent) return AUDCLNT_E_EVENTHANDLE_NOT_SET;
		if (running) return AUDCLNT_E_NOT_STOPPED;
		running = true;
		::SetEvent(changeEvent);
		return S_OK;
	}

	HRESULT CaptureSession::Stop()
	{
		std::lock_guard<std::mutex> control(controlMutex);
		if (!initialized) return AUDCLNT_E_NOT_INITIALIZED;
		std::lock_guard<std::mutex> guard(packetMutex);
		running = false;
		deviceResetRequested = true;
		physicalPacket = false;
		availableFrames = readyFrames = 0;
		if (!outstandingFrames) retainedPacket = false;
		++generation;
		::SetEvent(changeEvent);
		return S_OK;
	}

	void CaptureSession::ClearPackets()
	{
		availableFrames = readyFrames = readFrame = 0;
		discontinuityPending = true;
		physicalPacket = false;
		lastPhysicalTick = 0;
	}

	HRESULT CaptureSession::Reset()
	{
		std::lock_guard<std::mutex> control(controlMutex);
		if (!initialized) return AUDCLNT_E_NOT_INITIALIZED;
		std::lock_guard<std::mutex> guard(packetMutex);
		if (running) return AUDCLNT_E_NOT_STOPPED;
		if (outstandingFrames) return AUDCLNT_E_BUFFER_OPERATION_PENDING;
		ClearPackets();
		deviceResetRequested = true;
		retainedPacket = false;
		timelineFrames = 0;
		++generation;
		return S_OK;
	}

	void CaptureSession::Disconnect()
	{
		std::lock_guard<std::mutex> guard(packetMutex);
		ClearPackets();
		++generation;
		// An acquired packet stays valid until ReleaseBuffer; a released unread packet is obsolete.
		if (!outstandingFrames) retainedPacket = false;
	}

	void CaptureSession::Publish(const float* samples, UINT32 frames, UINT64 timestamp, bool discontinuity, bool timestampError)
	{
		if (!samples || !frames || frames > RING_FRAMES) return;
		std::unique_lock<std::mutex> guard(packetMutex);
		if (!running || deviceResetRequested) return;
		if (availableFrames + frames > RING_FRAMES || discontinuity)
		{
			availableFrames = 0;
			discontinuityPending = true;
		}
		for (UINT32 index = 0; index < frames; ++index)
		{
			const auto offset = (readFrame + availableFrames + index) % RING_FRAMES;
			ring[offset] = samples[index];
			timestampErrors[offset] = timestampError;
			timestamps[offset] = timestamp + UINT64(index) * 10000000 / SAMPLE_RATE;
		}
		availableFrames += frames;
		lastPhysicalTick = GetTickCount64();
		const bool ready = QueueCapturedPacket();
		guard.unlock();
		if (ready && gameEvent) ::SetEvent(gameEvent);
	}

	// Called under packetMutex. Physical delivery follows capture, not the silence timer.
	bool CaptureSession::QueueCapturedPacket()
	{
		if (!running || availableFrames < periodFrames) return false;
		if (!readyFrames)
		{
			readyFrames = periodFrames;
			readyPosition = timelineFrames;
			// Virtual clock follows delivery time; GetBuffer reports the hardware sample timestamp separately.
			readyTimestamp = CaptureTime() - UINT64(periodFrames) * 10000000 / SAMPLE_RATE;
			timelineFrames += periodFrames;
		}
		return true;
	}

	void CaptureSession::Tick(UINT64 timestamp)
	{
		std::unique_lock<std::mutex> guard(packetMutex);
		if (!running) return;
		const UINT64 lastCapture = lastPhysicalTick.load();
		const UINT64 captureGraceMs = (UINT64(periodFrames) * 2000 + SAMPLE_RATE - 1) / SAMPLE_RATE;
		if (lastCapture && GetTickCount64() - lastCapture <= captureGraceMs) return;
		if (readyFrames) discontinuityPending = true;
		readyFrames = periodFrames;
		readyPosition = timelineFrames;
		readyTimestamp = timestamp;
		timelineFrames += periodFrames;
		guard.unlock();
		if (gameEvent) ::SetEvent(gameEvent);
	}

	HRESULT CaptureSession::GetBuffer(BYTE** data, UINT32* frames, DWORD* flags, UINT64* position, UINT64* timestamp)
	{
		if (!data || !frames || !flags) return E_POINTER;
		if (!initialized) return AUDCLNT_E_NOT_INITIALIZED;
		std::unique_lock<std::mutex> guard(packetMutex, std::try_to_lock);
		if (!guard.owns_lock()) { *frames = 0; return AUDCLNT_S_BUFFER_EMPTY; }
		if (outstandingFrames) return AUDCLNT_E_OUT_OF_ORDER;
		if (!running || (!readyFrames && !retainedPacket)) { *frames = 0; return AUDCLNT_S_BUFFER_EMPTY; }
		if (!retainedPacket)
		{
			packetPosition = readyPosition;
			packetTimestamp = readyTimestamp;
			packetGeneration = generation;
			readyFrames = 0;
			packetFlags = discontinuityPending ? AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY : 0;
			discontinuityPending = false;
			// The Player 2 cable stream stays open (the game never re-enumerates devices after boot), so the
			// toggle is applied here, per packet: cable audio while it is on, silence while it is off. That is
			// what makes "Use the Real Tone Cable for Player 2" a hot swap at any point in the game.
			const bool gateOpen = !gatedByPlayerTwoToggle || IsCableForPlayerTwoEnabled();
			const bool physical = gateOpen && availableFrames >= periodFrames && GetTickCount64() - lastPhysicalTick.load() < 1000;
			physicalPacket = physical;
			if (physical)
			{
				// Bound latency after a delayed consumer, without an ever-growing input backlog.
				if (availableFrames > periodFrames * 2)
				{
					const UINT32 discarded = availableFrames - periodFrames;
					readFrame = (readFrame + discarded) % RING_FRAMES;
					availableFrames = periodFrames;
					packetFlags |= AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
				}
				packetTimestamp = timestamps[readFrame];
				float packetPeak = 0.0f;
				for (UINT32 frame = 0; frame < periodFrames; ++frame)
				{
					if (timestampErrors[(readFrame + frame) % RING_FRAMES]) packetFlags |= AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR;
					const float input = ring[(readFrame + frame) % RING_FRAMES] * volume->GetGain();
					const float value = std::isfinite(input) ? std::clamp(input, -1.0f, 1.0f) : 0.0f;
					packetPeak = std::max(packetPeak, std::fabs(value));
					for (UINT32 channel = 0; channel < gameFormat.nChannels; ++channel)
					{
						const UINT32 sample = frame * gameFormat.nChannels + channel;
						if (floatFormat) std::memcpy(packet.data() + sample * 4, &value, 4);
						else
						{
							const int16_t pcm = value >= 1.0f ? 32767 : static_cast<int16_t>(value * 32768.0f);
							std::memcpy(packet.data() + sample * 2, &pcm, 2);
						}
					}
				}
				readFrame = (readFrame + periodFrames) % RING_FRAMES;
				availableFrames -= periodFrames;
				// Player 2's SIGNAL row: this wrapper is the only observer of the Player 2 cable, so
				// the meter is fed here, per packet the game consumes, with the level the game gets.
				if (gatedByPlayerTwoToggle) CableInput::ReportPlayerTwoPacket(packetPeak, false);
			}
			else
			{
				std::memset(packet.data(), 0, periodFrames * gameFormat.nBlockAlign);
				packetFlags |= AUDCLNT_BUFFERFLAGS_SILENT;
				// Gate open but no cable audio (unplugged, or the capture thread stalled): a silent
				// packet keeps the row alive at -90 dB, which is the honest reading. Gate closed
				// reports nothing, so the row falls to its "cable off" state instead.
				if (gatedByPlayerTwoToggle && gateOpen) CableInput::ReportPlayerTwoPacket(0.0f, true);
			}
		}
		retainedPacket = false;
		outstandingFrames = periodFrames;
		*data = packet.data(); *frames = periodFrames; *flags = packetFlags;
		if (position) *position = packetPosition;
		if (timestamp) *timestamp = packetTimestamp;
		return S_OK;
	}

	HRESULT CaptureSession::ReleaseBuffer(UINT32 frames)
	{
		std::unique_lock<std::mutex> guard(packetMutex);
		if (!outstandingFrames) return frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
		if (frames != 0 && frames != outstandingFrames) return AUDCLNT_E_INVALID_SIZE;
		retainedPacket = running && frames == 0 && packetGeneration == generation;
		outstandingFrames = 0;
		const bool ready = !retainedPacket && QueueCapturedPacket();
		guard.unlock();
		if (ready && gameEvent) ::SetEvent(gameEvent);
		return S_OK;
	}

	HRESULT CaptureSession::GetNextPacketSize(UINT32* frames)
	{
		if (!frames) return E_POINTER;
		if (!initialized) return AUDCLNT_E_NOT_INITIALIZED;
		std::unique_lock<std::mutex> guard(packetMutex, std::try_to_lock);
		*frames = guard.owns_lock() && running && (readyFrames || retainedPacket) ? periodFrames : 0;
		return S_OK;
	}

	bool CaptureSession::IsPhysicalPacket() const { return running && !deviceResetRequested && physicalPacket && GetTickCount64() - lastPhysicalTick.load() < 1000; }
	UINT64 CaptureSession::GetGeneration() const { return generation; }
	bool CaptureSession::IsInitialized() const { return initialized; }
	UINT32 CaptureSession::GetPeriodFrames() const { return periodFrames; }

	HRESULT CaptureSession::GetClockPosition(UINT64* position, UINT64* timestamp)
	{
		if (!position) return E_POINTER;
		if (!initialized) return AUDCLNT_E_NOT_INITIALIZED;
		std::lock_guard<std::mutex> guard(packetMutex);
		*position = timelineFrames;
		if (timestamp) *timestamp = timelineFrames ? readyTimestamp + UINT64(periodFrames) * 10000000 / SAMPLE_RATE : CaptureTime();
		return S_OK;
	}

	HRESULT CaptureSession::OpenDevice(IMMDeviceEnumerator* enumerator)
	{
		ComPtr<IMMDevice> device;
		HRESULT result = S_OK;
		if (!selectedEndpoint.empty()) result = enumerator->GetDevice(selectedEndpoint.c_str(), &device);
		else
		{
			ComPtr<IMMDeviceCollection> devices;
			result = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices);
			UINT count = 0;
			if (SUCCEEDED(result)) result = devices->GetCount(&count);
			std::vector<std::pair<std::wstring, ComPtr<IMMDevice>>> cables;
			for (UINT index = 0; SUCCEEDED(result) && index < count; ++index)
			{
				ComPtr<IMMDevice> candidate;
				result = devices->Item(index, &candidate);
				LPWSTR candidateId = nullptr;
				if (SUCCEEDED(result) && IsCable(candidate.Get()) && SUCCEEDED(candidate->GetId(&candidateId)))
				{
					cables.emplace_back(candidateId, candidate);
					CoTaskMemFree(candidateId);
				}
			}
			// Two cables would otherwise return ERROR_DUP_NAME forever, so neither player gets input unless one is
			// picked by hand. Split by endpoint ID instead: Player 1 takes the first, the Player 2
			// cable (the one beside an RS_ASIO input) the second. The proxy's speaker-mode reader applies the same
			// rule for Player 1 (VirtualCableCapture::OpenDevice), so the two never read the same cable.
			if (SUCCEEDED(result) && !cables.empty())
			{
				std::sort(cables.begin(), cables.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
				const size_t pick = cables.size() > 1 && gatedByPlayerTwoToggle ? 1 : 0;
				device = cables[pick].second;
				static std::atomic<bool> reported{ false };
				if (cables.size() > 1 && !reported.exchange(true))
					LOG_INFO("(PERSISTENT INPUT) " << cables.size() << " Real Tone Cables with none selected: "
						<< (gatedByPlayerTwoToggle ? "Player 2 takes the second" : "Player 1 takes the first") << " by endpoint ID" << std::endl);
			}
		}
		if (FAILED(result)) return result;
		if (!device) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
		ComPtr<IMMEndpoint> direction;
		EDataFlow flow = eAll;
		result = device.As(&direction);
		if (SUCCEEDED(result)) result = direction->GetDataFlow(&flow);
		if (FAILED(result) || flow != eCapture) return FAILED(result) ? result : E_INVALIDARG;
		LPWSTR id = nullptr;
		result = device->GetId(&id);
		if (FAILED(result)) return result;
		activeEndpoint = id;
		CoTaskMemFree(id);
		result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &backend);
		WAVEFORMATEX format{ WAVE_FORMAT_IEEE_FLOAT, 1, SAMPLE_RATE, SAMPLE_RATE * 4, 4, 32, 0 };
		if (SUCCEEDED(result)) result = backend->Initialize(AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
			100000, 0, &format, nullptr);
		if (SUCCEEDED(result)) result = backend->SetEventHandle(captureEvent);
		if (SUCCEEDED(result)) result = backend->GetService(IID_PPV_ARGS(&capture));
		if (SUCCEEDED(result)) result = backend->Start();
		if (FAILED(result)) { CloseDevice(); return result; }
		Disconnect();
		LOG_INFO("(PERSISTENT INPUT) Physical capture opened; game client retained" << std::endl);
		return S_OK;
	}

	HRESULT CaptureSession::DrainDevice()
	{
		// Bound each wake so a noisy backend cannot starve stop/device-change handling.
		for (UINT iteration = 0; iteration < 32; ++iteration)
		{
			BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 timestamp = 0;
			HRESULT result = capture->GetBuffer(&data, &frames, &flags, nullptr, &timestamp);
			if (FAILED(result) || result == AUDCLNT_S_BUFFER_EMPTY) return FAILED(result) ? result : S_OK;
			if (!frames) return S_OK;
			if (frames > RING_FRAMES) { capture->ReleaseBuffer(frames); return AUDCLNT_E_BUFFER_SIZE_ERROR; }
			std::array<float, RING_FRAMES> silence{};
			if (!data && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) { capture->ReleaseBuffer(frames); return E_POINTER; }
			Publish(flags & AUDCLNT_BUFFERFLAGS_SILENT ? silence.data() : reinterpret_cast<float*>(data), frames,
				timestamp, (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0,
				(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0);
			result = capture->ReleaseBuffer(frames);
			if (FAILED(result)) return result;
		}
		return S_OK;
	}

	void CaptureSession::CloseDevice()
	{
		if (backend) backend->Stop();
		capture.Reset(); backend.Reset(); activeEndpoint.clear();
		Disconnect();
	}

	void CaptureSession::Run()
	{
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		ComPtr<IMMDeviceEnumerator> enumerator;
		auto notifications = Make<CaptureNotifications>(changeEvent);
		HRESULT result = FAILED(com) ? com : CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
		bool registered = false;
		if (SUCCEEDED(result) && notifications)
		{
			result = enumerator->RegisterEndpointNotificationCallback(notifications.Get());
			registered = SUCCEEDED(result);
		}
		if (FAILED(result)) LOG_ERROR("(PERSISTENT INPUT) Device notifications unavailable: " << std::hex << result << std::dec << std::endl);
		DWORD taskIndex = 0;
		HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
		UINT64 nextOpen = 0;
		HANDLE events[] = { quitEvent, changeEvent, captureEvent };
		try
		{
			for (;;)
			{
				const DWORD wake = WaitForMultipleObjects(3, events, FALSE, 1000);
				if (wake == WAIT_OBJECT_0 || wake == WAIT_FAILED) break;
				if (deviceResetRequested.exchange(false)) CloseDevice();
				if (!running && backend) CloseDevice();
				if (wake == WAIT_OBJECT_0 + 1)
				{
					nextOpen = 0;
					if (backend)
					{
						ComPtr<IMMDevice> device;
						DWORD state = 0;
						if (FAILED(enumerator->GetDevice(activeEndpoint.c_str(), &device)) || FAILED(device->GetState(&state)) || !(state & DEVICE_STATE_ACTIVE))
							CloseDevice();
					}
				}
				const UINT64 now = CaptureTime();
				if (running && enumerator && !backend && now >= nextOpen)
				{
					result = OpenDevice(enumerator.Get());
					nextOpen = now + 10000000;
					if (FAILED(result) && result != lastOpenResult)
						LOG_WARNING("(PERSISTENT INPUT) Waiting for selected cable, HRESULT " << std::hex << result << std::dec << std::endl);
					lastOpenResult = result;
				}
				if (running && capture)
				{
					result = DrainDevice();
					if (FAILED(result))
					{
						LOG_WARNING("(PERSISTENT INPUT) Capture lost, HRESULT " << std::hex << result << std::dec << "; retaining silent game stream" << std::endl);
						CloseDevice();
						nextOpen = now + 10000000;
					}
				}
			}
		}
		catch (const std::exception& error) { LOG_ERROR("(PERSISTENT INPUT) Worker failed: " << error.what() << std::endl); }
		CloseDevice();
		if (registered) enumerator->UnregisterEndpointNotificationCallback(notifications.Get());
		notifications.Reset(); enumerator.Reset();
		if (task) AvRevertMmThreadCharacteristics(task);
		if (SUCCEEDED(com)) CoUninitialize();
	}

	void CaptureSession::RunClock()
	{
		DWORD index = 0;
		HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
		const UINT64 period = UINT64(periodFrames) * 10000000 / SAMPLE_RATE;
		UINT64 next = CaptureTime() + period;
		HANDLE events[] = { quitEvent, timer };
		for (;;)
		{
			const UINT64 before = CaptureTime();
			LARGE_INTEGER due{};
			due.QuadPart = -static_cast<LONGLONG>(next > before ? next - before : 1);
			if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
			{
				LOG_ERROR("(PERSISTENT INPUT) Clock timer failed: " << GetLastError() << std::endl);
				break;
			}
			if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
			const UINT64 now = CaptureTime();
			Tick(now - period);
			next = now - next > period ? now + period : next + period;
		}
		if (task) AvRevertMmThreadCharacteristics(task);
	}

	class PersistentCaptureClient final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IAudioCaptureClient, ICaptureState, FtmBase>
	{
	public:
		explicit PersistentCaptureClient(std::shared_ptr<CaptureSession> session) : session(std::move(session)) {}
		HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** data, UINT32* frames, DWORD* flags, UINT64* position, UINT64* timestamp) override { return session->GetBuffer(data, frames, flags, position, timestamp); }
		HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames) override { return session->ReleaseBuffer(frames); }
		HRESULT STDMETHODCALLTYPE GetNextPacketSize(UINT32* frames) override { return session->GetNextPacketSize(frames); }
		BOOL STDMETHODCALLTYPE IsPhysicalPacket() override { return session->IsPhysicalPacket(); }
		UINT64 STDMETHODCALLTYPE GetGeneration() override { return session->GetGeneration(); }
	private:
		std::shared_ptr<CaptureSession> session;
	};

	class CaptureClock final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IAudioClock, FtmBase>
	{
	public:
		explicit CaptureClock(std::shared_ptr<CaptureSession> session) : session(std::move(session)) {}
		HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* frequency) override { if (!frequency) return E_POINTER; *frequency = SAMPLE_RATE; return S_OK; }
		HRESULT STDMETHODCALLTYPE GetPosition(UINT64* position, UINT64* timestamp) override { return session->GetClockPosition(position, timestamp); }
		HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* characteristics) override { if (!characteristics) return E_POINTER; *characteristics = AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ; return S_OK; }
	private:
		std::shared_ptr<CaptureSession> session;
	};

	class PersistentAudioClient final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IAudioClient, FtmBase>
	{
	public:
		explicit PersistentAudioClient(std::shared_ptr<CaptureSession> session) : session(std::move(session))
		{
			capture = Make<PersistentCaptureClient>(this->session);
			clock = Make<CaptureClock>(this->session);
			if (!capture || !clock) throw std::bad_alloc();
		}
		~PersistentAudioClient() { if (session->IsGatedByPlayerTwoToggle()) LOG_INFO("(P2 CABLE SCAN) client released" << std::endl); }
		HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME duration, REFERENCE_TIME periodicity, const WAVEFORMATEX* format, LPCGUID) override
		{
			if (session->IsGatedByPlayerTwoToggle()) LOG_INFO("(P2 CABLE SCAN) client Initialize mode " << mode << " flags 0x" << std::hex << flags << std::dec << std::endl);
			if (mode != AUDCLNT_SHAREMODE_EXCLUSIVE || periodicity < 0 || (periodicity && periodicity != duration)) return E_INVALIDARG;
			return session->Initialize(format, flags, duration);
		}
		HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override
		{
			if (!frames) return E_POINTER;
			if (!session->IsInitialized()) return AUDCLNT_E_NOT_INITIALIZED;
			*frames = session->GetPeriodFrames(); return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* latency) override
		{
			if (!latency) return E_POINTER;
			if (!session->IsInitialized()) return AUDCLNT_E_NOT_INITIALIZED;
			*latency = UINT64(session->GetPeriodFrames()) * 10000000 / SAMPLE_RATE + 100000; return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* frames) override { return session->GetNextPacketSize(frames); }
		HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX* format, WAVEFORMATEX** closest) override
		{
			if (session->IsGatedByPlayerTwoToggle()) LOG_INFO("(P2 CABLE SCAN) client IsFormatSupported" << std::endl);
			if (closest) *closest = nullptr;
			return IsCaptureFormat(format) ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
		}
		HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override
		{
			if (session->IsGatedByPlayerTwoToggle()) LOG_INFO("(P2 CABLE SCAN) client GetMixFormat" << std::endl);
			if (!format) return E_POINTER;
			*format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
			if (!*format) return E_OUTOFMEMORY;
			**format = { WAVE_FORMAT_PCM, 2, SAMPLE_RATE, SAMPLE_RATE * 4, 4, 16, 0 }; return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* normal, REFERENCE_TIME* minimum) override
		{
			if (session->IsGatedByPlayerTwoToggle()) LOG_INFO("(P2 CABLE SCAN) client GetDevicePeriod" << std::endl);
			if (!normal && !minimum) return E_POINTER;
			if (normal) *normal = 100000;
			if (minimum) *minimum = 100000;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE Start() override { return session->Start(); }
		HRESULT STDMETHODCALLTYPE Stop() override { return session->Stop(); }
		HRESULT STDMETHODCALLTYPE Reset() override { return session->Reset(); }
		HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE event) override { return session->SetEvent(event); }
		HRESULT STDMETHODCALLTYPE GetService(REFIID id, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (!session->IsInitialized()) return AUDCLNT_E_NOT_INITIALIZED;
			if (id == __uuidof(IAudioCaptureClient)) return capture.CopyTo(id, object);
			if (id == __uuidof(IAudioClock)) return clock.CopyTo(id, object);
			return E_NOINTERFACE;
		}
	private:
		std::shared_ptr<CaptureSession> session;
		ComPtr<PersistentCaptureClient> capture;
		ComPtr<CaptureClock> clock;
	};

	HRESULT CreateCaptureClient(const std::wstring& endpointId, IAudioClient** client, bool gatedByPlayerTwoToggle)
	{
		if (!client) return E_POINTER;
		*client = nullptr;
		try
		{
			auto result = Make<PersistentAudioClient>(std::make_shared<CaptureSession>(endpointId, true, gatedByPlayerTwoToggle));
			return result ? result.CopyTo(client) : E_OUTOFMEMORY;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (const std::exception& error)
		{
			LOG_ERROR("(PERSISTENT INPUT) Client creation failed: " << error.what() << std::endl);
			return E_FAIL;
		}
	}
}
