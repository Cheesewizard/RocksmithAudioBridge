#pragma once

#include "CaptureFormat.h"
#include "IInputProcessor.hpp"

#include <cstddef>
#include <cstdint>

namespace Audio::AsioHook
{
	constexpr size_t INPUT_ROUTE_COUNT = 2;

	namespace Detail
	{
		class NoiseSuppressor final
		{
		public:
			void Configure(uint32_t sampleRate, float threshold);
			float NextGain(float sidechain, float threshold);
			void Process(float* samples, size_t sampleCount, uint32_t sampleRate, float threshold);
			void Reset();

		private:
			static constexpr float EXPANDER_RATIO = 6.0f;
			static constexpr float EXPANDER_RANGE_DB = -80.0f;
			static constexpr float DETECTOR_ATTACK_SECONDS = 0.002f;
			static constexpr float DETECTOR_RELEASE_SECONDS = 0.080f;
			static constexpr float GAIN_ATTACK_SECONDS = 0.002f;
			static constexpr float GAIN_RELEASE_SECONDS = 0.180f;
			static constexpr float OPEN_CONFIRM_SECONDS = 0.003f;
			static constexpr float CLOSE_HYSTERESIS = 0.65f;
			// Opening level, relative to the configured threshold. An absolute level would hard-mute every
			// note between the threshold and that level, leaving the input dead after a pause until one
			// hit is loud enough. The 3 ms confirm still rejects spikes.
			static constexpr float ATTACK_ABOVE_THRESHOLD_DB = 6.0f;

			float energy = 0.0f;
			float appliedGain = 0.0f;
			float detectorAttackCoef = 0.0f;
			float detectorReleaseCoef = 0.0f;
			float gainAttackCoef = 0.0f;
			float gainReleaseCoef = 0.0f;
			float rangeGain = 0.0001f;
			float configuredThreshold = -1.0f;
			float attackThreshold = 0.0316228f;
			uint32_t openConfirmSamples = 1;
			uint32_t openConfirmCount = 0;
			uint32_t configuredRate = 0;
			bool isOpen = false;
		};
	}

	// Validates and chains RS_ASIO's existing PortAudio unmarshal patch. This observes the
	// IAudioCaptureClient RS_ASIO already created instead of creating another ASIO host.
	void Install();

	// Called from the game loop so buffers and processor state are prepared off the audio thread.
	void Poll();

	// Ownership stays with the caller. Active endpoints are assigned in RS_ASIO.ini order.
	void SetProcessor(size_t routeIndex, IInputProcessor* inputProcessor);


	// Make-up gain (dB) applied to the real guitar input before the game's amp and note gate.
	// Compensates for RS_ASIO/interface inputs arriving quieter than a hot Real Tone Cable, which
	// otherwise makes the game's level-sensitive gate mute sustains and bends early. 0 dB = off.
	void SetInputGainDb(float decibels);

	// Adaptive suppressor threshold in dBFS, keyed to the raw pre-gain input. It requires a sustained
	// onset, rejects idle noise and brief spikes, and closes gradually after notes. >= 0 dB = off.
	void SetNoiseGateThresholdDb(float decibels);
	float GetNoiseGateThresholdDb();

	// Logs "(INPUT STAGES)" every 10 s: Player 1's peak at the device (proxy), at the game capture
	// before the conditioner, and after it, so a dead input stretch shows which stage went silent.
	void PollInputStageMeter();

	// Input compressor strength (0..1, 0 = off). Flattens the natural string-beat wobble before the game
	// amp so a quiet interface input doesn't warble the way a hot cable's compressed signal doesn't.
	void SetCompressorStrength(float strength);
	float GetCompressorStrength();

	// Mains-hum notch base frequency in Hz (0 = off, else 50 or 60). Notches out the 50/60 Hz ground-loop
	// hum comb a grounded interface injects and a single-USB Real Tone Cable does not; front of chain, so
	// it cleans the raw input before the gate/gain and the game see it.
	void SetHumFilterBaseHz(float baseHz);
	float GetHumFilterBaseHz();

	// Round-trip latency measurement (paired with the proxy's probe injection): arm a one-shot capture of
	// `frames` route-0 input samples (the looped-back probe), then the host correlates them against the probe.
	void StartLatencyCapture(int frames);
	bool IsLatencyCaptureDone();
	int GetLatencyCapture(const float** out);   // returns frames captured so far; *out = buffer
	float GetInputGainDb();

	void SetProcessingEnabled(bool enabled);
	bool IsProcessingEnabled();
	bool IsInputConfigured(size_t routeIndex);
	bool IsInputReady(size_t routeIndex);
}
