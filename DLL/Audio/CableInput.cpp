#include "stdafx.h"
#include "CableInput.hpp"
#include "SharedOutput.hpp"
#include "OutputTap.hpp"
#include "PersistentInput.hpp"
#include "AsioHook.hpp"
#include "ComVTable.hpp"

#include <functiondiscoverykeys_devpkey.h>
#include <timeapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace Audio::CableInput
{
	namespace
	{
		constexpr unsigned long PA_WASAPI_EXCLUSIVE = 1;
		constexpr size_t SLOT_MMDEVICE_ACTIVATE = 3;
		constexpr uint64_t FIRST_REPORT_DELAY_MS = 5000;
		constexpr uint64_t STALL_THRESHOLD_MS = 3000;
		bool masterEnabled = false;

		// PortAudio v19 (2012) public structs, 32-bit layout, matching the game's
		// Pa_OpenStream calls.
		struct PaStreamParameters
		{
			int device;
			int channelCount;
			unsigned long sampleFormat;
			double suggestedLatency;
			void* hostApiSpecificStreamInfo;
		};

		struct PaWasapiStreamInfo
		{
			unsigned long size;
			int hostApiType;
			unsigned long version;
			unsigned long flags;
			unsigned long channelMask;
			void* hostProcessorOutput;
			void* hostProcessorInput;
		};

		using PaOpenStream_t = int(__cdecl*)(
			void** stream,
			const PaStreamParameters* inputParameters,
			const PaStreamParameters* outputParameters,
			double sampleRate,
			unsigned long framesPerBuffer,
			unsigned long streamFlags,
			void* streamCallback,
			void* userData);
		using MmDeviceActivate_t = HRESULT(STDMETHODCALLTYPE*)(
			IMMDevice*, REFIID, DWORD, PROPVARIANT*, void**);

		PaOpenStream_t originalPaOpenStream = nullptr;
		MmDeviceActivate_t originalActivate = nullptr;
		// Set only while THIS thread is inside a PortAudio open, so the Activate wrapper never
		// touches PortAudio's enumeration probes. Input-only opens get the modern capture
		// client; output-only opens may get the passive diagnostic monitor.
		thread_local bool wrapCaptureActivation = false;
		thread_local bool monitorRenderActivation = false;
		bool modernInputEnabled = true;
		bool inputOwnedByAsio = false;
		bool persistentInputEnabled = false;
		bool persistentOutputEnabled = false;
		SharedOutput::Configuration routingConfiguration;
		// Output monitoring is OFF by default: with it on, launches can stall before the profile
		// screen with no PortAudio processing thread created and one thread parked inside a COM
		// call. Enable with RSMods.ini
		// [Mod Settings] MonitorOutput = on to diagnose "no sound"; every one-shot call on the
		// wrapper then logs, so a stall names the call it stopped at.
		bool outputMonitorEnabled = false;

		std::string DescribeIid(REFIID riid)
		{
			if (riid == __uuidof(IUnknown)) return "IUnknown";
			if (riid == __uuidof(IAudioClient)) return "IAudioClient";
			if (riid == __uuidof(IAudioClient2)) return "IAudioClient2";
			if (riid == __uuidof(IAudioClient3)) return "IAudioClient3";
			if (riid == __uuidof(IAudioRenderClient)) return "IAudioRenderClient";
			if (riid == __uuidof(IAudioCaptureClient)) return "IAudioCaptureClient";
			if (riid == __uuidof(IAudioClock)) return "IAudioClock";
			if (riid == __uuidof(IMarshal)) return "IMarshal";
			if (riid == __uuidof(IAgileObject)) return "IAgileObject";
			OLECHAR text[64] = {};
			StringFromGUID2(riid, text, 64);
			char narrow[64] = {};
			WideCharToMultiByte(CP_ACP, 0, text, -1, narrow, sizeof(narrow), nullptr, nullptr);
			return narrow;
		}

		// Pinned so MMDevAPI's class (and the vtable we patched) stays loaded for the process.
		IMMDeviceEnumerator* pinnedEnumerator = nullptr;
		IMMDevice* pinnedDevice = nullptr;

		// Liveness counters, written on the audio thread, read by Poll. One per direction.
		struct StreamStats
		{
			std::atomic<uint64_t> packets{ 0 };
			std::atomic<uint64_t> lastPacketTick{ 0 };
			std::atomic<uint32_t> peakBits{ 0 };
			std::atomic<uint64_t> startTick{ 0 };
			std::atomic<uint32_t> generation{ 0 };

			void OnStart()
			{
				packets.store(0, std::memory_order_relaxed);
				lastPacketTick.store(0, std::memory_order_relaxed);
				peakBits.store(0, std::memory_order_relaxed);
				startTick.store(GetTickCount64(), std::memory_order_release);
				generation.fetch_add(1, std::memory_order_release);
			}

			void OnPacket(float peak, bool silent)
			{
				packets.fetch_add(1, std::memory_order_relaxed);
				lastPacketTick.store(GetTickCount64(), std::memory_order_relaxed);
				if (silent) return;
				uint32_t bits;
				std::memcpy(&bits, &peak, sizeof(bits));
				const uint32_t current = peakBits.load(std::memory_order_relaxed);
				float currentPeak;
				std::memcpy(&currentPeak, &current, sizeof(currentPeak));
				if (peak > currentPeak) peakBits.store(bits, std::memory_order_relaxed);
			}

			float TakePeak()
			{
				const uint32_t bits = peakBits.exchange(0, std::memory_order_relaxed);
				float peak;
				std::memcpy(&peak, &bits, sizeof(peak));
				return peak;
			}
		};
		StreamStats inputStats;
		StreamStats tapStats;
		StreamStats outputStats;
		// Player 2's input, whichever path carries it (P2 cable wrapper or ASIO route 1). Its own
		// stats + decaying meter so the second overlay row never borrows Player 1's figures.
		StreamStats playerTwoStats;
		std::atomic<uint32_t> playerTwoMeterBits{ 0 };
		std::atomic<bool> asioPath{ false };
		std::atomic<uint32_t> tapFrames{ 0 };
		std::atomic<uint32_t> tapRate{ 0 };

		std::mutex statusMutex;
		std::string statusDescription = "not installed";
		Diagnostics diagnostics;   // guarded by statusMutex
		std::atomic_bool overlayEnabled{ true };

		// Decaying peak for the on-screen signal bar: written per packet on the audio thread,
		// read by the render thread. Decays ~8% per packet, so a strum lingers ~0.5 s.
		std::atomic<uint32_t> meterBits{ 0 };
		std::atomic<uint64_t> dropoutCount{ 0 };
		// Exponential average of the capture timestamp lag, in ms, as float bits.
		std::atomic<uint32_t> measuredLagBits{ 0 };
		std::atomic<bool> measuredAny{ false };
		std::atomic<uint64_t> measuredTick{ 0 };
		// Raw inputs of the last measurement, for the throttled evidence line in Poll.
		std::atomic<int64_t> lastRawDelta100ns{ 0 };
		std::atomic<uint32_t> lastRawFrames{ 0 };
		std::atomic<uint32_t> lastRawSource{ 0 };   // 1 = tap (AsioHook), 2 = capture client

		// QueryPerformanceCounter in the 100 ns units WASAPI uses for its timestamps.
		uint64_t Now100ns()
		{
			static LARGE_INTEGER frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
			LARGE_INTEGER counter{};
			QueryPerformanceCounter(&counter);
			return frequency.QuadPart > 0
				? static_cast<uint64_t>(counter.QuadPart * 10000000.0 / frequency.QuadPart)
				: 0;
		}

		void UpdateMeterBits(std::atomic<uint32_t>& target, float peak)
		{
			uint32_t bits = target.load(std::memory_order_relaxed);
			float meter;
			std::memcpy(&meter, &bits, sizeof(meter));
			meter = std::max(peak, meter * 0.92f);
			std::memcpy(&bits, &meter, sizeof(bits));
			target.store(bits, std::memory_order_relaxed);
		}

		void UpdateMeter(float peak) { UpdateMeterBits(meterBits, peak); }

		float ReadMeterBits(const std::atomic<uint32_t>& source)
		{
			const uint32_t bits = source.load(std::memory_order_relaxed);
			float meter = 0.0f;
			std::memcpy(&meter, &bits, sizeof(meter));
			return meter;
		}

		void SetStatus(const std::string& description)
		{
			std::lock_guard<std::mutex> guard(statusMutex);
			statusDescription = description;
		}

		enum class Sample { Unsupported, Float32, Int16, Int24, Int32 };

		struct PcmLayout
		{
			Sample sample = Sample::Unsupported;
			uint32_t channels = 0;
			uint32_t rate = 0;
			uint32_t bytesPerSample = 0;
			uint32_t blockAlign = 0;

			bool IsUsable() const
			{
				return sample != Sample::Unsupported && channels > 0 && rate > 0
					&& blockAlign == channels * bytesPerSample;
			}
		};

		PcmLayout ReadLayout(const WAVEFORMATEX* format)
		{
			PcmLayout layout;
			if (!format) return layout;

			bool isFloat = false;
			bool isPcm = false;
			if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
				&& format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			{
				const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
				isFloat = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
				isPcm = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != 0;
			}
			else
			{
				isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
				isPcm = format->wFormatTag == WAVE_FORMAT_PCM;
			}

			if (isFloat && format->wBitsPerSample == 32) layout.sample = Sample::Float32;
			else if (isPcm && format->wBitsPerSample == 16) layout.sample = Sample::Int16;
			else if (isPcm && format->wBitsPerSample == 24) layout.sample = Sample::Int24;
			else if (isPcm && format->wBitsPerSample == 32) layout.sample = Sample::Int32;

			layout.channels = format->nChannels;
			layout.rate = format->nSamplesPerSec;
			layout.bytesPerSample = format->wBitsPerSample / 8;
			layout.blockAlign = format->nBlockAlign;
			return layout;
		}

		bool CopyFormat(const WAVEFORMATEX* source, WAVEFORMATEXTENSIBLE& destination)
		{
			if (!source) return false;
			const size_t bytes = std::min<size_t>(
				sizeof(WAVEFORMATEX) + source->cbSize, sizeof(WAVEFORMATEXTENSIBLE));
			std::memset(&destination, 0, sizeof(destination));
			std::memcpy(&destination, source, bytes);
			return true;
		}

		std::string Describe(const PcmLayout& layout)
		{
			std::ostringstream text;
			switch (layout.sample)
			{
			case Sample::Float32: text << "32-bit float"; break;
			case Sample::Int16: text << "16-bit PCM"; break;
			case Sample::Int24: text << "24-bit PCM"; break;
			case Sample::Int32: text << "32-bit PCM"; break;
			default: text << "unsupported"; break;
			}
			text << " " << layout.rate << " Hz " << layout.channels << "ch";
			return text.str();
		}

		std::string DescribeHresult(HRESULT hr)
		{
			std::ostringstream text;
			text << "0x" << std::hex << static_cast<unsigned long>(hr) << std::dec;
			return text.str();
		}

		float ReadSample(const uint8_t* sample, Sample kind)
		{
			switch (kind)
			{
			case Sample::Float32: return *reinterpret_cast<const float*>(sample);
			case Sample::Int16: return static_cast<float>(*reinterpret_cast<const int16_t*>(sample)) / 32768.0f;
			case Sample::Int32: return static_cast<float>(*reinterpret_cast<const int32_t*>(sample)) / 2147483648.0f;
			case Sample::Int24:
			{
				const uint32_t packed = static_cast<uint32_t>(sample[0])
					| (static_cast<uint32_t>(sample[1]) << 8)
					| (static_cast<uint32_t>(sample[2]) << 16);
				const int32_t value = (packed & 0x00800000u) != 0
					? static_cast<int32_t>(packed) - 0x01000000
					: static_cast<int32_t>(packed);
				return static_cast<float>(value) / 8388608.0f;
			}
			default: return 0.0f;
			}
		}

		void WriteSample(uint8_t* sample, Sample kind, float value)
		{
			if (!std::isfinite(value)) value = 0.0f;
			value = std::clamp(value, -1.0f, 1.0f);
			switch (kind)
			{
			case Sample::Float32:
				*reinterpret_cast<float*>(sample) = value;
				break;
			case Sample::Int16:
				*reinterpret_cast<int16_t*>(sample) = static_cast<int16_t>(
					std::clamp(value * 32768.0f, -32768.0f, 32767.0f));
				break;
			case Sample::Int32:
				*reinterpret_cast<int32_t*>(sample) = static_cast<int32_t>(
					std::clamp(static_cast<double>(value) * 2147483648.0, -2147483648.0, 2147483647.0));
				break;
			case Sample::Int24:
			{
				const int32_t scaled = static_cast<int32_t>(
					std::clamp(value * 8388608.0f, -8388608.0f, 8388607.0f));
				const uint32_t packed = static_cast<uint32_t>(scaled);
				sample[0] = static_cast<uint8_t>(packed & 0xff);
				sample[1] = static_cast<uint8_t>((packed >> 8) & 0xff);
				sample[2] = static_cast<uint8_t>((packed >> 16) & 0xff);
				break;
			}
			default:
				break;
			}
		}

		float MeasurePeak(const BYTE* packet, const PcmLayout& layout, UINT32 frames)
		{
			if (!packet || layout.sample == Sample::Unsupported || layout.blockAlign == 0) return 0.0f;
			float peak = 0.0f;
			for (UINT32 frame = 0; frame < frames; ++frame)
				peak = std::max(peak, std::fabs(ReadSample(packet + static_cast<size_t>(frame) * layout.blockAlign, layout.sample)));
			return peak;
		}

		std::string DescribeLevel(float peak)
		{
			if (peak <= 0.0f) return "silence";
			std::ostringstream text;
			text << std::fixed << std::setprecision(1) << 20.0 * std::log10(peak) << " dBFS peak";
			return text.str();
		}

		std::string ReadDeviceName(IMMDevice* device)
		{
			if (!device) return "unknown device";
			IPropertyStore* store = nullptr;
			if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store) return "unknown device";

			PROPVARIANT value;
			PropVariantInit(&value);
			std::string name = "unknown device";
			if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
			{
				const int length = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, nullptr, nullptr);
				if (length > 1)
				{
					std::string converted(static_cast<size_t>(length) - 1, '\0');
					WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, converted.data(), length, nullptr, nullptr);
					name = converted;
				}
			}
			PropVariantClear(&value);
			store->Release();
			return name;
		}

		// IAudioCaptureClient handed to PortAudio. Owns packetization: every engine packet is
		// copied (converting when the engine format differs from the one PortAudio negotiated)
		// into an internal ring and released immediately, and PortAudio is served fixed
		// period-sized chunks. PortAudio's exclusive event-mode path assumes one full host
		// buffer per event (true of a real exclusive stream, buffer == period). When that does not
		// hold (e.g. a 1056-frame buffer fed by 480-frame packets) PortAudio reads stale frames
		// and the game hears scrambled audio. Serving exact chunks makes the contract hold on any
		// driver, any period, any fallback rung. Also counts packets and tracks the input peak
		// for the liveness report. Aggregates the free-threaded marshaler so PortAudio's
		// CoMarshalInterThreadInterfaceInStream hands the proc thread this same pointer.
		class CaptureClient final : public IAudioCaptureClient
		{
		public:
			CaptureClient(IAudioCaptureClient* realClient, const PcmLayout& engine, const PcmLayout& presented,
				UINT32 engineBufferFrames, UINT32 chunkFrames)
				: real(realClient), engineLayout(engine), presentedLayout(presented), chunkFrames(chunkFrames)
			{
				CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(this), &marshaler);
				sameLayout = engine.sample == presented.sample && engine.channels == presented.channels
					&& engine.blockAlign == presented.blockAlign;
				capacityFrames = std::max<UINT32>({ engineBufferFrames * 4, chunkFrames * 8, 8192 });
				ring.assign(static_cast<size_t>(capacityFrames) * presentedLayout.blockAlign, 0);
			}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
			{
				if (!ppv) return E_POINTER;
				if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioCaptureClient))
				{
					*ppv = static_cast<IAudioCaptureClient*>(this);
					AddRef();
					return S_OK;
				}
				if (riid == __uuidof(IMarshal) && marshaler)
					return marshaler->QueryInterface(riid, ppv);
				*ppv = nullptr;
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef() override
			{
				return ++refCount;
			}

			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG remaining = --refCount;
				if (remaining == 0) delete this;
				return remaining;
			}

			// All three methods run on PortAudio's processing thread only, so the ring needs
			// no locking. The engine is drained on every call so its own buffer never overruns.
			HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** data, UINT32* frames, DWORD* flags,
				UINT64* devicePosition, UINT64* qpcPosition) override
			{
				if (!data || !frames) return E_POINTER;
				const HRESULT pull = Drain();
				if (FAILED(pull)) return pull;
				if (pendingFrames != 0) return AUDCLNT_E_OUT_OF_ORDER;

				if (Available() < chunkFrames)
				{
					*data = nullptr;
					*frames = 0;
					if (flags) *flags = 0;
					return AUDCLNT_S_BUFFER_EMPTY;
				}

				// PortAudio's exclusive event path takes exactly one chunk per event, so any
				// backlog in the ring would be served forever, one chunk stale (the stock poll path
				// drains everything instead). Serve the NEWEST whole chunk and drop the rest: a rare
				// glitch on a hiccup instead of permanent latency.
				const uint64_t startedTick = inputStats.startTick.load(std::memory_order_relaxed);
				const bool pastStartup = startedTick != 0 && GetTickCount64() - startedTick > 5000;
				while (Available() >= 2 * chunkFrames)
				{
					readFrame += chunkFrames;
					discontinuityPending = true;
					// The first second is the engine's pre-roll draining; only later skips are
					// the game's audio thread genuinely falling behind.
					if (pastStartup) dropoutCount.fetch_add(1, std::memory_order_relaxed);
				}
				Compact();

				*data = ring.data() + static_cast<size_t>(readFrame) * presentedLayout.blockAlign;
				*frames = chunkFrames;
				if (flags)
				{
					*flags = discontinuityPending ? AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY : 0;
					discontinuityPending = false;
				}
				if (devicePosition) *devicePosition = framesServed;
				// The device timestamp of this chunk's first frame, interpolated from the engine
				// packet it came from, so the game (and the latency measurement) sees the real
				// capture time rather than "now".
				const uint64_t chunkQpc = QpcForAbsoluteFrame(totalWritten - Available());
				if (qpcPosition) *qpcPosition = chunkQpc;
				if (chunkQpc != 0 && presentedLayout.rate > 0)
				{
					const int64_t delta = static_cast<int64_t>(Now100ns()) - static_cast<int64_t>(chunkQpc);
					lastRawDelta100ns.store(delta, std::memory_order_relaxed);
					lastRawFrames.store(chunkFrames, std::memory_order_relaxed);
					lastRawSource.store(2, std::memory_order_relaxed);
					ReportCaptureTimestampLag(delta / 10000.0);
				}
				else
				{
					ReportCaptureTimestampLag(-1.0);
				}
				pendingFrames = chunkFrames;
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 framesRead) override
			{
				if (framesRead == 0)
				{
					pendingFrames = 0;
					return S_OK;
				}
				if (framesRead != pendingFrames) return AUDCLNT_E_INVALID_SIZE;
				readFrame += framesRead;
				framesServed += framesRead;
				pendingFrames = 0;
				Compact();
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE GetNextPacketSize(UINT32* packetFrames) override
			{
				if (!packetFrames) return E_POINTER;
				const HRESULT pull = Drain();
				if (FAILED(pull)) return pull;
				*packetFrames = Available() >= chunkFrames ? chunkFrames : 0;
				return S_OK;
			}

		private:
			~CaptureClient()
			{
				if (marshaler) marshaler->Release();
				if (real) real->Release();
			}

			UINT32 Available() const
			{
				return writeFrame - readFrame;
			}

			// Device timestamp for an absolute frame index, from the newest packet stamp at or
			// before it. 0 when no stamp covers it (engine without timestamps).
			uint64_t QpcForAbsoluteFrame(uint64_t absoluteFrame) const
			{
				if (presentedLayout.rate == 0) return 0;
				const uint32_t count = std::min<uint32_t>(stampWrite, STAMP_COUNT);
				for (uint32_t back = 0; back < count; ++back)
				{
					const Stamp& s = stamps[(stampWrite - 1 - back) % STAMP_COUNT];
					if (s.absoluteFrame <= absoluteFrame)
						return s.qpc + static_cast<uint64_t>((absoluteFrame - s.absoluteFrame) * 10000000.0 / presentedLayout.rate);
				}
				return 0;
			}

			// Slide unread frames to the front once the read cursor has moved past a quarter of
			// the ring (or whenever the writer needs the room); a few kilobytes at most, on the
			// audio thread, no allocation.
			void Compact(bool force = false)
			{
				if (readFrame == 0 || (!force && readFrame < capacityFrames / 4)) return;
				const size_t bytes = static_cast<size_t>(Available()) * presentedLayout.blockAlign;
				if (bytes > 0)
					std::memmove(ring.data(), ring.data() + static_cast<size_t>(readFrame) * presentedLayout.blockAlign, bytes);
				writeFrame -= readFrame;
				readFrame = 0;
			}

			// Pulls every packet the engine currently holds into the ring.
			HRESULT Drain()
			{
				for (;;)
				{
					UINT32 next = 0;
					HRESULT hr = real->GetNextPacketSize(&next);
					if (FAILED(hr)) return hr;
					if (next == 0) return S_OK;

					BYTE* source = nullptr;
					UINT32 frames = 0;
					DWORD flags = 0;
					UINT64 devicePos = 0;
					UINT64 packetQpc = 0;
					hr = real->GetBuffer(&source, &frames, &flags, &devicePos, &packetQpc);
					if (hr == AUDCLNT_S_BUFFER_EMPTY) return S_OK;
					if (FAILED(hr)) return hr;
					if (frames == 0 || source == nullptr)
					{
						real->ReleaseBuffer(frames);
						return S_OK;
					}

					const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
					if (frames > capacityFrames)
					{
						// Cannot happen (the engine never hands out more than its buffer, and the
						// ring is at least four of those); drop the packet rather than overrun.
						real->ReleaseBuffer(frames);
						discontinuityPending = true;
						continue;
					}
					if (Available() + frames > capacityFrames)
					{
						// PortAudio stopped consuming (a stall on its side); keep the newest
						// audio and flag the gap rather than blocking the engine.
						const UINT32 drop = Available() + frames - capacityFrames;
						readFrame += std::min(drop, Available());
						discontinuityPending = true;
						dropoutCount.fetch_add(1, std::memory_order_relaxed);
					}
					if (writeFrame + frames > capacityFrames) Compact(true);

					uint8_t* destination = ring.data() + static_cast<size_t>(writeFrame) * presentedLayout.blockAlign;
					float peak = 0.0f;
					if (silent)
					{
						std::memset(destination, 0, static_cast<size_t>(frames) * presentedLayout.blockAlign);
					}
					else if (sameLayout)
					{
						std::memcpy(destination, source, static_cast<size_t>(frames) * presentedLayout.blockAlign);
						peak = MeasurePeak(source, engineLayout, frames);
					}
					else
					{
						for (UINT32 frame = 0; frame < frames; ++frame)
						{
							const float value = ReadSample(source + static_cast<size_t>(frame) * engineLayout.blockAlign, engineLayout.sample);
							peak = std::max(peak, std::fabs(value));
							uint8_t* frameOut = destination + static_cast<size_t>(frame) * presentedLayout.blockAlign;
							for (uint32_t channel = 0; channel < presentedLayout.channels; ++channel)
								WriteSample(frameOut + channel * presentedLayout.bytesPerSample, presentedLayout.sample, value);
						}
					}
					// Remember where this packet starts (absolute frame index) and when its first
					// frame was captured; the engine reports the timestamp in 100 ns QPC units.
					if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0 && packetQpc != 0)
					{
						stamps[stampWrite % STAMP_COUNT] = { totalWritten, packetQpc };
						++stampWrite;
					}
					totalWritten += frames;
					writeFrame += frames;
					inputStats.OnPacket(peak, silent);

					hr = real->ReleaseBuffer(frames);
					if (FAILED(hr)) return hr;
				}
			}

			std::atomic<ULONG> refCount{ 1 };
			IAudioCaptureClient* real;
			IUnknown* marshaler = nullptr;
			PcmLayout engineLayout;
			PcmLayout presentedLayout;
			UINT32 chunkFrames;
			bool sameLayout = false;
			UINT32 capacityFrames = 0;
			UINT32 readFrame = 0;
			UINT32 writeFrame = 0;
			UINT32 pendingFrames = 0;
			UINT64 framesServed = 0;
			bool discontinuityPending = false;
			std::vector<uint8_t> ring;

			struct Stamp { uint64_t absoluteFrame; uint64_t qpc; };
			static constexpr uint32_t STAMP_COUNT = 64;
			Stamp stamps[STAMP_COUNT] = {};
			uint32_t stampWrite = 0;
			uint64_t totalWritten = 0;   // frames ever appended to the ring (absolute index of writeFrame)
		};

		// IAudioClient handed to PortAudio in place of the endpoint's real client. Accepts the
		// exclusive, event-driven request PortAudio makes and satisfies it with a modern
		// shared-mode stream, re-activating a fresh real client for each fallback attempt
		// (a failed Initialize is not reliably retryable on the same object).
		class ModernAudioClient final : public IAudioClient
		{
		public:
			ModernAudioClient(IAudioClient* realClient, IMMDevice* endpoint)
				: real(realClient), device(endpoint)
			{
				device->AddRef();
				deviceName = ReadDeviceName(device);
				CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(this), &marshaler);
			}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
			{
				if (!ppv) return E_POINTER;
				if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioClient))
				{
					*ppv = static_cast<IAudioClient*>(this);
					AddRef();
					return S_OK;
				}
				if (riid == __uuidof(IMarshal) && marshaler)
					return marshaler->QueryInterface(riid, ppv);
				*ppv = nullptr;
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef() override
			{
				return ++refCount;
			}

			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG remaining = --refCount;
				if (remaining == 0) delete this;
				return remaining;
			}

			HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE, DWORD streamFlags,
				REFERENCE_TIME bufferDuration, REFERENCE_TIME, const WAVEFORMATEX* format, LPCGUID session) override
			{
				if (!format) return E_POINTER;
				if (initialized) return AUDCLNT_E_ALREADY_INITIALIZED;

				CopyFormat(format, presentedFormat);
				presentedLayout = ReadLayout(&presentedFormat.Format);
				if (!presentedLayout.IsUsable())
				{
					// Not a PCM/float shape we can present; hand the request to the engine
					// untouched (shared, so the cable still opens) and report what we did.
					const HRESULT hr = real->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, bufferDuration, 0, format, session);
					if (SUCCEEDED(hr)) FinishInitialize("legacy shared (unrecognised format passed through)", presentedLayout, false, 0, streamFlags);
					return hr;
				}

				WAVEFORMATEXTENSIBLE mixFormat{};
				PcmLayout mixLayout;
				{
					WAVEFORMATEX* mix = nullptr;
					if (SUCCEEDED(real->GetMixFormat(&mix)) && mix)
					{
						CopyFormat(mix, mixFormat);
						mixLayout = ReadLayout(&mixFormat.Format);
						CoTaskMemFree(mix);
					}
				}
				const bool mixConvertible = mixLayout.IsUsable() && mixLayout.rate == presentedLayout.rate;
				deviceMixFormat = mixFormat;

				struct Attempt
				{
					const char* label;
					bool modern;
					bool useMixFormat;
					bool raw;
					bool autoConvert;
				};
				const Attempt attempts[] =
				{
					{ "IAudioClient3 low-latency, native format, raw", true, false, true, false },
					{ "IAudioClient3 low-latency, native format", true, false, false, false },
					{ "IAudioClient3 low-latency, engine mix format, raw", true, true, true, false },
					{ "IAudioClient3 low-latency, engine mix format", true, true, false, false },
					// A device whose Windows format differs in rate (e.g. the cable on a fresh USB
					// port) fails every rung above; asking the engine to convert on the
					// low-latency stream keeps the period instead of dropping to the 22 ms floor.
					{ "IAudioClient3 low-latency, native format, engine auto-convert", true, false, false, true },
					{ "legacy shared event-driven, engine auto-convert", false, false, false, true },
					{ "legacy shared event-driven, engine mix format", false, true, false, false },
				};

				HRESULT lastError = E_FAIL;
				bool firstAttempt = true;
				for (const Attempt& attempt : attempts)
				{
					if (attempt.useMixFormat && !mixConvertible) continue;
					if (!firstAttempt && !Reactivate()) break;
					firstAttempt = false;

					const WAVEFORMATEX* engineFormat = attempt.useMixFormat ? &mixFormat.Format : &presentedFormat.Format;
					UINT32 periodFrames = 0;
					std::string note;
					const HRESULT hr = attempt.modern
						? InitializeModern(engineFormat, streamFlags, session, attempt.raw, attempt.autoConvert, periodFrames, note)
						: InitializeLegacy(engineFormat, streamFlags, bufferDuration, session, attempt.autoConvert);
					if (SUCCEEDED(hr))
					{
						std::string label = attempt.label;
						if (!note.empty()) label += " (" + note + ")";
						FinishInitialize(label, attempt.useMixFormat ? mixLayout : presentedLayout,
							attempt.useMixFormat, periodFrames, streamFlags);
						return S_OK;
					}
					lastError = hr;
					LOG_INFO("(CABLE INPUT) " << deviceName << ": " << attempt.label << " unavailable ("
						<< DescribeHresult(hr) << "; game wants " << Describe(presentedLayout)
						<< ", Windows format is " << (mixLayout.IsUsable() ? Describe(mixLayout) : "unknown")
						<< "); trying the next path" << std::endl);
				}

				LOG_ERROR("(CABLE INPUT) " << deviceName << ": every modern and legacy shared open failed, last error "
					<< DescribeHresult(lastError) << "; PortAudio will report the device as unavailable" << std::endl);
				SetStatus("input open FAILED on " + deviceName + " (" + DescribeHresult(lastError) + ")");
				return lastError;
			}

			HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override
			{
				// PortAudio's exclusive event-mode path assumes one full host buffer per event,
				// which holds for a real exclusive stream (buffer == period) but not for the
				// shared stream underneath, where PortAudio would otherwise read stale frames.
				// Present the packet size as the buffer so the assumption holds.
				if (presentedBufferFrames > 0)
				{
					if (!frames) return E_POINTER;
					*frames = presentedBufferFrames;
					return S_OK;
				}
				return real->GetBufferSize(frames);
			}
			HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* latency) override { return real->GetStreamLatency(latency); }
			HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* padding) override { return real->GetCurrentPadding(padding); }

			HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX* format,
				WAVEFORMATEX** closestMatch) override
			{
				if (closestMatch) *closestMatch = nullptr;
				if (!format) return E_POINTER;
				// Anything PCM/float we can present is "supported": Initialize converts, or has
				// the engine convert, to reach it. PortAudio asks in exclusive mode for the
				// game's own 32-bit float mono 48 kHz, which is exactly the shape we want.
				if (ReadLayout(format).IsUsable()) return S_OK;
				return real->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, format, closestMatch);
			}

			HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override { return real->GetMixFormat(format); }
			HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* defaultPeriod, REFERENCE_TIME* minimumPeriod) override
			{
				return real->GetDevicePeriod(defaultPeriod, minimumPeriod);
			}

			HRESULT STDMETHODCALLTYPE Start() override
			{
				const HRESULT hr = real->Start();
				if (SUCCEEDED(hr))
				{
					inputStats.OnStart();
					dropoutCount.store(0, std::memory_order_relaxed);
				}
				return hr;
			}

			HRESULT STDMETHODCALLTYPE Stop() override
			{
				inputStats.startTick.store(0, std::memory_order_release);
				return real->Stop();
			}

			HRESULT STDMETHODCALLTYPE Reset() override { return real->Reset(); }
			HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE eventHandle) override { return real->SetEventHandle(eventHandle); }

			HRESULT STDMETHODCALLTYPE GetService(REFIID riid, void** ppv) override
			{
				if (!ppv) return E_POINTER;
				if (riid != __uuidof(IAudioCaptureClient)) return real->GetService(riid, ppv);

				IAudioCaptureClient* realCapture = nullptr;
				const HRESULT hr = real->GetService(riid, reinterpret_cast<void**>(&realCapture));
				if (FAILED(hr) || !realCapture) return hr;

				UINT32 bufferFrames = 0;
				real->GetBufferSize(&bufferFrames);
				// Always our own capture object, even without conversion: AsioHook patches the
				// capture client's vtable once and refuses a second implementation, so every
				// input stream this session must share one class. It serves exactly
				// presentedBufferFrames per GetBuffer, the buffer size PortAudio was told.
				*ppv = new CaptureClient(realCapture, engineLayout, presentedLayout, bufferFrames,
					presentedBufferFrames > 0 ? presentedBufferFrames : bufferFrames);
				return S_OK;
			}

		private:
			~ModernAudioClient()
			{
				if (marshaler) marshaler->Release();
				if (real) real->Release();
				if (device) device->Release();
			}

			bool Reactivate()
			{
				if (!originalActivate || !device) return false;
				IAudioClient* fresh = nullptr;
				const HRESULT hr = originalActivate(device, __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
					reinterpret_cast<void**>(&fresh));
				if (FAILED(hr) || !fresh)
				{
					LOG_ERROR("(CABLE INPUT) " << deviceName << ": could not re-activate the endpoint for a fallback ("
						<< DescribeHresult(hr) << ")" << std::endl);
					return false;
				}
				real->Release();
				real = fresh;
				return true;
			}

			HRESULT InitializeModern(const WAVEFORMATEX* engineFormat, DWORD streamFlags, LPCGUID session,
				bool raw, bool autoConvert, UINT32& periodFrames, std::string& note)
			{
				if (autoConvert) streamFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
				IAudioClient3* client3 = nullptr;
				if (FAILED(real->QueryInterface(__uuidof(IAudioClient3), reinterpret_cast<void**>(&client3))) || !client3)
					return E_NOINTERFACE;

				// Record what the driver offers WITHOUT raw mode too, so the log shows whether
				// raw processing costs (or buys) a smaller period on this device.
				UINT32 plainDefault = 0, plainFundamental = 0, plainMinimum = 0, plainMaximum = 0;
				const bool havePlainPeriods = SUCCEEDED(client3->GetSharedModeEnginePeriod(
					engineFormat, &plainDefault, &plainFundamental, &plainMinimum, &plainMaximum));

				if (raw)
				{
					// Raw mode bypasses the endpoint's signal-processing APOs (noise suppression,
					// AGC, "enhancements") that Windows may insert on a capture endpoint: they
					// add latency and colour a guitar signal. Best effort; the fallback attempt
					// runs without it.
					AudioClientProperties properties{};
					properties.cbSize = sizeof(properties);
					properties.bIsOffload = FALSE;
					properties.eCategory = AudioCategory_Other;
					properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
					const HRESULT rawResult = client3->SetClientProperties(&properties);
					if (FAILED(rawResult))
					{
						client3->Release();
						return rawResult;
					}
				}

				UINT32 defaultPeriod = 0, fundamental = 0, minimum = 0, maximum = 0;
				HRESULT hr = client3->GetSharedModeEnginePeriod(engineFormat, &defaultPeriod, &fundamental, &minimum, &maximum);
				if (FAILED(hr) && autoConvert && deviceMixFormat.Format.nSamplesPerSec > 0 && engineFormat->nSamplesPerSec > 0)
				{
					// The engine only quotes periods for formats it runs natively. When the engine
					// is converting for us, ask in the device's own format and scale the frame
					// count to the stream's rate (a 16 kHz endpoint quoting 160 frames per 10 ms
					// becomes 480 frames at 48 kHz).
					hr = client3->GetSharedModeEnginePeriod(&deviceMixFormat.Format, &defaultPeriod, &fundamental, &minimum, &maximum);
					if (SUCCEEDED(hr))
					{
						const double scale = static_cast<double>(engineFormat->nSamplesPerSec) / deviceMixFormat.Format.nSamplesPerSec;
						defaultPeriod = static_cast<UINT32>(defaultPeriod * scale + 0.5);
						minimum = static_cast<UINT32>(minimum * scale + 0.5);
						maximum = static_cast<UINT32>(maximum * scale + 0.5);
					}
				}
				if (SUCCEEDED(hr))
				{
					periodFrames = minimum;
					hr = client3->InitializeSharedAudioStream(streamFlags, minimum, engineFormat, session);
					if (SUCCEEDED(hr))
					{
						std::ostringstream text;
						text << "engine periods default " << defaultPeriod << " / min " << minimum
							<< " / max " << maximum << " frames";
						if (raw && havePlainPeriods && (plainMinimum != minimum || plainMaximum != maximum))
							text << "; without raw: min " << plainMinimum << " / max " << plainMaximum;
						note = text.str();
					}
				}
				client3->Release();
				return hr;
			}

			HRESULT InitializeLegacy(const WAVEFORMATEX* engineFormat, DWORD streamFlags,
				REFERENCE_TIME bufferDuration, LPCGUID session, bool autoConvert)
			{
				DWORD flags = streamFlags;
				if (autoConvert) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
				// Shared mode rounds a too-small request up to the engine's own period, so
				// PortAudio's exclusive-sized duration is safe to pass straight through.
				return real->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0, engineFormat, session);
			}

			void FinishInitialize(const std::string& label, const PcmLayout& engine, bool convert,
				UINT32 periodFrames, DWORD streamFlags)
			{
				initialized = true;
				engineLayout = engine;
				needsConversion = convert;

				UINT32 bufferFrames = 0;
				real->GetBufferSize(&bufferFrames);
				REFERENCE_TIME latency = 0;
				real->GetStreamLatency(&latency);
				// The chunk PortAudio is told is "the buffer" and receives per event. Modern
				// rungs know the engine period; legacy rungs derive it from the device's default
				// period so the buffer == period contract holds on every rung (see CaptureClient).
				UINT32 chunk = periodFrames;
				if (chunk == 0 && presentedLayout.rate > 0)
				{
					REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
					if (SUCCEEDED(real->GetDevicePeriod(&defaultPeriod, &minimumPeriod)) && defaultPeriod > 0)
						chunk = static_cast<UINT32>((defaultPeriod * presentedLayout.rate + 5000000) / 10000000);
				}
				if (chunk == 0 || chunk > bufferFrames) chunk = bufferFrames;
				presentedBufferFrames = chunk;

				std::ostringstream text;
				text << deviceName << ": " << label
					<< "; presented to the game as " << Describe(presentedLayout)
					<< (convert ? " (converted from " + Describe(engine) + ")" : "")
					<< "; engine buffer " << bufferFrames << " frames";
				if (presentedLayout.rate > 0)
					text << std::fixed << std::setprecision(2) << " (" << 1000.0 * bufferFrames / presentedLayout.rate << " ms)";
				if (periodFrames > 0) text << ", period " << periodFrames << " frames";
				text << ", served to PortAudio in " << presentedBufferFrames << "-frame chunks";
				text << ", engine latency " << std::fixed << std::setprecision(2) << latency / 10000.0 << " ms"
					<< ((streamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) ? ", event-driven" : ", polled");
				LOG_INFO("(CABLE INPUT) " << text.str() << std::endl);
				SetStatus(text.str());

				std::lock_guard<std::mutex> guard(statusMutex);
				const bool modern = label.find("IAudioClient3") != std::string::npos;
				const bool raw = label.find(", raw") != std::string::npos;
				const bool converting = label.find("auto-convert") != std::string::npos || convert;
				diagnostics.inputPath = modern ? (raw ? "modern raw" : "modern") : "legacy shared";
				if (converting) diagnostics.inputPath += " +convert";
				diagnostics.inputFormat = Describe(presentedLayout);
				diagnostics.inputLatencyMs = presentedLayout.rate > 0
					? 1000.0 * presentedBufferFrames / presentedLayout.rate : 0.0;
				{
					std::ostringstream fmt;
					const uint32_t rate = deviceMixFormat.Format.nSamplesPerSec;
					if (rate > 0) fmt << (rate % 1000 == 0 ? std::to_string(rate / 1000) : std::to_string(rate / 1000.0).substr(0, 4)) << " kHz";
					diagnostics.deviceFormat = fmt.str();
				}
			}

			std::atomic<ULONG> refCount{ 1 };
			IAudioClient* real;
			IMMDevice* device;
			IUnknown* marshaler = nullptr;
			std::string deviceName;
			WAVEFORMATEXTENSIBLE presentedFormat{};
			WAVEFORMATEXTENSIBLE deviceMixFormat{};
			PcmLayout presentedLayout;
			PcmLayout engineLayout;
			bool initialized = false;
			bool needsConversion = false;
			UINT32 presentedBufferFrames = 0;
		};

		// Passive IAudioRenderClient: counts the buffers the game writes and measures their
		// peak, so "no sound at all" can be split into "the game writes silence" (game side)
		// and "the game writes audio the device never plays" (driver / exclusive-mode side).
		class RenderMonitorClient final : public IAudioRenderClient
		{
		public:
			RenderMonitorClient(IAudioRenderClient* realClient, const PcmLayout& layout)
				: real(realClient), engineLayout(layout)
			{
				CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(this), &marshaler);
			}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
			{
				if (!ppv) return E_POINTER;
				LOG_INFO("(OUTPUT) render client QueryInterface " << DescribeIid(riid) << " on thread " << GetCurrentThreadId() << std::endl);
				if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioRenderClient))
				{
					*ppv = static_cast<IAudioRenderClient*>(this);
					AddRef();
					return S_OK;
				}
				if (riid == __uuidof(IMarshal) && marshaler)
					return marshaler->QueryInterface(riid, ppv);
				*ppv = nullptr;
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG remaining = --refCount;
				if (remaining == 0) delete this;
				return remaining;
			}

			HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames, BYTE** data) override
			{
				const HRESULT hr = real->GetBuffer(frames, data);
				pending = (SUCCEEDED(hr) && data) ? *data : nullptr;
				return hr;
			}

			HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 framesWritten, DWORD flags) override
			{
				if (framesWritten > 0)
				{
					const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
					outputStats.OnPacket(silent ? 0.0f : MeasurePeak(pending, engineLayout, framesWritten), silent);
				}
				pending = nullptr;
				return real->ReleaseBuffer(framesWritten, flags);
			}

		private:
			~RenderMonitorClient()
			{
				if (marshaler) marshaler->Release();
				if (real) real->Release();
			}

			std::atomic<ULONG> refCount{ 1 };
			IAudioRenderClient* real;
			IUnknown* marshaler = nullptr;
			PcmLayout engineLayout;
			BYTE* pending = nullptr;
		};

		// Passive IAudioClient for the Wwise output: forwards every call untouched (share
		// mode, format, buffer sizes all stay the game's), and only records what was
		// negotiated and hands out the render monitor above.
		class OutputMonitorClient final : public IAudioClient
		{
		public:
			OutputMonitorClient(IAudioClient* realClient, IMMDevice* endpoint)
				: real(realClient)
			{
				deviceName = ReadDeviceName(endpoint);
				const HRESULT hr = CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(this), &marshaler);
				LOG_INFO("(OUTPUT) monitor wrapping " << deviceName << " on thread " << GetCurrentThreadId()
					<< ", free-threaded marshaler " << (SUCCEEDED(hr) ? "ready" : DescribeHresult(hr)) << std::endl);
			}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
			{
				if (!ppv) return E_POINTER;
				LOG_INFO("(OUTPUT) client QueryInterface " << DescribeIid(riid) << " on thread " << GetCurrentThreadId() << std::endl);
				if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioClient))
				{
					*ppv = static_cast<IAudioClient*>(this);
					AddRef();
					return S_OK;
				}
				if (riid == __uuidof(IMarshal) && marshaler)
					return marshaler->QueryInterface(riid, ppv);
				*ppv = nullptr;
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG remaining = --refCount;
				if (remaining == 0) delete this;
				return remaining;
			}

			HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE shareMode, DWORD streamFlags,
				REFERENCE_TIME bufferDuration, REFERENCE_TIME periodicity, const WAVEFORMATEX* format, LPCGUID session) override
			{
				const HRESULT hr = real->Initialize(shareMode, streamFlags, bufferDuration, periodicity, format, session);
				engineLayout = ReadLayout(format);
				UINT32 bufferFrames = 0;
				if (SUCCEEDED(hr)) real->GetBufferSize(&bufferFrames);
				std::ostringstream text;
				text << deviceName << ": " << (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE ? "EXCLUSIVE" : "shared")
					<< ((streamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) ? " event-driven" : " polled")
					<< ", " << Describe(engineLayout) << ", requested " << std::fixed << std::setprecision(2)
					<< bufferDuration / 10000.0 << " ms";
				if (SUCCEEDED(hr))
				{
					text << ", buffer " << bufferFrames << " frames";
					if (engineLayout.rate > 0) text << " (" << 1000.0 * bufferFrames / engineLayout.rate << " ms)";
					LOG_INFO("(OUTPUT) " << text.str() << std::endl);
				}
				else
				{
					LOG_ERROR("(OUTPUT) " << text.str() << " -> Initialize FAILED " << DescribeHresult(hr) << std::endl);
				}
				return hr;
			}

			HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override { return real->GetBufferSize(frames); }
			HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* latency) override { return real->GetStreamLatency(latency); }
			HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* padding) override { return real->GetCurrentPadding(padding); }
			HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE shareMode, const WAVEFORMATEX* format,
				WAVEFORMATEX** closestMatch) override
			{
				return real->IsFormatSupported(shareMode, format, closestMatch);
			}
			HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override { return real->GetMixFormat(format); }
			HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* defaultPeriod, REFERENCE_TIME* minimumPeriod) override
			{
				return real->GetDevicePeriod(defaultPeriod, minimumPeriod);
			}
			HRESULT STDMETHODCALLTYPE Start() override
			{
				LOG_INFO("(OUTPUT) Start on thread " << GetCurrentThreadId() << std::endl);
				const HRESULT hr = real->Start();
				if (SUCCEEDED(hr)) outputStats.OnStart();
				else LOG_ERROR("(OUTPUT) " << deviceName << ": Start FAILED " << DescribeHresult(hr) << std::endl);
				return hr;
			}
			HRESULT STDMETHODCALLTYPE Stop() override
			{
				LOG_INFO("(OUTPUT) Stop on thread " << GetCurrentThreadId() << std::endl);
				outputStats.startTick.store(0, std::memory_order_release);
				return real->Stop();
			}
			HRESULT STDMETHODCALLTYPE Reset() override
			{
				LOG_INFO("(OUTPUT) Reset on thread " << GetCurrentThreadId() << std::endl);
				return real->Reset();
			}
			HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE eventHandle) override
			{
				LOG_INFO("(OUTPUT) SetEventHandle on thread " << GetCurrentThreadId() << std::endl);
				return real->SetEventHandle(eventHandle);
			}
			HRESULT STDMETHODCALLTYPE GetService(REFIID riid, void** ppv) override
			{
				if (!ppv) return E_POINTER;
				LOG_INFO("(OUTPUT) GetService " << DescribeIid(riid) << " on thread " << GetCurrentThreadId() << std::endl);
				if (riid != __uuidof(IAudioRenderClient)) return real->GetService(riid, ppv);
				IAudioRenderClient* realRender = nullptr;
				const HRESULT hr = real->GetService(riid, reinterpret_cast<void**>(&realRender));
				if (FAILED(hr) || !realRender) return hr;
				*ppv = new RenderMonitorClient(realRender, engineLayout);
				return S_OK;
			}

		private:
			~OutputMonitorClient()
			{
				if (marshaler) marshaler->Release();
				if (real) real->Release();
			}

			std::atomic<ULONG> refCount{ 1 };
			IAudioClient* real;
			IUnknown* marshaler = nullptr;
			std::string deviceName;
			PcmLayout engineLayout;
		};

		HRESULT STDMETHODCALLTYPE Hook_Activate(IMMDevice* self, REFIID iid, DWORD clsContext,
			PROPVARIANT* activationParams, void** ppv)
		{
			const HRESULT hr = originalActivate(self, iid, clsContext, activationParams, ppv);
			if (FAILED(hr) || !ppv || !*ppv || iid != __uuidof(IAudioClient)
				|| (!wrapCaptureActivation && !monitorRenderActivation))
				return hr;

			EDataFlow flow = eRender;
			IMMEndpoint* endpoint = nullptr;
			if (SUCCEEDED(self->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(&endpoint))) && endpoint)
			{
				endpoint->GetDataFlow(&flow);
				endpoint->Release();
			}

			if (flow == eCapture && wrapCaptureActivation)
			{
				IMMDevice* selected = self;
				if (routingConfiguration.enabled && !routingConfiguration.inputDeviceId.empty())
				{
					IMMDeviceEnumerator* enumerator = nullptr;
					HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
						__uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
					if (SUCCEEDED(result))
					{
						result = enumerator->GetDevice(routingConfiguration.inputDeviceId.c_str(), &selected);
						enumerator->Release();
					}
					static_cast<IAudioClient*>(*ppv)->Release();
					*ppv = nullptr;
					if (FAILED(result)) return result;
					IMMEndpoint* selectedEndpoint = nullptr;
					result = selected->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(&selectedEndpoint));
					EDataFlow selectedFlow = eAll;
					if (SUCCEEDED(result)) { result = selectedEndpoint->GetDataFlow(&selectedFlow); selectedEndpoint->Release(); }
					if (FAILED(result) || selectedFlow != eCapture) { selected->Release(); return FAILED(result) ? result : E_INVALIDARG; }
					result = originalActivate(selected, iid, clsContext, activationParams, ppv);
					if (FAILED(result)) { selected->Release(); return result; }
				}
				*ppv = static_cast<IAudioClient*>(new ModernAudioClient(static_cast<IAudioClient*>(*ppv), selected));
				if (routingConfiguration.enabled && !routingConfiguration.inputDeviceId.empty()) selected->Release();
			}
			else if (flow == eRender && monitorRenderActivation)
			{
				// Diagnostic only, and only when routing did not already claim the render open.
				auto* monitor = new OutputMonitorClient(static_cast<IAudioClient*>(*ppv), self);
				*ppv = static_cast<IAudioClient*>(monitor);
			}
			return hr;
		}

		// Patches IMMDevice::Activate on MMDevAPI's endpoint class. Every endpoint object in the
		// process shares that vtable, so one patch covers the device PortAudio opens later.
		// Runs lazily on the thread that is opening the stream, where COM is already up.
		bool EnsureActivateHook()
		{
			static int state = 0; // 0 untried, 1 installed, -1 failed
			if (state != 0) return state > 0;
			state = -1;

			IMMDeviceEnumerator* enumerator = nullptr;
			HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				__uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
			if (FAILED(hr) || !enumerator)
			{
				LOG_ERROR("(CABLE INPUT) MMDeviceEnumerator unavailable (" << DescribeHresult(hr)
					<< "); staying on the legacy shared input" << std::endl);
				return false;
			}

			IMMDeviceCollection* collection = nullptr;
			hr = enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &collection);
			UINT count = 0;
			if (SUCCEEDED(hr) && collection) collection->GetCount(&count);
			IMMDevice* device = nullptr;
			if (count > 0) collection->Item(0, &device);
			if (collection) collection->Release();
			if (!device)
			{
				LOG_ERROR("(AUDIO ROUTING) No active audio endpoint is available for activation" << std::endl);
				enumerator->Release();
				return false;
			}

			originalActivate = reinterpret_cast<MmDeviceActivate_t>(
				ComVTable::PatchSlot(device, SLOT_MMDEVICE_ACTIVATE, reinterpret_cast<void*>(&Hook_Activate)));
			if (!originalActivate)
			{
				LOG_ERROR("(CABLE INPUT) Could not patch IMMDevice::Activate; staying on the legacy shared input" << std::endl);
				device->Release();
				enumerator->Release();
				return false;
			}

			pinnedEnumerator = enumerator;
			pinnedDevice = device;
			state = 1;
			LOG_INFO("(CABLE INPUT) Modern capture client armed for the game's next input open" << std::endl);
			return true;
		}

		int __cdecl Hook_PaOpenStream(void** stream, const PaStreamParameters* inputParameters,
			const PaStreamParameters* outputParameters, double sampleRate, unsigned long framesPerBuffer,
			unsigned long streamFlags, void* streamCallback, void* userData)
		{
			auto* streamInfo = (inputParameters != nullptr)
				? reinterpret_cast<PaWasapiStreamInfo*>(const_cast<void*>(inputParameters->hostApiSpecificStreamInfo))
				: nullptr;
			const bool inputOnlyOpen = inputParameters != nullptr && outputParameters == nullptr && streamInfo != nullptr;
			const bool outputOnlyOpen = inputParameters == nullptr && outputParameters != nullptr;
			if (outputOnlyOpen && outputMonitorEnabled && modernInputEnabled && !routingConfiguration.enabled && EnsureActivateHook())
			{
				// The Wwise sink: observe only. Nothing about the open changes. Skipped when
				// audio routing owns the render open below.
				monitorRenderActivation = true;
				const int result = originalPaOpenStream(stream, inputParameters, outputParameters, sampleRate,
					framesPerBuffer, streamFlags, streamCallback, userData);
				monitorRenderActivation = false;
				LOG_INFO("(OUTPUT) Pa_OpenStream output device " << outputParameters->device
					<< " ch " << outputParameters->channelCount << " sr " << sampleRate
					<< " frames " << framesPerBuffer << " -> result " << result << std::endl);
				return result;
			}
			if (outputOnlyOpen && routingConfiguration.enabled)
			{
				if (!persistentOutputEnabled || !outputParameters->hostApiSpecificStreamInfo)
				{
					LOG_ERROR("(AUDIO ROUTING) Refusing output open: permanent bridge output is not installed or stream information is missing." << std::endl);
					return -9999;
				}
				PaStreamParameters parameters = *outputParameters;
				const auto* callerInfo = static_cast<const PaWasapiStreamInfo*>(parameters.hostApiSpecificStreamInfo);
				if (callerInfo->size < offsetof(PaWasapiStreamInfo, flags) + sizeof(callerInfo->flags) || callerInfo->size > 4096) return -9999;
				// Preserve fields added after the prefix known to this mod.
				std::vector<byte> infoStorage(callerInfo->size);
				std::memcpy(infoStorage.data(), callerInfo, callerInfo->size);
				auto* info = reinterpret_cast<PaWasapiStreamInfo*>(infoStorage.data());
				info->flags |= PA_WASAPI_EXCLUSIVE;
				parameters.hostApiSpecificStreamInfo = info;
				const int result = originalPaOpenStream(stream, inputParameters, &parameters, sampleRate,
					framesPerBuffer, streamFlags, streamCallback, userData);
				LOG_INFO("(AUDIO ROUTING) Game output stream open result " << result << std::endl);
				return result;
			}
			if (!inputOnlyOpen || inputOwnedByAsio)
			{
#if defined(RSMODS_AUDIO_LIFECYCLE_TRACE)
				// When RS_ASIO owns capture every open passes straight through, so this is the only place
				// that shows whether the game issues an input open at all (e.g. after re-selecting Real
				// Tone Cable).
				if (inputParameters != nullptr && inputOwnedByAsio)
				{
					LOG_INFO("(INPUT DIAG) game opened an input stream under RS_ASIO ownership: device "
						<< inputParameters->device << " ch " << inputParameters->channelCount
						<< " sr " << sampleRate << " frames " << framesPerBuffer
						<< " (input-only=" << (inputOnlyOpen ? 1 : 0) << ")." << std::endl);
				}
#endif
				return originalPaOpenStream(stream, inputParameters, outputParameters, sampleRate,
					framesPerBuffer, streamFlags, streamCallback, userData);
			}

			const unsigned long callerFlags = streamInfo->flags;
			if (persistentInputEnabled)
			{
				streamInfo->flags = callerFlags | PA_WASAPI_EXCLUSIVE;
				const int result = originalPaOpenStream(stream, inputParameters, outputParameters, sampleRate,
					framesPerBuffer, streamFlags, streamCallback, userData);
				streamInfo->flags = callerFlags;
				LOG_INFO("(PERSISTENT INPUT) Game capture open result " << result << std::endl);
				return result;
			}
			int result = -1;
			bool usedModernPath = false;

			if (modernInputEnabled)
			{
				if (!EnsureActivateHook())
				{
					SetStatus("modern input activation hook unavailable");
					LOG_ERROR("(CABLE INPUT) Modern input activation hook unavailable" << std::endl);
					return -9999;
				}
				// Ask PortAudio for its exclusive, event-driven shape regardless of the ini: the
				// wrapped client turns that into a modern shared stream, and event mode is the
				// low-latency path PortAudio only takes for "exclusive" streams.
				streamInfo->flags = callerFlags | PA_WASAPI_EXCLUSIVE;
				wrapCaptureActivation = true;
				result = originalPaOpenStream(stream, inputParameters, outputParameters, sampleRate,
					framesPerBuffer, streamFlags, streamCallback, userData);
				wrapCaptureActivation = false;
				usedModernPath = true;
				if (result != 0)
				{
					SetStatus("modern input open failed");
					LOG_ERROR("(CABLE INPUT) Modern input open failed (PortAudio " << result << ")" << std::endl);
				}
			}

			else
			{
				// Legacy capture uses the device's shared format, independently of bridge playback.
				streamInfo->flags = callerFlags & ~PA_WASAPI_EXCLUSIVE;
				result = originalPaOpenStream(stream, inputParameters, outputParameters, sampleRate,
					framesPerBuffer, streamFlags, streamCallback, userData);
				if (result == 0)
				{
					SetStatus("legacy shared input (polled)");
					std::lock_guard<std::mutex> guard(statusMutex);
					diagnostics.inputPath = "stock shared (polled)";
					diagnostics.inputFormat.clear();
					diagnostics.inputLatencyMs = 0.0;
				}
			}
			streamInfo->flags = callerFlags;

			LOG_INFO("(CABLE INPUT) Pa_OpenStream input device " << inputParameters->device
				<< " ch " << inputParameters->channelCount << " fmt " << inputParameters->sampleFormat
				<< " sr " << sampleRate << " frames " << framesPerBuffer
				<< (usedModernPath ? " via modern client" : " via legacy shared")
				<< " -> result " << result << std::endl);
			return result;
		}

		void ReadCableDisplaySettings()
		{
			char executablePath[MAX_PATH];
			GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
			const auto iniPath = (std::filesystem::path(executablePath).parent_path() / "RSMods.ini").string();
			char monitor[16] = {};
			GetPrivateProfileStringA("Mod Settings", "MonitorOutput", "off", monitor, sizeof(monitor), iniPath.c_str());
			outputMonitorEnabled = _stricmp(monitor, "on") == 0;
			char overlay[16] = {};
			GetPrivateProfileStringA("Mod Settings", "AudioDiagnosticsOverlay", "on", overlay, sizeof(overlay), iniPath.c_str());
			overlayEnabled = _stricmp(overlay, "off") != 0;
		}

		bool ReadCableForPlayerTwoSetting(const std::filesystem::path& gameDirectory)
		{
			const auto iniPath = (gameDirectory / "RSMods.ini").string();
			char value[16]{};
			GetPrivateProfileStringA("Mod Settings", "CableForPlayerTwo", "off", value, sizeof(value), iniPath.c_str());
			return _stricmp(value, "on") == 0;
		}

		// The game writes its own view of both streams to audiodump.txt at start-up (when
		// Rocksmith.ini DumpAudioLog=1): "WASAPI::OpenStream(output): ... latency[ 3.00ms ]
		// exclusive[ YES ] ...". That is the authoritative output latency, so the overlay reads
		// it rather than guessing; re-read whenever the file grows (the sink reopens on focus
		// changes and device swaps).
		void RefreshAudioDumpFigures()
		{
			static uint64_t lastSize = ~0ull;
			static uint64_t lastCheckTick = 0;
			const uint64_t now = GetTickCount64();
			if (now - lastCheckTick < 2000) return;
			lastCheckTick = now;

			char executablePath[MAX_PATH];
			GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
			const auto path = std::filesystem::path(executablePath).parent_path() / "audiodump.txt";
			std::error_code error;
			const uint64_t size = std::filesystem::exists(path, error) ? std::filesystem::file_size(path, error) : 0;
			if (size == lastSize) return;
			lastSize = size;

			std::ifstream file(path, std::ios::in);
			if (!file) return;
			std::string line, outputLine, inputLine;
			while (std::getline(file, line))
			{
				if (line.find("WASAPI::OpenStream(output)") != std::string::npos) outputLine = line;
				else if (line.find("WASAPI::OpenStream(input)") != std::string::npos) inputLine = line;
			}

			auto parseLatency = [](const std::string& text) -> double
			{
				const size_t at = text.find("latency[");
				if (at == std::string::npos) return 0.0;
				return std::atof(text.c_str() + at + 8);
			};

			std::lock_guard<std::mutex> guard(statusMutex);
			if (!outputLine.empty() && !routingConfiguration.enabled)
			{
				diagnostics.outputLatencyMs = parseLatency(outputLine);
				diagnostics.outputExclusive = outputLine.find("exclusive[ YES ]") != std::string::npos;
				diagnostics.outputKnown = diagnostics.outputLatencyMs > 0.0;
			}
			if (!inputLine.empty()) diagnostics.inputLatencyGameMs = parseLatency(inputLine);
		}
	}

	void Install()
	{
		static bool installed = false;
		if (installed) return;
		installed = true;

		char executablePath[MAX_PATH];
		GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
		const auto gameDir = std::filesystem::path(executablePath).parent_path();
		const auto routingPath = gameDir / "RSMods.ini";
		if (GetPrivateProfileIntW(L"Audio Bridge", L"MasterEnabled", 1, routingPath.c_str()) != 1)
		{
			LOG_INFO("(CABLE INPUT) Audio Bridge master power is off; leaving audio setup untouched" << std::endl);
			return;
		}
		masterEnabled = true;
		routingConfiguration = SharedOutput::ReadConfiguration();
		// Host the control pipe regardless of routing state so the GUI can always find the running
		// game. When routing is off the bridge passes audio through untouched and the pipe reports a
		// passthrough status; engaging routing later registers the live output session with it.
		SharedOutput::StartControlServer();
		const auto asioSettings = (gameDir / "RS_ASIO.ini").string();
		char asioInputZero[512]{};
		char asioInputOne[512]{};
		char asioInputMicrophone[512]{};
		GetPrivateProfileStringA("Asio.Input.0", "Driver", "", asioInputZero, sizeof(asioInputZero), asioSettings.c_str());
		GetPrivateProfileStringA("Asio.Input.1", "Driver", "", asioInputOne, sizeof(asioInputOne), asioSettings.c_str());
		GetPrivateProfileStringA("Asio.Input.Mic", "Driver", "", asioInputMicrophone, sizeof(asioInputMicrophone), asioSettings.c_str());
		const bool hasConfiguredAsioInput = asioInputZero[0] != '\0' || asioInputOne[0] != '\0' || asioInputMicrophone[0] != '\0';
		// Cable mode turns RS_ASIO off by renaming its DLL and leaves RS_ASIO.ini alone, so EnableAsio=1 alone
		// does not mean RS_ASIO is loaded.
		const bool asioEnabled = GetPrivateProfileIntA("Config", "EnableAsio", 0, asioSettings.c_str()) == 1
			&& std::filesystem::exists(gameDir / "RS_ASIO.dll");
		const bool usesWindowsInput = GetPrivateProfileIntA("Config", "EnableWasapiInputs", 0, asioSettings.c_str()) == 1;
		if (asioEnabled && std::filesystem::exists(gameDir / "RS_ASIO.dll") && hasConfiguredAsioInput && !usesWindowsInput)
		{
			inputOwnedByAsio = true;
			asioPath.store(true, std::memory_order_release);
			SetStatus("RS_ASIO owns the input");
			ReadCableDisplaySettings();   // diagnostics HUD and output monitor switches
			{
				std::lock_guard<std::mutex> guard(statusMutex);
				diagnostics.rsAsio = true;
			}
			LOG_INFO("(CABLE INPUT) RS_ASIO present; leaving the input path to it" << std::endl);
			PersistentInput::SetCableForPlayerTwoEnabled(ReadCableForPlayerTwoSetting(gameDir));
			if (!routingConfiguration.enabled)
			{
				if (!PersistentInput::Install(L"", L"", false, false, PersistentInput::EnumeratorSource::RsAsio))
					LOG_ERROR("(CABLE INPUT) Could not install the live Player 2 Cable wrapper around RS_ASIO" << std::endl);
				return;
			}
			char outputDriver[512]{};
			GetPrivateProfileStringA("Asio.Output", "Driver", "", outputDriver, sizeof(outputDriver), asioSettings.c_str());
			if (GetPrivateProfileIntA("Config", "EnableWasapiOutputs", 0, asioSettings.c_str()) != 1 || outputDriver[0] != '\0')
			{
				LOG_ERROR("(AUDIO ROUTING) RS_ASIO must use EnableWasapiOutputs=1 and an empty Asio.Output Driver; output routing stays with RS_ASIO. Routing was not installed." << std::endl);
				if (!PersistentInput::Install(L"", L"", false, false, PersistentInput::EnumeratorSource::RsAsio))
					LOG_ERROR("(CABLE INPUT) Could not install the live Player 2 Cable wrapper around RS_ASIO" << std::endl);
				return;
			}
		}
		else if (asioEnabled)
			LOG_WARNING("(CABLE INPUT) RS_ASIO is enabled without an ASIO input; Cable mode requires EnableAsio=0" << std::endl);
		// Cable mode always gives the game the bridge's own input and output devices, so Rocksmith's start-up
		// device scan never reaches a Windows endpoint that can fail it ("No audio output device"). With no
		// OutputDevice chosen, the bridge plays to the Windows default.
		if (!inputOwnedByAsio) routingConfiguration.enabled = true;

		ReadCableDisplaySettings();
		modernInputEnabled = !inputOwnedByAsio;
		if (inputOwnedByAsio)
			LOG_INFO("(CABLE INPUT) RS_ASIO owns capture; the Audio Bridge ASIO driver reads the cable itself" << std::endl);
		auto* target = reinterpret_cast<byte*>(Offsets::func_PortAudioOpenStream.Get());
		if (target == nullptr)
		{
			if (inputOwnedByAsio) PersistentInput::Install(L"", L"", false, false, PersistentInput::EnumeratorSource::RsAsio);
			SetStatus("unsupported game version");
			LOG_ERROR("(CABLE INPUT) Pa_OpenStream offset unknown for this Rocksmith version; input left stock" << std::endl);
			return;
		}

		originalPaOpenStream = reinterpret_cast<PaOpenStream_t>(
			DetourFunction(target, reinterpret_cast<byte*>(&Hook_PaOpenStream)));
		if (originalPaOpenStream == nullptr)
		{
			if (inputOwnedByAsio) PersistentInput::Install(L"", L"", false, false, PersistentInput::EnumeratorSource::RsAsio);
			SetStatus("Pa_OpenStream detour failed");
			LOG_ERROR("(CABLE INPUT) Failed to detour Pa_OpenStream; input left stock" << std::endl);
			return;
		}
		if (!inputOwnedByAsio)
		{
			// Cable mode owns both game-facing endpoints. Do not ask RS_ASIO to enumerate or
			// initialize an ASIO driver just to reach a Windows speaker. The permanent output
			// stays valid while its independent physical backend opens, disappears or recovers.
			const bool replaceOutput = true;
			const auto source = asioEnabled
				? PersistentInput::EnumeratorSource::RsAsio
				: PersistentInput::EnumeratorSource::Game;
			const bool installed = PersistentInput::Install(routingConfiguration.inputDeviceId,
				routingConfiguration.outputDeviceId, true, replaceOutput, source, true);
			if (!installed)
			{
				SetStatus("permanent Cable installation failed: unsupported or modified game calls");
				LOG_ERROR("(PERSISTENT INPUT) Could not verify the device-enumeration call sites. Cable capture was not installed." << std::endl);
				return;
			}
			persistentInputEnabled = true;
			persistentOutputEnabled = replaceOutput;
			modernInputEnabled = false;
		}

		SetStatus(persistentInputEnabled ? "Persistent input: waiting for cable" : inputOwnedByAsio ? "ASIO input; shared output armed" :
			(modernInputEnabled ? "armed, waiting for the game to open its input" : "legacy shared input"));
		{
			std::lock_guard<std::mutex> guard(statusMutex);
			diagnostics.installed = true;
			if (persistentInputEnabled) diagnostics.inputPath = "persistent Cable input";
		}
		LOG_INFO("(CABLE INPUT) Pa_OpenStream detoured; "
			<< (persistentInputEnabled ? "persistent Cable input enabled" : modernInputEnabled ? "modern capture client enabled" : "legacy shared input only")
			<< (persistentOutputEnabled ? "; permanent shared output enabled" : "; game output unchanged")
			<< (outputMonitorEnabled ? "; output monitor ON (MonitorOutput=on)" : "; output monitor off")
			<< std::endl);
	}

	namespace
	{
		constexpr uint64_t LEVEL_REPORT_INTERVAL_MS = 20000;

		struct PollState
		{
			uint32_t reportedGeneration = 0;
			bool stalled = false;
			uint64_t lastLevelReportTick = 0;
		};

		void PollDirection(StreamStats& stats, PollState& state, const char* tag, const char* noun)
		{
			const uint64_t started = stats.startTick.load(std::memory_order_acquire);
			if (started == 0) return;
			const uint64_t now = GetTickCount64();
			const uint32_t generation = stats.generation.load(std::memory_order_acquire);
			const uint64_t packets = stats.packets.load(std::memory_order_relaxed);
			const uint64_t lastPacket = stats.lastPacketTick.load(std::memory_order_relaxed);

			if (state.reportedGeneration != generation)
			{
				if (now - started < FIRST_REPORT_DELAY_MS) return;
				state.reportedGeneration = generation;
				state.stalled = false;
				state.lastLevelReportTick = now;
				if (packets == 0)
				{
					LOG_ERROR(tag << " " << noun << " started " << (now - started) / 1000
						<< " s ago but has moved NO audio buffers" << std::endl);
					state.stalled = true;
				}
				else
				{
					LOG_INFO(tag << " " << noun << " alive: " << packets << " buffers in " << (now - started) / 1000
						<< " s, " << DescribeLevel(stats.TakePeak()) << std::endl);
				}
				return;
			}

			const bool quiet = lastPacket == 0 || now - lastPacket > STALL_THRESHOLD_MS;
			if (quiet && !state.stalled)
			{
				state.stalled = true;
				LOG_WARNING(tag << " " << noun << " stalled: no buffers for " << STALL_THRESHOLD_MS / 1000 << " s" << std::endl);
			}
			else if (!quiet && state.stalled)
			{
				state.stalled = false;
				LOG_INFO(tag << " " << noun << " resumed, " << DescribeLevel(stats.TakePeak()) << std::endl);
			}
			else if (!quiet && now - state.lastLevelReportTick >= LEVEL_REPORT_INTERVAL_MS)
			{
				// Periodic level line: the peak since the last report, so a strum (input) or
				// the menu music (output) shows up as a number without any other tooling.
				state.lastLevelReportTick = now;
				LOG_INFO(tag << " " << noun << " level: " << DescribeLevel(stats.TakePeak())
					<< " over the last " << LEVEL_REPORT_INTERVAL_MS / 1000 << " s (" << packets << " buffers total)" << std::endl);
			}
		}
	}

	void Poll()
	{
		if (!masterEnabled) return;

		SharedOutput::RefreshProxyOutput();
		if (inputOwnedByAsio)
		{
			const int proxyInputMode = OutputTap::ProxyInputMode();
			if (proxyInputMode == 2) SetStatus("Real Tone Cable fallback is live");
			else if (proxyInputMode == 1) SetStatus("ASIO input unavailable; waiting for Real Tone Cable");
			else SetStatus("ASIO input is live");
			std::lock_guard<std::mutex> guard(statusMutex);
			diagnostics.proxyInputMode = proxyInputMode;
		}
		if (persistentInputEnabled)
		{
			const bool ready = AsioHook::IsInputReady(0);
			SetStatus(ready ? "Persistent input: cable ready" : "Persistent input: waiting for cable");
			if (!ready) { UpdateMeter(0.0f); measuredAny.store(false, std::memory_order_relaxed); }
		}
		static PollState inputState;
		static PollState outputState;
		static PollState playerTwoState;
		PollDirection(tapStats, inputState, "(INPUT)", "Input");
		AsioHook::PollInputStageMeter();
		PollDirection(outputStats, outputState, "(OUTPUT)", "Output");
		PollDirection(playerTwoStats, playerTwoState, "(INPUT P2)", "Player 2 input");

		// Overlay figures: packets per second over a 1 s window, stall state, and the
		// game's own latency lines. Off the audio thread, a few dozen bytes of work.
		static uint64_t rateTick = 0;
		static uint64_t ratePackets = 0;
		static uint64_t ratePlayerTwoPackets = 0;
		const uint64_t now = GetTickCount64();
		const uint64_t packets = tapStats.packets.load(std::memory_order_relaxed);
		const uint64_t playerTwoPackets = playerTwoStats.packets.load(std::memory_order_relaxed);
		if (rateTick == 0) { rateTick = now; ratePackets = packets; ratePlayerTwoPackets = playerTwoPackets; }
		else if (now - rateTick >= 1000)
		{
			const double perSecond = 1000.0 * static_cast<double>(packets - ratePackets) / static_cast<double>(now - rateTick);
			const double playerTwoPerSecond = 1000.0 * static_cast<double>(playerTwoPackets - ratePlayerTwoPackets) / static_cast<double>(now - rateTick);
			rateTick = now;
			ratePackets = packets;
			ratePlayerTwoPackets = playerTwoPackets;
			// Player 2's path: the P2 cable wrapper beside RS_ASIO (always listed once installed;
			// its toggle gates the audio per packet) or a configured second ASIO input route.
			const bool playerTwoCable = PersistentInput::IsCableForPlayerTwoAvailable();
			const bool playerTwoRoute = AsioHook::IsInputConfigured(1);
			std::lock_guard<std::mutex> guard(statusMutex);
			diagnostics.packetsPerSecond = perSecond;
			diagnostics.streamActive = tapStats.startTick.load(std::memory_order_acquire) != 0;
			diagnostics.stalled = inputState.stalled;
			diagnostics.playerTwoInput = playerTwoCable || playerTwoRoute;
			diagnostics.playerTwoCableFeedOff = playerTwoCable && !playerTwoRoute && !PersistentInput::IsCableForPlayerTwoEnabled();
			diagnostics.playerTwoPacketsPerSecond = playerTwoPerSecond;
			diagnostics.playerTwoStreamActive = playerTwoStats.startTick.load(std::memory_order_acquire) != 0;
			diagnostics.playerTwoStalled = playerTwoState.stalled;
		}
		RefreshAudioDumpFigures();

		// Evidence line for the latency measurement (every 5 s while a stream runs): the raw
		// clock delta between "now" at hand-off and the packet's device timestamp, the packet
		// length, and the averaged figure, so the timestamp's meaning can be settled from the
		// log rather than assumed.
		static uint64_t lastEvidenceTick = 0;
		if (measuredAny.load(std::memory_order_relaxed) && now - lastEvidenceTick >= 5000)
		{
			lastEvidenceTick = now;
			const uint32_t bits = measuredLagBits.load(std::memory_order_relaxed);
			float ema = 0.0f;
			std::memcpy(&ema, &bits, sizeof(ema));
			// Record timer resolution as diagnostic context without changing it.
			ULONG timerMax = 0, timerMin = 0, timerCurrent = 0;
			using NtQueryTimerResolution_t = LONG(NTAPI*)(PULONG, PULONG, PULONG);
			static const auto queryTimer = reinterpret_cast<NtQueryTimerResolution_t>(
				GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution"));
			if (queryTimer) queryTimer(&timerMax, &timerMin, &timerCurrent);
			LOG_INFO("(CABLE INPUT) latency evidence: source=" << lastRawSource.load(std::memory_order_relaxed)
				<< " now-qpc=" << std::fixed << std::setprecision(2) << lastRawDelta100ns.load(std::memory_order_relaxed) / 10000.0
				<< " ms, packet=" << lastRawFrames.load(std::memory_order_relaxed) << " frames, timestamp-lag ema=" << ema
				<< " ms, system timer " << std::setprecision(3) << timerCurrent / 10000.0 << " ms" << std::endl);
		}
	}

	std::string DescribeStatus()
	{
		std::lock_guard<std::mutex> guard(statusMutex);
		return statusDescription;
	}

	Diagnostics GetDiagnostics()
	{
		Diagnostics copy;
		{
			std::lock_guard<std::mutex> guard(statusMutex);
			copy = diagnostics;
		}
		copy.meterPeak = ReadMeterBits(meterBits);
		copy.playerTwoMeterPeak = ReadMeterBits(playerTwoMeterBits);
		copy.dropouts = dropoutCount.load(std::memory_order_relaxed);
		const uint32_t ageBits = measuredLagBits.load(std::memory_order_relaxed);
		float age = 0.0f;
		std::memcpy(&age, &ageBits, sizeof(age));
		copy.captureTimestampValid = measuredAny.load(std::memory_order_acquire)
			&& GetTickCount64() - measuredTick.load(std::memory_order_relaxed) < 1000
			&& std::isfinite(age) && age >= 0.0f;
		const uint32_t rate = tapRate.load(std::memory_order_relaxed);
		copy.capturePacketMs = rate ? 1000.0 * lastRawFrames.load(std::memory_order_relaxed) / rate : 0.0;
		copy.captureTimestampLagMs = age;
		copy.tapPacketFrames = tapFrames.load(std::memory_order_relaxed);
		copy.tapSampleRate = tapRate.load(std::memory_order_relaxed);
		return copy;
	}

	void ReportTapPacket(float peak, bool silent, uint32_t frames, uint32_t sampleRate)
	{
		if (tapStats.startTick.load(std::memory_order_acquire) == 0) tapStats.OnStart();
		tapFrames.store(frames, std::memory_order_relaxed);
		tapRate.store(sampleRate, std::memory_order_relaxed);
		tapStats.OnPacket(peak, silent);
		UpdateMeter(silent ? 0.0f : peak);
	}

	void ReportPlayerTwoPacket(float peak, bool silent)
	{
		if (playerTwoStats.startTick.load(std::memory_order_acquire) == 0) playerTwoStats.OnStart();
		playerTwoStats.OnPacket(peak, silent);
		UpdateMeterBits(playerTwoMeterBits, silent ? 0.0f : peak);
	}

	bool IsAsioPath()
	{
		return asioPath.load(std::memory_order_acquire);
	}

	void ReportMeasuredInputRaw(int64_t delta100ns, uint32_t frames)
	{
		lastRawDelta100ns.store(delta100ns, std::memory_order_relaxed);
		lastRawFrames.store(frames, std::memory_order_relaxed);
		lastRawSource.store(1, std::memory_order_relaxed);
	}

	void ReportCaptureTimestampLag(double lagMs)
	{
		if (!std::isfinite(lagMs) || lagMs < 0.0)
		{
			measuredAny.store(false, std::memory_order_release);
			return;
		}
		uint32_t bits = measuredLagBits.load(std::memory_order_relaxed);
		float current = 0.0f;
		std::memcpy(&current, &bits, sizeof(current));
		const uint64_t now = GetTickCount64();
		const bool first = !measuredAny.load(std::memory_order_relaxed)
			|| now - measuredTick.load(std::memory_order_relaxed) >= 1000;
		const float next = first ? static_cast<float>(lagMs)
			: current + 0.01f * (static_cast<float>(lagMs) - current);
		std::memcpy(&bits, &next, sizeof(bits));
		measuredLagBits.store(bits, std::memory_order_relaxed);
		measuredTick.store(now, std::memory_order_relaxed);
		measuredAny.store(true, std::memory_order_release);
	}

	bool IsOverlayEnabled()
	{
		return overlayEnabled.load(std::memory_order_acquire);
	}

	void SetOverlayEnabled(bool enabled)
	{
		overlayEnabled.store(enabled, std::memory_order_release);
	}
}
