#pragma once

#include "Framework/AudioInput.hpp"

#include "InputBandLimit.hpp"
#include "InputCompressor.hpp"
#include "../Shared/NoiseSuppressor.hpp"

#include <atomic>
#include <cstdint>

namespace AudioBridge::Input
{
	// What the noise gate pedal shows: the detector level (dBFS) and whether the gate is open.
	struct NoiseGateMeter
	{
		float detectorDb = -120.0f;
		bool open = false;
	};

	// The front of Player 1's guitar input, before the game's amp and pitch detection: noise gate (keyed to the raw
	// level) -> compressor -> make-up gain with soft clip -> band-limit. Each stage is skipped while its control is
	// off, so with all three off the input is untouched.
	// Only input 0 (Player 1) is conditioned, as in 4.x; the settings are Player 1's. Setters are for MainThread, and
	// everything they change reaches the audio thread through atomics.
	class InputConditioner final : public Framework::IInputProcessor
	{
	public:
		void Prepare(std::uint32_t input, const Framework::CaptureFormat& format) override;
		bool Process(std::uint32_t input, float* const* channels, std::uint32_t channelCount, std::uint32_t frames,
			bool active) noexcept override;
		std::uint32_t GetLatencyFrames(std::uint32_t input) const override;

		// -24..+24 dB, 0 = off.
		void SetInputGainDb(float decibels);
		// Open threshold in dBFS on the gate's filtered detector, -90..-20; >= 0 switches the gate off.
		// resetGate starts the detector fresh (switching on); a new threshold while it runs keeps its state, so a
		// ringing note is not cut.
		void SetNoiseGateThresholdDb(float decibels, bool resetGate);
		void SetNoiseGateShape(const Audio::AsioHook::Detail::NoiseGateShape& shape);
		// 0..1, 0 = off.
		void SetCompressorStrength(float strength);

		// False when the gate hasn't run in the last 500 ms (off, or no input).
		bool TryGetNoiseGateMeter(NoiseGateMeter& meter) const;

		float GetInputGainDb() const;
		// 0 = off.
		float GetNoiseGateThresholdDb() const;
		Audio::AsioHook::Detail::NoiseGateShape GetNoiseGateShape() const;
		float GetCompressorStrength() const;

	private:
		static constexpr std::uint32_t CONDITIONED_INPUT = 0;

		void RunNoiseGate(float* const* channels, std::uint32_t channelCount, std::uint32_t frames, float threshold);
		void RunCompressor(float* const* channels, std::uint32_t channelCount, std::uint32_t frames, float strength);
		void RunGain(float* const* channels, std::uint32_t channelCount, std::uint32_t frames, float gain);

		std::atomic<float> inputGainLinear{ 1.0f };
		std::atomic<float> gateThresholdLinear{ 0.0f };   // 0 = off
		std::atomic<std::uint32_t> gateThresholdRevision{ 0 };
		std::atomic<float> gateDecayMs{ 200.0f };
		std::atomic<float> gateReductionDb{ -60.0f };
		std::atomic<float> gateAttackMs{ 0.5f };
		std::atomic<float> compressorStrength{ 0.0f };

		std::atomic<float> meterDetectorDb{ -120.0f };
		std::atomic<bool> meterOpen{ false };
		std::atomic<std::int64_t> meterUpdatedMs{ 0 };

		// Input 0's audio thread, and Prepare (which the chain never runs alongside that thread's Process).
		std::uint32_t sampleRate = 0;
		std::uint32_t processedGateRevision = 0;
		Audio::AsioHook::Detail::NoiseSuppressor noiseGate;
		InputCompressor compressor;
		InputBandLimit bandLimit;
	};

	// The one conditioner on Player 1's input: InputConditionerMod puts it in the chain and applies its settings;
	// the per-tone gates (ToneGates), the control pipe and External amp's feed read and set it.
	InputConditioner& SharedConditioner();
}
