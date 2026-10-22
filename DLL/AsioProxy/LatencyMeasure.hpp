#pragma once

#include <cstdint>

// Measured latency in ASIO mode, sample-exact on the driver's own callback clock.
//
// Why: the readout used to add the driver's reported input and output latency to a guessed figure for the game's own
// processing. Neither part was measured, so the total could not be trusted, nor compared with Cable mode's (which
// times only the software part).
//
// Guitar to ear in ASIO mode is three parts: the interface's input (converter, USB, driver buffers), what happens
// between the input buffer and the output buffer (the game, or the amp link's Tone designer / amp sim), and the
// interface's output. Two measurements cover them, each laying three chirps on the proxy's buffer timeline, so the
// answer is counted in samples with no clock stamps to jitter:
//
//   Game (no cable, about a second of silence): the chirps replace every input the game reads, before the amp link
//   publishes its DI, so the game's amp and the amp link both get them. The game's output (straight after the host's
//   callback) and the amp link's return (what MixAmpReturn added) are recorded; the lag of each is that path's
//   processing, input buffer to output buffer. Nothing reaches the speakers meanwhile.
//
//   Hardware (a patch cable from the interface's output into the input the guitar uses): the chirps go out of every
//   output and are looked for on every input the game opened. The lag is the interface's real input plus output
//   latency, what getLatencies only reports. The game hears silence meanwhile (no feedback through its amp), and the
//   output stays muted afterwards (HoldMuted) until the player has unplugged the cable and says so: a guitar input
//   patched to the output would otherwise feed back through the game's amp.
//
// Guitar to ear = hardware round trip (measured, else the driver's figures) + the path's processing.
// The DLL side (Audio::LatencyCalibration) decides when to run, stores the results per setup and feeds the readout.
namespace AsioProxy::LatencyMeasure
{
	enum class Kind : int32_t { Game = 1, Hardware = 2 };
	enum class State : int32_t { Idle = 0, WaitingQuiet = 1, Running = 2, Analysing = 3, HoldMuted = 4 };
	enum Outcome : int32_t { None = 0, Measured = 1, NotQuiet = -1, NoAnswer = -2, Disagreed = -3, Stopped = -4, Failed = -5,
		NoSignal = -6 };   // Hardware: the inputs stayed at the noise floor (the output too low, or the cable elsewhere)

	// Exported as-is (RSModsAsio_GetLatencyMeasure): plain data, the same layout in the proxy and the game DLL.
	struct Result
	{
		int32_t state;          // State
		int32_t kind;           // the last measurement's Kind (0 = none yet)
		int32_t outcome;        // Outcome of the last measurement
		int32_t sequence;       // bumps each time a measurement ends
		int32_t sampleRate;     // the stream now (0 = no stream)
		int32_t bufferSize;
		int32_t host64;         // the driver runs in the 64-bit driver host
		float gameMs;           // Game: the game's amp, input buffer to output buffer; -1 = no answer
		float ampMs;            // Game: the amp link's return (Tone designer / amp sim); -1 = no answer
		float roundTripMs;      // Hardware: output buffer back to input buffer through the cable; -1 = none
		int32_t inputChannel;   // Hardware: the host input (0-based among the game's inputs) that heard it
		wchar_t driver[64];     // the bound driver's name, for keying stored results
	};

	// The proxy's log (ProxyLog), for the per-chirp detail.
	void SetLogger(void (*log)(const char* format, ...));
	// Control thread (the proxy's bind): the stream the next measurement runs on. Frames 0 = no stream.
	void SetStream(const wchar_t* driver, long bufferSize, double sampleRate, bool host64);
	// Control thread (export): false when a measurement is running, or there is no stream.
	bool Start(Kind kind);
	// Ends HoldMuted: the cable is unplugged.
	void Release();
	Result Get();

	// Audio thread, once per callback, in this order. All are no-ops while Idle (one relaxed load).
	bool Active() noexcept;
	// `channels`: each of the host's input buffers as floats, `frames` long. Returns true when they were changed
	// (chirps injected, or silenced) and must be written back.
	bool Inputs(float* const* channels, int count, long frames) noexcept;
	void GameOutput(const float* mono, long frames) noexcept;    // straight after the host's callback
	void HeardOutput(const float* mono, long frames) noexcept;   // after the amp link's return is mixed in
	// Last, before the buffers go to the driver: true when every output must be replaced by `replacement`
	// (silence, or the chirps), `frames` long. Advances the timeline.
	bool FinalOutput(float* replacement, long frames) noexcept;
}
