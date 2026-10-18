#include "stdafx.h"
#include "OverlayInputCapture.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

// Rocksmith reads the mouse ONLY through DirectInput 8 (no raw input, GetCursorPos or GetAsyncKeyState), so
// swallowing WM_ mouse messages in the WndProc is not enough: while the overlay has the mouse, the game's
// DirectInput mouse reads must come back empty too. The keyboard is the same: typing into an overlay text field
// (tone names, search boxes) drove the song list behind it, so while a text field is active the game's DirectInput
// keyboard reads come back empty as well.
//
// The game creates its DirectInput interface before Install() runs, so hooking DirectInput8Create is too late.
// Install() instead makes a throwaway mouse device of its own and patches GetDeviceState / GetDeviceData in
// the device vtable it gets, which every device object of that class shares, so devices created earlier are covered too.
//
// Other programs hook the same interfaces. The Steam in-game overlay gives DirectInput devices its own vtable
// (inside gameoverlayrenderer.dll), and on Learn & Play the Ubisoft wrapper also hooks ntdll!NtProtectVirtualMemory;
// with both in the process, calling VirtualProtect on Steam's vtable jumped to an unmapped address
// (crash dump 2026-10-18 15:30, fault inside the hooked protect called from PatchSlot). So the patching:
//  - chains to whatever the slot holds when it is patched, never to an assumed dinput8 original;
//  - writes the slot with a compare-and-swap, so a slot another hooker changed in between is never overwritten;
//  - changes page protection only when the page is not already writable, and then through the direct syscall
//    (MemUtil::HookedVirtualProtect), never the hooked VirtualProtect;
//  - gives each patched vtable its own hook functions that know their originals, instead of looking the original up
//    by the object's current vtable, so a hooker that later copies our pointer into its own table still chains;
//  - on Shutdown restores a slot only if it still holds our hook; otherwise the hook stays and passes through.
namespace OverlayInputCapture
{
	namespace
	{
		constexpr size_t GET_CAPABILITIES_SLOT = 3;
		constexpr size_t GET_DEVICE_STATE_SLOT = 9;
		constexpr size_t GET_DEVICE_DATA_SLOT = 10;
		constexpr int MAX_VTABLES = 2;   // the A and W device classes have their own vtables

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

		// The functions each patched vtable held before our hooks went in; the hooks chain to these.
		struct PatchedVtable
		{
			void** vtable = nullptr;
			GetCapabilitiesFunction getCapabilities = nullptr;
			GetDeviceStateFunction getDeviceState = nullptr;
			GetDeviceDataFunction getDeviceData = nullptr;
			bool stateHooked = false;
			bool dataHooked = false;
		};
		PatchedVtable patched[MAX_VTABLES];
		int patchedCount = 0;

		std::atomic<bool> mouseCaptured{ false };
		std::atomic<bool> keyboardCaptured{ false };
		std::atomic<bool> installed{ false };
		std::atomic<bool> loggedStateRead{ false };
		std::atomic<bool> loggedDataRead{ false };
		std::atomic<bool> loggedCaptureEnabled{ false };
		std::atomic<bool> loggedKeyboardRead{ false };

		void* ReadSlot(void** slot) { return *reinterpret_cast<void* volatile*>(slot); }

		// The shared vtable also serves joysticks, so only mouse and keyboard devices are filtered.
		// The device type is cached per device object (the game keeps a handful for its whole life).
		BYTE DeviceType(IUnknown* self, const PatchedVtable& table)
		{
			thread_local struct { IUnknown* device; BYTE type; } cache[8] = {};
			thread_local unsigned next = 0;
			for (const auto& entry : cache) if (entry.device == self) return entry.type;
			DIDEVCAPS caps{};
			caps.dwSize = sizeof(caps);
			const BYTE type = SUCCEEDED(table.getCapabilities(self, &caps)) ? static_cast<BYTE>(GET_DIDEVICE_TYPE(caps.dwDevType)) : 0;
			cache[next++ % 8] = { self, type };
			return type;
		}

		bool IsMouse(IUnknown* self, const PatchedVtable& table) { return DeviceType(self, table) == DI8DEVTYPE_MOUSE; }
		bool IsKeyboard(IUnknown* self, const PatchedVtable& table) { return DeviceType(self, table) == DI8DEVTYPE_KEYBOARD; }

		void LogKeyboardBlocked()
		{
			if (!loggedKeyboardRead.exchange(true, std::memory_order_relaxed))
				LOG_INFO("[OverlayInputCapture] Blocking Rocksmith's DirectInput keyboard while an overlay text field is active." << std::endl);
		}

		HRESULT FilterDeviceState(const PatchedVtable& table, IUnknown* self, DWORD size, LPVOID data)
		{
			const HRESULT result = table.getDeviceState(self, size, data);
			if (FAILED(result) || data == nullptr) return result;
			if (keyboardCaptured.load(std::memory_order_relaxed) && size == 256 && IsKeyboard(self, table))
			{
				// Every key reads as up.
				LogKeyboardBlocked();
				std::memset(data, 0, size);
				return result;
			}
			if (!mouseCaptured.load(std::memory_order_relaxed)) return result;
			if (size != sizeof(DIMOUSESTATE) && size != sizeof(DIMOUSESTATE2)) return result;   // not a mouse format
			if (!IsMouse(self, table)) return result;
			if (!loggedStateRead.exchange(true, std::memory_order_relaxed))
				LOG_INFO("[OverlayInputCapture] Blocking Rocksmith's DirectInput mouse state under the overlay." << std::endl);
			// No buttons, no wheel (otherwise scrolling a panel scrolls the song list behind it) and no movement.
			auto* state = static_cast<DIMOUSESTATE*>(data);
			state->lX = state->lY = state->lZ = 0;
			std::memset(state->rgbButtons, 0, size - offsetof(DIMOUSESTATE, rgbButtons));
			return result;
		}

		HRESULT FilterDeviceData(const PatchedVtable& table, IUnknown* self, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD itemCount, DWORD flags)
		{
			const HRESULT result = table.getDeviceData(self, size, data, itemCount, flags);
			if (FAILED(result) || data == nullptr || itemCount == nullptr) return result;
			if (keyboardCaptured.load(std::memory_order_relaxed) && IsKeyboard(self, table))
			{
				LogKeyboardBlocked();
				*itemCount = 0;
				return result;
			}
			if (!mouseCaptured.load(std::memory_order_relaxed)) return result;
			if (!IsMouse(self, table)) return result;
			if (!loggedDataRead.exchange(true, std::memory_order_relaxed))
				LOG_INFO("[OverlayInputCapture] Blocking Rocksmith's buffered DirectInput mouse events under the overlay." << std::endl);
			// Every buffered mouse event (buttons, wheel, movement) is dropped, so a press and its release are
			// never split between the overlay and the game.
			*itemCount = 0;
			return result;
		}

		template <int Index>
		HRESULT STDMETHODCALLTYPE HookGetDeviceState(IUnknown* self, DWORD size, LPVOID data)
		{
			return FilterDeviceState(patched[Index], self, size, data);
		}

		template <int Index>
		HRESULT STDMETHODCALLTYPE HookGetDeviceData(IUnknown* self, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD itemCount, DWORD flags)
		{
			return FilterDeviceData(patched[Index], self, size, data, itemCount, flags);
		}

		void* const STATE_HOOKS[MAX_VTABLES] = { reinterpret_cast<void*>(&HookGetDeviceState<0>), reinterpret_cast<void*>(&HookGetDeviceState<1>) };
		void* const DATA_HOOKS[MAX_VTABLES] = { reinterpret_cast<void*>(&HookGetDeviceData<0>), reinterpret_cast<void*>(&HookGetDeviceData<1>) };

		enum class SwapResult { Swapped, SlotChanged, Failed };

		// Writes `replacement` into the slot only while it still holds `expected`.
		SwapResult SwapSlot(void** slot, void* expected, void* replacement)
		{
			MEMORY_BASIC_INFORMATION page{};
			if (!NT_SUCCESS(MemUtil::HookedQueryVirtualMemory(slot, &page, sizeof(page))) || page.State != MEM_COMMIT) return SwapResult::Failed;
			const DWORD protection = page.Protect & 0xFF;
			const bool writable = (page.Protect & PAGE_GUARD) == 0 && (protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE);
			ULONG previous = 0;
			if (!writable)
			{
				// Keep the execute bit if the page has one: other code may share it.
				const bool executable = protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_WRITECOPY;
				if (!NT_SUCCESS(MemUtil::HookedVirtualProtect(slot, sizeof(void*), executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, previous)))
					return SwapResult::Failed;
			}
			const bool swapped = InterlockedCompareExchangePointer(slot, replacement, expected) == expected;
			if (!writable)
			{
				ULONG unused = 0;
				MemUtil::HookedVirtualProtect(slot, sizeof(void*), previous, unused);
			}
			return swapped ? SwapResult::Swapped : SwapResult::SlotChanged;
		}

		// Records what the slot holds now as the function to chain to, then swaps in the hook. A slot that changes
		// between the read and the swap (another hooker patching at the same time) is re-read and tried again.
		template <typename Function>
		bool HookSlot(void** slot, void* hook, Function& original)
		{
			for (int attempt = 0; attempt < 4; ++attempt)
			{
				void* current = ReadSlot(slot);
				if (current == nullptr || current == hook) return false;
				original = reinterpret_cast<Function>(current);
				const SwapResult result = SwapSlot(slot, current, hook);
				if (result == SwapResult::Swapped) return true;
				if (result == SwapResult::Failed) return false;
			}
			return false;
		}

		std::string OwnerModule(const void* address)
		{
			HMODULE module = nullptr;
			wchar_t path[MAX_PATH] = {};
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module)
				|| GetModuleFileNameW(module, path, MAX_PATH) == 0)
				return "no module";
			const wchar_t* name = wcsrchr(path, L'\\');
			name = name ? name + 1 : path;
			std::string narrow;
			for (; *name; ++name) narrow += static_cast<char>(*name < 0x80 ? *name : '?');
			return narrow;
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
				if (!known && patchedCount < MAX_VTABLES)
				{
					const int index = patchedCount;
					PatchedVtable& entry = patched[index];
					entry.vtable = vtable;
					entry.getCapabilities = reinterpret_cast<GetCapabilitiesFunction>(ReadSlot(&vtable[GET_CAPABILITIES_SLOT]));
					// Each original is recorded before its hook goes in, so a game thread that lands in a hook always finds it.
					entry.stateHooked = HookSlot(&vtable[GET_DEVICE_STATE_SLOT], STATE_HOOKS[index], entry.getDeviceState);
					entry.dataHooked = HookSlot(&vtable[GET_DEVICE_DATA_SLOT], DATA_HOOKS[index], entry.getDeviceData);
					// Where the vtable and the chained functions live shows which other hookers are in the chain.
					LOG_INFO("[OverlayInputCapture] DirectInput device vtable is in " << OwnerModule(vtable)
						<< "; GetDeviceState chains to " << OwnerModule(reinterpret_cast<void*>(entry.getDeviceState))
						<< ", GetDeviceData to " << OwnerModule(reinterpret_cast<void*>(entry.getDeviceData)) << "." << std::endl);
					if (!entry.stateHooked || !entry.dataHooked)
						LOG_ERROR("[OverlayInputCapture] Could not patch every DirectInput device vtable slot (GetDeviceState "
							<< (entry.stateHooked ? "ok" : "failed") << ", GetDeviceData " << (entry.dataHooked ? "ok" : "failed") << ")." << std::endl);
					if (entry.stateHooked || entry.dataHooked) ++patchedCount;
					else entry = PatchedVtable{};
				}
				device->Release();
			}
			else LOG_ERROR("[OverlayInputCapture] Could not create a DirectInput mouse device to find its vtable." << std::endl);
			directInput->Release();
		}

		// Puts the original back only if the slot still holds our hook; a slot someone else has changed since keeps
		// their pointer (which may chain to our hook, so the hook stays in place and passes everything through).
		void UnhookSlot(void** slot, void* hook, void* original, const char* name)
		{
			if (SwapSlot(slot, hook, original) != SwapResult::Swapped)
				LOG_INFO("[OverlayInputCapture] Left the DirectInput " << name << " slot alone: another hook replaced ours." << std::endl);
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

	void SetKeyboardCapture(bool shouldCapture)
	{
		keyboardCaptured.store(shouldCapture, std::memory_order_relaxed);
	}

	void Shutdown()
	{
		mouseCaptured.store(false, std::memory_order_relaxed);
		keyboardCaptured.store(false, std::memory_order_relaxed);
		// The table entries stay so a call already inside a hook still finds its original.
		for (int i = 0; i < patchedCount; ++i)
		{
			if (patched[i].stateHooked)
				UnhookSlot(&patched[i].vtable[GET_DEVICE_STATE_SLOT], STATE_HOOKS[i], reinterpret_cast<void*>(patched[i].getDeviceState), "GetDeviceState");
			if (patched[i].dataHooked)
				UnhookSlot(&patched[i].vtable[GET_DEVICE_DATA_SLOT], DATA_HOOKS[i], reinterpret_cast<void*>(patched[i].getDeviceData), "GetDeviceData");
		}
		installed.store(false, std::memory_order_release);
	}
}
