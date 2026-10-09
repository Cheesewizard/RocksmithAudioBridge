#pragma once

#include <string>

// Modern WASAPI capture for the Real Tone Cable (and any other PortAudio WASAPI input).
//
// Rocksmith drives its guitar input through a 2012-era PortAudio whose WASAPI backend only
// knows two shapes: legacy shared mode (polled, ~22 ms) and exclusive mode. The Real Tone
// Cable's driver has NO exclusive input format, so ExclusiveMode=1 fails the open with
// -9996 ("cable not detected") and ExclusiveMode=0 leaves a 22 ms polled stream. Both are
// decided by ini state the player can't reliably edit.
//
// This module removes the ini from the equation. It wraps the IAudioClient PortAudio
// activates for a capture endpoint in a mod-owned client that always opens a modern
// shared-mode, event-driven stream (IAudioClient3 low-latency period when Windows 10+
// offers it, legacy shared with format auto-conversion otherwise) while presenting to
// PortAudio exactly the exclusive, event-driven stream it asked for. PortAudio's own
// stream layout, the GetBuffer tap in AsioHook, the Drop Pedal shifter and Note by Note
// all keep working unchanged: they see the same PaWasapiStream and the same
// IAudioCaptureClient interface, just fed by a better engine.
//
// Output (the Wwise sink) is never touched.
namespace Audio::CableInput
{
	// Detours Pa_OpenStream and prepares the IMMDevice::Activate wrapper. Call once at
	// startup, before the game opens its input stream. Safe to call with RS_ASIO present
	// (it then does nothing: RS_ASIO owns the input).
	void Install();

	// Game-loop liveness reporting: logs whether the wrapped input is delivering audio,
	// and when it stalls or resumes. Never runs on the audio thread.
	void Poll();

	// One-line human summary of the active input path for overlays and diagnostics.
	std::string DescribeStatus();

	// Snapshot for the in-game audio overlay (latency, signal, health). Cheap to copy; safe
	// from the render thread.
	struct Diagnostics
	{
		bool installed = false;        // the cable client is in place (RS_ASIO absent, detour ok)
		bool rsAsio = false;           // RS_ASIO owns the input; nothing wrapped
		bool streamActive = false;     // an input stream has been started
		std::string inputPath;         // "modern raw", "modern", "engine convert", "legacy shared"
		std::string inputFormat;       // what the game receives
		double inputLatencyMs = 0.0;   // one served chunk (the period) in ms; 0 = unknown
		double inputLatencyGameMs = 0.0; // PortAudio's own figure from audiodump.txt; 0 = unknown
		double outputLatencyMs = 0.0;  // the game's figure from audiodump.txt; 0 = unknown
		bool outputExclusive = false;
		bool outputKnown = false;
		double packetsPerSecond = 0.0;
		bool stalled = false;
		float meterPeak = 0.0f;        // decaying linear peak (0..1) for the signal bar
		std::string deviceFormat;      // the Windows format the cable came up with ("16 kHz")
		uint64_t dropouts = 0;         // packets discarded because the game's audio thread fell behind
		double captureTimestampLagMs = 0.0;  // QPC timestamp to game delivery; not physical input latency.
		double capturePacketMs = 0.0;
		bool captureTimestampValid = false;    // at least one measurement has been taken this stream
		// ASIO path (RS_ASIO present). The AsioHook tap on RS_ASIO's capture client is the
		// only observer, so the signal meter, packet rate and stall state above come from it.
		uint32_t tapPacketFrames = 0;  // frames per packet the tap sees (the ASIO buffer size)
		uint32_t tapSampleRate = 0;
		int proxyInputMode = 0;        // 0 = physical ASIO, 1 = waiting for RTC, 2 = RTC fallback live

		// Player 2 input (the second SIGNAL row of the overlay, drawn in 2-player only). Fed by
		// whichever path carries Player 2: the Real Tone Cable wrapper beside RS_ASIO
		// (PersistentCapture, gated by "Use the Real Tone Cable for Player 2") or the second ASIO
		// input route (AsioHook route 1). Same semantics as the Player 1 fields above.
		bool playerTwoInput = false;       // some Player 2 input path exists at all
		bool playerTwoCableFeedOff = false; // the P2 cable wrapper exists but its toggle is off (silence)
		bool playerTwoStreamActive = false; // at least one packet has been delivered to the game
		bool playerTwoStalled = false;
		double playerTwoPacketsPerSecond = 0.0;
		float playerTwoMeterPeak = 0.0f;   // decaying linear peak (0..1)
	};
	Diagnostics GetDiagnostics();

	// Per-packet signal feed from the shared Player-1 input tap for every capture path.
	// The capture client does not also feed this meter. Audio thread; never blocks.
	void ReportTapPacket(float peak, bool silent, uint32_t frames, uint32_t sampleRate);

	// Per-packet signal feed for Player 2's input, from every path that can carry it (the P2
	// cable wrapper's GetBuffer and AsioHook route 1). Audio thread; lock-free; never blocks.
	void ReportPlayerTwoPacket(float peak, bool silent);

	// QPC timestamp to game delivery in ms. Device timestamp semantics vary, so this
	// measures timestamp lag only, without a packet midpoint correction. Negative or
	// non-finite values invalidate the measurement; they never enter the average.
	void ReportCaptureTimestampLag(double lagMs);
	// Raw inputs of the tap's measurement (clock delta in 100 ns units, packet frames), for
	// the evidence line in Poll.
	void ReportMeasuredInputRaw(int64_t delta100ns, uint32_t frames);
	// True when RS_ASIO owns the input; its timestamps must not be interpreted as WASAPI timestamps.
	bool IsAsioPath();

	// RSMods.ini [Mod Settings] AudioDiagnosticsOverlay (default on).
	bool IsOverlayEnabled();
	void SetOverlayEnabled(bool enabled);
}
