#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <thread>
#include <wrl.h>
#include "../AsioProxy/OutputGuardDsp.h"
#include "OutputRateMatcher.hpp"

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
		UINT32 padding = 0, queuedFrames = 0;
		uint64_t expiredFrames = 0, overruns = 0, emptyObservations = 0;
		uint64_t longestPumpGapMs = 0;
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
			++status.generation;
			status.state = BackendState::Opening;
			status.error = E_PENDING;
			desiredPeriod = requestedPeriod;
			count = 0;
			status.queuedFrames = 0;
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
			status.state = BackendState::Opening;
			status.error = E_PENDING;
			count = status.queuedFrames = 0;
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
			}
			return snapshot;
		}

		void Submit(const float* samples, UINT32 frames, ULONGLONG submittedAt)
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (status.state != BackendState::Running && status.state != BackendState::Priming) return;
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
		void Expire(ULONGLONG now)
		{
			if (count && now - ages[head] > 40) rateMatcher.Reseed();
			while (count && now - ages[head] > 40)
			{
				head = (head + 1) % CAPACITY;
				--count;
				++status.expiredFrames;
			}
			status.queuedFrames = count;
		}

		void Close()
		{
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

		HRESULT OpenAttempt(const std::wstring& endpoint, UINT32 desired)
		{
			resolvedEndpoint = endpoint;
			HRESULT result = factory(resolvedEndpoint, &client);
			if (SUCCEEDED(result) && !client) result = E_UNEXPECTED;
			WAVEFORMATEX format{ WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0 };
			UINT32 normal = 0;
			if (SUCCEEDED(result))
			{
				HRESULT lowLatency = client->GetSharedModeEnginePeriod(&format, &normal, &fundamental, &minimum, &maximum);
				period = desired ? desired : normal;
				if (SUCCEEDED(lowLatency) && (!minimum || !fundamental || period < minimum || period > maximum || period > 48000 || period % fundamental)) lowLatency = E_INVALIDARG;
				if (SUCCEEDED(lowLatency)) lowLatency = client->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, &format, nullptr);
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
				const UINT32 capacity = std::min(CAPACITY, target > padding ? target - padding : 0);
				if (count) oldestFrame = ages[head];
				unsigned consumed = 0;
				frames = rateMatcher.Read([this](unsigned frame, unsigned channel) { return fifo[((head + frame) % CAPACITY) * 2 + channel]; }, count, staging.data(), capacity, consumed);
				head = (head + consumed) % CAPACITY;
				count -= consumed;
				status.queuedFrames = count;
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
					ApplyOutputGuard(frames);
					std::memcpy(destination, staging.data(), frames * 2 * sizeof(float));
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
				if (notifications && notifications->changed.exchange(false))
				{
					LOG_INFO("(AUDIO ROUTING) Windows devices changed; reopening the configured bridge output" << std::endl);
					ReapplyOutput();
				}
				OutputBackendStatus request = GetStatus();
				if (!request.generation) continue;
				if (generation != request.generation || (!client && GetTickCount64() >= nextRetry))
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
					}
					if (run) { result = client->Start(); started = SUCCEEDED(result); }
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
						status.clockMeasured = false;
					}
				}
				if (client)
				{
					bool run = false;
					{
						std::lock_guard<std::mutex> guard(mutex);
						run = shouldRun;
					}
					if (run != started)
					{
						const HRESULT transition = run ? client->Start() : client->Stop();
						if (FAILED(transition)) { Fail(transition, generation); continue; }
						started = run;
						std::lock_guard<std::mutex> guard(mutex);
						if (status.generation == generation) status.state = run ? BackendState::Running : BackendState::Priming;
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
	};
}
