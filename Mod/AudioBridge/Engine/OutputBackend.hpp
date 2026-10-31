#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <thread>
#include <wrl.h>
#include "../../Shared/OutputGuardDsp.h"
#include "AsioOutputDevice.hpp"
#include "ExclusiveOutputDevice.hpp"
#include "DeviceVolume.hpp"
#include "AppAudioMix.hpp"
#include "../../ExternalAmp/CableAmpLink.hpp"
#include "OutputRateMatcher.hpp"
#include "OutputReserve.hpp"
#include "../../Recorder/GameAudioRecorder.hpp"
#include "../../Core/Text.hpp"

namespace Audio::SharedOutput
{
	using EndpointFactory = std::function<HRESULT(std::wstring&, IAudioClient3**)>;

	class OutputNotifications final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMMNotificationClient, Microsoft::WRL::FtmBase>
	{
	public:
		std::atomic<bool> changed{ false };
		HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
		{
			if (flow == eRender && role == eConsole) changed.store(true);
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { changed.store(true); return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { changed.store(true); return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { changed.store(true); return S_OK; }
		HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
	};

	enum class BackendState { Disconnected, Opening, Priming, Running, Recovering };

	struct OutputBackendStatus
	{
		BackendState state = BackendState::Disconnected;
		HRESULT error = AUDCLNT_E_DEVICE_INVALIDATED;
		std::wstring requestedEndpoint;
		std::wstring activeEndpoint;
		uint64_t generation = 0;
		UINT32 period = 0, minimum = 0, fundamental = 0, maximum = 0;
		UINT32 outputMode = 0; // 0 unknown, 1 shared, 2 exclusive event, 3 exclusive polling, 4 ASIO
		UINT32 outputBufferFrames = 0;
		UINT32 padding = 0, queuedFrames = 0;
		uint64_t expiredFrames = 0, overruns = 0, emptyObservations = 0, trims = 0;   // trims: backlogs dropped at once
		uint64_t trimmedFrames = 0;   // frames the trims dropped (expiredFrames: frames that waited past the age limit)
		uint64_t longestPumpGapMs = 0;
		UINT32 cushionFrames = 0;   // AsioFill's extra reserve after the game was late (0 = the lean minimum)
		bool reserveOn = false;     // AsioFill holds the loading reserve (OutputReserve)
		double clockPpm = 0, correctionPpm = 0;
		bool clockMeasured = false;
	};

	// The driver owner retains its state if a driver call stalls. No caller waits on
	// a driver while holding the virtual clock, game buffer, or status service.
	class OutputBackend final : public std::enable_shared_from_this<OutputBackend>
	{
	public:
		explicit OutputBackend(EndpointFactory factory, bool watchDevices = false,
			Microsoft::WRL::ComPtr<OutputNotifications> notifications = Microsoft::WRL::Make<OutputNotifications>(),
			bool allowDefaultFallback = false)
			: factory(std::move(factory)), watchDevices(watchDevices), notifications(std::move(notifications)), allowDefaultFallback(allowDefaultFallback)
		{
			wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!wake || !finished)
			{
				if (wake) CloseHandle(wake);
				if (finished) CloseHandle(finished);
				throw std::runtime_error("Could not create physical output events");
			}
			OutputGuardDsp::Config guardConfig;
			guardConfig.sampleRate = 48000.0f;
			outputGuard.Configure(guardConfig);
			outputGuard.Reset(2);
		}

		~OutputBackend()
		{
			CloseHandle(wake);
			CloseHandle(finished);
		}

		void Launch()
		{
			// Every backend after the launch's first is the game rebuilding its output client (it does on each focus
			// change). The one it replaces may still hold the device for a moment; see Open.
			static std::atomic<unsigned> launches{ 0 };
			if (launches.fetch_add(1) > 0) handOverUntil = GetTickCount64() + kHandOverMs;
			std::thread([self = shared_from_this()]()
			{
				try { self->Run(); }
				catch (...)
				{
					LOG_ERROR("(AUDIO ROUTING) Physical output worker failed unexpectedly" << std::endl);
					self->Fail(E_UNEXPECTED, self->GetStatus().generation);
					SetEvent(self->finished);
				}
			}).detach();
		}

		void Shutdown()
		{
			quit.store(true);
			SetEvent(wake);
			if (WaitForSingleObject(finished, 250) == WAIT_TIMEOUT)
				LOG_ERROR("(AUDIO ROUTING) Driver shutdown pending; retaining backend resources until its owner returns" << std::endl);
		}

		void Request(const std::wstring& endpoint, UINT32 requestedPeriod)
		{
			std::lock_guard<std::mutex> guard(mutex);
			status.requestedEndpoint = endpoint;
			status.activeEndpoint.clear();
			status.outputMode = 0;
			status.outputBufferFrames = 0;
			++status.generation;
			status.state = BackendState::Opening;
			status.error = E_PENDING;
			desiredPeriod = requestedPeriod;
			count = 0;
			status.queuedFrames = 0;
			cushion = status.cushionFrames = 0;
			ResetReserve();
			SetEvent(wake);
		}

		void SetRunning(bool value)
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (shouldRun == value) return;
			shouldRun = value;
			if (!value)
			{
				count = status.queuedFrames = 0;
				++status.generation;
				status.error = E_PENDING;
				status.activeEndpoint.clear();
				status.outputMode = 0;
				status.outputBufferFrames = 0;
				status.state = BackendState::Opening;
			}
			SetEvent(wake);
		}

		void ReapplyOutput()
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (!status.generation) return;
			++status.generation;
			status.activeEndpoint.clear();
			status.outputMode = 0;
			status.outputBufferFrames = 0;
			status.state = BackendState::Opening;
			status.error = E_PENDING;
			count = status.queuedFrames = 0;
			cushion = status.cushionFrames = 0;
			ResetReserve();
			SetEvent(wake);
		}

		OutputBackendStatus GetStatus()
		{
			std::lock_guard<std::mutex> guard(mutex);
			auto snapshot = status;
			if ((snapshot.state == BackendState::Running || snapshot.state == BackendState::Priming) && lastProgress.load() && GetTickCount64() - lastProgress.load() > 500)
			{
				snapshot.state = BackendState::Recovering;
				snapshot.error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
				snapshot.activeEndpoint.clear();
				snapshot.outputMode = 0;
				snapshot.outputBufferFrames = 0;
			}
			return snapshot;
		}

		void Submit(const float* samples, UINT32 frames, ULONGLONG submittedAt)
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (status.state != BackendState::Running && status.state != BackendState::Priming) return;
			// The longest pause in the game's own writing: how long its audio thread stalled (sizes OutputReserve).
			if (lastSubmittedAt && submittedAt > lastSubmittedAt) longestGameGapMs = (std::max)(longestGameGapMs, submittedAt - lastSubmittedAt);
			lastSubmittedAt = submittedAt;
			Expire(GetTickCount64());
			if (GetTickCount64() - submittedAt > 40)
			{
				status.expiredFrames += frames;
				return;
			}
			if (frames > CAPACITY) return;
			if (frames > CAPACITY - count)
			{
				// Drop only the oldest frames needed to fit the newcomer, not the
				// whole FIFO, and keep the servo's learned correction. A full flush
				// plus cold reset turns one backlog into an underrun and a re-cooled
				// clock ratio, so the next fill overruns again and never settles.
				const UINT32 excess = frames - (CAPACITY - count);
				head = (head + excess) % CAPACITY;
				count -= excess;
				status.expiredFrames += excess;
				++status.overruns;
				rateMatcher.Reseed();
			}
			for (UINT32 frame = 0; frame < frames; ++frame)
			{
				const UINT32 index = (head + count + frame) % CAPACITY;
				fifo[index * 2] = samples[frame * 2];
				fifo[index * 2 + 1] = samples[frame * 2 + 1];
				ages[index] = submittedAt;
			}
			count += frames;
			status.queuedFrames = count;
			SetEvent(wake);
		}

		void SetGamePacketFrames(UINT32 frames) { if (frames) gamePacketFrames.store(frames); }

		// The longest gap between two game packets since the last call (the CABLE LATENCY line, every second).
		ULONGLONG TakeLongestGameGapMs()
		{
			std::lock_guard<std::mutex> guard(mutex);
			const ULONGLONG gap = longestGameGapMs;
			longestGameGapMs = 0;
			return gap;
		}

		// The recorder's game track, taken as the device is filled: the game plus the Tone designer's guitar, which is
		// mixed in here and so was missing from a track taken where the game hands its sound over. Other
		// programs' sound (AppAudioMix) is added after this, so a take does not record YouTube or Discord.
		void SetRecorder(std::shared_ptr<RecordingSession> session) { recordTap.store(std::move(session)); }
		// Frames between a game packet handed over now and the speaker: the FIFO plus what the device holds (Windows'
		// padding) or will hold (the ASIO / exclusive device's own delay). For GameToneLatency.
		UINT32 HeardDelayFrames() const { return heardDelayFrames.load(std::memory_order_relaxed); }

		bool ConfigureOutputGuard(bool limiterOn, float ceiling, bool agcOn, float target)
		{
			if (!std::isfinite(ceiling) || ceiling <= 0.0f || ceiling > 1.0f) return false;
			if (!std::isfinite(target) || target <= 0.0f || target > 1.0f) return false;
			guardLimiterOn.store(false);
			guardAgcOn.store(false);
			guardCeiling.store(ceiling);
			guardTarget.store(target);
			guardLimiterOn.store(limiterOn);
			guardAgcOn.store(agcOn);
			return true;
		}

	private:
		// Frames older than ageLimitMs are dropped: 40 ms, plus the loading reserve while AsioFill holds one (its
		// frames wait ~50 ms by design).
		void Expire(ULONGLONG now)
		{
			if (count && now - ages[head] > ageLimitMs) rateMatcher.Reseed();
			while (count && now - ages[head] > ageLimitMs)
			{
				head = (head + 1) % CAPACITY;
				--count;
				++status.expiredFrames;
			}
			status.queuedFrames = count;
		}

		// Under mutex: back to the lean queue with no reserve (a new output, or the same one reopened).
		void ResetReserve()
		{
			reserveOn = status.reserveOn = false;
			ageLimitMs = 40;
			leanAskedAt = quietSince = 0;
			lastSubmittedAt = 0;
		}

		// Under mutex: drop the oldest `excess` frames, blending the ~2 ms after the cut with the frames that would have
		// played there, so the cut is not a click (the boot's backlog cuts were audible).
		void DropOldest(UINT32 excess)
		{
			const UINT32 fade = (std::min)(96u, count - excess);
			for (UINT32 frame = 0; frame < fade; ++frame)
			{
				const float in = float(frame + 1) / float(fade + 1);
				const UINT32 kept = ((head + excess + frame) % CAPACITY) * 2, dropped = ((head + frame) % CAPACITY) * 2;
				fifo[kept] = fifo[kept] * in + fifo[dropped] * (1.0f - in);
				fifo[kept + 1] = fifo[kept + 1] * in + fifo[dropped + 1] * (1.0f - in);
			}
			head = (head + excess) % CAPACITY;
			count -= excess;
			status.trimmedFrames += excess;
			++status.trims;
		}

		void Close()
		{
			if (asio)
			{
				const bool ranAWhile = asio->Callbacks() > 0;
				// A driver stuck in its callback is abandoned, never freed under it (AsioOutputDevice::Close).
				if (asio->Close()) { if (ranAWhile) AsioCrashGuard::Clear(); asio.reset(); }
				else asio.release();
				asioStartedAt = 0;
			}
			if (exclusive) { exclusive->Close(); exclusive.reset(); asioStartedAt = 0; }
			if (client && started) client->Stop();
			started = false;
			if (clock) { clock->Release(); clock = nullptr; }
			if (render) { render->Release(); render = nullptr; }
			if (client) { client->Release(); client = nullptr; }
			clockStartQpc = 0;
			lastPump = 0;
		}

		void Fail(HRESULT error, uint64_t generation)
		{
			bool report = false;
			{
				std::lock_guard<std::mutex> guard(mutex);
				if (status.generation == generation)
				{
					report = status.error != error;
					status.error = error;
					status.state = BackendState::Recovering;
					status.activeEndpoint.clear();
					status.outputMode = 0;
					status.outputBufferFrames = 0;
					status.padding = status.queuedFrames = count = 0;
					status.clockMeasured = false;
				}
			}
			if (report) LOG_ERROR("(AUDIO ROUTING) Physical output recovery, HRESULT " << std::hex << error << std::dec << std::endl);
			nextRetry = GetTickCount64() + 1000;
			Close();
		}

		HRESULT Open(const std::wstring& endpoint, UINT32 desired, bool& usedDefaultFallback)
		{
			usedDefaultFallback = false;
			HRESULT result = OpenAttempt(endpoint, desired);
			if (SUCCEEDED(result)) fallbackReported = false;
			if (SUCCEEDED(result) || endpoint.empty() || !allowDefaultFallback) return result;
			// A rebuilt client's output can still be held by the backend it replaces (an ASIO driver opens once per
			// process, an exclusive device once per system). Falling back here adopted the Windows default for good, so
			// an alt-tab undid the player's Switch output pick: keep retrying the pick (every second) for a short while.
			if (GetTickCount64() < handOverUntil) return result;

			// Retried every second while no output is available; report it once until an output opens again.
			if (!fallbackReported)
				LOG_INFO("(AUDIO ROUTING) Configured output could not open; retrying the current Windows default" << std::endl);
			fallbackReported = true;
			Close();
			result = OpenAttempt(L"", 0);
			usedDefaultFallback = SUCCEEDED(result);
			if (usedDefaultFallback) fallbackReported = false;
			return result;
		}

		HRESULT OpenConverted(UINT32 desired, WAVEFORMATEX& format, HRESULT lowLatencyError)
		{
			WAVEFORMATEX* mix = nullptr;
			if (client && SUCCEEDED(client->GetMixFormat(&mix)) && mix)
			{
				LOG_INFO("(AUDIO ROUTING) Low-latency output open failed (HRESULT " << std::hex << lowLatencyError << std::dec
					<< ") on a device running " << mix->nSamplesPerSec << " Hz, " << mix->nChannels
					<< " ch; opening a converted shared stream" << std::endl);
				CoTaskMemFree(mix);
			}
			if (client) { client->Release(); client = nullptr; }   // a failed Initialize leaves the client unusable
			HRESULT result = factory(resolvedEndpoint, &client);
			if (SUCCEEDED(result) && !client) result = E_UNEXPECTED;
			minimum = fundamental = maximum = 0;
			// A converted stream runs Windows' standard ~10 ms engine period whatever buffer the player saved for the
			// low-latency path, so never pace it faster than that (a 128-frame period queues only 256 frames against a
			// 480-frame engine pass and crackles continuously).
			period = (std::max)(desired, 480u);
			const REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(period) * 2 * 10000000 / 48000;
			if (SUCCEEDED(result))
				result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
					AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
					duration, 0, &format, nullptr);
			UINT32 frames = 0;
			if (SUCCEEDED(result)) result = client->GetBufferSize(&frames);
			if (SUCCEEDED(result) && frames / 2 < period) period = frames / 2;
			if (SUCCEEDED(result) && period == 0) result = AUDCLNT_E_BUFFER_SIZE_ERROR;
			return result;
		}

		// No saved buffer ("Automatic"): about 3 ms instead of Windows' default ~10 ms period. The default period was
		// most of the delay cable players heard after each note, and nearly every
		// device takes 144 frames. Rounded up to the device's step and never below its minimum or above its default,
		// so a device without small periods keeps the default. Requires GetSharedModeEnginePeriod's limits.
		UINT32 AutomaticPeriod(UINT32 normal) const
		{
			if (!minimum || !fundamental || !normal) return normal;
			const UINT32 automatic = FitPeriod(AUTOMATIC_PERIOD_FRAMES, normal);
			return automatic > normal ? normal : automatic;
		}

		// The Playback buffer setting is one value for every device, so fit it to this device: up to its step and
		// minimum, or its default when even that is above its maximum. Without this a size the device does not take
		// would drop to the converted ~10 ms path (OpenConverted) instead of the nearest small one.
		UINT32 FitPeriod(UINT32 wanted, UINT32 normal) const
		{
			if (!minimum || !fundamental) return wanted;
			UINT32 fitted = (std::max)(minimum, wanted);
			fitted = (fitted + fundamental - 1) / fundamental * fundamental;
			return fitted > maximum ? normal : fitted;
		}

		// Cable mode playing through an ASIO interface's own driver ("asio:<driver>", AsioOutputDevice.hpp). Its buffer
		// size is the one set in the interface's control panel; there is no Windows period, so the Playback buffer
		// card hides itself (minimum, step and maximum stay 0).
		HRESULT OpenAsio(const std::wstring& endpoint)
		{
			resolvedEndpoint = endpoint;
			const std::wstring name = AsioOutputDevice::DriverName(endpoint);
			if (AsioCrashGuard::CrashedLastTime(name))
			{
				LOG_WARNING("(ASIO OUTPUT) '" << AudioBridge::Text::Utf8(name) << "' was opening when the game closed last time; not reopened at"
					" launch (pick it again on the Output page to try it)" << std::endl);
				return HRESULT_FROM_WIN32(ERROR_CANCELLED);
			}
			AsioCrashGuard::Mark(name);
			auto device = std::make_unique<AsioOutputDevice>();
			std::string failure;
			if (!device->Open(name, [this](float* stereo, uint32_t frames) { AsioFill(stereo, frames); }, failure))
			{
				AsioCrashGuard::Clear();   // it refused without taking the game down
				LOG_WARNING("(ASIO OUTPUT) '" << AudioBridge::Text::Utf8(name) << "' would not open: " << failure << std::endl);
				return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_AVAILABLE);
			}
			asio = std::move(device);
			AppAudioMix::Start();   // here, not in AsioFill: it reads the registry and starts a thread
			period = bufferFrames = asio->BufferFrames();
			// What this buffer waits before it is heard: the driver's own output latency (the ASIO convention includes the
			// buffer), or one buffer when it reports none.
			asioDeviceDelay = asio->OutputLatencyFrames() ? asio->OutputLatencyFrames() : asio->BufferFrames();
			minimum = fundamental = maximum = 0;
			asioClockQpc = asioClockFrames = 0;
			outputGuard.Reset(2);
			return S_OK;
		}

		// Cable mode on a Windows device in exclusive mode ("excl:<id>", ExclusiveOutputDevice.hpp): the device's own period,
		// usually ~3 ms, instead of Windows' shared mixer. Driven like an ASIO output: its thread pulls AsioFill.
		// Devices whose driver stops in event-driven exclusive mode, remembered so the next open goes straight to polling
		// (the stall is a second of high, broken sound each time). HKCU\Software\RSMods\AsioProxy\ExclusivePolling,
		// one DWORD per Windows endpoint id.
		static bool NeedsPolling(const std::wstring& windowsId)
		{
			DWORD value = 0, size = sizeof(value);
			return RegGetValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy\\ExclusivePolling", windowsId.c_str(), RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value;
		}
		static void RememberPolling(const std::wstring& windowsId)
		{
			const DWORD value = 1;
			RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy\\ExclusivePolling", windowsId.c_str(), REG_DWORD, &value, sizeof(value));
		}

		HRESULT OpenExclusive(const std::wstring& endpoint, UINT32 desired)
		{
			if (endpoint != exclusiveTried)
			{
				exclusiveTried = endpoint;
				exclusiveStage = NeedsPolling(ExclusiveOutputDevice::WindowsId(endpoint)) ? 1 : 0;
			}
			if (exclusiveStage >= 2)
			{
				// Neither way kept playing: shared mode on the same device, so the player still hears the game. The overlay
				// sees the device open without "excl:" and says exclusive mode did not work on it.
				LOG_WARNING("(EXCLUSIVE OUTPUT) This device does not keep playing in exclusive mode; using shared mode on it" << std::endl);
				return OpenAttempt(ExclusiveOutputDevice::WindowsId(endpoint), desired);
			}
			resolvedEndpoint = endpoint;
			auto device = std::make_unique<ExclusiveOutputDevice>();
			std::string failure;
			if (!device->Open(ExclusiveOutputDevice::WindowsId(endpoint), factory, desired, exclusiveStage == 1, [this](float* stereo, uint32_t frames) { AsioFill(stereo, frames); }, failure))
			{
				LOG_WARNING("(EXCLUSIVE OUTPUT) Could not open the device in exclusive mode: " << failure << std::endl);
				exclusiveFailure = failure;
				return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_AVAILABLE);
			}
			exclusiveFailure.clear();
			exclusive = std::move(device);
			AppAudioMix::Start();   // here, not in AsioFill: it reads the registry and starts a thread
			period = bufferFrames = exclusive->BufferFrames();
			asioDeviceDelay = exclusive->OutputLatencyFrames();
			minimum = fundamental = maximum = 0;
			asioClockQpc = asioClockFrames = 0;
			outputGuard.Reset(2);
			return S_OK;
		}

		HRESULT StartDevice()
		{
			if (exclusive)
			{
				if (!exclusive->Start()) return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_AVAILABLE);
				asioStartedAt = GetTickCount64();
				return S_OK;
			}
			if (asio)
			{
				if (!asio->Start()) return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_AVAILABLE);
				asioStartedAt = GetTickCount64();
				return S_OK;
			}
			return client->Start();
		}

		HRESULT StopDevice()
		{
			if (asio) { asio->Stop(); asioStartedAt = 0; return S_OK; }
			if (exclusive) { exclusive->Stop(); asioStartedAt = 0; return S_OK; }
			return client->Stop();
		}

		// The ASIO driver's thread, once per buffer: the same FIFO, rate matcher, Tone designer return and output guard
		// as Pump, pulled by the interface's clock instead of a Windows event. Always fills the whole buffer (silence
		// where the game has not delivered yet).
		void AsioFill(float* stereo, uint32_t frames)
		{
			const ULONGLONG now = GetTickCount64();
			unsigned produced = 0;
			bool current = false;
			{
				std::lock_guard<std::mutex> guard(mutex);
				if (asioGeneration != status.generation || quit.load() || frames > CAPACITY) return;
				Expire(now);
				status.padding = 0;
				// The game delivers a packet at a time (CableGamePeriod, 128 frames by default) while the interface takes
				// `frames` at a time, so the FIFO swings by a game packet: keep a packet plus two buffers in hand, and drop
				// a backlog above that at once.
				const UINT32 gamePacket = gamePacketFrames.load(std::memory_order_relaxed);
				// A packet, one buffer and 1 ms of slack (two buffers queued 6-7 ms here on a 3 ms exclusive device).
				// The game is clocked by the cable 1 ms at a time, so a second buffer of reserve was idle delay;
				// "device found empty" in the 30 s (CABLE LATENCY) line shows whether this one is enough.
				// Plus a cushion that adapts: while the game loads its menus it is late and the lean minimum
				// ran dry ~400-600 times in the first 80 s of every boot (clicks), then never again. A run dry while the
				// game is delivering adds a game packet (at most once a second, up to 10 ms); 30 s without one takes a
				// packet back every 10 s. Shrinking only lowers the servo's centre, so the queue drains without a cut.
				if (count) cushionFedAt = now;
				const UINT32 lean = (std::min)(CAPACITY, gamePacket + frames + 48 + cushion);
				// The loading reserve (OutputReserve): ~50 ms in hand while the game boots and sits in menus,
				// where its audio thread stalls for tens of ms. Switched on at once (the queue fills from the game's
				// catch-up after a stall, or from silence slipped in while the output is quiet); switched off at a quiet
				// moment, or after 1 s anyway, with one crossfaded cut down to the lean queue.
				if (OutputReserve::Wanted())
				{
					leanAskedAt = 0;
					if (!reserveOn) { reserveOn = true; ageLimitMs = 40 + OutputReserve::kFrames / 48; }
				}
				else if (reserveOn)
				{
					if (!leanAskedAt) leanAskedAt = now;
					if ((quietSince && now - quietSince >= 20) || now - leanAskedAt >= 1000) { reserveOn = false; ageLimitMs = 40; leanAskedAt = 0; }
				}
				status.reserveOn = reserveOn;
				const UINT32 headroom = (std::max)(gamePacket, frames);
				// Room left for the reserve: none for a buffer over half the FIFO, where it stays at the lean queue.
				const UINT32 room = headroom * 2 < CAPACITY ? CAPACITY - headroom * 2 : 0;
				const UINT32 target = reserveOn ? (std::max)(lean, (std::min)(room, OutputReserve::kFrames)) : lean;
				if (count > target + headroom) DropOldest(count - target);
				// Quiet for 20 ms with the reserve under half (and the game delivering): slip silence in ahead of what is queued.
				if (reserveOn && count && quietSince && now - quietSince >= 20 && count * 2 < target)
				{
					const UINT32 add = target - count;
					head = (head + CAPACITY - add) % CAPACITY;
					for (UINT32 frame = 0; frame < add; ++frame)
					{
						const UINT32 index = (head + frame) % CAPACITY;
						fifo[index * 2] = fifo[index * 2 + 1] = 0.0f;
						ages[index] = now;
					}
					count += add;
				}
				const ULONGLONG oldestFrame = count ? ages[head] : now;
				unsigned consumed = 0;
				produced = rateMatcher.Read([this](unsigned frame, unsigned channel) { return fifo[((head + frame) % CAPACITY) * 2 + channel]; }, count, staging.data(), frames, consumed);
				head = (head + consumed) % CAPACITY;
				count -= consumed;
				status.queuedFrames = count;
				heardDelayFrames.store(count + asioDeviceDelay, std::memory_order_relaxed);
				if (produced < frames)
				{
					++status.emptyObservations;
					// The game is running, just late (not stopped or not started yet). Not while the reserve is held: what
					// the game does while loading says nothing about the queue a song needs.
					if (now - cushionFedAt <= 100 && !reserveOn)
					{
						cushionEmptyAt = now;
						if (now - cushionChangedAt >= 1000 && cushion + gamePacket <= 480) { cushion += gamePacket; cushionChangedAt = now; }
					}
				}
				else if (cushion && now - cushionEmptyAt >= 30000 && now - cushionChangedAt >= 10000)
				{
					cushion -= (std::min)(cushion, gamePacket);
					cushionChangedAt = now;
				}
				status.cushionFrames = cushion;
				current = produced && now - oldestFrame <= ageLimitMs;
				// The game's own sound in this buffer (before Tone designer and other apps): below -60 dBFS is quiet.
				float peak = 0.0f;
				if (current) for (unsigned i = 0; i < produced * 2; ++i) peak = (std::max)(peak, std::abs(staging[i]));
				if (peak >= 0.001f) quietSince = 0;
				else if (!quietSince) quietSince = now;
				// The interface's clock against QPC over 2 s windows (callback jitter averages out), then the same servo as
				// the Windows path, centred on the target fill.
				LARGE_INTEGER qpc{};
				QueryPerformanceCounter(&qpc);
				if (!asioClockQpc) { asioClockQpc = static_cast<UINT64>(qpc.QuadPart); asioClockFrames = 0; }
				else
				{
					asioClockFrames += frames;
					LARGE_INTEGER counterFrequency{};
					QueryPerformanceFrequency(&counterFrequency);
					const double seconds = double(static_cast<UINT64>(qpc.QuadPart) - asioClockQpc) / double(counterFrequency.QuadPart);
					if (seconds >= 2.0)
					{
						const double measured = (double(asioClockFrames) / seconds / 48000.0 - 1.0) * 1000000.0;
						if (std::isfinite(measured) && std::abs(measured) < 5000)
						{
							status.clockPpm = status.clockMeasured ? status.clockPpm * 0.9 + measured * 0.1 : measured;
							status.clockMeasured = true;
							rateMatcher.Measure(status.clockPpm, double(count) - target);
							status.correctionPpm = rateMatcher.GetCorrectionPpm();
						}
						asioClockQpc = static_cast<UINT64>(qpc.QuadPart);
						asioClockFrames = 0;
					}
				}
			}
			if (!current) produced = 0;
			std::fill(staging.begin() + produced * 2, staging.begin() + frames * 2, 0.0f);
			CableAmpLink::Render(staging.data(), frames, asioDeviceDelay, frames);   // the Tone designer's return on the cable (no-op otherwise)
			RecordHeard(frames);
			// Other programs' sound: they cannot play on a device held exclusively or through ASIO (AppAudioMix).
			AppAudioMix::Mix(staging.data(), frames);
			ApplyOutputGuard(frames);
			// Exclusive mode skips Windows' own volume: on a device without hardware volume the Output page's device
			// volume is applied here (DeviceVolume).
			if (exclusive)
			{
				const float gain = DeviceVolume::SoftwareGain();
				if (gain != 1.0f) for (uint32_t i = 0; i < frames * 2; ++i) staging[i] *= gain;
			}
			std::memcpy(stereo, staging.data(), frames * 2 * sizeof(float));
			lastProgress.store(now);
		}

		HRESULT OpenAttempt(const std::wstring& endpoint, UINT32 desired)
		{
			if (AsioOutputDevice::IsAsioEndpoint(endpoint)) return OpenAsio(endpoint);
			if (ExclusiveOutputDevice::IsExclusiveEndpoint(endpoint)) return OpenExclusive(endpoint, desired);
			resolvedEndpoint = endpoint;
			HRESULT result = factory(resolvedEndpoint, &client);
			if (SUCCEEDED(result) && !client) result = E_UNEXPECTED;
			WAVEFORMATEX format{ WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0 };
			UINT32 normal = 0;
			if (SUCCEEDED(result))
			{
				HRESULT lowLatency = client->GetSharedModeEnginePeriod(&format, &normal, &fundamental, &minimum, &maximum);
				if (SUCCEEDED(lowLatency)) period = desired ? FitPeriod(desired, normal) : AutomaticPeriod(normal);
				else period = desired ? desired : normal;
				const HRESULT queried = lowLatency;
				if (SUCCEEDED(lowLatency) && (!minimum || !fundamental || period < minimum || period > maximum || period > 48000 || period % fundamental)) lowLatency = E_INVALIDARG;
				if (SUCCEEDED(lowLatency)) lowLatency = client->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, &format, nullptr);
				// What the device allows decides whether the Playback buffer can go below Windows' 10 ms: a driver whose
				// minimum is its default period (many USB drivers) stays at 10 ms whatever is chosen.
				LOG_INFO("(AUDIO ROUTING) Playback device periods: default " << normal << ", minimum " << minimum << ", step " << fundamental
					<< ", maximum " << maximum << " frames (query " << std::hex << queried << std::dec << "); asked "
					<< (desired ? std::to_string(desired) : std::string("Automatic")) << ", using " << period
					<< (SUCCEEDED(lowLatency) ? " (small-period stream)" : " -> standard stream instead (Windows' default period)") << std::endl);
				// The small-period stream only accepts the device's own Windows format. A device set to
				// 44.1 kHz, or to 5.1 / 7.1, refuses 48 kHz stereo float there, so reopen it as a standard
				// shared stream and let Windows convert (a 10 ms engine period instead of the small one).
				if (FAILED(lowLatency)) result = OpenConverted(desired, format, lowLatency);
			}
			if (SUCCEEDED(result)) result = client->SetEventHandle(wake);
			if (SUCCEEDED(result)) result = client->GetBufferSize(&bufferFrames);
			if (SUCCEEDED(result) && bufferFrames < period) result = AUDCLNT_E_BUFFER_SIZE_ERROR;
			if (SUCCEEDED(result)) result = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
			if (SUCCEEDED(result)) result = client->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&clock));
			if (SUCCEEDED(result)) result = clock->GetFrequency(&frequency);
			if (SUCCEEDED(result) && !frequency) result = E_UNEXPECTED;
			if (SUCCEEDED(result))
			{
				BYTE* data = nullptr;
				result = render->GetBuffer(period, &data);
				if (SUCCEEDED(result)) result = render->ReleaseBuffer(period, AUDCLNT_BUFFERFLAGS_SILENT);
				if (SUCCEEDED(result)) outputGuard.Reset(2);
			}
			return result;
		}

		HRESULT Pump(uint64_t generation)
		{
			UINT32 padding = 0;
			HRESULT result = client->GetCurrentPadding(&padding);
			if (FAILED(result)) return result;
			if (padding > bufferFrames) return E_UNEXPECTED;
			const auto now = GetTickCount64();
			UINT32 frames = 0;
			ULONGLONG oldestFrame = now;
			{
				std::lock_guard<std::mutex> guard(mutex);
				if (generation != status.generation || quit.load()) return S_FALSE;
				Expire(now);
				status.padding = padding;
				if (started && padding == 0) ++status.emptyObservations;
				if (lastPump) status.longestPumpGapMs = std::max<uint64_t>(status.longestPumpGapMs, now - lastPump);
				lastPump = now;
				const UINT32 target = std::min(bufferFrames, period * 2);
				// A backlog more than a period over the target is dropped at once (oldest first). The rate servo below
				// only slews a few hundred ppm, so a cold start that queued ~800 frames too many took two minutes to drain
				// and every note in that time came out 15-20 ms late (about 1000 FIFO frames falling
				// to under 100 over two minutes). At start that excess is silence; in steady play the servo keeps it from building.
				if (count && count + padding > target + period)
				{
					const UINT32 excess = std::min(count, count + padding - target);
					head = (head + excess) % CAPACITY;
					count -= excess;
					status.trimmedFrames += excess;
					++status.trims;
				}
				const UINT32 capacity = std::min(CAPACITY, target > padding ? target - padding : 0);
				if (count) oldestFrame = ages[head];
				unsigned consumed = 0;
				frames = rateMatcher.Read([this](unsigned frame, unsigned channel) { return fifo[((head + frame) % CAPACITY) * 2 + channel]; }, count, staging.data(), capacity, consumed);
				head = (head + consumed) % CAPACITY;
				count -= consumed;
				status.queuedFrames = count;
				heardDelayFrames.store(count + padding + frames, std::memory_order_relaxed);
			}
			if (frames)
			{
				BYTE* destination = nullptr;
				result = render->GetBuffer(frames, &destination);
				if (FAILED(result)) return result;
				bool current = false;
				{
					std::lock_guard<std::mutex> guard(mutex);
					current = generation == status.generation && !quit.load() && GetTickCount64() - oldestFrame <= 40;
				}
				if (current)
				{
					CableAmpLink::Render(staging.data(), frames, padding, period);   // the Tone designer's return on the cable (no-op otherwise)
					RecordHeard(frames);
					ApplyOutputGuard(frames);
					std::memcpy(destination, staging.data(), frames * 2 * sizeof(float));
				}
				else
				{
					std::fill(staging.begin(), staging.begin() + frames * 2, 0.0f);
					RecordHeard(frames);   // silence played: the take stays in step with the video
				}
				result = render->ReleaseBuffer(frames, current ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
				if (FAILED(result)) return result;
			}
			UINT64 position = 0, qpc = 0;
			result = clock->GetPosition(&position, &qpc);
			if (FAILED(result)) return result;
			if (result == S_OK && started)
			{
				if (clockStartQpc && (qpc < clockStartQpc || position < clockStartPosition)) return AUDCLNT_E_DEVICE_INVALIDATED;
				if (!clockStartQpc) { clockStartQpc = qpc; clockStartPosition = position; }
				// Re-measure four times a second. The servo slews at a bounded rate
				// per measurement, so a one-second cadence takes many seconds to
				// converge after a device switch, long enough for the cold FIFO to
				// overrun or starve first. A 250 ms window still spans
				// enough frames for a stable ppm estimate (the EMA below smooths the
				// added quantization noise) while reacting to queue drift far sooner.
				else if (qpc > clockStartQpc + 2500000 && position >= clockStartPosition)
				{
					const double measured = (double(position - clockStartPosition) * 10000000 / (qpc - clockStartQpc) / frequency - 1) * 1000000;
					std::lock_guard<std::mutex> guard(mutex);
					if (generation == status.generation && std::isfinite(measured) && std::abs(measured) < 5000)
					{
						status.clockPpm = status.clockMeasured ? status.clockPpm * 0.9 + measured * 0.1 : measured;
						status.clockMeasured = true;
						rateMatcher.Measure(status.clockPpm, double(count + padding + frames) - std::min(bufferFrames, period * 2));
						status.correctionPpm = rateMatcher.GetCorrectionPpm();
					}
					clockStartQpc = qpc; clockStartPosition = position;
				}
			}
			return S_OK;
		}

		void ApplyOutputGuard(UINT32 frames)
		{
			const bool limiterOn = guardLimiterOn.load();
			const bool agcOn = guardAgcOn.load();
			if (!limiterOn && !agcOn) return;

			const float ceiling = guardCeiling.load();
			const float target = guardTarget.load();
			outputGuard.SetLimiterLive(limiterOn, ceiling);
			outputGuard.SetAgcLive(agcOn, target);
			double meanSquare[2]{};
			for (UINT32 frame = 0; frame < frames; ++frame)
			{
				for (UINT32 channel = 0; channel < 2; ++channel)
				{
					const float sample = staging[frame * 2 + channel];
					guardChannels[channel][frame] = sample;
					meanSquare[channel] += static_cast<double>(sample) * sample;
				}
			}
			meanSquare[0] /= frames;
			meanSquare[1] /= frames;
			outputGuard.BeginBlock(meanSquare, 2, static_cast<int>(frames));
			outputGuard.ProcessChannel(0, guardChannels[0].data(), static_cast<int>(frames));
			outputGuard.ProcessChannel(1, guardChannels[1].data(), static_cast<int>(frames));
			for (UINT32 frame = 0; frame < frames; ++frame)
			{
				staging[frame * 2] = guardChannels[0][frame];
				staging[frame * 2 + 1] = guardChannels[1][frame];
			}
		}

		void Run()
		{
			const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
			bool registered = false;
			if (watchDevices && SUCCEEDED(com))
			{
				HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
				if (SUCCEEDED(result)) result = notifications ? enumerator->RegisterEndpointNotificationCallback(notifications.Get()) : E_OUTOFMEMORY;
				registered = SUCCEEDED(result);
				if (FAILED(result)) LOG_ERROR("(AUDIO ROUTING) Could not subscribe to output device changes, HRESULT " << std::hex << result << std::dec << std::endl);
			}
			DWORD index = 0;
			HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
			uint64_t generation = 0;
			while (!quit.load())
			{
				if (WaitForSingleObject(wake, 10) == WAIT_FAILED) { Fail(HRESULT_FROM_WIN32(GetLastError()), generation); break; }
				// An ASIO output does not care about Windows' device list (headphones plugged in, a device switched on).
				// Nor an exclusive one: it notices losing its device itself (its watchdog below).
				if (notifications && notifications->changed.exchange(false) && !asio && !exclusive)
				{
					LOG_INFO("(AUDIO ROUTING) Windows devices changed; reopening the configured bridge output" << std::endl);
					ReapplyOutput();
				}
				OutputBackendStatus request = GetStatus();
				if (!request.generation) continue;
				if (generation != request.generation || (!client && !asio && !exclusive && GetTickCount64() >= nextRetry))
				{
					generation = request.generation;
					UINT32 desired = 0;
					bool run = false;
					{
						std::lock_guard<std::mutex> guard(mutex);
						if (generation != status.generation) continue;
						desired = desiredPeriod;
						run = shouldRun;
						status.state = BackendState::Opening;
						count = status.queuedFrames = 0;
						rateMatcher.Reset();
					}
					Close();
					bool usedDefaultFallback = false;
					HRESULT result = FAILED(com) ? com : Open(request.requestedEndpoint, desired, usedDefaultFallback);
					if (FAILED(result)) { Fail(result, generation); continue; }
					{
						std::lock_guard<std::mutex> guard(mutex);
						if (generation != status.generation || quit.load()) continue;
						status.state = BackendState::Priming;
						asioGeneration = generation;   // AsioFill serves only this generation
					}
					if (run) { result = StartDevice(); started = SUCCEEDED(result); }
					if (FAILED(result)) { Fail(result, generation); continue; }
					{
						std::lock_guard<std::mutex> guard(mutex);
						if (generation != status.generation || quit.load()) continue;
						if (usedDefaultFallback)
						{
							status.requestedEndpoint = resolvedEndpoint;
							desiredPeriod = 0;
							LOG_INFO("(AUDIO ROUTING) Adopted the fallback output until the user explicitly selects another device" << std::endl);
						}
						status.activeEndpoint = resolvedEndpoint;
						status.error = S_OK;
						lastProgress.store(GetTickCount64());
						status.state = run ? BackendState::Running : BackendState::Priming;
						status.period = period; status.minimum = minimum; status.fundamental = fundamental; status.maximum = maximum;
						status.outputBufferFrames = exclusive ? exclusive->DeviceBufferFrames() : bufferFrames;
						status.outputMode = asio ? 4u : (exclusive ? (exclusive->Polling() ? 3u : 2u) : 1u);
						status.clockMeasured = false;
					}
				}
				if (client || asio || exclusive)
				{
					bool run = false;
					{
						std::lock_guard<std::mutex> guard(mutex);
						run = shouldRun;
					}
					if (run != started)
					{
						const HRESULT transition = run ? StartDevice() : StopDevice();
						if (FAILED(transition)) { Fail(transition, generation); continue; }
						started = run;
						std::lock_guard<std::mutex> guard(mutex);
						if (status.generation == generation) status.state = run ? BackendState::Running : BackendState::Priming;
					}
					if (exclusive)
					{
						// Its own thread does the work (AsioFill). Here: a failure it reported, and a stall watchdog.
						const ULONGLONG now = GetTickCount64();
						// A device that played for 10 s and then stopped was unplugged or taken; one that stopped sooner has a
						// driver that does not keep exclusive mode going this way: next try is polling, then shared mode.
						const bool brief = !asioStartedAt || now - asioStartedAt < 10000;
						if (exclusive->TakeResetRequest())
						{
							if (brief) ++exclusiveStage;
							LOG_WARNING("(EXCLUSIVE OUTPUT) The device stopped taking audio after " << exclusive->Callbacks() << " periods"
								<< (exclusive->Polling() ? " (polling)" : "") << "; reopening it" << std::endl);
							Fail(AUDCLNT_E_DEVICE_INVALIDATED, generation);
							continue;
						}
						if (!started) { lastProgress.store(now); continue; }
						const ULONGLONG last = exclusive->LastCallbackTick();
						if ((last && now - last > 1000) || (!last && asioStartedAt && now - asioStartedAt > 2000))
						{
							if (brief && !exclusive->Polling()) RememberPolling(ExclusiveOutputDevice::WindowsId(resolvedEndpoint));
							if (brief) ++exclusiveStage;
							LOG_WARNING("(EXCLUSIVE OUTPUT) The device stopped asking for audio after " << exclusive->Callbacks() << " periods"
								<< (exclusive->Polling() ? " (polling)" : "") << "; reopening it" << (brief && exclusiveStage == 1 ? " polling" : "") << std::endl);
							Fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), generation);
						}
						continue;
					}
					if (asio)
					{
						// The driver's own thread does the work (AsioFill). Here: its reset requests, and a stall watchdog.
						const ULONGLONG now = GetTickCount64();
						if (asio->TakeResetRequest())
						{
							LOG_INFO("(ASIO OUTPUT) '" << AudioBridge::Text::Utf8(asio->Name()) << "' asked for a reset; reopening it" << std::endl);
							ReapplyOutput();
							continue;
						}
						if (!started) { lastProgress.store(now); continue; }
						const ULONGLONG last = asio->LastCallbackTick();
						if ((last && now - last > 1000) || (!last && asioStartedAt && now - asioStartedAt > 2000))
						{
							LOG_WARNING("(ASIO OUTPUT) '" << AudioBridge::Text::Utf8(asio->Name()) << "' stopped calling back; reopening it" << std::endl);
							Fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), generation);
							continue;
						}
						// Ten seconds of playing: it did not take the game down while opening, so the boot guard can let go.
						if (asioStartedAt && now - asioStartedAt > 10000) AsioCrashGuard::Clear();
						continue;
					}
					const auto began = GetTickCount64();
					const HRESULT result = Pump(generation);
					if (FAILED(result)) Fail(result, generation);
					else if (GetTickCount64() - began > 500) Fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), generation);
					else lastProgress.store(GetTickCount64());
				}
			}
			Close();
			if (registered) enumerator->UnregisterEndpointNotificationCallback(notifications.Get());
			enumerator.Reset();
			notifications.Reset();
			if (task) AvRevertMmThreadCharacteristics(task);
			if (SUCCEEDED(com)) CoUninitialize();
			SetEvent(finished);
		}

		static constexpr UINT32 CAPACITY = 4096;
		static constexpr UINT32 AUTOMATIC_PERIOD_FRAMES = 144;   // 3 ms at 48 kHz, see AutomaticPeriod
		std::atomic<std::shared_ptr<RecordingSession>> recordTap;   // SetRecorder
		void RecordHeard(uint32_t frames) { if (auto tap = recordTap.load()) tap->SubmitWet(staging.data(), frames); }
		std::atomic<UINT32> gamePacketFrames{ 480 };             // what the game delivers at a time (OutputSession)
		std::atomic<UINT32> heardDelayFrames{ 0 };               // HeardDelayFrames
		std::unique_ptr<AsioOutputDevice> asio;                  // Cable mode on an ASIO interface's own driver (OpenAsio)
		std::unique_ptr<ExclusiveOutputDevice> exclusive;        // Cable mode on a Windows device in exclusive mode (OpenExclusive)
		std::string exclusiveFailure;                            // backend thread: why the last exclusive open failed
		std::wstring exclusiveTried;                             // backend thread: the "excl:" endpoint exclusiveStage is about
		int exclusiveStage = 0;                                  // 0 event driven, 1 polling, 2 shared mode instead (this session)
		uint64_t asioGeneration = 0;                             // under mutex: the generation AsioFill serves
		UINT64 asioClockQpc = 0, asioClockFrames = 0;            // under mutex: AsioFill's clock window
		UINT32 cushion = 0;                                      // under mutex: AsioFill's adaptive extra reserve, frames
		ULONGLONG cushionFedAt = 0, cushionEmptyAt = 0, cushionChangedAt = 0;   // under mutex: AsioFill
		bool reserveOn = false;                                  // under mutex: AsioFill holds the loading reserve
		UINT32 ageLimitMs = 40;                                  // under mutex: Expire's limit (ResetReserve)
		ULONGLONG leanAskedAt = 0, quietSince = 0;               // under mutex: AsioFill's switch to lean
		ULONGLONG lastSubmittedAt = 0, longestGameGapMs = 0;     // under mutex: Submit's gap between game packets
		ULONGLONG asioStartedAt = 0;                             // backend thread
		uint32_t asioDeviceDelay = 0;                            // frames until a filled ASIO buffer is heard (latency log)
		EndpointFactory factory;
		bool watchDevices;
		Microsoft::WRL::ComPtr<OutputNotifications> notifications;
		bool allowDefaultFallback;
		std::wstring resolvedEndpoint;
		std::mutex mutex;
		OutputBackendStatus status;
		UINT32 desiredPeriod = 0;
		bool shouldRun = false;
		std::atomic<bool> quit{ false };
		std::atomic<ULONGLONG> lastProgress{ 0 };
		HANDLE wake = nullptr, finished = nullptr;
		OutputRateMatcher rateMatcher;
		std::array<float, CAPACITY * 2> fifo{}, staging{};
		std::array<std::array<float, CAPACITY>, 2> guardChannels{};
		std::array<ULONGLONG, CAPACITY> ages{};
		OutputGuardDsp::OutputGuard outputGuard;
		std::atomic<bool> guardLimiterOn{ false }, guardAgcOn{ false };
		std::atomic<float> guardCeiling{ 1.0f }, guardTarget{ 0.10f };
		UINT32 head = 0, count = 0;
		IAudioClient3* client = nullptr;
		IAudioRenderClient* render = nullptr;
		IAudioClock* clock = nullptr;
		UINT32 bufferFrames = 0, period = 0, minimum = 0, fundamental = 0, maximum = 0;
		UINT64 frequency = 0, clockStartPosition = 0, clockStartQpc = 0;
		ULONGLONG nextRetry = 0, lastPump = 0;
		bool started = false;
		bool fallbackReported = false;
		static constexpr ULONGLONG kHandOverMs = 4000;
		ULONGLONG handOverUntil = 0;   // Launch: no default fallback before this (a rebuilt client's hand-over)
	};
}
