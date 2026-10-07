#include "stdafx.h"
#include "VstHostLink.hpp"
#include "VstHostControl.hpp"

#include <shellapi.h>
#include <algorithm>
#include <filesystem>

namespace VstHostLink
{
	namespace
	{
		constexpr wchar_t kSettingsKey[] = L"Software\\RSMods\\VstHost";
		constexpr wchar_t kHostMutex[] = L"Local\\RocksmithAudioBridge.VstHost";   // held by the running host (Main.cpp)
		constexpr ULONGLONG kSwitchTimeoutMs = 15000;

		HANDLE g_mapping = nullptr;
		VstHostControl::Shared* g_shared = nullptr;
		bool g_openFailed = false;
		ULONGLONG g_lastBeat = 0;
		LONG g_lastHostBeat = 0;
		ULONGLONG g_hostBeatSeenAt = 0;

		std::vector<Plugin> g_plugins;
		bool g_scanned = false;
		std::wstring g_pendingLaunch;     // plugin to start once the running host has exited
		ULONGLONG g_pendingSince = 0;

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

		// The host process is gone only once it releases its single-instance mutex (it clears hostPid earlier,
		// while it is still saving and unloading).
		bool HostProcessGone()
		{
			HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kHostMutex);
			if (!mutex) return true;
			CloseHandle(mutex);
			return false;
		}

		bool HostAlive(VstHostControl::Shared* shared)
		{
			const ULONGLONG now = GetTickCount64();
			const LONG beat = shared->hostBeat;
			if (beat != g_lastHostBeat) { g_lastHostBeat = beat; g_hostBeatSeenAt = now; }
			if (shared->hostPid == 0) return false;
			if (now - g_hostBeatSeenAt < static_cast<ULONGLONG>(VstHostControl::kAliveMs)) return true;
			// A heavy plugin can take seconds to load, and the host's beat pauses meanwhile; it is still there as
			// long as the process is.
			return shared->loading != 0 && !HostProcessGone();
		}

		void Launch(const std::wstring& plugin)
		{
			if (VstHostControl::Shared* shared = Shared()) InterlockedExchange(&shared->quitRequested, 0);
			const std::wstring args = plugin.empty() ? std::wstring() : L"\"" + plugin + L"\"";
			ShellExecuteW(nullptr, L"open", HostPath().c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
		}

		std::wstring ReadLastPlugin()
		{
			wchar_t value[MAX_PATH * 2]{};
			DWORD size = sizeof(value);
			if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"LastPlugin", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
			return value;
		}

		// 64-bit only: the host is a 64-bit program. A single-file .vst3 is a DLL (PE machine field); a bundle
		// folder carries a Contents\x86_64-win build.
		bool Is64Bit(const std::filesystem::path& path)
		{
			std::error_code error;
			if (std::filesystem::is_directory(path, error))
				return std::filesystem::exists(path / L"Contents" / L"x86_64-win", error);
			HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
			if (file == INVALID_HANDLE_VALUE) return false;
			IMAGE_DOS_HEADER dos{};
			IMAGE_FILE_HEADER header{};
			DWORD signature = 0, read = 0;
			bool is64 = false;
			if (ReadFile(file, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE
				&& SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER
				&& ReadFile(file, &signature, sizeof(signature), &read, nullptr) && signature == IMAGE_NT_SIGNATURE
				&& ReadFile(file, &header, sizeof(header), &read, nullptr) && read == sizeof(header))
				is64 = header.Machine == IMAGE_FILE_MACHINE_AMD64;
			CloseHandle(file);
			return is64;
		}

		void Scan()
		{
			g_plugins.clear();
			// This is a 32-bit process, where %CommonProgramFiles% points at the x86 folder; plugins live in the
			// 64-bit one, which CommonProgramW6432 names.
			wchar_t common[MAX_PATH]{};
			if (!GetEnvironmentVariableW(L"CommonProgramW6432", common, MAX_PATH)) wcscpy_s(common, L"C:\\Program Files\\Common Files");
			const std::filesystem::path root = std::filesystem::path(common) / L"VST3";
			std::error_code error;
			std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
			for (; !error && it != end; it.increment(error))
			{
				const std::filesystem::path& path = it->path();
				if (_wcsicmp(path.extension().c_str(), L".vst3") != 0) continue;
				if (it->is_directory(error)) it.disable_recursion_pending();   // a bundle: do not look inside
				if (!Is64Bit(path)) continue;
				Plugin plugin;
				plugin.path = path.wstring();
				const std::wstring stem = path.stem().wstring();
				plugin.name = std::string(stem.begin(), stem.end());
				g_plugins.push_back(plugin);
			}
			std::sort(g_plugins.begin(), g_plugins.end(), [](const Plugin& a, const Plugin& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
			g_scanned = true;
			LOG_INFO("(VST HOST) found " << g_plugins.size() << " 64-bit VST3 plugin(s) in " << root.string() << std::endl);
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
		// A plugin switch: start the new host once the old one has saved its settings and exited.
		if (!g_pendingLaunch.empty() && (HostProcessGone() || now - g_pendingSince > kSwitchTimeoutMs))
		{
			Launch(g_pendingLaunch);
			g_pendingLaunch.clear();
		}
	}

	Snapshot GetSnapshot()
	{
		Snapshot snapshot;
		snapshot.hostInstalled = GetFileAttributesW(HostPath().c_str()) != INVALID_FILE_ATTRIBUTES;
		snapshot.switching = !g_pendingLaunch.empty();
		snapshot.lastPlugin = ReadLastPlugin();
		VstHostControl::Shared* shared = Shared();
		if (!shared) return snapshot;
		snapshot.hostRunning = HostAlive(shared);
		if (snapshot.hostRunning)
		{
			char name[sizeof(shared->pluginName) + 1]{};
			memcpy(name, shared->pluginName, sizeof(shared->pluginName));
			snapshot.pluginName = name;
			wchar_t path[VstHostControl::kPathChars + 1]{};
			memcpy(path, const_cast<const wchar_t*>(shared->pluginPath), sizeof(shared->pluginPath));
			snapshot.currentPlugin = path;
			char error[sizeof(shared->loadError) + 1]{};
			memcpy(error, shared->loadError, sizeof(shared->loadError));
			snapshot.loadError = error;
			snapshot.switching = snapshot.switching || shared->loading != 0 || shared->loadRequest != shared->loadDone;
		}
		snapshot.popoutWanted = shared->popoutWanted != 0;
		snapshot.popoutShown = shared->popoutShown != 0;
		return snapshot;
	}

	const std::vector<Plugin>& Plugins()
	{
		if (!g_scanned) Scan();
		return g_plugins;
	}

	void Rescan() { Scan(); }

	void SetPopout(bool wanted)
	{
		if (VstHostControl::Shared* shared = Shared()) InterlockedExchange(&shared->popoutWanted, wanted ? 1 : 0);
	}

	void StartHost() { Launch({}); }

	void LoadPlugin(const std::wstring& path)
	{
		VstHostControl::Shared* shared = Shared();
		if (!shared || HostProcessGone()) { Launch(path); return; }
		if (HostAlive(shared))
		{
			// The running host swaps the plugin itself (it saves the current one's settings first), keeping the
			// audio link and the popout up.
			if (path.size() >= static_cast<size_t>(VstHostControl::kPathChars)) return;
			memset(shared->loadPath, 0, sizeof(shared->loadPath));
			memcpy(shared->loadPath, path.c_str(), path.size() * sizeof(wchar_t));
			InterlockedIncrement(&shared->loadRequest);   // full barrier: the path is written before the request
			LOG_INFO("(VST HOST) switching plugin in the running host" << std::endl);
			return;
		}
		// A host that is still closing: start the new one once it has saved its settings and exited.
		g_pendingLaunch = path;
		g_pendingSince = GetTickCount64();
	}

	void StopHost()
	{
		g_pendingLaunch.clear();
		if (VstHostControl::Shared* shared = Shared()) InterlockedExchange(&shared->quitRequested, 1);
	}
}
