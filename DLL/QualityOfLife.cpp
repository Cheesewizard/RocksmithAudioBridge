#include "stdafx.h"
#include "QualityOfLife.hpp"

#include <cstring>

namespace QualityOfLife {
	namespace {
		// The two-cable check is call, test, jz, mov, lea, call: 25 bytes from ptr_twoRTCBypass.
		constexpr size_t TWO_RTC_PATCH_SIZE = 25;
		bool twoRtcPatchedByUs = false;
		BYTE twoRtcSavedBytes[TWO_RTC_PATCH_SIZE] = {};
	}

	/// <summary>
	/// Patches Two RTC cables error message.
	/// RS_ASIO 0.6.x NOPs this same check itself. The upstream toggle took any leading NOP for its own patch and,
	/// with the setting off, "restored" 6 bytes from a 3-byte string onto RS_ASIO's NOPs; the leftover bytes decoded
	/// as call 0x910C9CF9 and crashed the game at profile select. So the bytes are saved when we patch, only our own
	/// patch is ever undone, and a check RS_ASIO already removed is left alone.
	/// </summary>
	void PatchTwoRTC()
	{
		if (twoRtcPatchedByUs) return;
		const BYTE* site = reinterpret_cast<const BYTE*>(Offsets::ptr_twoRTCBypass.Get());
		if (site[0] == 0x90) {
			static bool logged = false;
			if (!logged) LOG_INFO("(TWO RTC) The two-cable message check is already removed (RS_ASIO); leaving it." << std::endl);
			logged = true;
			return;
		}

		std::memcpy(twoRtcSavedBytes, site, TWO_RTC_PATCH_SIZE);
		char patch[TWO_RTC_PATCH_SIZE];
		std::fill_n(patch, TWO_RTC_PATCH_SIZE, static_cast<char>(0x90));
		if (MemUtil::PatchAdr(Offsets::ptr_twoRTCBypass, patch, sizeof(patch)))
			twoRtcPatchedByUs = true;
	}

	/// <summary>
	/// Undoes PatchTwoRTC, putting back the bytes it replaced. Does nothing if we did not patch.
	/// </summary>
	void RestoreTwoRTC()
	{
		if (!twoRtcPatchedByUs) return;
		if (MemUtil::PatchAdr(Offsets::ptr_twoRTCBypass, twoRtcSavedBytes, TWO_RTC_PATCH_SIZE))
			twoRtcPatchedByUs = false;
	}

	/// <summary>
	/// RS spawns two processes, one of which complains about Steam not being active. 
	/// We find that one and close it.
	/// </summary>
	HANDLE GetMessageBoxProcess()
	{
		HWND hWnd = FindWindowW(L"#32770", L"Error."); // Dialog box class
		if (!hWnd) {
			return nullptr; 
		}

		DWORD dwProcessId = 0;
		GetWindowThreadProcessId(hWnd, &dwProcessId);
		if (dwProcessId == 0) {
			return nullptr;
		}
		
		return OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE, FALSE, dwProcessId);
	}

	/// <summary>
	/// Stops the game from starting two instances
	/// </summary>
	void StopTwoRSInstances() {
		HANDLE handle = GetMessageBoxProcess();

		if (!handle) {
			return;
		}

		std::vector<wchar_t> processNameBuffer(MAX_PATH);
		HMODULE hMod;

		if (DWORD cNeeded; EnumProcessModulesEx(handle, &hMod, sizeof(hMod), &cNeeded, LIST_MODULES_32BIT | LIST_MODULES_64BIT))
		{
			if (GetModuleBaseNameW(handle, hMod, processNameBuffer.data(), processNameBuffer.size()))
			{
				std::wstring processName(processNameBuffer.data());

				if (processName == L"Rocksmith2014.exe") {
					LOG_INFO("Found parasitic process '" << processName.c_str() << "'. Terminating." << std::endl);

					if (!TerminateProcess(handle, 0)) {
						LOG_ERROR("Failed to terminate process. Error: " << GetLastError() << std::endl);
					}
				}
				else {
					LOG_INFO("Found dialog box, but process '" << processName.c_str() << "' is not the target. Not terminating." << std::endl);
				}
			}
		}

		CloseHandle(handle);
	}
}