#pragma once

#include "../../Shared/AsioInterface.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

// An ASIO interface as the bridge's playback device in Cable mode (output only).
//
// Why: in Cable mode the game's sound (and the Tone designer's return) plays through a Windows device, and Windows'
// shared audio adds its own period (10 ms on most USB interfaces) plus the USB audio driver's buffering. A player
// whose interface has an ASIO driver (an M-Track, a Focusrite) can play through that driver instead, at the buffer
// size set in its own control panel, while the guitar stays on the Real Tone Cable. The game stays in Cable mode;
// RS_ASIO is not involved.
//
// The output backend (OutputBackend.hpp) drives it: Open loads the driver and creates buffers for two outputs at
// 48 kHz, the driver's own thread calls `fill` once per buffer for interleaved stereo float, and this converts to the
// driver's sample format. Rules learnt from the bridge's ASIO driver: the driver DLL is pinned and never unloaded while
// the game runs (a driver thread that outlived FreeLibrary crashed the game), and a driver stuck in its callback when
// closed is left allocated (quarantined) rather than freed under it. One ASIO output at a time.
//
// Endpoint ids: the Output page and RSMods.ini [Audio Bridge] OutputDevice name it "asio:<driver name>", next to
// Windows endpoint ids.
namespace Audio::SharedOutput
{
	// Never boot twice into an ASIO output that took the game down (some drivers crash the process, nothing inside it
	// can catch that; the bridge's ASIO driver keeps the same kind of record). The driver's name is written before it
	// opens and cleared once it refuses cleanly, plays for 10 s, or closes after playing. Found still set at the next
	// launch, the same driver is not reopened automatically; picking it on the Output page clears it and tries again.
	// HKCU\Software\RSMods\AsioProxy\CableAsioOutputPending.
	namespace AsioCrashGuard
	{
		void Mark(const std::wstring& driverName);
		void Clear();
		bool CrashedLastTime(const std::wstring& driverName);   // true only on the first open of this launch
	}

	class AsioOutputDevice
	{
	public:
		using Fill = std::function<void(float* stereo, uint32_t frames)>;

		static bool IsAsioEndpoint(const std::wstring& endpointId);
		static std::wstring DriverName(const std::wstring& endpointId);
		static std::wstring EndpointOf(const std::wstring& driverName);
		static bool IsInstalled(const std::wstring& driverName);

		~AsioOutputDevice();

		// Loads, initialises and prepares the driver (buffers created, not started). `failure` says why not, in words
		// for the overlay. Called on the output backend's thread.
		bool Open(const std::wstring& driverName, Fill fill, std::string& failure);
		bool Start();
		void Stop();
		// Stops (if needed), disposes the buffers and releases the driver. False: the driver did not leave its callback
		// in time, so this object must be abandoned, never deleted (its callback may still be running in it).
		bool Close();

		uint32_t BufferFrames() const { return static_cast<uint32_t>(m_bufferSize); }
		uint32_t OutputLatencyFrames() const { return static_cast<uint32_t>(m_outputLatency > 0 ? m_outputLatency : 0); }
		ULONGLONG LastCallbackTick() const { return m_lastCallback.load(std::memory_order_acquire); }
		uint64_t Callbacks() const { return m_callbacks.load(std::memory_order_acquire); }
		// The driver asked to be reset (its control panel changed the buffer size, a device was switched): reopen it.
		bool TakeResetRequest() { return m_resetRequested.exchange(false); }
		const std::wstring& Name() const { return m_name; }

	private:
		void Process(long index) noexcept;
		static void Convert(const float* stereo, long channel, void* destination, long type, long frames) noexcept;
		static void OnBufferSwitch(long index, ASIOBool direct);
		static ASIOTime* OnBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct);
		static void OnSampleRateDidChange(ASIOSampleRate rate);
		static long OnAsioMessage(long selector, long value, void* message, double* opt);

		static std::atomic<AsioOutputDevice*> s_active;
		static ASIOCallbacks s_callbacks;

		std::wstring m_name;
		Fill m_fill;
		HMODULE m_module = nullptr;
		IAsioDriver* m_driver = nullptr;
		bool m_buffersCreated = false;
		bool m_started = false;
		long m_bufferSize = 0;
		long m_outputLatency = 0;               // getLatencies after createBuffers (frames)
		long m_outputCount = 0;                // 1 (mono interface: both channels summed) or 2
		ASIOBufferInfo m_infos[2]{};
		long m_types[2]{};
		std::vector<float> m_stereo;
		std::atomic<bool> m_running{ false };
		std::atomic<int> m_inFlight{ 0 };
		std::atomic<ULONGLONG> m_lastCallback{ 0 };
		std::atomic<uint64_t> m_callbacks{ 0 };
		std::atomic<bool> m_resetRequested{ false };
		bool m_supportsOutputReady = false;
	};
}
