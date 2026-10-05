#pragma once

#include "PersistentInput.hpp"
#include "PersistentVolume.hpp"
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
		HRESULT OpenDevice(IMMDeviceEnumerator* enumerator);
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
	};
}
