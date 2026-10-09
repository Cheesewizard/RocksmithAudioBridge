#include "stdafx.h"
#include "OverlayInputCapture.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

// Rocksmith reads the mouse ONLY through DirectInput 8 (no raw input, GetCursorPos or GetAsyncKeyState), so
// swallowing WM_ mouse messages in the WndProc is not enough: while the overlay has the mouse, the game's
// DirectInput mouse reads must come back empty too.
//
// The game creates its DirectInput interface before Install() runs, so hooking DirectInput8Create is too late.
// Install() instead makes a throwaway mouse device of its own and patches GetDeviceState / GetDeviceData in
// dinput8.dll's device vtables, which every device object shares, so devices created earlier are covered too.
namespace OverlayInputCapture
{
	namespace
	{
		constexpr size_t GET_CAPABILITIES_SLOT = 3;
		constexpr size_t GET_DEVICE_STATE_SLOT = 9;
		constexpr size_t GET_DEVICE_DATA_SLOT = 10;

		using DirectInput8CreateFunction = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
		using GetCapabilitiesFunction = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, LPDIDEVCAPS);
		using GetDeviceStateFunction = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, DWORD, LPVOID);
		using GetDeviceDataFunction = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

		const GUID DIRECT_INPUT8_A_GUID =
		{ 0xbf798031, 0x483a, 0x4da2, { 0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00 } };
		const GUID DIRECT_INPUT8_W_GUID =
		{ 0xbf798030, 0x483a, 0x4da2, { 0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00 } };
		const GUID SYS_MOUSE_GUID =
		{ 0x6f1d2b60, 0xd5a0, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

		// One patched vtable per character set (the A and W device classes have their own vtables).
		struct PatchedVtable
		{
			void** vtable = nullptr;
			GetCapabilitiesFunction getCapabilities = nullptr;
			GetDeviceStateFunction getDeviceState = nullptr;
			GetDeviceDataFunction getDeviceData = nullptr;
		};
		PatchedVtable patched[2];
		int patchedCount = 0;

		std::atomic<bool> mouseCaptured{ false };
		std::atomic<bool> installed{ false };
		std::atomic<bool> loggedStateRead{ false };
		std::atomic<bool> loggedDataRead{ false };
		std::atomic<bool> loggedCaptureEnabled{ false };

		const PatchedVtable* Find(IUnknown* self)
		{
			void** vtable = *reinterpret_cast<void***>(self);
			for (int i = 0; i < patchedCount; ++i)
				if (patched[i].vtable == vtable) return &patched[i];
			return nullptr;
		}

		// The shared vtable also serves the keyboard and any joystick, so only mouse devices are filtered.
		// The answer is cached per device object (the game keeps a handful for its whole life).
		bool IsMouse(IUnknown* self, const PatchedVtable& table)
		{
			thread_local struct { IUnknown* device; bool mouse; } cache[8] = {};
			thread_local unsigned next = 0;
			for (const auto& entry : cache) if (entry.device == self) return entry.mouse;
			DIDEVCAPS caps{};
			caps.dwSize = sizeof(caps);
			const bool mouse = SUCCEEDED(table.getCapabilities(self, &caps)) && GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_MOUSE;
			cache[next++ % 8] = { self, mouse };
			return mouse;
		}

		HRESULT STDMETHODCALLTYPE HookGetDeviceState(IUnknown* self, DWORD size, LPVOID data)
		{
			const PatchedVtable* table = Find(self);
			if (table == nullptr) return DIERR_GENERIC;   // cannot happen: only patched vtables point here
			const HRESULT result = table->getDeviceState(self, size, data);
			if (FAILED(result) || data == nullptr || !mouseCaptured.load(std::memory_order_relaxed)) return result;
			if (size != sizeof(DIMOUSESTATE) && size != sizeof(DIMOUSESTATE2)) return result;   // not a mouse format
			if (!IsMouse(self, *table)) return result;
			if (!loggedStateRead.exchange(true, std::memory_order_relaxed))
				LOG_INFO("[OverlayInputCapture] Blocking Rocksmith's DirectInput mouse state under the overlay." << std::endl);
			// No buttons, no wheel (otherwise scrolling a panel scrolls the song list behind it) and no movement.
			auto* state = static_cast<DIMOUSESTATE*>(data);
			state->lX = state->lY = state->lZ = 0;
			std::memset(state->rgbButtons, 0, size - offsetof(DIMOUSESTATE, rgbButtons));
			return result;
		}

		HRESULT STDMETHODCALLTYPE HookGetDeviceData(IUnknown* self, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD itemCount, DWORD flags)
		{
			const PatchedVtable* table = Find(self);
			if (table == nullptr) return DIERR_GENERIC;
			const HRESULT result = table->getDeviceData(self, size, data, itemCount, flags);
			if (FAILED(result) || data == nullptr || itemCount == nullptr || !mouseCaptured.load(std::memory_order_relaxed))
				return result;
			if (!IsMouse(self, *table)) return result;
			if (!loggedDataRead.exchange(true, std::memory_order_relaxed))
				LOG_INFO("[OverlayInputCapture] Blocking Rocksmith's buffered DirectInput mouse events under the overlay." << std::endl);
			// Every buffered mouse event (buttons, wheel, movement) is dropped, so a press and its release are
			// never split between the overlay and the game.
			*itemCount = 0;
			return result;
		}

		bool PatchSlot(void** slot, void* hook)
		{
			DWORD oldProtection = 0;
			if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection)) return false;
			*slot = hook;
			DWORD unused = 0;
			VirtualProtect(slot, sizeof(void*), oldProtection, &unused);
			return true;
		}

		// Creates a throwaway system-mouse device for one character set and patches its class vtable.
		void PatchCharacterSet(DirectInput8CreateFunction create, const GUID& iid)
		{
			IDirectInput8A* directInput = nullptr;   // A and W share the layout of every slot used here
			if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, iid, reinterpret_cast<LPVOID*>(&directInput), nullptr)) || directInput == nullptr)
			{
				LOG_ERROR("[OverlayInputCapture] Could not create a DirectInput interface to find the mouse vtable." << std::endl);
				return;
			}
			IDirectInputDevice8A* device = nullptr;
			if (SUCCEEDED(directInput->CreateDevice(SYS_MOUSE_GUID, &device, nullptr)) && device != nullptr)
			{
				void** vtable = *reinterpret_cast<void***>(device);
				bool known = false;
				for (int i = 0; i < patchedCount; ++i) known |= patched[i].vtable == vtable;
				if (!known && patchedCount < 2)
				{
					PatchedVtable entry;
					entry.vtable = vtable;
					entry.getCapabilities = reinterpret_cast<GetCapabilitiesFunction>(vtable[GET_CAPABILITIES_SLOT]);
					entry.getDeviceState = reinterpret_cast<GetDeviceStateFunction>(vtable[GET_DEVICE_STATE_SLOT]);
					entry.getDeviceData = reinterpret_cast<GetDeviceDataFunction>(vtable[GET_DEVICE_DATA_SLOT]);
					// Record first so a game thread that lands in a hook mid-patch always finds its originals.
					patched[patchedCount++] = entry;
					if (!PatchSlot(&vtable[GET_DEVICE_STATE_SLOT], reinterpret_cast<void*>(&HookGetDeviceState))
						|| !PatchSlot(&vtable[GET_DEVICE_DATA_SLOT], reinterpret_cast<void*>(&HookGetDeviceData)))
						LOG_ERROR("[OverlayInputCapture] Could not patch the DirectInput mouse vtable." << std::endl);
				}
				device->Release();
			}
			else LOG_ERROR("[OverlayInputCapture] Could not create a DirectInput mouse device to find its vtable." << std::endl);
			directInput->Release();
		}
	}

	bool Install()
	{
		if (installed.load(std::memory_order_acquire)) return true;
		HMODULE dinput = GetModuleHandleW(L"dinput8.dll");
		if (dinput == nullptr) dinput = LoadLibraryW(L"dinput8.dll");
		const auto create = dinput ? reinterpret_cast<DirectInput8CreateFunction>(GetProcAddress(dinput, "DirectInput8Create")) : nullptr;
		if (create == nullptr)
		{
			LOG_ERROR("[OverlayInputCapture] dinput8.dll is unavailable; overlay clicks will reach the game." << std::endl);
			return false;
		}
		PatchCharacterSet(create, DIRECT_INPUT8_A_GUID);
		PatchCharacterSet(create, DIRECT_INPUT8_W_GUID);
		if (patchedCount == 0) return false;
		installed.store(true, std::memory_order_release);
		LOG_INFO("[OverlayInputCapture] DirectInput mouse capture installed on " << patchedCount << " shared device vtable(s)." << std::endl);
		return true;
	}

	void SetMouseCapture(bool shouldCapture)
	{
		mouseCaptured.store(shouldCapture, std::memory_order_relaxed);
		if (shouldCapture && !loggedCaptureEnabled.exchange(true, std::memory_order_relaxed))
			LOG_INFO("[OverlayInputCapture] ImGui requested DirectInput mouse capture." << std::endl);
	}

	void Shutdown()
	{
		mouseCaptured.store(false, std::memory_order_relaxed);
		// Put the original methods back; the table entries stay so a call already inside a hook still resolves.
		for (int i = 0; i < patchedCount; ++i)
		{
			PatchSlot(&patched[i].vtable[GET_DEVICE_STATE_SLOT], reinterpret_cast<void*>(patched[i].getDeviceState));
			PatchSlot(&patched[i].vtable[GET_DEVICE_DATA_SLOT], reinterpret_cast<void*>(patched[i].getDeviceData));
		}
		installed.store(false, std::memory_order_release);
	}
}
