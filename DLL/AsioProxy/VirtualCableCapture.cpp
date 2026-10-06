#include "VirtualCableCapture.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <utility>
#include <vector>
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

namespace AsioProxy
{
	using Microsoft::WRL::ComPtr;

	namespace
	{
		constexpr PROPERTYKEY DEVICE_INSTANCE = { { 0xb3f8fa53, 0x0004, 0x438e, { 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc } }, 2 };
		constexpr PROPERTYKEY USB_FUNCTION_INSTANCE = { { 0xb3f8fa53, 0x0004, 0x438e, { 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc } }, 39 };
		constexpr wchar_t CABLE_IDENTITY[] = L"VID_12BA&PID_00FF";

		bool HasCableIdentity(IPropertyStore* properties)
		{
			if (!properties) return false;
			for (const PROPERTYKEY& key : { DEVICE_INSTANCE, USB_FUNCTION_INSTANCE })
			{
				PROPVARIANT value{};
				const HRESULT result = properties->GetValue(key, &value);
				bool matches = false;
				if (SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal)
				{
					constexpr size_t identityLength = _countof(CABLE_IDENTITY) - 1;
					for (const wchar_t* candidate = value.pwszVal; *candidate; ++candidate)
					{
						if (_wcsnicmp(candidate, CABLE_IDENTITY, identityLength) != 0) continue;
						const wchar_t following = candidate[identityLength];
						matches = following == L'\0' || following == L'&' || following == L'\\' || following == L'#';
						if (matches) break;
					}
				}
				PropVariantClear(&value);
				if (matches) return true;
			}
			return false;
		}

		class Notifications final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMMNotificationClient>
		{
		public:
			explicit Notifications(HANDLE wake) : wake(wake) {}
			HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { SetEvent(wake); return S_OK; }
			HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { SetEvent(wake); return S_OK; }
			HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { SetEvent(wake); return S_OK; }
			HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { SetEvent(wake); return S_OK; }

		private:
			HANDLE wake;
		};
	}

	VirtualCableCapture::VirtualCableCapture()
	{
		LARGE_INTEGER frequency{};
		if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) qpcFrequency = frequency.QuadPart;
		quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!quitEvent || !wakeEvent || !captureEvent)
		{
			if (quitEvent) CloseHandle(quitEvent);
			if (wakeEvent) CloseHandle(wakeEvent);
			if (captureEvent) CloseHandle(captureEvent);
			quitEvent = wakeEvent = captureEvent = nullptr;
		}
	}

	VirtualCableCapture::~VirtualCableCapture()
	{
		Stop();
		if (quitEvent) CloseHandle(quitEvent);
		if (wakeEvent) CloseHandle(wakeEvent);
		if (captureEvent) CloseHandle(captureEvent);
	}

	bool VirtualCableCapture::Start()
	{
		if (!quitEvent || !wakeEvent || !captureEvent) return false;
		if (running.exchange(true, std::memory_order_acq_rel)) return true;
		ResetEvent(quitEvent);
		largestSpan.store(0, std::memory_order_relaxed);
		spanWindowMax = 0; spanPreviousMax = 0; spanWindowCount = 0;
		lastDeliveryTicks = 0;
		resetRequested.store(true, std::memory_order_release);
		if (testMode.load(std::memory_order_acquire)) return true;
		try { worker = std::thread(&VirtualCableCapture::Run, this); }
		catch (...) { running.store(false, std::memory_order_release); return false; }
		SetEvent(wakeEvent);
		return true;
	}

	void VirtualCableCapture::Stop()
	{
		if (!running.exchange(false, std::memory_order_acq_rel)) return;
		if (worker.joinable())
		{
			SetEvent(quitEvent);
			SetEvent(wakeEvent);
			worker.join();
			ResetEvent(quitEvent);
			CloseDevice();
		}
		resetRequested.store(true, std::memory_order_release);
	}

	// Reader side: drop whatever is buffered and wait to prime again. The learned clock correction
	// (integralPpm) survives unless the caller clears it: the two clocks did not change.
	void VirtualCableCapture::ResetReader()
	{
		readIndex = writeFrame.load(std::memory_order_acquire);
		readFrame.store(readIndex, std::memory_order_release);
		readPhase = 0.0;
		primed = false;
		fadeIn = 0;
		windowMinFill = INT32_MAX;
		windowFrames = 0;
		correctionPpm = integralPpm;
		primedFlag.store(0, std::memory_order_relaxed);
	}

	void VirtualCableCapture::Read(int32_t* stereoSamples, uint32_t frames)
	{
		if (!stereoSamples || !frames) return;
		if (resetRequested.exchange(false, std::memory_order_acq_rel))
		{
			integralPpm = 0.0;
			margin = MARGIN_FRAMES;
			lastMargin.store(margin, std::memory_order_relaxed);
			ResetReader();
		}
		const auto silence = [&] { std::memset(stereoSamples, 0, static_cast<size_t>(frames) * 2 * sizeof(int32_t)); };

		const uint64_t write = writeFrame.load(std::memory_order_acquire);
		const int64_t fill = static_cast<int64_t>(write - readIndex);
		lastFill.store(static_cast<int32_t>(fill), std::memory_order_relaxed);
		// The fill saw-tooths: +one delivery when it arrives, -one block per read. Keep its MINIMUM at one
		// block plus a margin, so the read just before the next delivery still finds a full block. Start
		// one delivery jitter span above that: however the deliveries bunch up, the fill then bottoms out at
		// the target.
		const int32_t target = static_cast<int32_t>(frames) + margin;
		const int32_t spanFrames = static_cast<int32_t>(largestSpan.load(std::memory_order_relaxed));
		const int64_t startFill = static_cast<int64_t>(target) + spanFrames + 2;   // +2: interpolation look-ahead

		if (!primed)
		{
			if (fill < startFill) { silence(); return; }
			if (fill > startFill) readIndex = write - static_cast<uint64_t>(startFill);   // start at the target, not behind a backlog
			readPhase = 0.0;
			primed = true;
			fadeIn = FADE_FRAMES;
			windowMinFill = INT32_MAX;
			windowFrames = 0;
			primedFlag.store(1, std::memory_order_relaxed);
		}
		else if (fill > startFill + 4800)
		{
			// A backlog far beyond what the drift correction trims (device burst, long stall): jump to the
			// target instead of carrying 100+ ms of extra latency.
			readIndex = write - static_cast<uint64_t>(startFill);
			readPhase = 0.0;
			fadeIn = FADE_FRAMES;
			skips.fetch_add(1, std::memory_order_relaxed);
		}

		const double ratio = 1.0 + correctionPpm * 1e-6;
		const double span = readPhase + static_cast<double>(frames - 1) * ratio;
		if (readIndex + static_cast<uint64_t>(span) + 3 > write)
		{
			// Ran dry. One silent block, then prime again (the next block fades back in).
			// The writer was later than the margin allows: widen it so this timing does not repeat.
			underruns.fetch_add(1, std::memory_order_relaxed);
			margin = (std::min)(margin + MARGIN_STEP_FRAMES, MARGIN_MAX_FRAMES);
			lastMargin.store(margin, std::memory_order_relaxed);
			primed = false;
			primedFlag.store(0, std::memory_order_relaxed);
			silence();
			return;
		}
		if (fill < windowMinFill) windowMinFill = static_cast<int32_t>(fill);

		// Cubic Hermite interpolation at the fractional read position. At ratios this close to 1 it is
		// transparent for a guitar signal; linear interpolation would dull the top end as the phase drifts.
		for (uint32_t frame = 0; frame < frames; ++frame)
		{
			const float x0 = At(readIndex - 1), x1 = At(readIndex), x2 = At(readIndex + 1), x3 = At(readIndex + 2);
			const float t = static_cast<float>(readPhase);
			const float c1 = 0.5f * (x2 - x0);
			const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
			const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
			float value = ((c3 * t + c2) * t + c1) * t + x1;
			if (fadeIn > 0) { value *= static_cast<float>(FADE_FRAMES - fadeIn) / FADE_FRAMES; --fadeIn; }
			value = std::clamp(value, -1.0f, 1.0f);
			const int32_t pcm = value >= 1.0f ? INT32_MAX : static_cast<int32_t>(value * 2147483647.0f);
			stereoSamples[frame * 2] = pcm;
			stereoSamples[frame * 2 + 1] = pcm;
			readPhase += ratio;
			const double whole = std::floor(readPhase);
			readIndex += static_cast<uint64_t>(whole);
			readPhase -= whole;
		}
		readFrame.store(readIndex - 1, std::memory_order_release);   // keep x0 of the next block readable

		// Drift servo, once per ~250 ms: steer the fill minimum to the target. Positive error = too much
		// buffered = consume slightly faster. PI: the integral learns the cable-vs-PC clock offset (tens to
		// hundreds of ppm), the proportional term removes the residual. 3000 ppm is ~5 cents at the clamp;
		// in steady state it sits at the clock offset, far below audibility.
		windowFrames += frames;
		if (windowFrames >= SERVO_WINDOW_FRAMES && windowMinFill != INT32_MAX)
		{
			const double error = static_cast<double>(windowMinFill - target);
			integralPpm = std::clamp(integralPpm + 0.5 * error, -1500.0, 1500.0);
			correctionPpm = std::clamp(integralPpm + 6.0 * error, -3000.0, 3000.0);
			windowMinFill = INT32_MAX;
			windowFrames = 0;
			lastPpm.store(static_cast<int32_t>(std::lround(correctionPpm)), std::memory_order_relaxed);
		}
	}

	VirtualCableCapture::Stats VirtualCableCapture::GetStats() const
	{
		Stats stats;
		stats.underruns = underruns.load(std::memory_order_relaxed);
		stats.overflowDrops = overflowDrops.load(std::memory_order_relaxed);
		stats.skips = skips.load(std::memory_order_relaxed);
		stats.deviceGlitches = deviceGlitches.load(std::memory_order_relaxed);
		stats.fillFrames = lastFill.load(std::memory_order_relaxed);
		stats.correctionPpm = lastPpm.load(std::memory_order_relaxed);
		stats.primed = primedFlag.load(std::memory_order_relaxed);
		stats.marginFrames = lastMargin.load(std::memory_order_relaxed);
		return stats;
	}

	void VirtualCableCapture::InjectForTest(const int32_t* stereoSamples, uint32_t frames)
	{
		if (!stereoSamples || !frames) return;
		uint32_t offset = 0;
		while (offset < frames)
		{
			const uint32_t count = (std::min)(MAX_PACKET_FRAMES, frames - offset);
			for (uint32_t frame = 0; frame < count; ++frame)
				packet[frame] = static_cast<float>(stereoSamples[(offset + frame) * 2] / 2147483648.0);
			PushMono(packet.data(), count);
			offset += count;
		}
	}

	// Writer side (capture thread, or the test injector; never both). Lock-free: publish writeFrame after
	// the samples are in place. A full ring drops the incoming packet rather than touching the reader's
	// position; the reader's backlog jump recovers the latency afterwards.
	void VirtualCableCapture::PushMono(const float* samples, uint32_t frames)
	{
		if (!samples || !frames) return;
		// Delivery jitter span for the reader's start fill. The reader drains at a steady 48 kHz, so compare
		// the deliveries with a steady 48 kHz line: level = (frames delivered) - (frames the line has
		// reached). The fill moves with that level, so the deepest fall of the level from an earlier peak
		// (its maximum drawdown) is how far the fill sinks below where it stood just after a delivery. On-time
		// 10 ms packets give exactly one packet (480). A late wake that drains two or three packets back to
		// back, or wakes on a coarse timer that deliver one packet one time and two the next, give more
		// (counting packet sizes alone would miss both). A burst with nothing after it is a rise, not a fall,
		// so it costs nothing; the span is never below the largest packet. A gap past the WASAPI buffer is a
		// stall (data was lost anyway), not the cadence: the line restarts there. The span grows at once
		// and shrinks only after a whole window (~1 s) without it, so a late wake keeps counting across the
		// re-prime it caused. The peak restarts each window, so cable-vs-PC clock drift (which tilts the
		// line) adds at most a window's worth of drift, a few frames.
		LARGE_INTEGER nowTicks{};
		QueryPerformanceCounter(&nowTicks);
		const int64_t now = nowTicks.QuadPart;
		if (!lastDeliveryTicks || (now - lastDeliveryTicks) * 48000 / qpcFrequency > STALL_FRAMES)
		{
			deliveryBaseTicks = now;
			deliveredSinceBase = 0;
			spanPeak = 0;
		}
		lastDeliveryTicks = now;
		const int64_t before = deliveredSinceBase - (now - deliveryBaseTicks) * 48000 / qpcFrequency;
		const int64_t after = before + static_cast<int64_t>(frames);
		deliveredSinceBase += frames;
		const int64_t drawdown = (std::min)(spanPeak - before, MAX_SPAN_FRAMES);
		spanWindowMax = (std::max)(spanWindowMax, static_cast<uint32_t>((std::max)(drawdown, static_cast<int64_t>(frames))));
		spanPeak = (std::max)(spanPeak, after);
		largestSpan.store((std::max)(spanPreviousMax, spanWindowMax), std::memory_order_relaxed);
		if (++spanWindowCount >= SPAN_WINDOW_DELIVERIES)
		{
			spanPreviousMax = spanWindowMax;
			spanWindowMax = 0;
			spanPeak = after;
			spanWindowCount = 0;
		}
		const uint64_t write = writeFrame.load(std::memory_order_relaxed);
		const uint64_t read = readFrame.load(std::memory_order_acquire);
		if (write - read + frames > RING_FRAMES - 8)
		{
			overflowDrops.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		for (uint32_t frame = 0; frame < frames; ++frame)
			ring[static_cast<uint32_t>(write + frame) & RING_MASK] = samples[frame];
		writeFrame.store(write + frames, std::memory_order_release);
	}

	bool VirtualCableCapture::OpenDevice()
	{
		ComPtr<IMMDeviceEnumerator> enumerator;
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return false;
		ComPtr<IMMDeviceCollection> devices;
		if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) return false;
		std::vector<std::pair<std::wstring, ComPtr<IMMDevice>>> cables;
		UINT count = 0;
		if (FAILED(devices->GetCount(&count))) return false;
		for (UINT index = 0; index < count; ++index)
		{
			ComPtr<IMMDevice> candidate;
			if (FAILED(devices->Item(index, &candidate))) continue;
			ComPtr<IPropertyStore> properties;
			LPWSTR candidateId = nullptr;
			if (SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, &properties)) && HasCableIdentity(properties.Get())
				&& SUCCEEDED(candidate->GetId(&candidateId)))
			{
				cables.emplace_back(candidateId, candidate);
				CoTaskMemFree(candidateId);
			}
		}
		if (cables.empty()) return false;
		// With two Real Tone Cables (Player 1 + Player 2), Player 2's cable is the one RSMods.ini [Audio Bridge] InputDevice names in ASIO mode;
		// with that unset, both sides split by endpoint ID independently (Player 1 the first, Player 2 the second,
		// see PersistentCapture::OpenDevice), so they never read the same cable.
		ComPtr<IMMDevice> cable = cables.front().second;
		if (cables.size() > 1)
		{
			std::sort(cables.begin(), cables.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
			wchar_t playerTwo[512]{};
			wchar_t exePath[MAX_PATH]{};
			GetModuleFileNameW(nullptr, exePath, MAX_PATH);
			std::wstring ini = exePath;
			const size_t slash = ini.find_last_of(L"\\/");
			ini = (slash == std::wstring::npos ? std::wstring() : ini.substr(0, slash + 1)) + L"RSMods.ini";
			GetPrivateProfileStringW(L"Audio Bridge", L"InputDevice", L"", playerTwo, 512, ini.c_str());
			cable = cables.front().second;
			for (const auto& entry : cables)
				if (playerTwo[0] == 0 || _wcsicmp(entry.first.c_str(), playerTwo) != 0) { cable = entry.second; break; }
			static bool reported = false;
			if (!reported && logFn)
			{
				reported = true;
				logFn("virtual cable: %u Real Tone Cables; Player 1 takes the %s one (Player 2 %s)", static_cast<unsigned>(cables.size()),
					playerTwo[0] ? "first one not selected for Player 2" : "first by endpoint ID",
					playerTwo[0] ? "uses the RSMods.ini InputDevice" : "takes the second");
			}
		}
		LPWSTR id = nullptr;
		if (FAILED(cable->GetId(&id))) return false;
		activeEndpoint = id; CoTaskMemFree(id);
		ComPtr<IAudioClient> client;
		if (FAILED(cable->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return false;
		WAVEFORMATEX format{ WAVE_FORMAT_IEEE_FLOAT, 1, 48000, 48000 * 4, 4, 32, 0 };
		HRESULT result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
			300000, 0, &format, nullptr);   // 30 ms engine buffer: headroom for a late wake, no added latency
		if (SUCCEEDED(result)) result = client->SetEventHandle(captureEvent);
		ComPtr<IAudioCaptureClient> captureClient;
		if (SUCCEEDED(result)) result = client->GetService(IID_PPV_ARGS(&captureClient));
		if (SUCCEEDED(result)) result = client->Start();
		if (FAILED(result)) return false;
		backend = client;
		capture = captureClient;
		physicalCapture.store(true, std::memory_order_release);
		return true;
	}

	void VirtualCableCapture::CloseDevice()
	{
		if (backend) backend->Stop();
		capture.Reset();
		backend.Reset();
		physicalCapture.store(false, std::memory_order_release);
		activeEndpoint.clear();
	}

	bool VirtualCableCapture::DrainDevice()
	{
		auto* captureClient = capture.Get();
		for (uint32_t attempt = 0; attempt < 32; ++attempt)
		{
			BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
			const HRESULT result = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
			if (result == AUDCLNT_S_BUFFER_EMPTY) return true;
			if (FAILED(result)) return false;
			if (frames)
			{
				if (!data && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) { captureClient->ReleaseBuffer(frames); return false; }
				if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) deviceGlitches.fetch_add(1, std::memory_order_relaxed);
				const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
				const float* source = reinterpret_cast<const float*>(data);
				uint32_t offset = 0;
				while (offset < frames)
				{
					const uint32_t count = (std::min)(MAX_PACKET_FRAMES, frames - offset);
					for (uint32_t frame = 0; frame < count; ++frame)
					{
						const float value = silent ? 0.0f : source[offset + frame];
						packet[frame] = std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
					}
					PushMono(packet.data(), count);
					offset += count;
				}
			}
			if (FAILED(captureClient->ReleaseBuffer(frames))) return false;
		}
		return true;
	}

	void VirtualCableCapture::Run()
	{
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		DWORD taskIndex = 0;
		HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
		ComPtr<IMMDeviceEnumerator> enumerator;
		ComPtr<Notifications> notifications;
		if (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE)
		{
			if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
			{
				notifications = Microsoft::WRL::Make<Notifications>(wakeEvent);
				if (notifications) enumerator->RegisterEndpointNotificationCallback(notifications.Get());
			}
		}
		while (running.load(std::memory_order_acquire))
		{
			const HANDLE events[] = { quitEvent, wakeEvent, captureEvent };
			const DWORD wait = WaitForMultipleObjects(3, events, FALSE, 250);
			if (wait == WAIT_OBJECT_0 || !running.load(std::memory_order_acquire)) break;
			if (backend && enumerator)
			{
				ComPtr<IMMDevice> device; DWORD state = 0;
				if (FAILED(enumerator->GetDevice(activeEndpoint.c_str(), &device)) || FAILED(device->GetState(&state)) || !(state & DEVICE_STATE_ACTIVE)) { CloseDevice(); nextOpenTick = 0; }
			}
			const uint64_t now = GetTickCount64();
			if (!testMode.load(std::memory_order_acquire) && !backend && enumerator && now >= nextOpenTick) { if (OpenDevice()) nextOpenTick = 0; else nextOpenTick = now + 1000; }
			if (capture && !DrainDevice()) { CloseDevice(); nextOpenTick = now + 1000; }
			LogStatsIfChanged();
		}
		if (enumerator && notifications) enumerator->UnregisterEndpointNotificationCallback(notifications.Get());
		CloseDevice();
		if (task) AvRevertMmThreadCharacteristics(task);
		if (SUCCEEDED(com)) CoUninitialize();
	}

	// Capture thread: one line to RocksmithAudioBridge-log.txt every 30 s while capturing, and at once
	// when a glitch counter moves, so the log shows whether the input ever dropped a block.
	void VirtualCableCapture::LogStatsIfChanged()
	{
		if (!logFn || !physicalCapture.load(std::memory_order_acquire)) return;
		const Stats now = GetStats();
		const bool glitch = now.underruns != lastLogged.underruns || now.overflowDrops != lastLogged.overflowDrops
			|| now.skips != lastLogged.skips || now.deviceGlitches != lastLogged.deviceGlitches;
		const uint64_t tick = GetTickCount64();
		if (!glitch && tick < nextLogTick) return;
		nextLogTick = tick + 30000;
		lastLogged = now;
		logFn("cable input: fill %ld frames (margin %ld), drift correction %ld ppm, underruns %lu, overflow drops %lu, skips %lu, device discontinuities %lu%s",
			static_cast<long>(now.fillFrames), static_cast<long>(now.marginFrames), static_cast<long>(now.correctionPpm), static_cast<unsigned long>(now.underruns),
			static_cast<unsigned long>(now.overflowDrops), static_cast<unsigned long>(now.skips),
			static_cast<unsigned long>(now.deviceGlitches), now.primed ? "" : " (priming)");
	}
}
