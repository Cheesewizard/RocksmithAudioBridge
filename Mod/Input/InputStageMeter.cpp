#include "InputStageMeter.hpp"

#include "InputConditioner.hpp"
#include "RsAsioConfig.hpp"
#include "../AudioBridge/Cable/CableInput.hpp"
#include "../AudioBridge/Engine/GameToneLatency.hpp"

#include "Log.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace AudioBridge::Input::StageMeter
{
	namespace
	{
		constexpr std::uint32_t PLAYER_ONE = 0;
		constexpr std::uint32_t PLAYER_TWO = 1;
		constexpr std::uint64_t LOG_INTERVAL_MS = 10000;
		constexpr std::uint64_t PROXY_RETRY_MS = 250;
		constexpr long MAX_PROXY_FRAMES = 4096;
		constexpr float INT16_TO_FLOAT = 1.0f / 32768.0f;
		constexpr float INT24_TO_FLOAT = 1.0f / 8388608.0f;
		constexpr float INT32_TO_FLOAT = 1.0f / 2147483648.0f;

		// 4.x's StagePeak: the loudest packet peak since the last log line, and how many packets it saw.
		struct StagePeak
		{
			std::atomic<std::uint32_t> bits{ 0 };
			std::atomic<std::uint32_t> buffers{ 0 };

			void Note(float peak) noexcept
			{
				buffers.fetch_add(1, std::memory_order_relaxed);
				if (!(peak > 0.0f)) return;
				const std::uint32_t current = bits.load(std::memory_order_relaxed);
				float currentPeak;
				std::memcpy(&currentPeak, &current, sizeof(currentPeak));
				if (peak <= currentPeak) return;
				std::uint32_t wanted;
				std::memcpy(&wanted, &peak, sizeof(wanted));
				bits.store(wanted, std::memory_order_relaxed);
			}

			float TakePeak()
			{
				const std::uint32_t taken = bits.exchange(0, std::memory_order_relaxed);
				float peak;
				std::memcpy(&peak, &taken, sizeof(peak));
				return peak;
			}
		};

		StagePeak stageProxyRaw;          // the real device's buffers inside the bridge driver, before RS_ASIO
		StagePeak stageCaptureRaw;        // what RS_ASIO handed the game, before the conditioner
		StagePeak stageAfterConditioner;  // what the game and detection actually hear
		std::atomic<std::uint32_t> silentFlagPackets{ 0 };   // game packets flagged silent by the capture
		std::atomic<std::uint32_t> replacedWithSilence{ 0 }; // bridge placeholder packets handed over as silence
		std::array<std::atomic<bool>, Framework::kMaxAudioInputs> packetSilent{};

		// The bridge driver's input observer (Driver/AsioProxy/AsioProxyDriver.cpp, RSModsAsio_SetInputObserver).
		struct ProxyInputChannel
		{
			void* buffer;
			long channelNum;
			long type;
		};
		using ProxyInputObserver_t = void(__cdecl*)(const ProxyInputChannel*, long, long, double);
		using SetProxyInputObserver_t = void(__cdecl*)(ProxyInputObserver_t);

		std::atomic<bool> proxyObserverInstalled{ false };
		std::atomic<int> proxyChannel{ 0 };
		std::uint64_t nextProxyAttemptTick = 0;
		bool proxyConfigRead = false;
		bool proxyWanted = false;

		std::int32_t ReadInt24(const std::uint8_t* sample)
		{
			std::int32_t value = sample[0] | (sample[1] << 8) | (sample[2] << 16);
			if (value & 0x800000) value |= ~0xFFFFFF;
			return value;
		}

		float Peak(const float* samples, std::uint32_t frames) noexcept
		{
			float peak = 0.0f;
			for (std::uint32_t frame = 0; frame < frames; ++frame) peak = std::max(peak, std::fabs(samples[frame]));
			return peak;
		}

		// Stage meter 1, on the driver's buffer switch thread. Observes only: never writes the driver's buffers.
		void __cdecl OnProxyInput(const ProxyInputChannel* channels, long channelCount, long frames, double sampleRate)
		{
			if (!channels || channelCount <= 0 || frames <= 0 || frames > MAX_PROXY_FRAMES || !std::isfinite(sampleRate)
				|| sampleRate < 1.0) return;
			const int selected = proxyChannel.load(std::memory_order_relaxed);
			for (long index = 0; index < channelCount; ++index)
			{
				if (channels[index].channelNum != selected || !channels[index].buffer) continue;
				static thread_local std::vector<float> mono;
				mono.resize(static_cast<std::size_t>(frames));
				for (long frame = 0; frame < frames; ++frame)
				{
					float value = 0.0f;
					switch (channels[index].type)
					{
					case 19: value = static_cast<const float*>(channels[index].buffer)[frame]; break;   // ASIOSTFloat32LSB
					case 18: value = static_cast<float>(static_cast<const std::int32_t*>(channels[index].buffer)[frame]) * INT32_TO_FLOAT; break;
					case 17: value = static_cast<float>(ReadInt24(static_cast<const std::uint8_t*>(channels[index].buffer) + frame * 3)) * INT24_TO_FLOAT; break;
					case 16: value = static_cast<const std::int16_t*>(channels[index].buffer)[frame] * INT16_TO_FLOAT; break;
					default: return;
					}
					mono[static_cast<std::size_t>(frame)] = value;
				}
				stageProxyRaw.Note(Peak(mono.data(), static_cast<std::uint32_t>(frames)));
				// The same samples time the game tones in ASIO mode (picks against the game's output on the bridge's clock).
				if (sampleRate == 48000.0) Audio::GameToneLatency::AsioInput(mono.data(), static_cast<std::uint32_t>(frames));
				return;
			}
		}

		void TryInstallProxyObserver()
		{
			if (proxyObserverInstalled.load(std::memory_order_relaxed)) return;
			if (!proxyConfigRead)
			{
				// Only ASIO mode has the driver in the input path; its Player 1 channel is RS_ASIO's.
				proxyConfigRead = true;
				proxyWanted = IsRsAsioEnabled();
				if (proxyWanted) proxyChannel.store(std::max(0, ReadRsAsioInputs().channels[PLAYER_ONE]), std::memory_order_relaxed);
			}
			if (!proxyWanted) return;
			const std::uint64_t now = GetTickCount64();
			if (now < nextProxyAttemptTick) return;
			nextProxyAttemptTick = now + PROXY_RETRY_MS;
			HMODULE proxy = GetModuleHandleW(L"RocksmithAudioBridgeAsio.dll");
			if (!proxy) return;
			const auto setObserver = reinterpret_cast<SetProxyInputObserver_t>(GetProcAddress(proxy, "RSModsAsio_SetInputObserver"));
			if (!setObserver) return;
			setObserver(&OnProxyInput);
			proxyObserverInstalled.store(true, std::memory_order_release);
			LOG_INFO("[InputStages] Bridge driver input observer installed (ASIO channel " << proxyChannel.load() << ")." << std::endl);
		}

		std::string Describe(StagePeak& stage)
		{
			const std::uint32_t buffers = stage.buffers.exchange(0, std::memory_order_relaxed);
			const float peak = stage.TakePeak();
			std::ostringstream text;
			if (buffers == 0) text << "no buffers";
			else if (!(peak > 0.0f)) text << "digital silence (" << buffers << " buffers)";
			else text << std::fixed << std::setprecision(1) << 20.0 * std::log10(peak) << " dBFS peak (" << buffers << " buffers)";
			return text.str();
		}

		// 4.x's PollInputStageMeter: one line every 10 s bracketing Player 1's input chain.
		void LogStages()
		{
			static std::uint64_t lastTick = 0;
			const std::uint64_t now = GetTickCount64();
			if (lastTick == 0) { lastTick = now; return; }
			if (now - lastTick < LOG_INTERVAL_MS) return;
			lastTick = now;

			const std::string proxyRaw = proxyObserverInstalled.load(std::memory_order_acquire)
				? Describe(stageProxyRaw) : std::string("n/a (no proxy observer)");
			const std::string captureRaw = Describe(stageCaptureRaw);
			const std::string afterConditioner = Describe(stageAfterConditioner);
			const std::uint32_t silentFlagged = silentFlagPackets.exchange(0, std::memory_order_relaxed);
			const std::uint32_t replaced = replacedWithSilence.exchange(0, std::memory_order_relaxed);
			const InputConditioner& conditioner = SharedConditioner();
			LOG_INFO("(INPUT STAGES) device raw (proxy): " << proxyRaw
				<< " | game capture raw: " << captureRaw
				<< " | after conditioner: " << afterConditioner
				<< " | silent-flagged packets: " << silentFlagged
				<< " | replaced with silence: " << replaced
				<< " | suppressor threshold " << std::fixed << std::setprecision(1) << conditioner.GetNoiseGateThresholdDb()
				<< " dBFS (opens 6 dB above it), gain " << conditioner.GetInputGainDb() << " dB, compressor "
				<< std::setprecision(2) << conditioner.GetCompressorStrength() << std::endl);
		}
	}

	void Observer::Prepare(std::uint32_t input, const Framework::CaptureFormat& format)
	{
		if (input >= sampleRates.size()) return;
		sampleRates[input].store(format.sampleRate, std::memory_order_release);
	}

	// Runs whether or not the owner is Active: it only reads. Silent packets arrive as zeros (CaptureTap), and count
	// as silent the way 4.x's flag check did.
	bool Observer::Process(std::uint32_t input, float* const* channels, std::uint32_t channelCount, std::uint32_t frames,
		bool) noexcept
	{
		if (input >= sampleRates.size() || channelCount == 0 || frames == 0) return false;
		const bool silent = packetSilent[input].load(std::memory_order_relaxed);
		const float peak = silent ? 0.0f : Peak(channels[0], frames);
		if (point == Point::CaptureRaw)
		{
			if (input == PLAYER_ONE && !silent) stageCaptureRaw.Note(peak);
			return false;
		}

		if (input == PLAYER_ONE)
		{
			Audio::CableInput::ReportTapPacket(peak, silent, frames, sampleRates[input].load(std::memory_order_relaxed));
			stageAfterConditioner.Note(peak);
		}
		else if (input == PLAYER_TWO)
		{
			// Player 2's second ASIO input; the P2 cable wrapper reports from its own capture (CableCaptureSession).
			Audio::CableInput::ReportPlayerTwoPacket(peak, silent);
		}
		return false;
	}

	std::uint32_t Observer::GetLatencyFrames(std::uint32_t) const
	{
		return 0;
	}

	Observer& CaptureRawObserver()
	{
		static Observer observer(Point::CaptureRaw);
		return observer;
	}

	Observer& AfterConditionerObserver()
	{
		static Observer observer(Point::AfterConditioner);
		return observer;
	}

	void BeginPacket(std::uint32_t input, bool silentFlagged) noexcept
	{
		if (input >= packetSilent.size()) return;
		packetSilent[input].store(silentFlagged, std::memory_order_relaxed);
		if (silentFlagged && input == PLAYER_ONE) silentFlagPackets.fetch_add(1, std::memory_order_relaxed);
	}

	void NoteReplacedWithSilence(std::uint32_t input) noexcept
	{
		if (input == PLAYER_ONE) replacedWithSilence.fetch_add(1, std::memory_order_relaxed);
	}

	void Tick()
	{
		TryInstallProxyObserver();
		LogStages();
	}
}
