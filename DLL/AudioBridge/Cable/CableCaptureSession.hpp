#pragma once

#include "AudioBridge/Devices/DeviceEnumeratorHook.hpp"
#include "CableEndpointVolume.hpp"
#include "UsbCableCapture.hpp"
#include <wrl.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Audio::PersistentInput
{
	constexpr UINT32 SAMPLE_RATE = 48000;
	constexpr UINT32 MAX_PACKET_FRAMES = 4096;
	constexpr UINT32 RING_FRAMES = 8192;

	bool IsCaptureFormat(const WAVEFORMATEX* format);
	bool IsCable(IMMDevice* device);
	// UsbDk is installed but missed Player 1's cable, which Windows sees: almost always a fresh UsbDk install waiting
	// for a Windows restart. The cable plays through Windows meanwhile; the game screen tells the player why.
	bool IsDirectUsbWaitingForRestart();

	// Player 1's input in Cable mode. The game only ever sees our persistent cable device; this is the hardware behind
	// it. Automatic (no choice) is a cable with the Real Tone Cable USB ID (12BA:00FF), read over UsbDk when it can be.
	// Without one, and while none has opened this launch, it is the Windows default recording device, the way the game
	// itself takes other cables when Rocksmith.ini has RealToneCableOnly=0 (it skips them at 1, and so do we).
	// A chosen device that is not a Real Tone Cable is always read through Windows.
	enum class PlayerOneInputKind { None, RealToneCable, ChosenDevice, WindowsDefault };
	struct PlayerOneInput
	{
		PlayerOneInputKind kind = PlayerOneInputKind::None;
		std::wstring name;   // Windows' friendly name of the open device
	};
	struct CaptureDeviceInfo
	{
		std::wstring id;
		std::wstring name;
		bool realToneCable = false;
	};
	PlayerOneInput GetPlayerOneInput();
	// RSMods.ini [Audio Bridge] CableInputDevice: an endpoint ID, or empty for automatic. Saved and applied within a second.
	std::wstring GetPlayerOneInputChoice();
	void SetPlayerOneInputChoice(const std::wstring& endpointId);
	// Active Windows recording devices, for the overlay's picker.
	std::vector<CaptureDeviceInfo> ListCaptureDevices();

	class CaptureSession
	{
	public:
		explicit CaptureSession(std::wstring endpointId, bool enableHardware = true, bool gatedByPlayerTwoToggle = false);
		~CaptureSession();
		HRESULT Initialize(const WAVEFORMATEX* format, DWORD flags, REFERENCE_TIME duration);
		HRESULT SetEvent(HANDLE event);
		HRESULT Start();
		HRESULT Stop();
		HRESULT Reset();
		HRESULT GetBuffer(BYTE** data, UINT32* frames, DWORD* flags, UINT64* position, UINT64* timestamp);
		HRESULT ReleaseBuffer(UINT32 frames);
		HRESULT GetNextPacketSize(UINT32* frames);
		HRESULT GetClockPosition(UINT64* position, UINT64* timestamp);
		void Publish(const float* samples, UINT32 frames, UINT64 timestamp, bool discontinuity, bool timestampError = false);
		void Disconnect();
		void Tick(UINT64 timestamp);
		bool IsPhysicalPacket() const;
		UINT64 GetGeneration() const;
		bool IsInitialized() const;
		UINT32 GetPeriodFrames() const;
		bool IsGatedByPlayerTwoToggle() const { return gatedByPlayerTwoToggle; }

	private:
		void Run();
		void RunClock();
		HRESULT OpenDevice(IMMDeviceEnumerator* enumerator, const std::wstring& endpointId);
		HRESULT OpenEndpoint(IMMDevice* device);
		HRESULT OpenUsb();
		void NoteOpened(PlayerOneInputKind kind);
		HRESULT DrainDevice();
		void CloseDevice();
		void ClearPackets();
		bool QueueCapturedPacket();
		void CloseEvents();

		std::wstring selectedEndpoint;
		const bool enableHardware;
		const bool gatedByPlayerTwoToggle;
		std::mutex controlMutex;
		std::mutex packetMutex;
		std::thread worker;
		std::thread scheduler;
		HANDLE quitEvent = nullptr;
		HANDLE changeEvent = nullptr;
		HANDLE captureEvent = nullptr;
		HANDLE timer = nullptr;
		HANDLE gameEvent = nullptr;
		std::atomic<bool> initialized{ false };
		std::atomic<bool> running{ false };
		std::atomic<bool> deviceResetRequested{ false };
		std::atomic<bool> physicalPacket{ false };
		std::atomic<UINT64> generation{ 0 };
		std::atomic<UINT64> lastPhysicalTick{ 0 };
		std::atomic<UINT64> lastPhysicalTime{ 0 };   // CaptureTime (QPC, 100 ns) of the last cable delivery, for Tick
		std::atomic<uint64_t> silenceFills{ 0 };     // packets of silence Tick gave the game after cable data had flowed
		WAVEFORMATEX gameFormat{};
		Microsoft::WRL::ComPtr<CableVolume> volume;
		UINT32 periodFrames = 480;
		bool floatFormat = false;
		std::array<float, RING_FRAMES> ring{};
		std::array<bool, RING_FRAMES> timestampErrors{};
		std::array<UINT64, RING_FRAMES> timestamps{};
		std::array<BYTE, MAX_PACKET_FRAMES * 8> packet{};
		UINT32 readFrame = 0;
		UINT32 availableFrames = 0;
		UINT32 outstandingFrames = 0;
		UINT32 readyFrames = 0;
		UINT64 timelineFrames = 0;
		UINT64 readyPosition = 0;
		UINT64 readyTimestamp = 0;
		UINT64 packetGeneration = 0;
		bool discontinuityPending = true;
		bool retainedPacket = false;
		DWORD packetFlags = 0;
		UINT64 packetPosition = 0;
		UINT64 packetTimestamp = 0;
		Microsoft::WRL::ComPtr<IAudioClient> backend;
		Microsoft::WRL::ComPtr<IAudioCaptureClient> capture;
		std::wstring activeEndpoint;
		HRESULT lastOpenResult = S_OK;
		// The cable read directly over USB (its audio function on WinUSB) instead of `backend`; one or the other.
		UsbCableCapture usb;
		bool usbOpen = false;
		PlayerOneInputKind openKind = PlayerOneInputKind::None;   // what Player 1's open device is (worker thread)
		UINT64 seenChoiceGeneration = 0;
		UINT32 clockFrames = 0;   // packetMutex: input frames toward the next NotifyCableInputFrames
	};
}
