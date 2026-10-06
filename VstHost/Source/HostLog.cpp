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
	}

	void LogOpen()
	{
		wchar_t exePath[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		std::wstring path = exePath;
		const size_t slash = path.find_last_of(L"\\/");
		path = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"RocksmithAudioBridge-vst-host-log.txt";
		std::lock_guard<std::mutex> guard(g_lock);
		if (!g_file) g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);   // readable while the host runs
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
