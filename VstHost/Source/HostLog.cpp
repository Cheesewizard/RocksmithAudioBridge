#include "HostLog.hpp"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>
#include <string>

namespace VstHost
{
	namespace
	{
		std::mutex g_lock;
		FILE* g_file = nullptr;

		// The last run's log is copied to %APPDATA%\RocksmithAudioBridge\VstHost\vst-host-log-previous.txt before
		// it is overwritten (a problem is often reported after the host was started again), outside the game folder.
		// Not while another host still writes it: a second copy started by mistake must not replace the evidence.
		void KeepPreviousRun(const std::wstring& log)
		{
			HANDLE alone = CreateFileW(log.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			if (alone == INVALID_HANDLE_VALUE) return;   // missing, or in use
			CloseHandle(alone);
			wchar_t appData[MAX_PATH]{};
			if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) return;
			const std::wstring folder = std::wstring(appData) + L"\\RocksmithAudioBridge\\VstHost";
			CreateDirectoryW((std::wstring(appData) + L"\\RocksmithAudioBridge").c_str(), nullptr);
			CreateDirectoryW(folder.c_str(), nullptr);
			CopyFileW(log.c_str(), (folder + L"\\vst-host-log-previous.txt").c_str(), FALSE);
		}
	}

	void LogOpen()
	{
		wchar_t exePath[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		std::wstring path = exePath;
		const size_t slash = path.find_last_of(L"\\/");
		path = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"RocksmithAudioBridge-vst-host-log.txt";
		std::lock_guard<std::mutex> guard(g_lock);
		if (g_file) return;
		KeepPreviousRun(path);
		g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);   // readable while the host runs
	}

	void Log(const char* format, ...)
	{
		char text[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);

		SYSTEMTIME now{};
		GetLocalTime(&now);
		std::lock_guard<std::mutex> guard(g_lock);
		if (!g_file) return;
		fprintf(g_file, "%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, text);
		fflush(g_file);
	}
}
