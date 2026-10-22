#pragma once

#include <Windows.h>
#include <TlHelp32.h>
#include <string>

namespace VstHostLink::HostProcessIdentity
{
	inline bool MatchesProcess(HANDLE process, const std::wstring& expected)
	{
		wchar_t path[32768]{};
		DWORD size = static_cast<DWORD>(_countof(path));
		if (!QueryFullProcessImageNameW(process, 0, path, &size)) return false;
		return _wcsicmp(path, expected.c_str()) == 0 || _wcsicmp(path, (expected + L".previous").c_str()) == 0;
	}

	inline DWORD FindProcess(DWORD preferredPid, bool enumerate, const std::wstring& expected, const wchar_t* windowClass)
	{
		auto verify = [&expected](DWORD pid) -> DWORD
		{
			if (pid == 0) return 0;
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
			if (!process) return 0;
			const bool match = WaitForSingleObject(process, 0) == WAIT_TIMEOUT && MatchesProcess(process, expected);
			CloseHandle(process);
			return match ? pid : 0;
		};

		if (const DWORD verified = verify(preferredPid)) return verified;
		HWND window = FindWindowExW(HWND_MESSAGE, nullptr, windowClass, nullptr);
		DWORD windowPid = 0;
		if (window) GetWindowThreadProcessId(window, &windowPid);
		if (const DWORD verified = verify(windowPid)) return verified;
		if (!enumerate) return 0;

		const auto slash = expected.find_last_of(L"\\/");
		const auto filename = expected.substr(slash == std::wstring::npos ? 0 : slash + 1);
		const auto parkedFilename = filename + L".previous";
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return 0;
		PROCESSENTRY32W entry{ sizeof(entry) };
		if (Process32FirstW(snapshot, &entry))
		{
			do
			{
				if (_wcsicmp(entry.szExeFile, filename.c_str()) == 0
					|| _wcsicmp(entry.szExeFile, parkedFilename.c_str()) == 0)
				{
					if (const DWORD verified = verify(entry.th32ProcessID))
					{
						CloseHandle(snapshot);
						return verified;
					}
				}
			} while (Process32NextW(snapshot, &entry));
		}
		CloseHandle(snapshot);
		return 0;
	}
}
