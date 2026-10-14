#include "stdafx.h"
#include "UsbCableCapture.hpp"
#include <cfgmgr32.h>
#include <usb.h>
#include "UsbDkHelper.h"
#include <avrt.h>
#include <array>
#include <algorithm>
#include <filesystem>
#include <mutex>

#pragma comment(lib, "avrt.lib")

namespace Audio::PersistentInput
{
	UINT64 CaptureTime();

	namespace
	{
		constexpr USHORT kVendorId = 0x12BA;
		constexpr USHORT kProductId = 0x00FF;
		constexpr ULONG kStreamingInterface = 1;
		constexpr ULONG kStreamingAlternate = 1;
		constexpr ULONG kEndpoint = 0x82;
		constexpr ULONG kSampleRate = 48000;
		constexpr ULONG kPacketBytes = 100;
		constexpr size_t kSlots = 32;
		constexpr DWORD kStallMs = 2000;

		struct Api
		{
			HMODULE module = nullptr;
			std::mutex loadMutex;
			decltype(&UsbDk_GetDevicesList) getDevicesList = nullptr;
			decltype(&UsbDk_ReleaseDevicesList) releaseDevicesList = nullptr;
			decltype(&UsbDk_GetConfigurationDescriptor) getConfigurationDescriptor = nullptr;
			decltype(&UsbDk_ReleaseConfigurationDescriptor) releaseConfigurationDescriptor = nullptr;
			decltype(&UsbDk_StartRedirect) startRedirect = nullptr;
			decltype(&UsbDk_StopRedirect) stopRedirect = nullptr;
			decltype(&UsbDk_ReadPipe) readPipe = nullptr;
			decltype(&UsbDk_WritePipe) writePipe = nullptr;
			decltype(&UsbDk_AbortPipe) abortPipe = nullptr;
			decltype(&UsbDk_SetAltsetting) setAltsetting = nullptr;
			decltype(&UsbDk_GetRedirectorSystemHandle) getSystemHandle = nullptr;

			bool Load()
			{
				std::lock_guard<std::mutex> lock(loadMutex);
				if (module) return true;
				std::vector<std::filesystem::path> candidates;
				wchar_t value[32768]{};
				DWORD length = GetEnvironmentVariableW(L"ProgramFiles(x86)", value, ARRAYSIZE(value));
				if (length && length < ARRAYSIZE(value)) candidates.emplace_back(value);
				length = GetEnvironmentVariableW(L"ProgramFiles", value, ARRAYSIZE(value));
				if (length && length < ARRAYSIZE(value)) candidates.emplace_back(value);
				length = GetEnvironmentVariableW(L"ProgramW6432", value, ARRAYSIZE(value));
				if (length && length < ARRAYSIZE(value)) candidates.emplace_back(value);
				for (const auto& root : candidates)
				{
					const auto file = root / L"UsbDk Runtime Library" / (sizeof(void*) == 4 ? L"UsbDkHelper_x86.dll" : L"UsbDkHelper.dll");
					module = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
					if (module) break;
				}
				if (!module) return false;
#define LOAD_API(name, field) field = reinterpret_cast<decltype(field)>(GetProcAddress(module, name)); if (!field) { FreeLibrary(module); module = nullptr; return false; }
				LOAD_API("UsbDk_GetDevicesList", getDevicesList)
				LOAD_API("UsbDk_ReleaseDevicesList", releaseDevicesList)
				LOAD_API("UsbDk_GetConfigurationDescriptor", getConfigurationDescriptor)
				LOAD_API("UsbDk_ReleaseConfigurationDescriptor", releaseConfigurationDescriptor)
				LOAD_API("UsbDk_StartRedirect", startRedirect)
				LOAD_API("UsbDk_StopRedirect", stopRedirect)
				LOAD_API("UsbDk_ReadPipe", readPipe)
				LOAD_API("UsbDk_WritePipe", writePipe)
				LOAD_API("UsbDk_AbortPipe", abortPipe)
				LOAD_API("UsbDk_SetAltsetting", setAltsetting)
				LOAD_API("UsbDk_GetRedirectorSystemHandle", getSystemHandle)
#undef LOAD_API
				return true;
			}

			~Api() { if (module) FreeLibrary(module); }
		};

		Api& GetApi() { static Api api; return api; }

		std::wstring MakePath(const USB_DK_DEVICE_ID& id) { return std::wstring(L"usbdk://") + id.DeviceID + L"\n" + id.InstanceID; }

		void LogUnavailable(const char* reason)
		{
			static std::atomic<bool> logged{ false };
			if (!logged.exchange(true)) LOG_WARNING("(USB CABLE) UsbDk unavailable: " << reason << std::endl);
		}

		bool ParsePath(const std::wstring& path, USB_DK_DEVICE_ID& id)
		{
			if (path.rfind(L"usbdk://", 0) != 0) return false;
			constexpr size_t prefixLength = 8;
			const size_t split = path.find(L'\n', prefixLength);
			if (split == std::wstring::npos || split <= prefixLength || split + 1 >= path.size()) return false;
			ZeroMemory(&id, sizeof(id));
			if (split - prefixLength >= MAX_DEVICE_ID_LEN || path.size() - split - 1 >= MAX_DEVICE_ID_LEN) return false;
			wcsncpy_s(id.DeviceID, path.substr(prefixLength, split - prefixLength).c_str(), _TRUNCATE);
			wcsncpy_s(id.InstanceID, path.substr(split + 1).c_str(), _TRUNCATE);
			return true;
		}

		HRESULT LastError() { const DWORD error = GetLastError(); return error ? HRESULT_FROM_WIN32(error) : E_FAIL; }

		bool HasCableStream(const USB_CONFIGURATION_DESCRIPTOR* descriptor, ULONG length)
		{
			const BYTE* cursor = reinterpret_cast<const BYTE*>(descriptor);
			const BYTE* end = cursor + length;
			bool matchingInterface = false;
			while (cursor + 2 <= end && cursor[0] >= 2 && cursor + cursor[0] <= end)
			{
				if (cursor[1] == USB_INTERFACE_DESCRIPTOR_TYPE && cursor[0] >= sizeof(USB_INTERFACE_DESCRIPTOR))
				{
					const auto* iface = reinterpret_cast<const USB_INTERFACE_DESCRIPTOR*>(cursor);
					matchingInterface = iface->bInterfaceNumber == kStreamingInterface && iface->bAlternateSetting == kStreamingAlternate;
				}
				if (matchingInterface && cursor[1] == USB_ENDPOINT_DESCRIPTOR_TYPE && cursor[0] >= sizeof(USB_ENDPOINT_DESCRIPTOR))
				{
					const auto* endpoint = reinterpret_cast<const USB_ENDPOINT_DESCRIPTOR*>(cursor);
					if (endpoint->bEndpointAddress == kEndpoint && endpoint->bInterval == 1 && (endpoint->bmAttributes & 3) == USB_ENDPOINT_TYPE_ISOCHRONOUS)
					{
						const USHORT packetBytes = endpoint->wMaxPacketSize & 0x07FF;
						return packetBytes >= 96 && packetBytes <= kPacketBytes;
					}
				}
				cursor += cursor[0];
			}
			return false;
		}
	}

	struct UsbCableCapture::Slot
	{
		std::array<UCHAR, kPacketBytes> data{};
		ULONG64 packetLength = kPacketBytes;
		USB_DK_ISO_TRANSFER_RESULT result{};
		USB_DK_TRANSFER_REQUEST request{};
		OVERLAPPED overlapped{};
		bool pending = false;
	};

	bool UsbCableCapture::IsRuntimeInstalled() { return GetApi().Load(); }

	std::vector<std::wstring> UsbCableCapture::FindCables()
	{
		std::vector<std::wstring> paths;
		Api& api = GetApi();
		if (!api.Load()) { LogUnavailable("the installed helper DLL could not be loaded"); return paths; }
		PUSB_DK_DEVICE_INFO devices = nullptr; ULONG count = 0;
		if (!api.getDevicesList(&devices, &count)) { LogUnavailable("UsbDk_GetDevicesList failed"); return paths; }
		for (ULONG index = 0; index < count; ++index)
		{
			if (devices[index].DeviceDescriptor.idVendor != kVendorId || devices[index].DeviceDescriptor.idProduct != kProductId) continue;
			USB_DK_CONFIG_DESCRIPTOR_REQUEST request{};
			request.ID = devices[index].ID;
			PUSB_CONFIGURATION_DESCRIPTOR descriptor = nullptr;
			ULONG length = 0;
			if (!api.getConfigurationDescriptor(&request, &descriptor, &length)) continue;
			if (descriptor && length >= sizeof(USB_CONFIGURATION_DESCRIPTOR) && HasCableStream(descriptor, length))
			{
				paths.push_back(MakePath(devices[index].ID));
			}
			if (descriptor) api.releaseConfigurationDescriptor(descriptor);
		}
		api.releaseDevicesList(devices);
		std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
		return paths;
	}

	UsbCableCapture::~UsbCableCapture() { Stop(); }

	HRESULT UsbCableCapture::Start(const std::wstring& devicePath, Sink callback)
	{
		Stop(); path = devicePath; sink = std::move(callback); failed = false;
		HRESULT result = Open();
		if (SUCCEEDED(result)) { quit = CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!quit) result = LastError(); }
		if (SUCCEEDED(result)) { try { worker = std::thread(&UsbCableCapture::Run, this); } catch (...) { result = E_OUTOFMEMORY; } }
		if (FAILED(result)) { LOG_WARNING("(USB CABLE) UsbDk start failed, HRESULT " << std::hex << result << std::dec << std::endl); Close(); }
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
		Api& api = GetApi();
		if (!api.Load()) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
		USB_DK_DEVICE_ID id{}; if (!ParsePath(path, id)) return E_INVALIDARG;
		device = api.startRedirect(&id);
		if (!device || device == INVALID_HANDLE_VALUE) return LastError();
		systemHandle = api.getSystemHandle(device);
		if (!systemHandle || systemHandle == INVALID_HANDLE_VALUE) return LastError();
		USB_DK_CONFIG_DESCRIPTOR_REQUEST request{}; request.ID = id; PUSB_CONFIGURATION_DESCRIPTOR descriptor = nullptr; ULONG length = 0;
		if (!api.getConfigurationDescriptor(&request, &descriptor, &length)) return LastError();
		const bool valid = descriptor && length >= sizeof(USB_CONFIGURATION_DESCRIPTOR) && HasCableStream(descriptor, length);
		if (descriptor) api.releaseConfigurationDescriptor(descriptor);
		if (!valid) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
		if (!api.setAltsetting(device, kStreamingInterface, kStreamingAlternate)) return LastError();
		std::array<UCHAR, 11> control{};
		control[0] = 0x22; control[1] = 0x01; control[3] = 1; control[4] = kEndpoint; control[6] = 3;
		control[8] = static_cast<UCHAR>(kSampleRate); control[9] = static_cast<UCHAR>(kSampleRate >> 8); control[10] = static_cast<UCHAR>(kSampleRate >> 16);
		USB_DK_TRANSFER_REQUEST transfer{}; transfer.Buffer = control.data(); transfer.BufferLength = control.size(); transfer.TransferType = ControlTransferType;
		if (api.writePipe(device, &transfer, nullptr) == TransferFailure || transfer.Result.GenResult.BytesTransferred < 3 || transfer.Result.GenResult.UsbdStatus != 0) return LastError();
		control[0] = 0xA2; control[1] = 0x81; transfer = {}; transfer.Buffer = control.data(); transfer.BufferLength = control.size(); transfer.TransferType = ControlTransferType;
		if (api.readPipe(device, &transfer, nullptr) == TransferFailure || transfer.Result.GenResult.BytesTransferred < 3 || transfer.Result.GenResult.UsbdStatus != 0 || control[8] != static_cast<UCHAR>(kSampleRate) || control[9] != static_cast<UCHAR>(kSampleRate >> 8) || control[10] != static_cast<UCHAR>(kSampleRate >> 16)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
		for (size_t index = 0; index < kSlots; ++index) { auto* slot = new Slot(); slot->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!slot->overlapped.hEvent) { delete slot; return LastError(); } slots.push_back(slot); }
		return S_OK;
	}

	void UsbCableCapture::Close()
	{
		Api& api = GetApi();
		for (Slot* slot : slots) if (slot->pending) api.abortPipe(device, kEndpoint);
		for (Slot* slot : slots) { if (slot->pending) { DWORD ignored = 0; GetOverlappedResult(systemHandle, &slot->overlapped, &ignored, TRUE); } if (slot->overlapped.hEvent) CloseHandle(slot->overlapped.hEvent); delete slot; }
		slots.clear();
		if (device && device != INVALID_HANDLE_VALUE && !api.stopRedirect(device)) LOG_WARNING("(USB CABLE) UsbDk_StopRedirect failed, error " << GetLastError() << std::endl);
		device = INVALID_HANDLE_VALUE; systemHandle = nullptr;
		if (quit) CloseHandle(quit); quit = nullptr;
	}

	void UsbCableCapture::Run()
	{
		Api& api = GetApi(); DWORD taskIndex = 0; HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex); if (task) AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
		std::array<float, 64> samples{}; bool discontinuity = true;
		auto submit = [&](Slot& slot)
		{
			ResetEvent(slot.overlapped.hEvent);
			slot.request = {};
			slot.result = {};
			slot.packetLength = kPacketBytes;
			slot.request.EndpointAddress = kEndpoint;
			slot.request.Buffer = slot.data.data();
			slot.request.BufferLength = slot.data.size();
			slot.request.TransferType = IsochronousTransferType;
			slot.request.IsochronousPacketsArraySize = 1;
			slot.request.IsochronousPacketsArray = &slot.packetLength;
			slot.request.Result.IsochronousResultsArray = &slot.result;
			const TransferResult status = api.readPipe(device, &slot.request, &slot.overlapped);
			if (status == TransferSuccess) SetEvent(slot.overlapped.hEvent);
			slot.pending = status != TransferFailure;
			return slot.pending;
		};
		for (Slot* slot : slots)
		{
			if (!submit(*slot))
			{
				LOG_WARNING("(USB CABLE) UsbDk_ReadPipe failed while queueing, error " << GetLastError() << std::endl);
				failed = true;
				break;
			}
		}
		for (size_t next = 0; !failed && !slots.empty(); next = (next + 1) % slots.size())
		{
			HANDLE waits[] = { quit, slots[next]->overlapped.hEvent }; const DWORD wake = WaitForMultipleObjects(2, waits, FALSE, kStallMs); if (wake == WAIT_OBJECT_0) break; if (wake != WAIT_OBJECT_0 + 1) { failed = true; break; }
			Slot& slot = *slots[next]; DWORD transferred = 0; const bool completed = GetOverlappedResult(systemHandle, &slot.overlapped, &transferred, FALSE); slot.pending = false; const ULONG64 actual = slot.result.ActualLength;
			if (!completed || slot.result.TransferResult != 0 || actual == 0 || actual > slot.data.size() || (actual & 1))
			{
				LOG_WARNING("(USB CABLE) UsbDk isochronous packet failed, Win32 error " << GetLastError() << ", USB status " << slot.result.TransferResult << std::endl);
				discontinuity = true;
				failed = !submit(slot);
				continue;
			}
			const UINT32 frames = static_cast<UINT32>(actual / 2); for (UINT32 frame = 0; frame < frames; ++frame) samples[frame] = static_cast<int16_t>(slot.data[frame * 2] | (slot.data[frame * 2 + 1] << 8)) / 32768.0f;
			sink(samples.data(), frames, CaptureTime() - UINT64(frames) * 10000000 / kSampleRate, discontinuity);
			discontinuity = false;
			if (!submit(slot))
			{
				LOG_WARNING("(USB CABLE) UsbDk_ReadPipe failed while resubmitting, error " << GetLastError() << std::endl);
				failed = true;
			}
		}
		if (failed) LOG_WARNING("(USB CABLE) UsbDk stream stopped; capture will be reopened" << std::endl); if (task) AvRevertMmThreadCharacteristics(task);
	}
}
