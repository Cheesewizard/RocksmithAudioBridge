#include "stdafx.h"
#include "VstHostLink.hpp"
#include "VstHostControl.hpp"

#include <shellapi.h>

namespace VstHostLink
{
	namespace
	{
		HANDLE g_mapping = nullptr;
		VstHostControl::Shared* g_shared = nullptr;
		bool g_openFailed = false;
		ULONGLONG g_lastBeat = 0;
		LONG g_lastHostBeat = 0;
		ULONGLONG g_hostBeatSeenAt = 0;

		VstHostControl::Shared* Shared()
		{
			if (!g_shared && !g_openFailed)
			{
				g_shared = VstHostControl::Open(g_mapping);
				g_openFailed = g_shared == nullptr;
			}
			return g_shared;
		}

		std::wstring HostPath()
		{
			wchar_t exe[MAX_PATH]{};
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			std::wstring path = exe;
			const size_t slash = path.find_last_of(L"\\/");
			return (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"RocksmithAudioBridgeVstHost.exe";
		}

		bool HostAlive(VstHostControl::Shared* shared)
		{
			const ULONGLONG now = GetTickCount64();
			const LONG beat = shared->hostBeat;
			if (beat != g_lastHostBeat) { g_lastHostBeat = beat; g_hostBeatSeenAt = now; }
			return shared->hostPid != 0 && now - g_hostBeatSeenAt < static_cast<ULONGLONG>(VstHostControl::kAliveMs);
		}
	}

	void OnFrame(bool overlayVisible)
	{
		VstHostControl::Shared* shared = Shared();
		if (!shared) return;
		shared->overlayVisible = overlayVisible ? 1 : 0;
		const ULONGLONG now = GetTickCount64();
		if (now - g_lastBeat >= static_cast<ULONGLONG>(VstHostControl::kBeatMs) / 2)
		{
			g_lastBeat = now;
			InterlockedIncrement(&shared->overlayBeat);
		}
	}

	Snapshot GetSnapshot()
	{
		Snapshot snapshot;
		snapshot.hostInstalled = GetFileAttributesW(HostPath().c_str()) != INVALID_FILE_ATTRIBUTES;
		VstHostControl::Shared* shared = Shared();
		if (!shared) return snapshot;
		snapshot.hostRunning = HostAlive(shared);
		if (snapshot.hostRunning)
		{
			char name[sizeof(shared->pluginName) + 1]{};
			memcpy(name, shared->pluginName, sizeof(shared->pluginName));
			snapshot.pluginName = name;
		}
		snapshot.popoutWanted = shared->popoutWanted != 0;
		snapshot.popoutShown = shared->popoutShown != 0;
		return snapshot;
	}

	void SetPopout(bool wanted)
	{
		if (VstHostControl::Shared* shared = Shared()) InterlockedExchange(&shared->popoutWanted, wanted ? 1 : 0);
	}

	void StartHost()
	{
		const std::wstring path = HostPath();
		if (VstHostControl::Shared* shared = Shared()) InterlockedExchange(&shared->quitRequested, 0);
		// Opens behind the game only if it has to ask for a plugin; normally it reopens the last one silently.
		ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	void StopHost()
	{
		if (VstHostControl::Shared* shared = Shared()) InterlockedExchange(&shared->quitRequested, 1);
	}
}
