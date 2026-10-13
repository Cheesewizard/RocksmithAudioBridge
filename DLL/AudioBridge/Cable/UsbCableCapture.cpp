#include "stdafx.h"
#include "UsbCableCapture.hpp"
#include <avrt.h>
#include <setupapi.h>
#include <winusb.h>
#include <array>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "avrt.lib")

namespace Audio::PersistentInput
{
	UINT64 CaptureTime();   // CableCaptureSession.cpp: QPC in 100 ns units, the session's timestamp clock

	namespace
	{
		// The device interface GUID the WinUSB binding registers for the cable's audio function (DeviceInterfaceGUIDs
		// under the MI_00 instance's Device Parameters). The spike and the installer step use the same one.
		constexpr GUID kInterfaceGuid = { 0xf7b829a2, 0x6ca8, 0x4c4e, { 0xad, 0x19, 0x8b, 0xe9, 0x0d, 0x04, 0xc4, 0x12 } };
		constexpr USHORT kVendorId = 0x12BA;
		constexpr USHORT kProductId = 0x00FF;
		constexpr UCHAR kStreamingInterface = 1;
		constexpr UCHAR kStreamingAlternate = 1;
		constexpr UCHAR kEndpoint = 0x82;
		constexpr ULONG kSampleRate = 48000;
		// Transfers of one 1 ms frame each, queued ahead. More in the queue adds no delay (each completes as soon as its
		// frame arrives); it is how long the reader thread may be held up before the stream loses its place. 8 (8 ms)
		// broke a few times a minute while outputs were being switched (2026-10-12, error 87); 32 gives 32 ms.
		constexpr size_t kSlots = 32;
		constexpr DWORD kStallMs = 2000;           // no completion this long = the stream is gone

		HRESULT LastError()
		{
			const DWORD error = GetLastError();
			return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
		}
	}

	struct UsbCableCapture::Slot
	{
		std::vector<UCHAR> data;
		USBD_ISO_PACKET_DESCRIPTOR packet{};
		WINUSB_ISOCH_BUFFER_HANDLE registered = nullptr;
		OVERLAPPED overlapped{};
		bool pending = false;
	};

	std::vector<std::wstring> UsbCableCapture::FindCables()
	{
		std::vector<std::wstring> paths;
		const HDEVINFO devices = SetupDiGetClassDevsW(&kInterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
		if (devices == INVALID_HANDLE_VALUE) return paths;
		for (DWORD index = 0;; ++index)
		{
			SP_DEVICE_INTERFACE_DATA interfaceData{ sizeof(interfaceData) };
			if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &kInterfaceGuid, index, &interfaceData)) break;
			DWORD required = 0;
			SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &required, nullptr);
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
			std::vector<BYTE> storage(required);
			auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
			detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
			if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, required, nullptr, nullptr)) continue;
			std::wstring path = detail->DevicePath;
			std::wstring lower = path;
			std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
			if (lower.find(L"vid_12ba&pid_00ff&mi_00") != std::wstring::npos) paths.push_back(std::move(path));
		}
		SetupDiDestroyDeviceInfoList(devices);
		std::sort(paths.begin(), paths.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
		return paths;
	}

	UsbCableCapture::~UsbCableCapture() { Stop(); }

	HRESULT UsbCableCapture::Start(const std::wstring& devicePath, Sink callback)
	{
		Stop();
		path = devicePath;
		sink = std::move(callback);
		failed = false;
		HRESULT result = Open();
		if (SUCCEEDED(result))
		{
			quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!quit) result = LastError();
		}
		if (SUCCEEDED(result))
		{
			try { worker = std::thread(&UsbCableCapture::Run, this); }
			catch (const std::exception&) { result = E_OUTOFMEMORY; }
		}
		if (FAILED(result)) Close();
		return result;
	}

	void UsbCableCapture::Stop()
	{
		if (quit) SetEvent(quit);
		if (worker.joinable()) worker.join();
		Close();
	}

	HRESULT UsbCableCapture::Open()
	{
		device = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (device == INVALID_HANDLE_VALUE) return LastError();
		WINUSB_INTERFACE_HANDLE control = nullptr;
		if (!WinUsb_Initialize(device, &control)) return LastError();
		root = control;

		USB_DEVICE_DESCRIPTOR descriptor{};
		ULONG transferred = 0;
		if (!WinUsb_GetDescriptor(control, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, reinterpret_cast<PUCHAR>(&descriptor), sizeof(descriptor), &transferred)) return LastError();
		if (transferred != sizeof(descriptor) || descriptor.idVendor != kVendorId || descriptor.idProduct != kProductId) return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_AVAILABLE);

		// The function's first interface is audio control (0); streaming (1) is its associated interface.
		USB_INTERFACE_DESCRIPTOR settings{};
		if (!WinUsb_QueryInterfaceSettings(control, 0, &settings)) return LastError();
		WINUSB_INTERFACE_HANDLE stream = settings.bInterfaceNumber == kStreamingInterface ? control : nullptr;
		for (UCHAR index = 0; !stream && index < 4; ++index)
		{
			WINUSB_INTERFACE_HANDLE associated = nullptr;
			if (!WinUsb_GetAssociatedInterface(control, index, &associated)) return LastError();
			if (WinUsb_QueryInterfaceSettings(associated, 0, &settings) && settings.bInterfaceNumber == kStreamingInterface) stream = associated;
			else WinUsb_Free(associated);
		}
		if (!stream) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
		streaming = stream;

		if (!WinUsb_SetCurrentAlternateSetting(stream, kStreamingAlternate)) return LastError();
		alternateSelected = true;
		if (!WinUsb_QueryInterfaceSettings(stream, kStreamingAlternate, &settings)) return LastError();
		ULONG packetBytes = 0;
		for (UCHAR index = 0; index < settings.bNumEndpoints; ++index)
		{
			WINUSB_PIPE_INFORMATION_EX pipe{};
			if (!WinUsb_QueryPipeEx(stream, kStreamingAlternate, index, &pipe)) return LastError();
			if (pipe.PipeId != kEndpoint) continue;
			// 48 kHz mono 16-bit is 96 bytes a frame; the endpoint allows up to 100.
			if (pipe.PipeType != UsbdPipeTypeIsochronous || pipe.Interval != 1 || pipe.MaximumBytesPerInterval < 96 || pipe.MaximumBytesPerInterval > 1023)
				return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
			packetBytes = pipe.MaximumBytesPerInterval;
		}
		if (!packetBytes) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

		// UAC 1 sampling frequency control on the endpoint: set 48 kHz, then read it back.
		UCHAR rate[3] = { static_cast<UCHAR>(kSampleRate), static_cast<UCHAR>(kSampleRate >> 8), static_cast<UCHAR>(kSampleRate >> 16) };
		const WINUSB_SETUP_PACKET setRate{ 0x22, 0x01, 0x0100, kEndpoint, 3 };
		if (!WinUsb_ControlTransfer(stream, setRate, rate, 3, &transferred, nullptr)) return LastError();
		const WINUSB_SETUP_PACKET getRate{ 0xA2, 0x81, 0x0100, kEndpoint, 3 };
		if (!WinUsb_ControlTransfer(stream, getRate, rate, 3, &transferred, nullptr)) return LastError();
		if (transferred != 3 || (rate[0] | (rate[1] << 8) | (rate[2] << 16)) != static_cast<int>(kSampleRate)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

		for (size_t index = 0; index < kSlots; ++index)
		{
			auto* slot = new Slot();
			slots.push_back(slot);
			slot->data.resize(packetBytes);
			slot->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!slot->overlapped.hEvent) return LastError();
			if (!WinUsb_RegisterIsochBuffer(stream, kEndpoint, slot->data.data(), packetBytes, &slot->registered)) return LastError();
		}
		return S_OK;
	}

	void UsbCableCapture::Close()
	{
		for (Slot* slot : slots)
			if (slot->pending) CancelIoEx(device, &slot->overlapped);
		for (Slot* slot : slots)
		{
			if (slot->pending)
			{
				DWORD ignored = 0;
				WinUsb_GetOverlappedResult(streaming, &slot->overlapped, &ignored, TRUE);
			}
			if (slot->registered) WinUsb_UnregisterIsochBuffer(slot->registered);
			if (slot->overlapped.hEvent) CloseHandle(slot->overlapped.hEvent);
			delete slot;
		}
		slots.clear();
		// Back to the zero-bandwidth setting, so the bus reservation goes with us (it fails harmlessly when unplugged).
		if (alternateSelected && streaming) WinUsb_SetCurrentAlternateSetting(streaming, 0);
		alternateSelected = false;
		if (streaming && streaming != root) WinUsb_Free(streaming);
		if (root) WinUsb_Free(root);
		streaming = root = nullptr;
		if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
		device = INVALID_HANDLE_VALUE;
		if (quit) CloseHandle(quit);
		quit = nullptr;
	}

	void UsbCableCapture::Run()
	{
		DWORD taskIndex = 0;
		HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
		if (task) AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);

		// One 1 ms frame per transfer. ContinueStream keeps the frames back to back; when the stream cannot continue
		// (a transfer was resubmitted too late), it restarts at the next frame and the gap is reported downstream.
		auto submit = [&](Slot& slot, bool continueStream)
		{
			const HANDLE event = slot.overlapped.hEvent;
			slot.overlapped = {};
			slot.overlapped.hEvent = event;
			slot.packet = {};
			ResetEvent(event);
			const BOOL started = WinUsb_ReadIsochPipeAsap(slot.registered, 0, static_cast<ULONG>(slot.data.size()), continueStream ? TRUE : FALSE, 1, &slot.packet, &slot.overlapped);
			slot.pending = started || GetLastError() == ERROR_IO_PENDING;
			return slot.pending;
		};

		// Queue every slot, the first starting a new stream and the rest continuing it.
		auto queueAll = [&]()
		{
			for (size_t index = 0; index < slots.size(); ++index)
				if (!submit(*slots[index], index != 0)) return false;
			return true;
		};
		// The stream lost its place in the USB schedule (the reader was held up longer than the queue, or a transfer could
		// not continue the previous one): cancel what is queued and start again from the next frame. A few milliseconds
		// are lost, reported as a discontinuity; this used to stop the stream and cost a second or two of silence.
		auto restart = [&]()
		{
			for (Slot* slot : slots) if (slot->pending) CancelIoEx(device, &slot->overlapped);
			for (Slot* slot : slots)
			{
				if (!slot->pending) continue;
				DWORD ignored = 0;
				WinUsb_GetOverlappedResult(streaming, &slot->overlapped, &ignored, TRUE);
				slot->pending = false;
			}
			return queueAll();
		};
		// Errors that mean the cable is gone; anything else is a lost place in the schedule.
		auto gone = [](DWORD code)
		{
			return code == ERROR_DEVICE_NOT_CONNECTED || code == ERROR_GEN_FAILURE || code == ERROR_FILE_NOT_FOUND
				|| code == ERROR_BAD_COMMAND || code == ERROR_NO_SUCH_DEVICE || code == ERROR_DEV_NOT_EXIST || code == ERROR_INVALID_HANDLE;
		};

		bool discontinuity = true;
		std::string error;
		if (!queueAll()) error = "could not start the transfers (error " + std::to_string(GetLastError()) + ")";
		unsigned restarts = 0, restartsLogged = 0;
		ULONGLONG burstStart = 0, lastLog = 0;
		unsigned burst = 0;
		DWORD lastCode = 0;
		// Restarts the stream, or gives up when it keeps breaking (20 in 2 s): then the session reopens the cable.
		auto recover = [&](DWORD code) -> bool
		{
			const ULONGLONG now = GetTickCount64();
			if (now - burstStart > 2000) { burstStart = now; burst = 0; }
			lastCode = code;
			++restarts;
			discontinuity = true;
			if (++burst > 20) { error = "the stream kept breaking (error " + std::to_string(code) + ")"; return false; }
			if (!restart()) { error = "could not queue the transfers again (error " + std::to_string(GetLastError()) + ")"; return false; }
			return true;
		};

		std::array<float, 512> samples{};
		for (size_t next = 0; error.empty(); next = (next + 1) % slots.size())
		{
			if (restarts != restartsLogged && GetTickCount64() - lastLog >= 10000)
			{
				LOG_WARNING("(USB CABLE) Stream restarted " << restarts - restartsLogged << " time(s) since the last report (error "
					<< lastCode << "): the reader was held up, a few ms of guitar lost each time" << std::endl);
				restartsLogged = restarts;
				lastLog = GetTickCount64();
			}
			Slot& slot = *slots[next];
			const HANDLE waits[] = { quit, slot.overlapped.hEvent };
			const DWORD wake = WaitForMultipleObjects(2, waits, FALSE, kStallMs);
			if (wake == WAIT_OBJECT_0) break;
			if (wake == WAIT_TIMEOUT) { error = "no audio from the cable for 2 s"; break; }
			if (wake != WAIT_OBJECT_0 + 1) { error = "wait failed (error " + std::to_string(GetLastError()) + ")"; break; }
			DWORD ignored = 0;
			const BOOL completed = WinUsb_GetOverlappedResult(streaming, &slot.overlapped, &ignored, FALSE);
			slot.pending = false;
			if (!completed)
			{
				const DWORD code = GetLastError();
				if (gone(code)) { error = "transfer failed (error " + std::to_string(code) + "), cable unplugged?"; break; }
				if (!recover(code)) break;
				next = slots.size() - 1;   // the loop's step makes it 0: the restarted queue begins at slot 0
				continue;
			}

			const USBD_ISO_PACKET_DESCRIPTOR& packet = slot.packet;
			const bool valid = packet.Status == 0 && packet.Length > 0 && packet.Length % 2 == 0
				&& packet.Offset <= slot.data.size() && packet.Length <= slot.data.size() - packet.Offset;
			if (!valid) discontinuity = true;   // a lost or empty frame (the first few after starting are empty)
			else
			{
				const UINT32 frames = (std::min)(static_cast<UINT32>(packet.Length / 2), static_cast<UINT32>(samples.size()));
				const UCHAR* bytes = slot.data.data() + packet.Offset;
				for (UINT32 frame = 0; frame < frames; ++frame)
					samples[frame] = static_cast<int16_t>(bytes[frame * 2] | (bytes[frame * 2 + 1] << 8)) / 32768.0f;
				const UINT64 first = CaptureTime() - UINT64(frames) * 10000000 / kSampleRate;
				sink(samples.data(), frames, first, discontinuity);
				discontinuity = false;
			}

			if (!submit(slot, true))
			{
				const DWORD code = GetLastError();
				if (gone(code)) { error = "could not queue a transfer (error " + std::to_string(code) + "), cable unplugged?"; break; }
				if (!recover(code)) break;
				next = slots.size() - 1;
				continue;
			}
		}

		if (!error.empty())
		{
			LOG_WARNING("(USB CABLE) Stream stopped: " << error << std::endl);
			failed.store(true, std::memory_order_release);
		}
		if (task) AvRevertMmThreadCharacteristics(task);
	}
}
