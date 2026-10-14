#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// The Real Tone Cable read directly over USB through the installed UsbDk runtime.
//
// Why: Windows' audio engine hands the cable's input over in 10 ms packets (480 frames, its minimum for this device,
// and the cable has no exclusive mode), while the cable itself sends 1 ms isochronous packets. Read here, the guitar
// arrives every 1 ms: the Tone designer's amp link (CableAmpLink::FeedInput) gets it about 10 ms sooner.
//
// It is only another physical producer feeding CaptureSession::Publish, in place of the WASAPI capture. The game's own
// packets are CaptureSession's (CableGamePeriod, 128 frames by default); 1 ms arrival is what lets them be that small
// without waiting on a 10 ms burst.
//
// There is no fallback inside this class: UsbDk owns the redirected function while capture is active, and the normal
// WASAPI path remains the separate CaptureSession producer when UsbDk is unavailable.
//
// Contract with the device (verified on the cable with a standalone capture test): VID 12BA PID 00FF,
// full speed, audio streaming interface 1 alternate setting 1, isochronous IN endpoint 0x82, at most 100 bytes per
// 1 ms frame, 16-bit mono PCM, sample rate set with UAC 1 SET_CUR on the endpoint. Anything else is refused.
namespace Audio::PersistentInput
{
	class UsbCableCapture
	{
	public:
		// Mono float samples at 48 kHz, the QPC time (100 ns units) of the first sample, and whether samples were lost
		// before this packet. Called on the capture thread every millisecond; must not block.
		using Sink = std::function<void(const float* samples, UINT32 frames, UINT64 timestamp, bool discontinuity)>;

		// Serialized UsbDk device identities, sorted (the same player-to-cable rule as the WASAPI cables).
		static std::vector<std::wstring> FindCables();
		// UsbDk's runtime library is installed (its driver may still need a Windows restart to see a cable).
		static bool IsRuntimeInstalled();

		UsbCableCapture() = default;
		~UsbCableCapture();
		UsbCableCapture(const UsbCableCapture&) = delete;
		UsbCableCapture& operator=(const UsbCableCapture&) = delete;

		// Opens and validates the cable and starts streaming into `sink`. On failure nothing is left open.
		HRESULT Start(const std::wstring& path, Sink sink);
		void Stop();
		// The stream ended by itself (cable unplugged, transfers stalled or failed): Stop, then Start again later.
		bool Failed() const { return failed.load(std::memory_order_acquire); }
		const std::wstring& Path() const { return path; }

	private:
		struct Slot;
		HRESULT Open();
		void Close();
		void Run();

		std::wstring path;
		Sink sink;
		HANDLE device = INVALID_HANDLE_VALUE;
		HANDLE systemHandle = nullptr;
		std::vector<Slot*> slots;
		HANDLE quit = nullptr;
		std::thread worker;
		std::atomic<bool> failed{ false };
	};
}
