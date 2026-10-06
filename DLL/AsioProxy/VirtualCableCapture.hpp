#pragma once

#include <Windows.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace AsioProxy
{
	// Rocksmith cable input for the proxy's Virtual mode (no real ASIO device bound).
	//
	// A capture thread drains the cable's WASAPI shared stream (10 ms packets on the cable's own clock)
	// into a lock-free single-producer / single-consumer ring. The proxy's virtual clock reads it in
	// host-sized blocks (Read, audio thread). The two clocks never match exactly, so the reader keeps a
	// small safety margin buffered and trims the drift with a gentle fractional resampler (a few hundred
	// ppm at most, far below audible pitch change).
	//
	// Deliveries are not always one packet per 10 ms: a late capture wake drains two or three packets back
	// to back (that is what the 30 ms WASAPI buffer is for), so the fill can dip by more than one packet
	// below its level just after a delivery. The writer measures this delivery jitter span (see PushMono)
	// and the reader primes to it; for on-time 10 ms packets it equals one packet.
	class VirtualCableCapture final
	{
	public:
		using LogFn = void(*)(const char* format, ...);

		struct Stats
		{
			uint32_t underruns = 0;        // reader ran dry after priming: one silent block each
			uint32_t overflowDrops = 0;    // writer found the ring full and dropped a packet
			uint32_t skips = 0;            // reader jumped forward over a backlog
			uint32_t deviceGlitches = 0;   // WASAPI reported a data discontinuity (capture thread late)
			int32_t fillFrames = 0;        // frames buffered at the last read
			int32_t correctionPpm = 0;     // drift correction currently applied
			int32_t primed = 0;
			int32_t marginFrames = 0;      // current safety margin (grows after an underrun)
		};

		VirtualCableCapture();
		~VirtualCableCapture();
		VirtualCableCapture(const VirtualCableCapture&) = delete;
		VirtualCableCapture& operator=(const VirtualCableCapture&) = delete;

		bool Start();
		void Stop();
		void SetTestMode(bool enabled) { testMode.store(enabled, std::memory_order_release); }
		void SetLog(LogFn log) { logFn = log; }
		// Audio thread: produce `frames` interleaved stereo int32 samples (mono cable duplicated).
		void Read(int32_t* stereoSamples, uint32_t frames);
		void InjectForTest(const int32_t* stereoSamples, uint32_t frames);
		bool HasPhysicalCapture() const { return physicalCapture.load(std::memory_order_acquire); }
		Stats GetStats() const;

	private:
		static constexpr uint32_t RING_FRAMES = 16384;           // power of two
		static constexpr uint32_t RING_MASK = RING_FRAMES - 1;
		static constexpr uint32_t MAX_PACKET_FRAMES = 4096;
		static constexpr int32_t MARGIN_FRAMES = 128;            // starting margin over one block at the fill minimum (2.7 ms)
		static constexpr int32_t MARGIN_STEP_FRAMES = 128;       // added after each underrun
		static constexpr int32_t MARGIN_MAX_FRAMES = 2400;       // 50 ms ceiling
		static constexpr int32_t FADE_FRAMES = 64;
		static constexpr uint32_t SERVO_WINDOW_FRAMES = 12000;   // 250 ms of output per servo update
		static constexpr uint32_t SPAN_WINDOW_DELIVERIES = 100;  // ~1 s of 10 ms deliveries per span window
		static constexpr int64_t STALL_FRAMES = 1440;            // 30 ms = the WASAPI buffer: a longer gap is a stall, not cadence
		static constexpr int64_t MAX_SPAN_FRAMES = 2 * STALL_FRAMES;

		void Run();
		bool OpenDevice();
		void CloseDevice();
		bool DrainDevice();
		void PushMono(const float* samples, uint32_t frames);
		void ResetReader();
		void LogStatsIfChanged();
		float At(uint64_t index) const { return ring[static_cast<uint32_t>(index) & RING_MASK]; }

		HANDLE quitEvent = nullptr;
		HANDLE wakeEvent = nullptr;
		HANDLE captureEvent = nullptr;
		std::thread worker;
		std::atomic<bool> running{ false };
		std::atomic<bool> physicalCapture{ false };
		std::atomic<bool> testMode{ false };
		LogFn logFn = nullptr;

		// Ring: the writer owns writeFrame, the reader owns readFrame. No lock.
		std::atomic<uint64_t> readFrame{ 0 };
		std::atomic<uint64_t> writeFrame{ 0 };
		// Delivery jitter span in frames (see PushMono): how far the fill can fall below its level just after
		// a delivery. The reader's start fill is one block + margin + this.
		std::atomic<uint32_t> largestSpan{ 0 };
		int64_t lastDeliveryTicks = 0;    // writer only: QPC of the previous delivery, 0 = none since Start
		int64_t deliveryBaseTicks = 0;    // writer only: start of the steady 48 kHz reference line
		int64_t deliveredSinceBase = 0;   // writer only: frames delivered since deliveryBaseTicks
		int64_t spanPeak = 0;             // writer only: highest (delivered - reference) this window, just after a delivery
		uint32_t spanWindowMax = 0;       // writer only: this window's span
		uint32_t spanPreviousMax = 0;     // writer only: the last complete window's span
		uint32_t spanWindowCount = 0;     // writer only
		int64_t qpcFrequency = 1;
		std::atomic<bool> resetRequested{ true };
		std::array<float, RING_FRAMES> ring{};
		std::array<float, MAX_PACKET_FRAMES> packet{};

		// Reader state (audio thread only).
		uint64_t readIndex = 0;       // integer part of the read position
		double readPhase = 0.0;       // fractional part, [0, 1)
		bool primed = false;
		int32_t fadeIn = 0;
		int32_t margin = MARGIN_FRAMES;   // adaptive: a writer that is late more than the margin grows it
		int32_t windowMinFill = INT32_MAX;
		uint32_t windowFrames = 0;
		double integralPpm = 0.0;
		double correctionPpm = 0.0;

		// Published stats.
		std::atomic<uint32_t> underruns{ 0 };
		std::atomic<uint32_t> overflowDrops{ 0 };
		std::atomic<uint32_t> skips{ 0 };
		std::atomic<uint32_t> deviceGlitches{ 0 };
		std::atomic<int32_t> lastFill{ 0 };
		std::atomic<int32_t> lastPpm{ 0 };
		std::atomic<int32_t> primedFlag{ 0 };
		std::atomic<int32_t> lastMargin{ MARGIN_FRAMES };
		Stats lastLogged{};
		uint64_t nextLogTick = 0;

		Microsoft::WRL::ComPtr<IAudioClient> backend;
		Microsoft::WRL::ComPtr<IAudioCaptureClient> capture;
		std::wstring activeEndpoint;
		uint64_t nextOpenTick = 0;
	};
}
