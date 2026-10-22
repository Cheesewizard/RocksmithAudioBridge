#pragma once

#include "Shared/CaptureFormat.h"
#include "Shared/IInputProcessor.hpp"
#include "NoiseSuppressor.hpp"

#include <cstddef>
#include <cstdint>

namespace Audio::AsioHook
{
	constexpr size_t INPUT_ROUTE_COUNT = 2;

	// Detail::NoiseSuppressor lives in NoiseSuppressor.hpp (header-only), shared with the amp sim's feed.

	// Validates and chains RS_ASIO's existing PortAudio unmarshal patch. This observes the
	// IAudioCaptureClient RS_ASIO already created instead of creating another ASIO host.
	void Install();

	// Called from the game loop so buffers and processor state are prepared off the audio thread.
	void Poll();

	// Ownership stays with the caller. Active endpoints are assigned in RS_ASIO.ini order.
	void SetProcessor(size_t routeIndex, IInputProcessor* inputProcessor);

	// A source stage that runs BEFORE the route's processor, overwriting the captured input
	// when armed. Lets a synthetic input feed the real processor chain (e.g. the Drop
	// Pedal shifter). Ownership stays with the caller; a disarmed source is inert.
	void SetInputSource(size_t routeIndex, IInputProcessor* inputSource);
	void SetInputSourceActive(size_t routeIndex, bool active);

	// Make-up gain (dB) applied to the real guitar input before the game's amp and note gate.
	// Compensates for RS_ASIO/interface inputs arriving quieter than a hot Real Tone Cable, which
	// otherwise makes the game's level-sensitive gate mute sustains and bends early. 0 dB = off.
	void SetInputGainDb(float decibels);

	// Noise gate threshold in dBFS on the gate's filtered detector (NoiseSuppressor.hpp), keyed to the raw
	// pre-gain input. >= 0 dB = off.
	void SetNoiseGateThresholdDb(float decibels);
	float GetNoiseGateThresholdDb();
	// The Noise gate pedal's other dials: decay (ms), reduction (dB) and attack (ms). Clamped to the pedal's ranges.
	void SetNoiseGateShape(float decayMs, float reductionDb, float attackMs);
	Detail::NoiseGateShape GetNoiseGateShape();
	// For the pedal's meter and light: the detector level (dBFS) and open state of Player 1's gate. False when the gate
	// has not run in the last 500 ms (off, or no input).
	bool GetNoiseGateMeter(float& detectorDb, bool& open);

	// Logs "(INPUT STAGES)" every 10 s: Player 1's peak at the device (proxy), at the game capture
	// before the conditioner, and after it, so a dead input stretch shows which stage went silent.
	void PollInputStageMeter();

	// Input compressor strength (0..1, 0 = off). Flattens the natural string-beat wobble before the game
	// amp so a quiet interface input doesn't warble the way a hot cable's compressed signal doesn't.
	void SetCompressorStrength(float strength);
	float GetCompressorStrength();

	// True while Player 1's input chain (the capture hook) has handled a packet in the last 2 s. The hook is only
	// installed at launch when gain, noise suppressor or compressor is on (ModManager), so switching the
	// first one on mid-session does nothing until a restart; the overlay's Input page says so.
	bool InputChainRunning();

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
