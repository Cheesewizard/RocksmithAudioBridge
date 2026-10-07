#include "stdafx.h"
#include "VstHostLink.hpp"
#include "VstHostControl.hpp"
#include "ExternalAmp.hpp"
#include "../GameState.hpp"

#include <ShlObj.h>
#include <shellapi.h>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <atomic>
#include <thread>

namespace VstHostLink
{
	namespace
	{
		using VstHostControl::Command;

		constexpr wchar_t kSettingsKey[] = L"Software\\RSMods\\VstHost";
		constexpr wchar_t kHostMutex[] = L"Local\\RocksmithAudioBridge.VstHost";   // held by the running host (Main.cpp)
		constexpr ULONGLONG kSnapshotMs = 200;
		constexpr ULONGLONG kSongPollMs = 500;
		constexpr ULONGLONG kHungMs = 6000;

		HANDLE g_mapping = nullptr;
		VstHostControl::Shared* g_shared = nullptr;
		bool g_openFailed = false;
		ULONGLONG g_lastBeat = 0;
		LONG g_lastHostBeat = 0;
		ULONGLONG g_hostBeatSeenAt = 0;

		struct PendingCommand { Command op; LONG arg = 0, arg2 = 0; std::wstring text; };
		std::deque<PendingCommand> g_commands;
		LONG g_sentCommand = 0;

		std::vector<Plugin> g_plugins;
		bool g_scanned = false;
		std::atomic<bool> g_rescanWanted{ false };   // set by the folder dialog's thread; the render thread rescans
		std::vector<std::string> g_tones;
		ULONGLONG g_tonesReadAt = 0;

		Snapshot g_snapshot;
		ULONGLONG g_snapshotAt = 0;

		bool g_autoStartChecked = false;
		ULONGLONG g_songPolledAt = 0;
		std::string g_songKey;           // the song being played, empty outside songs
		std::string g_appliedSong;       // the song whose tone was last applied
		std::string g_toneBeforeSong;    // the player's tone before a song tone replaced it

		VstHostControl::Shared* Shared()
		{
			if (!g_shared && !g_openFailed)
			{
				g_shared = VstHostControl::Open(g_mapping);
				g_openFailed = g_shared == nullptr;
			}
			return g_shared;
		}

		std::wstring GameFolder()
		{
			wchar_t exe[MAX_PATH]{};
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			std::wstring path = exe;
			const size_t slash = path.find_last_of(L"\\/");
			return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
		}

		std::wstring HostPath() { return GameFolder() + L"RocksmithAudioBridgeVstHost.exe"; }

		std::wstring DataDirectory()
		{
			PWSTR appData = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData))) return {};
			std::wstring dir = std::wstring(appData) + L"\\RocksmithAudioBridge\\VstHost";
			CoTaskMemFree(appData);
			return dir;
		}

		std::wstring Wide(const std::string& text)
		{
			std::wstring out(text.size(), wchar_t{});
			out.resize(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()))));
			return out;
		}

		std::string Utf8(const std::wstring& text)
		{
			if (text.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
			return out;
		}

		DWORD ReadDword(const wchar_t* name, DWORD fallback)
		{
			DWORD value = 0, size = sizeof(value);
			return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : fallback;
		}

		void WriteDword(const wchar_t* name, DWORD value) { RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value, sizeof(value)); }

		std::wstring ReadString(const wchar_t* name)
		{
			wchar_t value[4096]{};
			DWORD size = sizeof(value);
			if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
			return value;
		}

		void WriteString(const wchar_t* name, const std::wstring& value)
		{
			RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
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
			const std::wstring args = L"--overlay" + (plugin.empty() ? std::wstring() : L" \"" + plugin + L"\"");
			ShellExecuteW(nullptr, L"open", HostPath().c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
		}

		void Queue(Command op, LONG arg = 0, LONG arg2 = 0, std::wstring text = {})
		{
			// A dragged level slider sends a stream of levels; only the newest one matters.
			if (op == Command::SetLevels && !g_commands.empty() && g_commands.back().op == Command::SetLevels)
			{
				g_commands.back().arg = arg;
				g_commands.back().arg2 = arg2;
				return;
			}
			g_commands.push_back({ op, arg, arg2, std::move(text) });
		}

		// One command in flight at a time: the next goes once the host has handled the last.
		void PumpCommands(VstHostControl::Shared* shared, bool hostRunning)
		{
			if (g_commands.empty()) return;
			if (!hostRunning) { if (HostProcessGone()) g_commands.clear(); return; }
			if (shared->commandDone != g_sentCommand && shared->commandSeq == g_sentCommand) return;
			const PendingCommand& next = g_commands.front();
			shared->commandOp = static_cast<LONG>(next.op);
			shared->commandArg = next.arg;
			shared->commandArg2 = next.arg2;
			memset(const_cast<wchar_t*>(shared->commandText), 0, sizeof(shared->commandText));
			wcsncpy_s(const_cast<wchar_t*>(shared->commandText), VstHostControl::kPathChars, next.text.c_str(), _TRUNCATE);
			g_sentCommand = InterlockedIncrement(&shared->commandSeq);   // full barrier: the fields are written first
			g_commands.pop_front();
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

		std::vector<std::wstring> ExtraFolders()
		{
			std::vector<std::wstring> folders;
			const std::wstring list = ReadString(L"ScanFolders");
			size_t start = 0;
			while (start < list.size())
			{
				const size_t end = list.find(L';', start);
				const std::wstring folder = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
				if (!folder.empty()) folders.push_back(folder);
				if (end == std::wstring::npos) break;
				start = end + 1;
			}
			return folders;
		}

		void Scan()
		{
			std::vector<Plugin> plugins;
			// This is a 32-bit process, where %CommonProgramFiles% points at the x86 folder; plugins live in the
			// 64-bit one, which CommonProgramW6432 names.
			wchar_t common[MAX_PATH]{};
			if (!GetEnvironmentVariableW(L"CommonProgramW6432", common, MAX_PATH)) wcscpy_s(common, L"C:\\Program Files\\Common Files");
			std::vector<std::wstring> roots{ (std::filesystem::path(common) / L"VST3").wstring() };
			for (const std::wstring& folder : ExtraFolders()) roots.push_back(folder);
			for (const std::wstring& root : roots)
			{
				std::error_code error;
				std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
				for (; !error && it != end; it.increment(error))
				{
					const std::filesystem::path& path = it->path();
					if (_wcsicmp(path.extension().c_str(), L".vst3") != 0) continue;
					if (it->is_directory(error)) it.disable_recursion_pending();   // a bundle: do not look inside
					if (!Is64Bit(path)) continue;
					const bool duplicate = std::any_of(plugins.begin(), plugins.end(),
						[&](const Plugin& p) { return _wcsicmp(p.path.c_str(), path.c_str()) == 0; });
					if (!duplicate) plugins.push_back({ Utf8(path.stem().wstring()), path.wstring() });
				}
			}
			std::sort(plugins.begin(), plugins.end(), [](const Plugin& a, const Plugin& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
			LOG_INFO("(VST HOST) found " << plugins.size() << " 64-bit VST3 plugin(s) in " << roots.size() << " folder(s)" << std::endl);
			g_plugins = std::move(plugins);
			g_scanned = true;
		}

		void ReadTones()
		{
			g_tones.clear();
			std::error_code error;
			for (const auto& entry : std::filesystem::directory_iterator(DataDirectory() + L"\\Tones", error))
				if (_wcsicmp(entry.path().extension().c_str(), L".rabtone") == 0) g_tones.push_back(Utf8(entry.path().stem().wstring()));
			std::sort(g_tones.begin(), g_tones.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
			g_tonesReadAt = GetTickCount64();
		}

		std::wstring SongTonesFile() { return DataDirectory() + L"\\SongTones.ini"; }

		std::string SongTone(const std::string& song)
		{
			if (song.empty()) return {};
			wchar_t tone[256]{};
			GetPrivateProfileStringW(L"Songs", Wide(song).c_str(), L"", tone, 256, SongTonesFile().c_str());
			return Utf8(tone);
		}

		bool ExclusiveFullscreen()
		{
			return GetPrivateProfileIntW(L"Renderer.Win32", L"Fullscreen", 0, (GameFolder() + L"Rocksmith.ini").c_str()) == 2;
		}

		// Song tones: entering a song with an assigned tone loads it; entering one without, after a song tone was
		// loaded, goes back to the tone the player had before.
		void FollowSong(VstHostControl::Shared* shared, bool hostRunning)
		{
			const ULONGLONG now = GetTickCount64();
			if (now - g_songPolledAt < kSongPollMs) return;
			g_songPolledAt = now;
			g_songKey = GameState::IsInSong() ? GameState::GetSongKey() : std::string();
			if (g_songKey.empty() || g_songKey == g_appliedSong || !hostRunning) return;
			g_appliedSong = g_songKey;
			const std::string tone = SongTone(g_songKey);
			char current[sizeof(shared->toneName) + 1]{};
			memcpy(current, shared->toneName, sizeof(shared->toneName));
			if (!tone.empty())
			{
				if (_stricmp(tone.c_str(), current) == 0) return;
				if (g_toneBeforeSong.empty()) g_toneBeforeSong = current;
				LOG_INFO("(VST HOST) song " << g_songKey << ": loading its tone '" << tone << "'" << std::endl);
				Queue(Command::LoadTone, 0, 0, Wide(tone));
			}
			else if (!g_toneBeforeSong.empty())
			{
				LOG_INFO("(VST HOST) song " << g_songKey << " has no tone of its own: back to '" << g_toneBeforeSong << "'" << std::endl);
				Queue(Command::LoadTone, 0, 0, Wide(g_toneBeforeSong));
				g_toneBeforeSong.clear();
			}
		}

		// Starts the host once per game session, when the player has used it before and the amp link exists.
		void AutoStart(bool hostRunning)
		{
			if (g_autoStartChecked || !GameState::GameLoaded) return;
			const ExternalAmp::Snapshot amp = ExternalAmp::GetSnapshot();
			if (!amp.proxyLoaded) return;
			g_autoStartChecked = true;
			const bool usedBefore = GetFileAttributesW((DataDirectory() + L"\\Current.rabtone").c_str()) != INVALID_FILE_ATTRIBUTES
				|| !ReadString(L"LastPlugin").empty();
			if (hostRunning || !usedBefore || ReadDword(L"AutoStart", 1) == 0) return;
			if (GetFileAttributesW(HostPath().c_str()) == INVALID_FILE_ATTRIBUTES) return;
			LOG_INFO("(VST HOST) starting with the game" << std::endl);
			Launch({});
		}

		void Refresh(VstHostControl::Shared* shared)
		{
			Snapshot s;
			s.hostInstalled = GetFileAttributesW(HostPath().c_str()) != INVALID_FILE_ATTRIBUTES;
			s.autoStart = ReadDword(L"AutoStart", 1) != 0;
			s.exclusiveFullscreen = ExclusiveFullscreen();
			s.songKey = g_songKey;
			s.songTone = SongTone(g_songKey);
			if (shared)
			{
				s.hostRunning = HostAlive(shared);
				s.hostHung = !s.hostRunning && shared->hostPid != 0 && shared->loading == 0 && !HostProcessGone()
					&& GetTickCount64() - g_hostBeatSeenAt > kHungMs;
				if (s.hostRunning)
				{
					const int count = std::clamp(static_cast<int>(shared->slotCount), 0, VstHostControl::kMaxSlots);
					for (int i = 0; i < count; ++i)
					{
						char name[VstHostControl::kNameChars + 1]{};
						memcpy(name, shared->slots[i].name, VstHostControl::kNameChars);
						s.slots.push_back({ name, shared->slots[i].bypass != 0 });
					}
					s.shownSlot = shared->shownSlot;
					char tone[VstHostControl::kNameChars + 1]{};
					memcpy(tone, shared->toneName, VstHostControl::kNameChars);
					s.toneName = tone;
					s.toneModified = shared->toneModified != 0;
					s.inputTenths = shared->inputTenths;
					s.outputTenths = shared->outputTenths;
					char error[sizeof(shared->loadError) + 1]{};
					memcpy(error, shared->loadError, sizeof(shared->loadError));
					s.loadError = error;
				}
				s.busy = shared->loading != 0 || !g_commands.empty() || shared->commandDone != shared->commandSeq;
				s.popoutWanted = shared->popoutWanted != 0;
				s.popoutShown = shared->popoutShown != 0;
			}
			g_snapshot = std::move(s);
			g_snapshotAt = GetTickCount64();
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
		const bool hostRunning = HostAlive(shared);
		AutoStart(hostRunning);
		FollowSong(shared, hostRunning);
		PumpCommands(shared, hostRunning);
	}

	Snapshot GetSnapshot()
	{
		VstHostControl::Shared* shared = Shared();
		if (GetTickCount64() - g_snapshotAt >= kSnapshotMs) Refresh(shared);
		Snapshot s = g_snapshot;
		if (shared && s.hostRunning)
		{
			// Meters every frame; the rest a few times a second (registry and file reads are not free).
			s.inputPeak = shared->inputPeak / 1000.0f;
			s.outputPeak = shared->outputPeak / 1000.0f;
		}
		return s;
	}

	const std::vector<Plugin>& Plugins()
	{
		if (!g_scanned || g_rescanWanted.exchange(false)) Scan();
		return g_plugins;
	}

	void Rescan() { Scan(); }

	std::vector<std::wstring> ScanFolders() { return ExtraFolders(); }

	void AddScanFolder()
	{
		// The folder dialog is modal; on its own thread it never stalls the game's frame.
		std::thread([]
		{
			CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			IFileOpenDialog* dialog = nullptr;
			if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
			{
				DWORD options = 0;
				dialog->GetOptions(&options);
				dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
				dialog->SetTitle(L"Add a folder with VST3 plugins");
				IShellItem* item = nullptr;
				PWSTR path = nullptr;
				if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
				{
					std::wstring list = ReadString(L"ScanFolders");
					if (list.find(path) == std::wstring::npos) WriteString(L"ScanFolders", list.empty() ? path : list + L";" + path);
					CoTaskMemFree(path);
					g_rescanWanted = true;
				}
				if (item) item->Release();
				dialog->Release();
			}
			CoUninitialize();
		}).detach();
	}

	const std::vector<std::string>& Tones()
	{
		if (GetTickCount64() - g_tonesReadAt > 1000) ReadTones();
		return g_tones;
	}

	void OpenTonesFolder()
	{
		const std::wstring folder = DataDirectory() + L"\\Tones";
		SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
		ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	void StartHost() { Launch({}); }

	void StopHost()
	{
		g_commands.clear();
		if (VstHostControl::Shared* shared = Shared()) InterlockedExchange(&shared->quitRequested, 1);
	}

	void ForceCloseHost()
	{
		VstHostControl::Shared* shared = Shared();
		if (!shared || shared->hostPid == 0) return;
		if (HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(shared->hostPid)))
		{
			LOG_WARNING("(VST HOST) not answering; closing it by force" << std::endl);
			TerminateProcess(process, 1);
			CloseHandle(process);
		}
		InterlockedExchange(&shared->hostPid, 0);
		g_commands.clear();
	}

	void SetAutoStart(bool on) { WriteDword(L"AutoStart", on ? 1 : 0); g_snapshotAt = 0; }

	void SetPopout(bool wanted)
	{
		if (VstHostControl::Shared* shared = Shared()) InterlockedExchange(&shared->popoutWanted, wanted ? 1 : 0);
	}

	void AddPlugin(const std::wstring& path)
	{
		VstHostControl::Shared* shared = Shared();
		if (!shared || !HostAlive(shared)) { if (HostProcessGone()) Launch(path); return; }
		Queue(Command::AddPlugin, 0, 0, path);
	}

	void ReplaceSlot(int slot, const std::wstring& path) { Queue(Command::ReplaceSlot, slot, 0, path); }
	void RemoveSlot(int slot) { Queue(Command::RemoveSlot, slot); }
	void MoveSlot(int slot, int delta) { Queue(Command::MoveSlot, slot, delta); }
	void SetBypass(int slot, bool bypass) { Queue(Command::SetBypass, slot, bypass ? 1 : 0); }
	void ShowSlot(int slot) { Queue(Command::ShowSlot, slot); }
	void ClearChain() { Queue(Command::ClearChain); }
	void SetLevels(int inputTenths, int outputTenths) { Queue(Command::SetLevels, inputTenths, outputTenths); }
	void LoadTone(const std::string& name) { g_toneBeforeSong.clear(); Queue(Command::LoadTone, 0, 0, Wide(name)); }
	void SaveTone(const std::string& name) { Queue(Command::SaveTone, 0, 0, Wide(name)); g_tonesReadAt = 0; }
	void DeleteTone(const std::string& name) { Queue(Command::DeleteTone, 0, 0, Wide(name)); g_tonesReadAt = 0; }

	void SetSongTone(const std::string& tone)
	{
		if (g_songKey.empty() || tone.empty()) return;
		SHCreateDirectoryExW(nullptr, DataDirectory().c_str(), nullptr);
		WritePrivateProfileStringW(L"Songs", Wide(g_songKey).c_str(), Wide(tone).c_str(), SongTonesFile().c_str());
		g_appliedSong = g_songKey;   // it is the tone playing now
		g_snapshotAt = 0;
	}

	void ClearSongTone()
	{
		if (g_songKey.empty()) return;
		WritePrivateProfileStringW(L"Songs", Wide(g_songKey).c_str(), nullptr, SongTonesFile().c_str());
		g_snapshotAt = 0;
	}
}
