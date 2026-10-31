#pragma once

#include <windows.h>
#include <audioclient.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// A Windows output device in exclusive mode, as the bridge's playback device in Cable mode.
//
// Why: in shared mode Windows mixes every app's sound and hands it to the device in its own period, 10 ms on most
// headphone jacks and USB devices, and the bridge has to keep up to a period queued in front of that. Measured on a
// headphone jack with the Tone designer: about 28 ms guitar to ear, two thirds of it Windows. In exclusive mode the
// bridge talks to the device's driver directly at the device's smallest period (often 3 ms), as an ASIO driver does.
// The price: while Rocksmith runs, no other program can play on that device (Windows says it is in use).
//
// Same shape as AsioOutputDevice, so the output backend drives both the same way: Open negotiates a format and period,
// this object's own thread waits for the device's event and calls `fill` once per period for interleaved stereo float
// at 48 kHz, and converts to the device's format (float, 32-bit, 24-bit or 16-bit integer).
//
// Endpoint ids: "excl:<Windows endpoint id>" on the Output page and in RSMods.ini [Audio Bridge] OutputDevice, next to
// plain Windows ids (shared mode) and "asio:<driver>".
namespace Audio::SharedOutput
{
	class ExclusiveOutputDevice
	{
	public:
		using Fill = std::function<void(float* stereo, uint32_t frames)>;
		using Activate = std::function<HRESULT(std::wstring& endpointId, IAudioClient3** client)>;

		static bool IsExclusiveEndpoint(const std::wstring& endpointId);
		static std::wstring WindowsId(const std::wstring& endpointId);    // "excl:<id>" -> "<id>"
		static std::wstring EndpointOf(const std::wstring& windowsId);    // "<id>" -> "excl:<id>"

		~ExclusiveOutputDevice();

		// Activates the device, finds a 48 kHz stereo format it takes in exclusive mode and initialises it at its smallest
		// period (or `desiredFrames` when that is larger). `failure` says why not, in words for the overlay. Called on the
		// output backend's thread (COM initialised there).
		// polling = false: event driven, the driver wakes us once per period (the lowest delay). Some laptop drivers stop
		// waking after a second in exclusive mode (some Realtek speakers: a reopen loop heard as a high, mangled
		// tone), so polling = true instead keeps three periods in the device and tops it up a period at a time on a 1 ms
		// timer, needing nothing from the driver but its fill level (one period more delay).
		bool Open(const std::wstring& windowsId, const Activate& activate, uint32_t desiredFrames, bool polling, Fill fill, std::string& failure);
		bool Start();
		void Stop();
		void Close();

		uint32_t BufferFrames() const { return m_chunkFrames; }   // what each fill call asks for
		uint32_t DeviceBufferFrames() const { return m_bufferFrames; }
		bool Polling() const { return m_polling; }
		uint32_t OutputLatencyFrames() const { return m_latencyFrames; }
		ULONGLONG LastCallbackTick() const { return m_lastCallback.load(std::memory_order_acquire); }
		uint64_t Callbacks() const { return m_callbacks.load(std::memory_order_acquire); }
		// The device went away or failed mid-stream (unplugged, another program took it): reopen it.
		bool TakeResetRequest() { return m_resetRequested.exchange(false); }
		const std::wstring& Name() const { return m_id; }
		const char* FormatName() const { return m_formatName; }

	private:
		void Run();
		bool Render(uint32_t frames) noexcept;   // fill, convert, hand to the device

		enum class Sample { Float32, Int32, Int24, Int16 };

		std::wstring m_id;
		Fill m_fill;
		IAudioClient3* m_client = nullptr;
		IAudioRenderClient* m_render = nullptr;
		HANDLE m_event = nullptr;
		HANDLE m_quit = nullptr;
		std::thread m_thread;
		uint32_t m_bufferFrames = 0;   // the device buffer
		uint32_t m_chunkFrames = 0;    // one period: the size of every fill
		bool m_polling = false;
		uint32_t m_latencyFrames = 0;
		Sample m_sample = Sample::Float32;
		const char* m_formatName = "";
		std::vector<float> m_stereo;
		std::atomic<bool> m_running{ false };
		std::atomic<ULONGLONG> m_lastCallback{ 0 };
		std::atomic<uint64_t> m_callbacks{ 0 };
		std::atomic<bool> m_resetRequested{ false };
	};
}
