#include "stdafx.h"
#include "AsioHook.hpp"
#include "AsioBufferState.hpp"
#include "AudioLifecycleTrace.hpp"
#include "ComVTable.hpp"
#include "CaptureProcessingGate.hpp"
#include "CableInput.hpp"
#include "MlAudioExporter.hpp"
#include "RawPitchVerifier.hpp"
#include "DrySignalRecording.hpp"
#include "PersistentInput.hpp"
#include "HumRemover.hpp"
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace Audio::AsioHook
{
	namespace
	{
		constexpr size_t SLOT_CAPTURE_CLIENT_GET_BUFFER = 3;
		constexpr uint32_t MAX_BUFFER_FRAMES = 4096;

		// Make-up gain applied to the real guitar input (route 0) before the game's amp and note
		// gate see it. A Real Tone Cable carries a hot built-in preamp (~+10-15 dB); an interface
		// through RS_ASIO arrives that much quieter, so the game's level-sensitive noise gate mutes
		// sustained notes early (worst on bends, where the fundamental momentarily dips). Lifting the
		// input here restores the cable's headroom above the gate. 1.0 = unity (feature off).
		std::atomic<float> g_inputGainLinear{ 1.0f };

		// Adaptive suppressor threshold, as a LINEAR amplitude, keyed to the RAW pre-gain input.
		// 0 = disabled (feature off, input untouched). The energy detector requires a sustained onset,
		// rejects idle floor and isolated spikes, then closes gradually after a note.
		std::atomic<float> g_gateThresholdLinear{ 0.0f };
		std::atomic<uint32_t> g_gateThresholdRevision{ 0 };

		// Input compressor strength, 0..1 (0 = disabled). Emulates what the game's amp does when a hot
		// Real Tone Cable drives it into compression: it squeezes the natural open-string AMPLITUDE
		// BEATING out of the signal BEFORE the game amp, so the amp no longer magnifies that wobble into
		// an audible warble on a quiet, undriven interface input (heard as the gate coming in and out).
		std::atomic<float> g_compressorStrength{ 0.0f };

		// Mains-hum notch base frequency in Hz: 0 = disabled (feature off, input untouched), else 50 or 60.
		// A grounded interface injects a stable ground-loop hum (a 50/60 Hz harmonic comb) that a single-USB
		// Real Tone Cable never has; a bank of narrow notches at the fundamental and its harmonics removes it
		// continuously (even during notes, unlike a gate) with negligible tone loss.
		std::atomic<float> g_humFilterBaseHz{ 0.0f };

		// Latency round-trip capture: when armed, raw route-0 input (the proxy's probe looped back through a
		// physical out->in loop) is copied into a fixed buffer until full, then the host cross-correlates it
		// against the known probe to recover the delay. Lock-free: the control thread never resizes (fixed
		// buffer), only arms/reads; the audio thread appends by index. One-shot per arm.
		constexpr int kLatencyMaxFrames = 48000;   // up to 1 s at 48 kHz
		float g_latencyCapture[kLatencyMaxFrames]{};
		std::atomic<int> g_latencyTarget{ 0 };   // >0 = capturing this many frames
		std::atomic<int> g_latencyFilled{ 0 };

		constexpr float INT16_TO_FLOAT = 1.0f / 32768.0f;
		constexpr float INT24_TO_FLOAT = 1.0f / 8388608.0f;
		constexpr float INT32_TO_FLOAT = 1.0f / 2147483648.0f;

		// Front-of-chain input low-pass cutoff (Hz), applied only while the make-up gain is engaged.
		// The DI guitar's musical energy sits below ~6 kHz (measured: below-6kHz RMS == full-band RMS on
		// the dry capture); everything above is interface hiss plus discrete ~19-22 kHz whine that a flat
		// make-up gain would otherwise lift into the game's amp as audible static. A real Tone Cable is
		// band-limited the same way. Tweak this one value to trade brightness against hiss.
		constexpr float INPUT_LOWPASS_HZ = 6000.0f;
		constexpr size_t MAX_FILTER_CHANNELS = 8;
		constexpr uint64_t UNMARSHAL_HOOK_RETRY_INTERVAL_MILLISECONDS = 250;
		constexpr uint64_t INPUT_STALL_MILLISECONDS = 3000;

		constexpr std::array<uint8_t, 11> UNMARSHAL_CALL_PRE_PATCH =
		{
			0xe8, 0x59, 0xdf, 0xff, 0xff, 0x57, 0xe8, 0x33, 0xe0, 0xff, 0xff
		};

		constexpr std::array<uint8_t, 11> UNMARSHAL_CALL_POST_PATCH =
		{
			0xe8, 0x97, 0xe2, 0xff, 0xff, 0x57, 0xe8, 0x71, 0xe3, 0xff, 0xff
		};

		constexpr std::array<uint8_t, 11> UNMARSHAL_CALL_LEARN_AND_PLAY =
		{
			0xe8, 0xd6, 0xfd, 0xff, 0xff, 0x56, 0xe8, 0xb0, 0xfe, 0xff, 0xff
		};

		const GUID PCM_SUBFORMAT =
		{
			0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
		};

		const GUID FLOAT_SUBFORMAT =
		{
			0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
		};

		struct RsAsioConfiguration
		{
			std::array<int, INPUT_ROUTE_COUNT> inputChannels{ 0, -1 };
			std::array<bool, INPUT_ROUTE_COUNT> inputConfigured{ false, false };
			std::array<std::string, INPUT_ROUTE_COUNT> inputSources;
			std::array<bool, INPUT_ROUTE_COUNT> inputInferred{ false, false };
		};

		struct PaWasapiSubStreamPrefix
		{
			IUnknown* clientParent;
			IUnknown* clientStream;
			IUnknown* clientProc;
			WAVEFORMATEXTENSIBLE waveFormat;
			uint8_t remaining[0xdc - sizeof(WAVEFORMATEXTENSIBLE)];
		};

		struct PaWasapiStreamPrefix
		{
			uint8_t prefix[0x108];
			PaWasapiSubStreamPrefix input;
			IUnknown* captureClientParent;
			IUnknown* captureClientStream;
			IAudioCaptureClient* captureClient;
		};

		enum class UnmarshalHookInstallResult
		{
			Installed,
			Retry,
			Failed
		};

		static_assert(sizeof(void*) == 4, "Rocksmith Audio Bridge and Rocksmith require a 32-bit build");
		static_assert(offsetof(PaWasapiStreamPrefix, input.waveFormat) == 0x114, "Unexpected PortAudio input format offset");
		static_assert(offsetof(PaWasapiStreamPrefix, captureClientParent) == 0x1f0, "Unexpected PortAudio capture parent offset");
		static_assert(offsetof(PaWasapiStreamPrefix, captureClient) == 0x1f8, "Unexpected PortAudio capture client offset");

		using UnmarshalStreamComPointers_t = HRESULT(__cdecl*)(void*);
		using CaptureGetBuffer_t = HRESULT(STDMETHODCALLTYPE*)(
			IAudioCaptureClient*, BYTE**, UINT32*, DWORD*, UINT64*, UINT64*);

		std::array<CaptureFormat, INPUT_ROUTE_COUNT> routeFormats;
		std::array<std::vector<float>, INPUT_ROUTE_COUNT> conversionBuffers;
		std::array<std::atomic<uint64_t>, INPUT_ROUTE_COUNT> routeLastBufferTick;
		std::array<std::atomic<IInputProcessor*>, INPUT_ROUTE_COUNT> activeProcessors;
		std::array<std::atomic<IAudioCaptureClient*>, INPUT_ROUTE_COUNT> routeCaptureClients;
		std::array<Microsoft::WRL::ComPtr<PersistentInput::ICaptureState>, INPUT_ROUTE_COUNT> bridgeStates;
		std::array<std::atomic<UINT64>, INPUT_ROUTE_COUNT> bridgeGenerations;
		std::array<int, INPUT_ROUTE_COUNT> selectedInputChannels{ -1, -1 };
		std::array<bool, INPUT_ROUTE_COUNT> configuredInputs{ false, false };
		std::array<std::atomic<bool>, INPUT_ROUTE_COUNT> inputReady;
		std::atomic<bool> proxyInputSeen{ false };
		// Player 1 input stage meters: the (INPUT) level is measured AFTER the conditioner, so on its own
		// it cannot say whether audio never arrived or the conditioner removed it. These peaks bracket
		// each stage; PollInputStageMeter logs them.
		struct StagePeak
		{
			std::atomic<uint32_t> bits{ 0 };
			std::atomic<uint32_t> buffers{ 0 };
			void Note(float peak)
			{
				buffers.fetch_add(1, std::memory_order_relaxed);
				if (!(peak > 0.0f)) return;
				uint32_t current = bits.load(std::memory_order_relaxed);
				float currentPeak;
				std::memcpy(&currentPeak, &current, sizeof(currentPeak));
				if (peak <= currentPeak) return;
				uint32_t wanted;
				std::memcpy(&wanted, &peak, sizeof(wanted));
				bits.store(wanted, std::memory_order_relaxed);
			}
			float TakePeak()
			{
				const uint32_t taken = bits.exchange(0, std::memory_order_relaxed);
				float peak;
				std::memcpy(&peak, &taken, sizeof(peak));
				return peak;
			}
		};
		StagePeak stageProxyRaw;          // the real device's buffers inside the proxy, before RS_ASIO
		StagePeak stageCaptureRaw;        // what RS_ASIO handed the game, before the conditioner
		StagePeak stageAfterConditioner;  // what the game and detection actually hear
		std::atomic<uint32_t> stageSilentFlagPackets{ 0 };      // game packets flagged silent by the capture
		std::atomic<uint32_t> stageReplacedWithSilence{ 0 };    // packets this hook swapped for zeros
		std::atomic<uint32_t> proxyInputRate{ 0 };
		std::atomic<int> proxyInputSampleFormat{ static_cast<int>(SampleFormat::Unsupported) };
		std::atomic<bool> bufferLayoutReady{ false };
		CaptureProcessingGate processingGate;
		std::atomic<bool> autoEnabledOnce{ false };
		std::mutex registrationMutex;
		void UpdateProcessingEnabled(bool enabled);

		UnmarshalStreamComPointers_t originalUnmarshalStreamComPointers = nullptr;
		CaptureGetBuffer_t originalCaptureGetBuffer = nullptr;
		void** captureClientVTable = nullptr;
		uint8_t* unmarshalPatchedTarget = nullptr;
		bool isUnmarshalHookInstallPending = false;
		bool isUnmarshalHookInstalled = false;
		bool hasLoggedWaitingForRsAsio = false;
		bool hasLoggedWaitingForRsAsioPatch = false;
		uint64_t nextUnmarshalHookAttemptTick = 0;
		bool proxyInputObserverInstalled = false;
		uint64_t nextProxyInputObserverAttemptTick = 0;

		struct ProxyInputChannel
		{
			void* buffer;
			long channelNum;
			long type;
		};
		using SetProxyInputObserver_t = void(__cdecl*)(void(__cdecl*)(const ProxyInputChannel*, long, long, double));

		float ClampSample(float value)
		{
			if (!std::isfinite(value)) return 0.0f;
			return std::clamp(value, -1.0f, 1.0f);
		}

		template<typename SampleType, int BIT_DEPTH>
		SampleType FloatToSignedInteger(float value)
		{
			static_assert(std::is_signed<SampleType>::value, "SampleType must be signed");
			constexpr int64_t magnitude = int64_t{ 1 } << (BIT_DEPTH - 1);
			const float clamped = ClampSample(value);
			if (clamped <= -1.0f) return static_cast<SampleType>(-magnitude);
			if (clamped >= 1.0f) return static_cast<SampleType>(magnitude - 1);
			return static_cast<SampleType>(clamped * static_cast<float>(magnitude));
		}

		int32_t ReadInt24(const uint8_t* sample)
		{
			const uint32_t packed = static_cast<uint32_t>(sample[0])
				| (static_cast<uint32_t>(sample[1]) << 8)
				| (static_cast<uint32_t>(sample[2]) << 16);

			return (packed & 0x00800000u) != 0
				? static_cast<int32_t>(packed) - 0x01000000
				: static_cast<int32_t>(packed);
		}

		void WriteInt24(int32_t value, uint8_t* sample)
		{
			const uint32_t packed = static_cast<uint32_t>(value);
			sample[0] = static_cast<uint8_t>(packed);
			sample[1] = static_cast<uint8_t>(packed >> 8);
			sample[2] = static_cast<uint8_t>(packed >> 16);
		}

		RsAsioConfiguration ReadRsAsioConfiguration()
		{
			CSimpleIniA reader;
			if (reader.LoadFile("RS_ASIO.ini") < 0) return {};

			RsAsioConfiguration configuration;
			const char* inputZeroDriver = reader.GetValue("Asio.Input.0", "Driver", "");
			const char* inputOneDriver = reader.GetValue("Asio.Input.1", "Driver", "");
			const bool hasInputZero = inputZeroDriver && *inputZeroDriver;
			const bool hasInputOne = inputOneDriver && *inputOneDriver;

			if (hasInputZero)
			{
				configuration.inputConfigured[0] = true;
				configuration.inputChannels[0] = static_cast<int>(reader.GetLongValue("Asio.Input.0", "Channel", 0));
				configuration.inputSources[0] = "[Asio.Input.0]";

				if (hasInputOne)
				{
					configuration.inputConfigured[1] = true;
					configuration.inputChannels[1] = static_cast<int>(reader.GetLongValue("Asio.Input.1", "Channel", -1));
					configuration.inputSources[1] = "[Asio.Input.1]";
				}
			}
			else if (hasInputOne)
			{
				configuration.inputConfigured[0] = true;
				configuration.inputChannels[0] = static_cast<int>(reader.GetLongValue("Asio.Input.1", "Channel", 0));
				configuration.inputSources[0] = "[Asio.Input.1], the only configured input section";
				configuration.inputInferred[0] = true;
			}
			else
			{
				const bool usesWindowsInput = reader.GetBoolValue("Config", "EnableWasapiInputs", false);
				if (usesWindowsInput)
				{
					configuration.inputConfigured[0] = true;
					configuration.inputChannels[0] = 0;
					configuration.inputSources[0] = "the Windows Cable input";
				}
			}

			return configuration;
		}

		// Cable mode deliberately leaves RS_ASIO installed but disables its host. Only an
		// explicitly enabled configuration may own the unmarshal path; otherwise install
		// the native Cable hook before the game's one boot-time stream unmarshal.
		std::string GameDirectory()
		{
			char path[MAX_PATH];
			const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
			if (length == 0 || length >= MAX_PATH) return "";
			const std::string modulePath(path, length);
			const size_t slash = modulePath.find_last_of("\\/");
			return slash == std::string::npos ? "" : modulePath.substr(0, slash + 1);
		}

		bool IsRsAsioEnabled()
		{
			const std::string directory = GameDirectory();
			if (GetFileAttributesA((directory + "RS_ASIO.dll").c_str()) == INVALID_FILE_ATTRIBUTES) return false;

			CSimpleIniA reader;
			if (reader.LoadFile((directory + "RS_ASIO.ini").c_str()) < 0) return false;
			return reader.GetBoolValue("Config", "EnableAsio", false);
		}

		// RS_ASIO loaded while its ini does not enable ASIO means RS_ASIO is running with nothing switched on: the
		// game sees no audio devices at all, and this mod falls back to Cable mode, which cannot install over
		// RS_ASIO's patches either. Seen in the field as a misnamed RS_ASIO.ini.txt and as a file with no [Config]
		// header. Say exactly which, since nothing else in either log points at the ini.
		void ReportUnreadableRsAsioConfiguration()
		{
			if (GetModuleHandleA("RS_ASIO.dll") == nullptr) return;   // Cable mode renames RS_ASIO away: expected

			const std::string directory = GameDirectory();
			const std::string ini = directory + "RS_ASIO.ini";
			std::string reason;
			if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				reason = GetFileAttributesA((ini + ".txt").c_str()) != INVALID_FILE_ATTRIBUTES
					? "the file is named RS_ASIO.ini.txt; rename it to RS_ASIO.ini (turn on file name extensions in Explorer to see this)"
					: "RS_ASIO.ini is missing from " + directory;
			}
			else
			{
				unsigned char bom[2]{};
				if (FILE* file = std::fopen(ini.c_str(), "rb"))
				{
					const size_t read = std::fread(bom, 1, sizeof(bom), file);
					std::fclose(file);
					if (read == 2 && ((bom[0] == 0xFF && bom[1] == 0xFE) || (bom[0] == 0xFE && bom[1] == 0xFF)))
						reason = "RS_ASIO.ini is saved as UTF-16 (\"Unicode\"); save it as UTF-8 or ANSI";
				}
				CSimpleIniA reader;
				if (reason.empty() && reader.LoadFile(ini.c_str()) < 0)
					reason = "RS_ASIO.ini could not be read";
				else if (reason.empty() && reader.GetSection("Config") == nullptr)
					reason = "RS_ASIO.ini has no [Config] section; its first line must be [Config], above EnableWasapiOutputs, EnableWasapiInputs and EnableAsio";
				else if (reason.empty())
					reason = "RS_ASIO.ini [Config] does not set EnableAsio=1 (Cable mode is chosen in the Audio Bridge tab, which turns RS_ASIO off)";
			}
			LOG_ERROR("[AsioHook] RS_ASIO.dll is loaded but its settings do not enable ASIO: " << reason
				<< ". The game will see no ASIO devices and Cable input cannot install over RS_ASIO." << std::endl);
		}

		bool IsRangeInsideModule(const void* address, size_t size, const MODULEINFO& moduleInfo)
		{
			const uintptr_t start = reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll);
			const uintptr_t end = start + moduleInfo.SizeOfImage;
			const uintptr_t rangeStart = reinterpret_cast<uintptr_t>(address);
			return rangeStart >= start && rangeStart <= end && size <= end - rangeStart;
		}

		template<size_t SIZE>
		void FindPattern(const MODULEINFO& moduleInfo, const std::array<uint8_t, SIZE>& pattern, std::vector<uint8_t*>& matches)
		{
			auto* start = static_cast<uint8_t*>(moduleInfo.lpBaseOfDll);
			auto* end = start + moduleInfo.SizeOfImage;
			auto* cursor = start;

			while (cursor < end)
			{
				cursor = std::search(cursor, end, pattern.begin(), pattern.end());
				if (cursor == end) return;
				matches.push_back(cursor);
				++cursor;
			}
		}

		bool ReplacePatchedTarget(uint8_t* target, void* replacement)
		{
			return MemUtil::PatchAdr(target + 1, &replacement, sizeof(replacement));
		}

		CaptureFormat ReadCaptureFormat(const WAVEFORMATEXTENSIBLE& waveFormat)
		{
			const WAVEFORMATEX& baseFormat = waveFormat.Format;
			CaptureFormat format;
			format.sampleRate = baseFormat.nSamplesPerSec;
			format.channelCount = baseFormat.nChannels;

			if (baseFormat.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
			{
				if (baseFormat.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return {};

				if (IsEqualGUID(waveFormat.SubFormat, FLOAT_SUBFORMAT) && baseFormat.wBitsPerSample == 32)
					format.sampleFormat = SampleFormat::Float32;
				else if (IsEqualGUID(waveFormat.SubFormat, PCM_SUBFORMAT))
				{
					switch (baseFormat.wBitsPerSample)
					{
					case 32: format.sampleFormat = SampleFormat::Int32; break;
					case 24: format.sampleFormat = SampleFormat::Int24; break;
					case 16: format.sampleFormat = SampleFormat::Int16; break;
					}
				}
			}
			else if (baseFormat.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && baseFormat.wBitsPerSample == 32)
			{
				format.sampleFormat = SampleFormat::Float32;
			}
			else if (baseFormat.wFormatTag == WAVE_FORMAT_PCM)
			{
				switch (baseFormat.wBitsPerSample)
				{
				case 32: format.sampleFormat = SampleFormat::Int32; break;
				case 24: format.sampleFormat = SampleFormat::Int24; break;
				case 16: format.sampleFormat = SampleFormat::Int16; break;
				}
			}

			const uint32_t bytesPerSample = baseFormat.wBitsPerSample / 8;
			if (!format.IsUsable() || bytesPerSample == 0
				|| baseFormat.nBlockAlign != format.channelCount * bytesPerSample)
				return {};

			return format;
		}

		bool CopyFirstChannelToFloat(const BYTE* packet, const CaptureFormat& format, uint32_t frameCount, float* output)
		{
			const size_t channelCount = format.channelCount;
			switch (format.sampleFormat)
			{
			case SampleFormat::Float32:
			{
				const float* samples = reinterpret_cast<const float*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					output[frame] = samples[static_cast<size_t>(frame) * channelCount];
				return true;
			}
			case SampleFormat::Int32:
			{
				const int32_t* samples = reinterpret_cast<const int32_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					output[frame] = static_cast<float>(samples[static_cast<size_t>(frame) * channelCount]) * INT32_TO_FLOAT;
				return true;
			}
			case SampleFormat::Int24:
			{
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const size_t sampleIndex = static_cast<size_t>(frame) * channelCount;
					output[frame] = static_cast<float>(ReadInt24(packet + sampleIndex * 3)) * INT24_TO_FLOAT;
				}
				return true;
			}
			case SampleFormat::Int16:
			{
				const int16_t* samples = reinterpret_cast<const int16_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					output[frame] = static_cast<float>(samples[static_cast<size_t>(frame) * channelCount]) * INT16_TO_FLOAT;
				return true;
			}
			default:
				return false;
			}
		}

		// Peak of the first channel, read straight from the packet. Allocation-free and buffer-free on
		// purpose: the input stage meter runs before the route's conversion buffer exists (it is only
		// allocated once input processing is prepared), and writing into it there crashes the game at
		// startup.
		bool MeasureFirstChannelPeak(const BYTE* packet, const CaptureFormat& format, uint32_t frameCount, float& peak)
		{
			peak = 0.0f;
			const size_t channelCount = format.channelCount;
			if (packet == nullptr || channelCount == 0) return false;
			for (uint32_t frame = 0; frame < frameCount; ++frame)
			{
				const size_t sampleIndex = static_cast<size_t>(frame) * channelCount;
				float value = 0.0f;
				switch (format.sampleFormat)
				{
				case SampleFormat::Float32: value = reinterpret_cast<const float*>(packet)[sampleIndex]; break;
				case SampleFormat::Int32: value = static_cast<float>(reinterpret_cast<const int32_t*>(packet)[sampleIndex]) * INT32_TO_FLOAT; break;
				case SampleFormat::Int24: value = static_cast<float>(ReadInt24(packet + sampleIndex * 3)) * INT24_TO_FLOAT; break;
				case SampleFormat::Int16: value = static_cast<float>(reinterpret_cast<const int16_t*>(packet)[sampleIndex]) * INT16_TO_FLOAT; break;
				default: return false;
				}
				if (std::isfinite(value)) peak = std::max(peak, std::fabs(value));
			}
			return true;
		}

		void CopyFloatToAllChannels(BYTE* packet, const CaptureFormat& format, uint32_t frameCount, const float* converted)
		{
			const size_t channelCount = format.channelCount;
			switch (format.sampleFormat)
			{
			case SampleFormat::Float32:
			{
				float* samples = reinterpret_cast<float*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const float value = ClampSample(converted[frame]);
					for (size_t channel = 0; channel < channelCount; ++channel)
						samples[static_cast<size_t>(frame) * channelCount + channel] = value;
				}
				break;
			}
			case SampleFormat::Int32:
			{
				int32_t* samples = reinterpret_cast<int32_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const int32_t value = FloatToSignedInteger<int32_t, 32>(converted[frame]);
					for (size_t channel = 0; channel < channelCount; ++channel)
						samples[static_cast<size_t>(frame) * channelCount + channel] = value;
				}
				break;
			}
			case SampleFormat::Int24:
			{
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const int32_t value = FloatToSignedInteger<int32_t, 24>(converted[frame]);
					for (size_t channel = 0; channel < channelCount; ++channel)
					{
						const size_t sampleIndex = static_cast<size_t>(frame) * channelCount + channel;
						WriteInt24(value, packet + sampleIndex * 3);
					}
				}
				break;
			}
			case SampleFormat::Int16:
			{
				int16_t* samples = reinterpret_cast<int16_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const int16_t value = FloatToSignedInteger<int16_t, 16>(converted[frame]);
					for (size_t channel = 0; channel < channelCount; ++channel)
						samples[static_cast<size_t>(frame) * channelCount + channel] = value;
				}
				break;
			}
			default:
				break;
			}
		}

	}

	namespace Detail
	{
		void NoiseSuppressor::Reset()
		{
			configuredThreshold = -1.0f;
			energy = 0.0f;
			appliedGain = rangeGain;
			openConfirmCount = 0;
			isOpen = false;
		}

		void NoiseSuppressor::Configure(uint32_t sampleRate, float threshold)
		{
			if (sampleRate == 0) return;
			if (sampleRate == configuredRate && threshold == configuredThreshold) return;

			configuredRate = sampleRate;
			configuredThreshold = threshold;
			const double fs = static_cast<double>(sampleRate);
			detectorAttackCoef = static_cast<float>(1.0 - std::exp(-1.0 / (DETECTOR_ATTACK_SECONDS * fs)));
			detectorReleaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / (DETECTOR_RELEASE_SECONDS * fs)));
			gainAttackCoef = static_cast<float>(1.0 - std::exp(-1.0 / (GAIN_ATTACK_SECONDS * fs)));
			gainReleaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / (GAIN_RELEASE_SECONDS * fs)));
			rangeGain = std::pow(10.0f, EXPANDER_RANGE_DB / 20.0f);
			attackThreshold = threshold * std::pow(10.0f, ATTACK_ABOVE_THRESHOLD_DB / 20.0f);
			openConfirmSamples = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(OPEN_CONFIRM_SECONDS * sampleRate)));
			energy = 0.0f;
			appliedGain = rangeGain;
			openConfirmCount = 0;
			isOpen = false;
		}

		float NoiseSuppressor::NextGain(float sidechain, float threshold)
		{
			if (!std::isfinite(sidechain) || threshold <= 0.0f) return 1.0f;
			const float magnitude = std::fabs(sidechain);
			const float sampleEnergy = magnitude * magnitude;
			const float detectorCoef = sampleEnergy > energy ? detectorAttackCoef : detectorReleaseCoef;
			energy += (sampleEnergy - energy) * detectorCoef;
			const float envelope = std::sqrt(std::max(energy, 0.0f));

			if (!isOpen)
			{
				if (envelope >= attackThreshold)
					openConfirmCount = std::min(openConfirmCount + 1, openConfirmSamples);
				else
					openConfirmCount = 0;
				if (openConfirmCount >= openConfirmSamples)
					isOpen = true;
			}
			else if (envelope < threshold * CLOSE_HYSTERESIS)
			{
				isOpen = false;
				openConfirmCount = 0;
			}

			float target = 1.0f;
			if (!isOpen)
			{
				if (envelope < threshold)
				{
					const float belowDb = 20.0f * std::log10(std::max(envelope, 1e-9f) / threshold);
					const float reductionDb = belowDb * (EXPANDER_RATIO - 1.0f);
					target = reductionDb <= EXPANDER_RANGE_DB
						? rangeGain
						: std::pow(10.0f, reductionDb / 20.0f);
				}
				else
					target = rangeGain;
			}

			const float coefficient = target > appliedGain ? gainAttackCoef : gainReleaseCoef;
			appliedGain += (target - appliedGain) * coefficient;
			return appliedGain;
		}

		void NoiseSuppressor::Process(float* samples, size_t sampleCount, uint32_t sampleRate, float threshold)
		{
			if (samples == nullptr || sampleCount == 0 || threshold <= 0.0f) return;
			Configure(sampleRate, threshold);
			for (size_t index = 0; index < sampleCount; ++index)
				samples[index] *= NextGain(samples[index], threshold);
		}
	}

	namespace
	{
		Detail::NoiseSuppressor g_noiseSuppressor;
		uint32_t g_processedGateThresholdRevision = 0;

		// Applies adaptive suppression to the raw capture buffer in place, ahead of the make-up gain, so
		// its detector sees the true input level (independent of the gain slider). Mirrors
		// ScaleBufferInPlace's format handling. Runs only while a threshold is set, so the feature-off
		// path stays bit-exact.
		void ProcessNoiseSuppressorInPlace(BYTE* packet, const CaptureFormat& format, uint32_t frameCount, float threshold)
		{
			const size_t channelCount = format.channelCount;
			if (channelCount == 0) return;
			g_noiseSuppressor.Configure(format.sampleRate, threshold);

			switch (format.sampleFormat)
			{
			case SampleFormat::Float32:
			{
				float* samples = reinterpret_cast<float*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const size_t base = static_cast<size_t>(frame) * channelCount;
					const float gain = g_noiseSuppressor.NextGain(samples[base], threshold);
					for (size_t channel = 0; channel < channelCount; ++channel)
						samples[base + channel] = ClampSample(samples[base + channel] * gain);
				}
				break;
			}
			case SampleFormat::Int32:
			{
				int32_t* samples = reinterpret_cast<int32_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const size_t base = static_cast<size_t>(frame) * channelCount;
					const float gain = g_noiseSuppressor.NextGain(static_cast<float>(samples[base]) * INT32_TO_FLOAT, threshold);
					for (size_t channel = 0; channel < channelCount; ++channel)
						samples[base + channel] = FloatToSignedInteger<int32_t, 32>(static_cast<float>(samples[base + channel]) * INT32_TO_FLOAT * gain);
				}
				break;
			}
			case SampleFormat::Int24:
			{
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const size_t base = static_cast<size_t>(frame) * channelCount;
					const float gain = g_noiseSuppressor.NextGain(static_cast<float>(ReadInt24(packet + base * 3)) * INT24_TO_FLOAT, threshold);
					for (size_t channel = 0; channel < channelCount; ++channel)
					{
						const size_t index = base + channel;
						const float scaled = static_cast<float>(ReadInt24(packet + index * 3)) * INT24_TO_FLOAT * gain;
						WriteInt24(FloatToSignedInteger<int32_t, 24>(scaled), packet + index * 3);
					}
				}
				break;
			}
			case SampleFormat::Int16:
			{
				int16_t* samples = reinterpret_cast<int16_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const size_t base = static_cast<size_t>(frame) * channelCount;
					const float gain = g_noiseSuppressor.NextGain(static_cast<float>(samples[base]) * INT16_TO_FLOAT, threshold);
					for (size_t channel = 0; channel < channelCount; ++channel)
						samples[base + channel] = FloatToSignedInteger<int16_t, 16>(static_cast<float>(samples[base + channel]) * INT16_TO_FLOAT * gain);
				}
				break;
			}
			default:
				break;
			}
		}

		// Feed-forward compressor run per frame over the raw route-0 input (mono channel-0 sidechain),
		// applied to every channel. A single "strength" macro (0..1) sweeps threshold, ratio and make-up
		// together: at 0 it is a bypass, toward 1 it clamps down harder. Fast attack / medium release
		// track and flatten the slow (1-6 Hz) string-beat wobble so the game amp receives a steadier
		// envelope and cannot magnify it into a warble. Audio-thread only; state carries across packets.
		struct Compressor
		{
			static constexpr float ATTACK_SECONDS = 0.008f;
			static constexpr float RELEASE_SECONDS = 0.090f;

			float ratio = 1.0f, thresholdDb = 0.0f, makeupLinear = 1.0f;
			float attackCoef = 0.0f, releaseCoef = 0.0f;
			float gainReductionDb = 0.0f;   // smoothed, <= 0
			uint32_t coeffRate = 0;
			float configuredStrength = -1.0f;

			void Configure(uint32_t sampleRate, float strength)
			{
				if (sampleRate == 0) return;
				if (sampleRate != coeffRate)
				{
					coeffRate = sampleRate;
					const double fs = static_cast<double>(sampleRate);
					attackCoef = static_cast<float>(1.0 - std::exp(-1.0 / (ATTACK_SECONDS * fs)));
					releaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / (RELEASE_SECONDS * fs)));
				}
				if (strength != configuredStrength)
				{
					configuredStrength = strength;
					const float s = std::clamp(strength, 0.0f, 1.0f);
					ratio = 1.0f + 5.0f * s;                 // 1:1 .. 6:1
					thresholdDb = -6.0f - 24.0f * s;         // -6 .. -30 dBFS
					// Modest auto make-up: restore roughly what a -6 dBFS peak loses, so smoothing the
					// wobble does not just quieten the note. Kept gentle to limit noise lift on the decay.
					const float lossAtRef = (thresholdDb - (-6.0f)) * (1.0f - 1.0f / ratio); // <= 0
					makeupLinear = std::pow(10.0f, (-lossAtRef * 0.6f) / 20.0f);
				}
			}

			inline float NextGain(float sidechain)
			{
				const float levelDb = 20.0f * std::log10(std::fabs(sidechain) + 1e-9f);
				const float target = levelDb > thresholdDb
					? (thresholdDb - levelDb) * (1.0f - 1.0f / ratio) : 0.0f; // <= 0
				const float coef = target < gainReductionDb ? attackCoef : releaseCoef;
				gainReductionDb += (target - gainReductionDb) * coef;
				return std::pow(10.0f, gainReductionDb / 20.0f) * makeupLinear;
			}
		};

		Compressor g_compressor;

		// Applies the input compressor to the raw capture buffer in place (after the gate, before the
		// make-up gain), mirroring ScaleBufferInPlace's format handling. Runs only when strength > 0, so
		// the feature-off path stays bit-exact.
		void ProcessCompressorInPlace(BYTE* packet, const CaptureFormat& format, uint32_t frameCount, float strength)
		{
			const size_t channelCount = format.channelCount;
			if (channelCount == 0) return;
			g_compressor.Configure(format.sampleRate, strength);

			switch (format.sampleFormat)
			{
			case SampleFormat::Float32:
			{
				float* samples = reinterpret_cast<float*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const size_t base = static_cast<size_t>(frame) * channelCount;
					const float gain = g_compressor.NextGain(samples[base]);
					for (size_t channel = 0; channel < channelCount; ++channel)
						samples[base + channel] = ClampSample(samples[base + channel] * gain);
				}
				break;
			}
			case SampleFormat::Int32:
			{
				int32_t* samples = reinterpret_cast<int32_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const size_t base = static_cast<size_t>(frame) * channelCount;
					const float gain = g_compressor.NextGain(static_cast<float>(samples[base]) * INT32_TO_FLOAT);
					for (size_t channel = 0; channel < channelCount; ++channel)
						samples[base + channel] = FloatToSignedInteger<int32_t, 32>(static_cast<float>(samples[base + channel]) * INT32_TO_FLOAT * gain);
				}
				break;
			}
			case SampleFormat::Int24:
			{
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const size_t base = static_cast<size_t>(frame) * channelCount;
					const float gain = g_compressor.NextGain(static_cast<float>(ReadInt24(packet + base * 3)) * INT24_TO_FLOAT);
					for (size_t channel = 0; channel < channelCount; ++channel)
					{
						const size_t index = base + channel;
						const float scaled = static_cast<float>(ReadInt24(packet + index * 3)) * INT24_TO_FLOAT * gain;
						WriteInt24(FloatToSignedInteger<int32_t, 24>(scaled), packet + index * 3);
					}
				}
				break;
			}
			case SampleFormat::Int16:
			{
				int16_t* samples = reinterpret_cast<int16_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const size_t base = static_cast<size_t>(frame) * channelCount;
					const float gain = g_compressor.NextGain(static_cast<float>(samples[base]) * INT16_TO_FLOAT);
					for (size_t channel = 0; channel < channelCount; ++channel)
						samples[base + channel] = FloatToSignedInteger<int16_t, 16>(static_cast<float>(samples[base + channel]) * INT16_TO_FLOAT * gain);
				}
				break;
			}
			default:
				break;
			}
		}

		// Soft knee above 0.7 so a hot make-up gain asymptotes toward full scale instead of hard
		// clipping or wrapping on transients.
		inline float SoftClipSample(float value)
		{
			if (value > 0.7f) return 0.7f + 0.3f * std::tanh((value - 0.7f) / 0.3f);
			if (value < -0.7f) return -0.7f + 0.3f * std::tanh((value + 0.7f) / 0.3f);
			return value;
		}

		// Front-of-chain input make-up gain: scales the raw capture buffer in place across every
		// channel, preserving layout. Runs before any pitch shift and independently of the processor
		// enable path, so it applies in normal play (cable or RS_ASIO) with no Drop Pedal needed.
		void ScaleBufferInPlace(BYTE* packet, const CaptureFormat& format, uint32_t frameCount, float gain)
		{
			const size_t total = static_cast<size_t>(frameCount) * format.channelCount;
			switch (format.sampleFormat)
			{
			case SampleFormat::Float32:
			{
				float* samples = reinterpret_cast<float*>(packet);
				for (size_t index = 0; index < total; ++index)
					samples[index] = ClampSample(SoftClipSample(samples[index] * gain));
				break;
			}
			case SampleFormat::Int32:
			{
				int32_t* samples = reinterpret_cast<int32_t*>(packet);
				for (size_t index = 0; index < total; ++index)
					samples[index] = FloatToSignedInteger<int32_t, 32>(SoftClipSample(static_cast<float>(samples[index]) * INT32_TO_FLOAT * gain));
				break;
			}
			case SampleFormat::Int24:
			{
				for (size_t index = 0; index < total; ++index)
				{
					const float scaled = SoftClipSample(static_cast<float>(ReadInt24(packet + index * 3)) * INT24_TO_FLOAT * gain);
					WriteInt24(FloatToSignedInteger<int32_t, 24>(scaled), packet + index * 3);
				}
				break;
			}
			case SampleFormat::Int16:
			{
				int16_t* samples = reinterpret_cast<int16_t*>(packet);
				for (size_t index = 0; index < total; ++index)
					samples[index] = FloatToSignedInteger<int16_t, 16>(SoftClipSample(static_cast<float>(samples[index]) * INT16_TO_FLOAT * gain));
				break;
			}
			default:
				break;
			}
		}

		// Second-order Butterworth low-pass (RBJ cookbook, Q = 1/sqrt(2)) run per channel over the
		// post-gain input. It is linear and cheap: it strips the broadband hiss and >19 kHz interface
		// whine that the flat make-up gain amplifies, and also smooths the soft-clip's transient
		// harmonics. Only touched on the capture (audio) thread, so no synchronisation is needed; state
		// carries across packets and resets when the sample rate changes.
		struct InputLowpass
		{
			float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
			uint32_t coeffRate = 0;
			struct Channel { float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f; };
			std::array<Channel, MAX_FILTER_CHANNELS> channels{};

			void Configure(uint32_t sampleRate)
			{
				if (sampleRate == 0 || sampleRate == coeffRate) return;
				coeffRate = sampleRate;
				const double w0 = 2.0 * 3.14159265358979323846 * INPUT_LOWPASS_HZ / static_cast<double>(sampleRate);
				const double cosw0 = std::cos(w0);
				const double sinw0 = std::sin(w0);
				const double alpha = sinw0 / 1.4142135623730951; // sinw0 / (2Q) with Q = 1/sqrt(2)
				const double a0 = 1.0 + alpha;
				b0 = static_cast<float>(((1.0 - cosw0) * 0.5) / a0);
				b1 = static_cast<float>((1.0 - cosw0) / a0);
				b2 = b0;
				a1 = static_cast<float>((-2.0 * cosw0) / a0);
				a2 = static_cast<float>((1.0 - alpha) / a0);
				channels = {};
			}

			inline float Process(size_t channel, float input)
			{
				Channel& c = channels[channel];
				const float output = b0 * input + b1 * c.x1 + b2 * c.x2 - a1 * c.y1 - a2 * c.y2;
				c.x2 = c.x1; c.x1 = input;
				c.y2 = c.y1; c.y1 = output;
				return output;
			}
		};

		InputLowpass g_inputLowpass;

		// Band-limits the raw capture buffer in place, mirroring ScaleBufferInPlace's format handling.
		// Called immediately after the make-up gain, on the same route-0/gain-engaged path, so the
		// unity/feature-off path is never touched and stays bit-exact.
		void FilterBufferInPlace(BYTE* packet, const CaptureFormat& format, uint32_t frameCount)
		{
			const size_t channelCount = format.channelCount;
			if (channelCount == 0 || channelCount > MAX_FILTER_CHANNELS) return;
			g_inputLowpass.Configure(format.sampleRate);

			switch (format.sampleFormat)
			{
			case SampleFormat::Float32:
			{
				float* samples = reinterpret_cast<float*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					for (size_t channel = 0; channel < channelCount; ++channel)
					{
						const size_t index = static_cast<size_t>(frame) * channelCount + channel;
						samples[index] = ClampSample(g_inputLowpass.Process(channel, samples[index]));
					}
				break;
			}
			case SampleFormat::Int32:
			{
				int32_t* samples = reinterpret_cast<int32_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					for (size_t channel = 0; channel < channelCount; ++channel)
					{
						const size_t index = static_cast<size_t>(frame) * channelCount + channel;
						const float filtered = g_inputLowpass.Process(channel, static_cast<float>(samples[index]) * INT32_TO_FLOAT);
						samples[index] = FloatToSignedInteger<int32_t, 32>(filtered);
					}
				break;
			}
			case SampleFormat::Int24:
			{
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					for (size_t channel = 0; channel < channelCount; ++channel)
					{
						const size_t index = static_cast<size_t>(frame) * channelCount + channel;
						const float filtered = g_inputLowpass.Process(channel, static_cast<float>(ReadInt24(packet + index * 3)) * INT24_TO_FLOAT);
						WriteInt24(FloatToSignedInteger<int32_t, 24>(filtered), packet + index * 3);
					}
				break;
			}
			case SampleFormat::Int16:
			{
				int16_t* samples = reinterpret_cast<int16_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					for (size_t channel = 0; channel < channelCount; ++channel)
					{
						const size_t index = static_cast<size_t>(frame) * channelCount + channel;
						const float filtered = g_inputLowpass.Process(channel, static_cast<float>(samples[index]) * INT16_TO_FLOAT);
						samples[index] = FloatToSignedInteger<int16_t, 16>(filtered);
					}
				break;
			}
			default:
				break;
			}
		}

		// Adaptive mains hum / buzz remover (HumRemover.hpp): measures the hum between notes on a background
		// thread and notches only the harmonics that are actually present, each ~1.5-5 Hz wide, up to 8 kHz.
		// (A fixed constant-Q comb cuts the upper mids audibly and misses buzz above its top notch.) The
		// analysis thread starts the first time the filter is switched on.
		Audio::HumRemover g_humRemover;
		std::atomic<bool> g_humWorkerStarted{ false };

		void HumAnalysisWorker()
		{
			Audio::HumRemover::Status logged{};
			for (;;)
			{
				Sleep(50);
				if (!g_humRemover.Analyze()) continue;
				const auto status = g_humRemover.GetStatus();
				const bool firstLock = logged.lines == 0 && status.lines > 0;
				if (firstLock || std::abs(status.lines - logged.lines) >= 5 || std::fabs(status.fundamentalHz - logged.fundamentalHz) >= 0.02)
				{
					LOG_INFO("(HUM FILTER) " << (firstLock ? "locked" : "updated") << ": mains " << std::fixed << std::setprecision(3)
						<< status.fundamentalHz << " Hz, notching " << status.lines << " line(s) up to " << std::setprecision(0)
						<< status.highestHz << " Hz, strongest " << std::setprecision(1) << status.strongestDb << " dB above its surroundings" << std::endl);
					logged = status;
				}
			}
		}

		std::string HumStatusText()
		{
			if (g_humFilterBaseHz.load(std::memory_order_relaxed) <= 0.0f) return " | hum filter off";
			const auto status = g_humRemover.GetStatus();
			if (status.lines == 0) return " | hum filter measuring";
			std::ostringstream text;
			text << " | hum filter " << std::fixed << std::setprecision(3) << status.fundamentalHz << " Hz, "
				<< status.lines << " line(s) to " << std::setprecision(0) << status.highestHz << " Hz";
			return text.str();
		}

		void EnsureHumWorker()
		{
			if (g_humWorkerStarted.exchange(true)) return;
			std::thread(HumAnalysisWorker).detach();
		}

		// Runs the hum remover over the raw capture buffer in place, front of chain, so every downstream stage
		// and the game see the de-hummed signal. Samples are always read (the analysis needs the quiet stretches)
		// but only written once a hum has been measured, so until then the input stays bit-exact.
		void ProcessHumFilterInPlace(BYTE* packet, const CaptureFormat& format, uint32_t frameCount, float baseHz)
		{
			const size_t channelCount = format.channelCount;
			if (channelCount == 0 || channelCount > MAX_FILTER_CHANNELS || frameCount > Audio::HumRemover::MAX_BLOCK_FRAMES) return;
			g_humRemover.BeginBlock(format.sampleRate, baseHz, channelCount);
			const bool write = g_humRemover.Active();

			switch (format.sampleFormat)
			{
			case SampleFormat::Float32:
			{
				float* samples = reinterpret_cast<float*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					for (size_t channel = 0; channel < channelCount; ++channel)
					{
						const size_t index = static_cast<size_t>(frame) * channelCount + channel;
						const float filtered = g_humRemover.Process(channel, samples[index]);
						if (write) samples[index] = ClampSample(filtered);
					}
				break;
			}
			case SampleFormat::Int32:
			{
				int32_t* samples = reinterpret_cast<int32_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					for (size_t channel = 0; channel < channelCount; ++channel)
					{
						const size_t index = static_cast<size_t>(frame) * channelCount + channel;
						const float filtered = g_humRemover.Process(channel, static_cast<float>(samples[index]) * INT32_TO_FLOAT);
						if (write) samples[index] = FloatToSignedInteger<int32_t, 32>(filtered);
					}
				break;
			}
			case SampleFormat::Int24:
			{
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					for (size_t channel = 0; channel < channelCount; ++channel)
					{
						const size_t index = static_cast<size_t>(frame) * channelCount + channel;
						const float filtered = g_humRemover.Process(channel, static_cast<float>(ReadInt24(packet + index * 3)) * INT24_TO_FLOAT);
						if (write) WriteInt24(FloatToSignedInteger<int32_t, 24>(filtered), packet + index * 3);
					}
				break;
			}
			case SampleFormat::Int16:
			{
				int16_t* samples = reinterpret_cast<int16_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					for (size_t channel = 0; channel < channelCount; ++channel)
					{
						const size_t index = static_cast<size_t>(frame) * channelCount + channel;
						const float filtered = g_humRemover.Process(channel, static_cast<float>(samples[index]) * INT16_TO_FLOAT);
						if (write) samples[index] = FloatToSignedInteger<int16_t, 16>(filtered);
					}
				break;
			}
			default:
				break;
			}
			g_humRemover.EndBlock();
		}

		int FindCaptureRoute(IAudioCaptureClient* captureClient)
		{
			for (size_t routeIndex = 0; routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
			{
				if (routeCaptureClients[routeIndex].load(std::memory_order_acquire) == captureClient)
					return static_cast<int>(routeIndex);
			}
			return -1;
		}

		SampleFormat ReadProxySampleFormat(long type)
		{
			switch (type)
			{
			case 18: return SampleFormat::Int32; // ASIOSTInt32LSB
			case 19: return SampleFormat::Float32; // ASIOSTFloat32LSB
			case 17: return SampleFormat::Int24; // ASIOSTInt24LSB
			case 16: return SampleFormat::Int16; // ASIOSTInt16LSB
			default: return SampleFormat::Unsupported;
			}
		}

		void __cdecl Hook_ProxyInput(
			const ProxyInputChannel* channels,
			long channelCount,
			long frames,
			double sampleRate)
		{
			if (!channels || channelCount <= 0 || frames <= 0 || !std::isfinite(sampleRate)
				|| sampleRate < 1.0 || sampleRate > UINT32_MAX) return;

			const int selectedChannel = selectedInputChannels[0];
			for (long index = 0; index < channelCount; ++index)
			{
				if (channels[index].channelNum != selectedChannel || !channels[index].buffer) continue;
				const SampleFormat sampleFormat = ReadProxySampleFormat(channels[index].type);
				if (sampleFormat == SampleFormat::Unsupported) return;

				{
					// Stage meter 1: the real device's samples, before RS_ASIO and the game see them.
					float peak = 0.0f;
					for (long frame = 0; frame < frames; ++frame)
					{
						float value = 0.0f;
						switch (sampleFormat)
						{
						case SampleFormat::Float32: value = static_cast<const float*>(channels[index].buffer)[frame]; break;
						case SampleFormat::Int32: value = static_cast<float>(static_cast<const int32_t*>(channels[index].buffer)[frame]) * INT32_TO_FLOAT; break;
						case SampleFormat::Int24: value = static_cast<float>(ReadInt24(static_cast<uint8_t*>(channels[index].buffer) + frame * 3)) * INT24_TO_FLOAT; break;
						case SampleFormat::Int16: value = static_cast<const int16_t*>(channels[index].buffer)[frame] * INT16_TO_FLOAT; break;
						default: break;
						}
						peak = std::max(peak, std::fabs(value));
					}
					stageProxyRaw.Note(peak);
				}
				proxyInputRate.store(static_cast<uint32_t>(sampleRate), std::memory_order_release);
				proxyInputSampleFormat.store(static_cast<int>(sampleFormat), std::memory_order_release);
				proxyInputSeen.store(true, std::memory_order_release);
				routeLastBufferTick[0].store(GetTickCount64(), std::memory_order_relaxed);
				AsioBufferState::lastPacket.store(GetTickCount64(), std::memory_order_relaxed);

				// The game's own capture stream is attached: Hook_CaptureGetBuffer processes this audio once
				// it reaches the game. Processing it here too would apply everything twice.
				if (routeCaptureClients[0].load(std::memory_order_acquire)) return;
				if (!inputReady[0].load(std::memory_order_acquire) || frames > MAX_BUFFER_FRAMES) return;
				CaptureCallbackScope callback(processingGate);
				if (!callback) return;

				float* converted = conversionBuffers[0].data();
				for (long frame = 0; frame < frames; ++frame)
				{
					switch (sampleFormat)
					{
					case SampleFormat::Float32: converted[frame] = static_cast<float*>(channels[index].buffer)[frame]; break;
					case SampleFormat::Int32: converted[frame] = static_cast<float>(static_cast<int32_t*>(channels[index].buffer)[frame]) * INT32_TO_FLOAT; break;
					case SampleFormat::Int24: converted[frame] = static_cast<float>(ReadInt24(static_cast<uint8_t*>(channels[index].buffer) + frame * 3)) * INT24_TO_FLOAT; break;
					case SampleFormat::Int16: converted[frame] = static_cast<int16_t*>(channels[index].buffer)[frame] * INT16_TO_FLOAT; break;
					default: return;
					}
				}

				IInputProcessor* processor = activeProcessors[0].load(std::memory_order_relaxed);
				if (processor) processor->Process(converted, static_cast<uint32_t>(frames));
				RawPitchVerifier::Observe(0, converted, static_cast<uint32_t>(frames), static_cast<uint32_t>(sampleRate));
				MlAudioExporter::Observe(0, converted, static_cast<uint32_t>(frames), static_cast<uint32_t>(sampleRate));
				for (long frame = 0; frame < frames; ++frame)
				{
					if (sampleFormat == SampleFormat::Float32) static_cast<float*>(channels[index].buffer)[frame] = converted[frame];
					else if (sampleFormat == SampleFormat::Int32) static_cast<int32_t*>(channels[index].buffer)[frame] = FloatToSignedInteger<int32_t, 32>(converted[frame]);
					else if (sampleFormat == SampleFormat::Int24) WriteInt24(FloatToSignedInteger<int32_t, 24>(converted[frame]), static_cast<uint8_t*>(channels[index].buffer) + frame * 3);
					else if (sampleFormat == SampleFormat::Int16) static_cast<int16_t*>(channels[index].buffer)[frame] = FloatToSignedInteger<int16_t, 16>(converted[frame]);
				}
				return;
			}
		}

		void TryInstallProxyInputObserver()
		{
			if (proxyInputObserverInstalled || GetTickCount64() < nextProxyInputObserverAttemptTick) return;
			nextProxyInputObserverAttemptTick = GetTickCount64() + UNMARSHAL_HOOK_RETRY_INTERVAL_MILLISECONDS;
			HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
			if (!proxy) return;
			auto setObserver = reinterpret_cast<SetProxyInputObserver_t>(GetProcAddress(proxy, "RSModsAsio_SetInputObserver"));
			if (!setObserver) return;
			setObserver(&Hook_ProxyInput);
			proxyInputObserverInstalled = true;
			LOG_INFO("[AsioHook] ASIO proxy input observer installed." << std::endl);
		}

		HRESULT STDMETHODCALLTYPE Hook_CaptureGetBuffer(
			IAudioCaptureClient* self,
			BYTE** data,
			UINT32* frameCount,
			DWORD* flags,
			UINT64* devicePosition,
			UINT64* performanceCounterPosition)
		{
			// PortAudio passes null for the timestamp; substitute our own so the packet's device
			// capture time is available for the measured input latency on every path,
			// including the stock shared stream this tap also sees.
			UINT64 localQpc = 0;
			UINT64* qpcOut = performanceCounterPosition ? performanceCounterPosition : &localQpc;
#if defined(RSMODS_AUDIO_LIFECYCLE_TRACE)
			const int traceRoute = FindCaptureRoute(self);
			LifecycleTrace::BufferEnter(traceRoute);
#endif
			const HRESULT result = originalCaptureGetBuffer(
				self, data, frameCount, flags, devicePosition, qpcOut);
#if defined(RSMODS_AUDIO_LIFECYCLE_TRACE)
			LifecycleTrace::BufferReturn(traceRoute, result, SUCCEEDED(result) && frameCount ? *frameCount : 0);
#endif

			if (FAILED(result) || result == AUDCLNT_S_BUFFER_EMPTY
				|| !frameCount || *frameCount == 0) return result;

			const int routeIndex = FindCaptureRoute(self);
			if (routeIndex < 0) return result;
			CaptureCallbackScope callback(processingGate);
			if (callback && bridgeStates[routeIndex])
			{
				if (!bridgeStates[routeIndex]->IsPhysicalPacket()
					|| bridgeStates[routeIndex]->GetGeneration() != bridgeGenerations[routeIndex].load())
				{
					routeLastBufferTick[routeIndex].store(0, std::memory_order_relaxed);
					if (routeIndex == 0) AsioBufferState::lastPacket.store(0);
					if (routeIndex == 0) stageReplacedWithSilence.fetch_add(1, std::memory_order_relaxed);
					if (*frameCount <= MAX_BUFFER_FRAMES && routeFormats[routeIndex].IsUsable())
					{
						static const std::array<float, MAX_BUFFER_FRAMES> silence{};
						RawPitchVerifier::Observe(static_cast<uint32_t>(routeIndex), silence.data(), *frameCount, routeFormats[routeIndex].sampleRate);
						MlAudioExporter::Observe(static_cast<uint32_t>(routeIndex), silence.data(), *frameCount, routeFormats[routeIndex].sampleRate);
						if (flags) *flags |= AUDCLNT_BUFFERFLAGS_SILENT;
					}
					return result;
				}
			}
			// Capture liveness exists before the game loop prepares the processor.
			routeLastBufferTick[routeIndex].store(GetTickCount64(), std::memory_order_relaxed);
			if (routeIndex == 0 && CableInput::IsAsioPath())
				AsioBufferState::lastPacket.store(GetTickCount64());

			// Stage meter 2: what RS_ASIO handed the game, BEFORE the conditioner below. Reads the packet
			// directly: the conversion buffer does not exist yet this early (see MeasureFirstChannelPeak).
			if (routeIndex == 0 && data && *data && *frameCount <= MAX_BUFFER_FRAMES
				&& routeFormats[routeIndex].IsUsable())
			{
				if (flags && (*flags & AUDCLNT_BUFFERFLAGS_SILENT))
					stageSilentFlagPackets.fetch_add(1, std::memory_order_relaxed);
				else
				{
					float peak = 0.0f;
					if (MeasureFirstChannelPeak(*data, routeFormats[routeIndex], *frameCount, peak))
						stageCaptureRaw.Note(peak);
				}
			}

			// Front-of-chain guitar input conditioner. Runs on every real packet, BEFORE the processing
			// gate below, so it lifts/cleans the input globally (cable or RS_ASIO, Drop Pedal or not) and
			// always precedes any pitch shift. Chain order: noise suppressor (keyed to the raw level) ->
			// make-up gain (+ soft-clip) -> band-limit. Every stage is a no-op when its control is off, so
			// the feature-off input stays bit-exact.
			if (routeIndex == 0 && data && *data && frameCount && *frameCount <= MAX_BUFFER_FRAMES
				&& routeFormats[routeIndex].IsUsable()
				&& !(flags && (*flags & AUDCLNT_BUFFERFLAGS_SILENT)))
			{
				// Mains-hum notch runs first, on the raw input, so the suppressor/compressor/gain and the game all
				// see the de-hummed signal (and the suppressor keys off a level no longer inflated by the hum).
				const float humBaseHz = g_humFilterBaseHz.load(std::memory_order_relaxed);
				if (humBaseHz > 0.0f)
					ProcessHumFilterInPlace(*data, routeFormats[routeIndex], *frameCount, humBaseHz);

				const uint32_t gateThresholdRevision = g_gateThresholdRevision.load(std::memory_order_acquire);
				if (gateThresholdRevision != g_processedGateThresholdRevision)
				{
					g_noiseSuppressor.Reset();
					g_processedGateThresholdRevision = gateThresholdRevision;
				}
				const float gateThreshold = g_gateThresholdLinear.load(std::memory_order_relaxed);
				if (gateThreshold > 0.0f)
					ProcessNoiseSuppressorInPlace(*data, routeFormats[routeIndex], *frameCount, gateThreshold);

				// Compressor sits after the gate and before the make-up gain: it flattens the string-beat
				// wobble so the game amp downstream cannot magnify it into a warble.
				const float compressorStrength = g_compressorStrength.load(std::memory_order_relaxed);
				if (compressorStrength > 0.0f)
					ProcessCompressorInPlace(*data, routeFormats[routeIndex], *frameCount, compressorStrength);

				const float inputGain = g_inputGainLinear.load(std::memory_order_relaxed);
				if (inputGain > 1.0001f || inputGain < 0.9999f)
				{
					ScaleBufferInPlace(*data, routeFormats[routeIndex], *frameCount, inputGain);
					// Band-limit after the gain so the same stage that smooths amplified hiss also
					// tames the soft-clip's transient harmonics. Tied to the gain-engaged path, so
					// the unity/feature-off input stays bit-exact.
					FilterBufferInPlace(*data, routeFormats[routeIndex], *frameCount);
				}
			}

			if (!callback || FindCaptureRoute(self) != routeIndex
				|| !inputReady[routeIndex].load(std::memory_order_acquire)) return result;

			if (routeIndex == 0 && !CableInput::IsAsioPath())
			{
				if (*qpcOut == 0 || (flags && (*flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)))
				{
					CableInput::ReportCaptureTimestampLag(-1.0);
				}
				else
				{
					LARGE_INTEGER counter{};
					static const LARGE_INTEGER frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
					QueryPerformanceCounter(&counter);
					const double now100ns = frequency.QuadPart > 0 ? counter.QuadPart * 10000000.0 / frequency.QuadPart : 0.0;
					const double delta100ns = now100ns - static_cast<double>(*qpcOut);
					CableInput::ReportMeasuredInputRaw(static_cast<int64_t>(delta100ns), *frameCount);
					CableInput::ReportCaptureTimestampLag(delta100ns / 10000.0);
				}
			}

			if (!data || !*data || *frameCount > MAX_BUFFER_FRAMES)
				return result;

			IInputProcessor* processor = activeProcessors[routeIndex].load(std::memory_order_relaxed);

			// A silent buffer has no samples for a processor to transform. Route 0 remains
			// observable so its timeline and diagnostics stay continuous.
			const bool silent = flags && (*flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
			const bool runsProcessor = processor != nullptr && !silent;
			const bool observesRoute = routeIndex == 0;
			// Route 1 is Player 2 on the second ASIO input; it only feeds the Player 2 SIGNAL row.
			const bool metersPlayerTwo = routeIndex == 1;
			if (!runsProcessor && !observesRoute && !metersPlayerTwo) return result;

			float* converted = conversionBuffers[routeIndex].data();
			if (silent)
				std::fill(converted, converted + *frameCount, 0.0f);
			else if (!CopyFirstChannelToFloat(*data, routeFormats[routeIndex], *frameCount, converted))
				return result;

			if (metersPlayerTwo)
			{
				float peak = 0.0f;
				for (UINT32 index = 0; index < *frameCount; ++index)
				{
					peak = std::max(peak, std::fabs(converted[index]));
				}
				CableInput::ReportPlayerTwoPacket(peak, silent);
			}

			// Input make-up gain was already applied in place to the raw buffer above (front of chain),
			// so the samples copied here are already boosted; the peak report, pitch verifiers, dry
			// recording and any processor all see the lifted signal.
			if (observesRoute)
			{
				float peak = 0.0f;
				for (UINT32 index = 0; index < *frameCount; ++index)
				{
					peak = std::max(peak, std::fabs(converted[index]));
				}
				CableInput::ReportTapPacket(peak, silent, *frameCount, routeFormats[routeIndex].sampleRate);
				stageAfterConditioner.Note(silent ? 0.0f : peak);   // stage meter 3

				// Latency loopback capture: copy the raw input (before any processor) while a measurement is armed.
				const int latTarget = g_latencyTarget.load(std::memory_order_relaxed);
				if (latTarget > 0)
				{
					int filled = g_latencyFilled.load(std::memory_order_relaxed);
					if (filled < latTarget)
					{
						int n = static_cast<int>(*frameCount);
						if (n > latTarget - filled) n = latTarget - filled;
						for (int i = 0; i < n; ++i) g_latencyCapture[filled + i] = converted[i];
						g_latencyFilled.store(filled + n, std::memory_order_relaxed);
					}
				}
			}

			bool modifiesBuffer = false;
			if (processor)
			{
				modifiesBuffer = processor->Process(converted, *frameCount);
			}
			if (observesRoute)
			{
				DrySignalRecording::Observe(converted, *frameCount, routeFormats[routeIndex].sampleRate);
				RawPitchVerifier::Observe(
					static_cast<uint32_t>(routeIndex),
					converted,
					*frameCount,
					routeFormats[routeIndex].sampleRate);
				// Tier-1 mirror of the same observation: the shared-memory export the
				// 64-bit companion pitch service reads. Identical semantics (silent
				// buffers already arrive here as zeros); a no-op until the game loop
				// has created the mapping, and never allocates or locks on this thread.
				MlAudioExporter::Observe(
					static_cast<uint32_t>(routeIndex),
					converted,
					*frameCount,
					routeFormats[routeIndex].sampleRate);
			}
			if (modifiesBuffer)
			{
				CopyFloatToAllChannels(*data, routeFormats[routeIndex], *frameCount, converted);
				if (silent && flags) *flags &= ~AUDCLNT_BUFFERFLAGS_SILENT;
			}
			return result;
		}

		void UpdateBufferLayoutReady()
		{
			for (size_t routeIndex = 0; routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
			{
				if (!configuredInputs[routeIndex]) continue;
				if ((!routeCaptureClients[routeIndex].load(std::memory_order_acquire)
					&& !(routeIndex == 0 && proxyInputSeen.load(std::memory_order_acquire)))
					|| !routeFormats[routeIndex].IsUsable()) return;
			}
			bufferLayoutReady.store(true, std::memory_order_release);
		}

		void RegisterCaptureStream(PaWasapiStreamPrefix* stream)
		{
			if (!stream || !stream->input.clientParent || !stream->captureClient) return;
			std::lock_guard<std::mutex> guard(registrationMutex);
			if (FindCaptureRoute(stream->captureClient) >= 0) return;

			size_t routeIndex = INPUT_ROUTE_COUNT;
			bool replacesCapture = false;
			for (size_t candidate = 0; candidate < INPUT_ROUTE_COUNT; ++candidate)
			{
				if (configuredInputs[candidate]
					&& !routeCaptureClients[candidate].load(std::memory_order_relaxed))
				{
					routeIndex = candidate;
					break;
				}
			}

			if (routeIndex == INPUT_ROUTE_COUNT)
			{
				// Stream churn: leaving a song and reloading makes the game abandon its capture
				// stream and open a NEW one. Ignoring the new stream would leave every route pinned
				// to a dead stream while the game hears the player through the new one, so the
				// tap, processors and observer hear nothing. With a single configured route the newest
				// stream IS the live one: rebind Player 1 to it and clear the auto-enable
				// latch so Poll brings processing back up through the normal path.
				size_t configuredCount = 0;
				for (size_t candidate = 0; candidate < INPUT_ROUTE_COUNT; ++candidate)
				{
					if (configuredInputs[candidate]) ++configuredCount;
				}
				if (configuredCount != 1)
				{
					// Two-cable attribution is unproven; never guess which player moved.
					LOG_WARNING("[AsioHook] Ignoring an additional capture stream after all configured routes were attached." << std::endl);
					return;
				}
				routeIndex = 0;
				while (routeIndex < INPUT_ROUTE_COUNT && !configuredInputs[routeIndex]) ++routeIndex;

				// LIVENESS GATE: the game and RS_ASIO open MULTIPLE capture streams, and
				// rebinding to every newcomer can move the route onto a stream carrying the
				// SONG's audio instead of the guitar. A binding whose stream still delivers buffers is
				// ALIVE and is never stolen; only a binding that has gone quiet for two
				// seconds (the game abandoned its stream on song exit) yields to a
				// newcomer.
				const uint64_t lastBuffer =
					routeLastBufferTick[routeIndex].load(std::memory_order_relaxed);
				if (lastBuffer != 0 && GetTickCount64() - lastBuffer < 2000)
				{
					LOG_INFO("[AsioHook] Ignoring a new capture stream while Player "
						<< routeIndex + 1 << "'s bound stream is still delivering buffers." << std::endl);
					return;
				}
				replacesCapture = true;
			}

			const CaptureFormat format = ReadCaptureFormat(stream->input.waveFormat);
			if (!format.IsUsable())
			{
				const WAVEFORMATEX& waveFormat = stream->input.waveFormat.Format;
				LOG_ERROR("[AsioHook] Player " << routeIndex + 1 << " negotiated an unsupported capture format: tag "
					<< waveFormat.wFormatTag << ", " << waveFormat.nSamplesPerSec << " Hz, "
					<< waveFormat.nChannels << " channel(s), " << waveFormat.wBitsPerSample << " bits." << std::endl);
				return;
			}

			void** vTable = ComVTable::GetVTable(stream->captureClient);
			if (!captureClientVTable)
			{
				originalCaptureGetBuffer = reinterpret_cast<CaptureGetBuffer_t>(
					vTable[SLOT_CAPTURE_CLIENT_GET_BUFFER]);
				void* replacedGetBuffer = ComVTable::PatchSlot(
					stream->captureClient,
					SLOT_CAPTURE_CLIENT_GET_BUFFER,
					Hook_CaptureGetBuffer);

				if (!replacedGetBuffer
					|| replacedGetBuffer != reinterpret_cast<void*>(originalCaptureGetBuffer))
				{
					originalCaptureGetBuffer = nullptr;
					LOG_ERROR("[AsioHook] Could not patch IAudioCaptureClient::GetBuffer." << std::endl);
					return;
				}
				captureClientVTable = vTable;
			}
			else if (captureClientVTable != vTable)
			{
				// A newcomer from another implementation cannot replace a live binding: with the Player 2
				// cable always listed, the game opens its stream a few milliseconds after Player 1's RS_ASIO
				// stream, before that stream has delivered a buffer, so the liveness gate above lets it
				// through. It is a different device, not a replacement; keep the binding and move on.
				if (replacesCapture)
				{
					LOG_INFO("[AsioHook] Ignoring a capture stream from a different implementation while Player "
						<< routeIndex + 1 << " stays bound." << std::endl);
					return;
				}
				LOG_ERROR("[AsioHook] Player " << routeIndex + 1
					<< " uses a different capture implementation; processing stays disabled." << std::endl);
				return;
			}

			processingGate.CloseAndWait();
			bridgeStates[routeIndex].Reset();
			stream->captureClient->QueryInterface(__uuidof(PersistentInput::ICaptureState),
				reinterpret_cast<void**>(bridgeStates[routeIndex].GetAddressOf()));
			bridgeGenerations[routeIndex] = bridgeStates[routeIndex] ? bridgeStates[routeIndex]->GetGeneration() : 0;
			inputReady[routeIndex].store(false, std::memory_order_relaxed);
			bufferLayoutReady.store(false, std::memory_order_release);
			autoEnabledOnce.store(false, std::memory_order_relaxed);
			routeLastBufferTick[routeIndex].store(0, std::memory_order_relaxed);
			routeFormats[routeIndex] = format;
			if (routeIndex == 0)
			{
				UINT32 frames = 0;
				Microsoft::WRL::ComPtr<IAudioClient> audioClient;
				HRESULT result = stream->input.clientProc
					? stream->input.clientProc->QueryInterface(IID_PPV_ARGS(&audioClient)) : E_POINTER;
				if (SUCCEEDED(result)) result = audioClient->GetBufferSize(&frames);
				if (FAILED(result)) LOG_ERROR("[AsioHook] Could not read input buffer size: " << std::hex << result << std::dec << std::endl);
				AsioBufferState::Configure(SUCCEEDED(result) ? frames : 0, format.sampleRate);
			}
			routeCaptureClients[routeIndex].store(stream->captureClient, std::memory_order_release);
			if (replacesCapture)
			{
				LOG_INFO("[AsioHook] Player " << routeIndex + 1
					<< " capture stream went quiet and was replaced; rebinding to the newest stream." << std::endl);
			}

			LOG_INFO("[InputCapture] Player " << routeIndex + 1 << " attached to the game capture client "
				<< stream->captureClient << " using " << DescribeFormat(format) << "." << std::endl);
			UpdateBufferLayoutReady();
		}

		HRESULT __cdecl Hook_UnmarshalStreamComPointers(void* stream)
		{
			const HRESULT result = originalUnmarshalStreamComPointers(stream);
			if (SUCCEEDED(result) && stream)
			{
				auto* audioStream = reinterpret_cast<PaWasapiStreamPrefix*>(stream);
#if defined(RSMODS_AUDIO_LIFECYCLE_TRACE)
				// INPUT DIAG (unplugged-boot -> RTC re-select never detects ASIO input): this fires whenever
				// the game unmarshals a stream's COM pointers, i.e. when it actually opens a stream. If the
				// game re-opens capture on RTC re-selection this logs capture-shaped=1 and RegisterCaptureStream
				// binds (logs "attached to the game capture client"); seeing only capture-shaped=0 means the
				// game re-opens output but never a capture stream after an unplugged boot.
				LOG_INFO("(INPUT DIAG) game unmarshalled a PortAudio stream; capture-shaped="
					<< ((audioStream->input.clientParent && audioStream->captureClient) ? 1 : 0)
					<< "." << std::endl);
				if (audioStream->input.clientParent && audioStream->captureClient)
				{
					LifecycleTrace::Attach(reinterpret_cast<IAudioClient*>(audioStream->input.clientProc), stream);
					LifecycleTrace::Record(LifecycleTrace::Kind::CaptureBind, audioStream->captureClient, stream);
				}
#endif
				RegisterCaptureStream(audioStream);
			}
			return result;
		}

		UnmarshalHookInstallResult InstallUnmarshalHook()
		{
			HMODULE gameModule = GetModuleHandleA(nullptr);
			MODULEINFO gameInfo{};
			if (!gameModule || !GetModuleInformation(GetCurrentProcess(), gameModule, &gameInfo, sizeof(gameInfo)))
			{
				LOG_ERROR("[AsioHook] Could not inspect the Rocksmith executable, error " << GetLastError() << "." << std::endl);
				return UnmarshalHookInstallResult::Failed;
			}

			if (!unmarshalPatchedTarget)
			{
				std::vector<uint8_t*> callSites;
				FindPattern(gameInfo, UNMARSHAL_CALL_PRE_PATCH, callSites);
				FindPattern(gameInfo, UNMARSHAL_CALL_POST_PATCH, callSites);
				FindPattern(gameInfo, UNMARSHAL_CALL_LEARN_AND_PLAY, callSites);

				std::vector<uint8_t*> unmarshalTargets;
				for (uint8_t* callSite : callSites)
				{
					const int32_t relativeTarget = *reinterpret_cast<const int32_t*>(callSite + 1);
					uint8_t* target = callSite + 5 + relativeTarget;
					if (!IsRangeInsideModule(target, 6, gameInfo))
					{
						LOG_ERROR("[AsioHook] A supported PortAudio unmarshal call targets outside the Rocksmith image; processing stays disabled." << std::endl);
						return UnmarshalHookInstallResult::Failed;
					}
					if (std::find(unmarshalTargets.begin(), unmarshalTargets.end(), target)
						== unmarshalTargets.end())
					{
						unmarshalTargets.push_back(target);
					}
				}

				if (unmarshalTargets.size() != 1)
				{
					LOG_ERROR("[AsioHook] Expected one supported PortAudio unmarshal target, found "
						<< unmarshalTargets.size() << " across " << callSites.size()
						<< " call sites; processing stays disabled." << std::endl);
					return UnmarshalHookInstallResult::Failed;
				}

				unmarshalPatchedTarget = unmarshalTargets[0];
			}

			HMODULE rsAsioModule = GetModuleHandleA("RS_ASIO.dll");
			if (!rsAsioModule)
			{
				// Native-cable path: detour the unmarshal function itself. Patching only the
				// two known direct callers misses the input stream: the input path reaches this
				// function through a different caller.
				// The function detour covers every caller and returns a trampoline for the game.
				if (IsRsAsioEnabled())
				{
					if (!hasLoggedWaitingForRsAsio)
					{
						hasLoggedWaitingForRsAsio = true;
						LOG_INFO("[AsioHook] RS_ASIO.dll exists but is not loaded yet; capture attachment will retry without waiting." << std::endl);
					}
					return UnmarshalHookInstallResult::Retry;
				}

				const PBYTE trampoline = DetourFunction(
					reinterpret_cast<PBYTE>(unmarshalPatchedTarget),
					reinterpret_cast<PBYTE>(&Hook_UnmarshalStreamComPointers));
				if (trampoline == nullptr)
				{
					LOG_ERROR("[InputCapture] Could not detour the native PortAudio unmarshal function; processing stays disabled." << std::endl);
					return UnmarshalHookInstallResult::Failed;
				}

				originalUnmarshalStreamComPointers =
					reinterpret_cast<UnmarshalStreamComPointers_t>(trampoline);
				LOG_INFO("[InputCapture] Native Real Tone Cable capture hook installed." << std::endl);
				return UnmarshalHookInstallResult::Installed;
			}

			MODULEINFO rsAsioInfo{};
			if (!GetModuleInformation(GetCurrentProcess(), rsAsioModule, &rsAsioInfo, sizeof(rsAsioInfo)))
			{
				LOG_ERROR("[AsioHook] Could not inspect RS_ASIO.dll, error " << GetLastError() << "." << std::endl);
				return UnmarshalHookInstallResult::Failed;
			}

			if (unmarshalPatchedTarget[0] != 0x68 || unmarshalPatchedTarget[5] != 0xc3)
			{
				if (!hasLoggedWaitingForRsAsioPatch)
				{
					hasLoggedWaitingForRsAsioPatch = true;
					LOG_INFO("[AsioHook] RS_ASIO has not installed the expected PortAudio unmarshal patch yet; capture attachment will retry without waiting." << std::endl);
				}
				return UnmarshalHookInstallResult::Retry;
			}

			void* rsAsioUnmarshal = nullptr;
			std::memcpy(&rsAsioUnmarshal, unmarshalPatchedTarget + 1, sizeof(rsAsioUnmarshal));
			if (!IsRangeInsideModule(rsAsioUnmarshal, 1, rsAsioInfo))
			{
				LOG_ERROR("[AsioHook] The existing unmarshal target is outside RS_ASIO.dll; processing stays disabled." << std::endl);
				return UnmarshalHookInstallResult::Failed;
			}

			originalUnmarshalStreamComPointers = reinterpret_cast<UnmarshalStreamComPointers_t>(rsAsioUnmarshal);
			if (!ReplacePatchedTarget(unmarshalPatchedTarget, reinterpret_cast<void*>(Hook_UnmarshalStreamComPointers)))
			{
				originalUnmarshalStreamComPointers = nullptr;
				LOG_ERROR("[AsioHook] Could not chain the existing RS_ASIO unmarshal hook." << std::endl);
				return UnmarshalHookInstallResult::Failed;
			}

			LOG_INFO("[AsioHook] Validated and attached to RS_ASIO's existing capture path without creating another ASIO host." << std::endl);
			return UnmarshalHookInstallResult::Installed;
		}

		void AttemptUnmarshalHookInstallation()
		{
			if (!isUnmarshalHookInstallPending || isUnmarshalHookInstalled) return;

			const auto result = InstallUnmarshalHook();
			switch (result)
			{
			case UnmarshalHookInstallResult::Installed:
				isUnmarshalHookInstalled = true;
				isUnmarshalHookInstallPending = false;
				break;
			case UnmarshalHookInstallResult::Retry:
				nextUnmarshalHookAttemptTick = GetTickCount64() + UNMARSHAL_HOOK_RETRY_INTERVAL_MILLISECONDS;
				break;
			case UnmarshalHookInstallResult::Failed:
				isUnmarshalHookInstallPending = false;
				break;
			}
		}
	}

	void Install()
	{
		static bool installed = false;
		if (installed) return;
		installed = true;

		LOG_INFO("[InputCapture] Installing the shared Drop Pedal input path." << std::endl);
#if defined(RSMODS_AUDIO_LIFECYCLE_TRACE)
		LifecycleTrace::Initialize();
#endif
		const bool hasRsAsio = IsRsAsioEnabled();
		const RsAsioConfiguration configuration = hasRsAsio
			? ReadRsAsioConfiguration()
			: RsAsioConfiguration{};
		bool hasConfiguredInput = false;

		if (!hasRsAsio)
		{
			ReportUnreadableRsAsioConfiguration();
			configuredInputs[0] = true;
			selectedInputChannels[0] = 0;
			hasConfiguredInput = true;
			LOG_INFO("[InputCapture] Real Tone Cable route selected for Player 1." << std::endl);
		}

		for (size_t routeIndex = 0; hasRsAsio && routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
		{
			configuredInputs[routeIndex] = configuration.inputConfigured[routeIndex];
			selectedInputChannels[routeIndex] = configuration.inputChannels[routeIndex];

			if (!configuredInputs[routeIndex])
				continue;

			hasConfiguredInput = true;
			if (selectedInputChannels[routeIndex] < 0)
			{
				LOG_ERROR("[AsioHook] Player " << routeIndex + 1
					<< " has no valid Channel in RS_ASIO.ini; processing stays disabled." << std::endl);
				return;
			}

			LOG_INFO("[AsioHook] Player " << routeIndex + 1 << " will follow the existing RS_ASIO endpoint for "
				<< configuration.inputSources[routeIndex] << ", ASIO channel "
				<< selectedInputChannels[routeIndex] << "." << std::endl);

			if (configuration.inputInferred[routeIndex])
			{
				LOG_WARNING("[AsioHook] Player " << routeIndex + 1
					<< " input routing was inferred because no matching [Asio.Input.N] section named a driver." << std::endl);
			}
		}

		if (!hasConfiguredInput)
		{
			LOG_ERROR("[InputCapture] RS_ASIO is installed but no input route is configured in RS_ASIO.ini." << std::endl);
			return;
		}

		isUnmarshalHookInstallPending = true;
		AttemptUnmarshalHookInstallation();
	}

		void Poll()
		{
			std::lock_guard<std::mutex> guard(registrationMutex);
			TryInstallProxyInputObserver();
			if (proxyInputSeen.load(std::memory_order_acquire) && !routeCaptureClients[0].load(std::memory_order_acquire))
			{
				routeFormats[0] = {
					static_cast<SampleFormat>(proxyInputSampleFormat.load(std::memory_order_acquire)),
					proxyInputRate.load(std::memory_order_acquire), 1 };
				bufferLayoutReady.store(false, std::memory_order_release);
				UpdateBufferLayoutReady();
			}
			for (size_t route = 0; route < INPUT_ROUTE_COUNT; ++route)
			{
				if (!bridgeStates[route] || bridgeStates[route]->GetGeneration() == bridgeGenerations[route].load()) continue;
				processingGate.CloseAndWait();
				bridgeGenerations[route] = bridgeStates[route]->GetGeneration();
				routeLastBufferTick[route] = 0;
				inputReady[route] = false;
				autoEnabledOnce = false;
			}
			if (isUnmarshalHookInstallPending
				&& GetTickCount64() >= nextUnmarshalHookAttemptTick)
			{
				AttemptUnmarshalHookInstallation();
			}

		if (autoEnabledOnce.load(std::memory_order_relaxed)) return;
		if (processingGate.IsOpen()) return;
		if (!bufferLayoutReady.load(std::memory_order_acquire)) return;

		for (size_t routeIndex = 0; routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
		{
			if (!configuredInputs[routeIndex]) continue;
			if (!routeFormats[routeIndex].IsUsable()) return;
			if (!activeProcessors[routeIndex].load(std::memory_order_relaxed)) return;
		}

		autoEnabledOnce.store(true, std::memory_order_relaxed);
		for (size_t routeIndex = 0; routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
		{
			if (!configuredInputs[routeIndex]) continue;

			conversionBuffers[routeIndex].assign(MAX_BUFFER_FRAMES, 0.0f);
			IInputProcessor* processor = activeProcessors[routeIndex].load(std::memory_order_relaxed);
			if (processor) processor->Prepare(routeFormats[routeIndex]);
			inputReady[routeIndex].store(true, std::memory_order_release);
			LOG_INFO("[AsioHook] Player " << routeIndex + 1 << " input prepared ("
				<< (processor ? "processor" : "no processor") << ")." << std::endl);
		}

		UpdateProcessingEnabled(true);
	}

	void SetProcessor(size_t routeIndex, IInputProcessor* inputProcessor)
	{
		if (routeIndex >= INPUT_ROUTE_COUNT)
		{
			LOG_ERROR("[AsioHook] Refusing to set processor for invalid route " << routeIndex << "." << std::endl);
			return;
		}
		std::lock_guard<std::mutex> guard(registrationMutex);
		if (activeProcessors[routeIndex].load(std::memory_order_relaxed) == inputProcessor) return;
		processingGate.CloseAndWait();
		inputReady[routeIndex].store(false, std::memory_order_release);
		autoEnabledOnce.store(false, std::memory_order_relaxed);
		activeProcessors[routeIndex].store(inputProcessor, std::memory_order_relaxed);
	}


	void SetInputGainDb(float decibels)
	{
		// Clamp to a sane make-up range: cut a little, boost up to a cable's hot preamp and beyond.
		if (!std::isfinite(decibels)) return;
		decibels = std::clamp(decibels, -24.0f, 24.0f);
		g_inputGainLinear.store(std::pow(10.0f, decibels / 20.0f), std::memory_order_relaxed);
	}

	void SetNoiseGateThresholdDb(float decibels)
	{
		// A threshold at or above 0 dBFS is meaningless, so it doubles as the "off" signal: store 0
		// (disabled, raw input untouched). The revision makes the audio thread reset its detector after
		// every live setting change, including off then on again at the same threshold.
		if (!std::isfinite(decibels) || decibels >= 0.0f)
		{
			g_gateThresholdLinear.store(0.0f, std::memory_order_relaxed);
			g_gateThresholdRevision.fetch_add(1, std::memory_order_release);
			return;
		}
		decibels = std::clamp(decibels, -90.0f, -20.0f);
		g_gateThresholdLinear.store(std::pow(10.0f, decibels / 20.0f), std::memory_order_relaxed);
		g_gateThresholdRevision.fetch_add(1, std::memory_order_release);
	}

	void StartLatencyCapture(int frames)
	{
		if (frames < 1) frames = 1;
		if (frames > kLatencyMaxFrames) frames = kLatencyMaxFrames;
		g_latencyTarget.store(0, std::memory_order_relaxed);    // disarm before reset so the audio thread stops appending
		g_latencyFilled.store(0, std::memory_order_relaxed);
		g_latencyTarget.store(frames, std::memory_order_relaxed);   // arm last
	}

	bool IsLatencyCaptureDone()
	{
		const int target = g_latencyTarget.load(std::memory_order_relaxed);
		return target > 0 && g_latencyFilled.load(std::memory_order_relaxed) >= target;
	}

	int GetLatencyCapture(const float** out)
	{
		if (out) *out = g_latencyCapture;
		return g_latencyFilled.load(std::memory_order_relaxed);
	}

	float GetInputGainDb()
	{
		const float gain = g_inputGainLinear.load(std::memory_order_relaxed);
		return gain > 0.0f ? 20.0f * std::log10(gain) : -24.0f;
	}

	float GetNoiseGateThresholdDb()
	{
		const float threshold = g_gateThresholdLinear.load(std::memory_order_relaxed);
		return threshold > 0.0f ? 20.0f * std::log10(threshold) : 0.0f; // 0 = off
	}

	void SetCompressorStrength(float strength)
	{
		// 0 = off (bypass, input untouched); 1 = maximum squeeze. Clamped to a sane macro range.
		if (!std::isfinite(strength)) return;
		g_compressorStrength.store(std::clamp(strength, 0.0f, 1.0f), std::memory_order_relaxed);
	}

	float GetCompressorStrength()
	{
		return g_compressorStrength.load(std::memory_order_relaxed);
	}

	void PollInputStageMeter()
	{
		// One line every 10 s bracketing Player 1's input chain, so a dead stretch says WHERE the signal
		// stopped: the device (proxy), RS_ASIO -> game, or the conditioner (suppressor/compressor/gain).
		constexpr uint64_t INTERVAL_MS = 10000;
		static uint64_t lastTick = 0;
		const uint64_t now = GetTickCount64();
		if (lastTick == 0) { lastTick = now; return; }
		if (now - lastTick < INTERVAL_MS) return;
		lastTick = now;
		auto describe = [](StagePeak& stage)
		{
			const uint32_t buffers = stage.buffers.exchange(0, std::memory_order_relaxed);
			const float peak = stage.TakePeak();
			std::ostringstream text;
			if (buffers == 0) text << "no buffers";
			else if (!(peak > 0.0f)) text << "digital silence (" << buffers << " buffers)";
			else text << std::fixed << std::setprecision(1) << 20.0 * std::log10(peak) << " dBFS peak (" << buffers << " buffers)";
			return text.str();
		};
		const std::string proxyRaw = proxyInputObserverInstalled ? describe(stageProxyRaw) : std::string("n/a (no proxy observer)");
		const std::string captureRaw = describe(stageCaptureRaw);
		const std::string afterConditioner = describe(stageAfterConditioner);
		const uint32_t silentFlagged = stageSilentFlagPackets.exchange(0, std::memory_order_relaxed);
		const uint32_t replaced = stageReplacedWithSilence.exchange(0, std::memory_order_relaxed);
		LOG_INFO("(INPUT STAGES) device raw (proxy): " << proxyRaw
			<< " | game capture raw (from RS_ASIO): " << captureRaw
			<< " | after conditioner: " << afterConditioner
			<< " | silent-flagged packets: " << silentFlagged
			<< " | replaced with silence: " << replaced
			<< " | suppressor threshold " << std::fixed << std::setprecision(1) << GetNoiseGateThresholdDb()
			<< " dBFS (opens 6 dB above it), gain " << GetInputGainDb() << " dB, compressor "
			<< std::setprecision(2) << GetCompressorStrength() << HumStatusText() << std::endl);
	}

	void SetHumFilterBaseHz(float baseHz)
	{
		// 0 (or <20) = off; else the mains fundamental (50 or 60). Clamped so a stray value can't build a
		// nonsensical notch bank. Any positive base arms the front-of-chain notch cascade.
		if (!std::isfinite(baseHz) || baseHz < 20.0f) { g_humFilterBaseHz.store(0.0f, std::memory_order_relaxed); return; }
		EnsureHumWorker();
		g_humFilterBaseHz.store(std::clamp(baseHz, 20.0f, 120.0f), std::memory_order_relaxed);
	}

	float GetHumFilterBaseHz()
	{
		return g_humFilterBaseHz.load(std::memory_order_relaxed);
	}

	namespace
	{
		void UpdateProcessingEnabled(bool enabled)
		{
			if (enabled && !bufferLayoutReady.load(std::memory_order_acquire))
			{
				LOG_ERROR("[InputCapture] Refusing to enable processing before an input capture route is attached." << std::endl);
				return;
			}

			if (enabled)
			{
				for (size_t routeIndex = 0; routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
				{
					if (!configuredInputs[routeIndex]) continue;
					if (!activeProcessors[routeIndex].load(std::memory_order_relaxed)
						|| (!routeCaptureClients[routeIndex].load(std::memory_order_acquire)
							&& !(routeIndex == 0 && proxyInputSeen.load(std::memory_order_acquire)))
						|| !routeFormats[routeIndex].IsUsable()
						|| !inputReady[routeIndex].load(std::memory_order_acquire))
					{
						LOG_ERROR("[AsioHook] Refusing to enable processing because Player "
							<< routeIndex + 1 << " is not ready." << std::endl);
						return;
					}
				}
			}

			if (enabled) processingGate.Open();
			else processingGate.CloseAndWait();
			LOG_INFO("[AsioHook] Processing " << (enabled ? "enabled" : "disabled") << std::endl);
		}
	}

	void SetProcessingEnabled(bool enabled)
	{
		std::lock_guard<std::mutex> guard(registrationMutex);
		UpdateProcessingEnabled(enabled);
	}

	bool IsProcessingEnabled()
	{
		if (!processingGate.IsOpen()) return false;
		for (size_t routeIndex = 0; routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
		{
			if (configuredInputs[routeIndex] && !IsInputReady(routeIndex)) return false;
		}
		return true;
	}

	bool IsInputConfigured(size_t routeIndex)
	{
		return routeIndex < INPUT_ROUTE_COUNT && configuredInputs[routeIndex];
	}

	bool IsInputReady(size_t routeIndex)
	{
		CaptureCallbackScope callback(processingGate);
		if (routeIndex >= INPUT_ROUTE_COUNT || !callback
			|| !inputReady[routeIndex].load(std::memory_order_acquire)) return false;
		if (bridgeStates[routeIndex] && (!bridgeStates[routeIndex]->IsPhysicalPacket()
			|| bridgeStates[routeIndex]->GetGeneration() != bridgeGenerations[routeIndex].load())) return false;
		const uint64_t lastBuffer = routeLastBufferTick[routeIndex].load(std::memory_order_relaxed);
		return lastBuffer != 0 && GetTickCount64() - lastBuffer < INPUT_STALL_MILLISECONDS;
	}
}

namespace Audio
{
	std::string DescribeFormat(const CaptureFormat& format)
	{
		const char* sampleType = "unsupported";
		switch (format.sampleFormat)
		{
		case SampleFormat::Float32: sampleType = "32-bit float"; break;
		case SampleFormat::Int32: sampleType = "32-bit PCM"; break;
		case SampleFormat::Int24: sampleType = "24-bit PCM"; break;
		case SampleFormat::Int16: sampleType = "16-bit PCM"; break;
		default: break;
		}

		return std::to_string(format.channelCount) + " channel(s), "
			+ std::to_string(format.sampleRate) + " Hz, " + sampleType;
	}
}
