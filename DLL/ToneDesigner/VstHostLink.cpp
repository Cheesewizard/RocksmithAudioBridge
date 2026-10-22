#include "stdafx.h"
#include "VstHostLink.hpp"
#include "ToneSwitchPopup.hpp"
#include "Shared/VstHostControl.hpp"
#include "Shared/ShellOpenAsync.hpp"
#include "ExternalAmp/ExternalAmp.hpp"
#include "SongLibrary.hpp"
#include "GameState.hpp"

#include <ShlObj.h>
#include <shellapi.h>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <atomic>
#include <thread>
#include <mutex>
#include "HostProcessIdentity.hpp"
#include "ToneGates.hpp"

namespace VstHostLink
{
	namespace
	{
		using VstHostControl::Command;

		constexpr wchar_t kSettingsKey[] = L"Software\\RSMods\\VstHost";
		constexpr wchar_t kHostMutex[] = L"Local\\RocksmithAudioBridge.VstHost";   // held by the running host (Main.cpp)
		constexpr ULONGLONG kSnapshotMs = 200;
		constexpr ULONGLONG kSongPollMs = 500;
		constexpr ULONGLONG kPrepareDwellMs = 2000;   // a song highlighted this long gets its tone's plugins loaded
		constexpr ULONGLONG kHungMs = 6000;
		constexpr wchar_t kHostWindowClass[] = L"RocksmithAudioBridgeVstHostMain";

		HANDLE g_mapping = nullptr;
		VstHostControl::Shared* g_shared = nullptr;
		bool g_openFailed = false;
		ULONGLONG g_lastBeat = 0;
		LONG g_lastHostBeat = 0;
		ULONGLONG g_hostBeatSeenAt = 0;

		struct PendingCommand { Command op; LONG arg = 0, arg2 = 0; std::wstring text; };
		std::deque<PendingCommand> g_commands;
		LONG g_sentCommand = 0;
		Command g_sentOp = Command::None;   // the command in flight: a LoadSpare or PrepareTone is a background load

		std::vector<Plugin> g_plugins;
		bool g_scanned = false;
		std::vector<std::wstring> g_failed;   // plugins that did not load this session (the host's failedPath)
		std::wstring g_lastFailed;
		std::atomic<bool> g_rescanWanted{ false };   // set by the folder dialog's thread; the render thread rescans
		std::vector<std::string> g_tones;   // the render thread's copy of SlowState::tones
		unsigned g_tonesVersion = ~0u;

		Snapshot g_snapshot;
		ULONGLONG g_snapshotAt = 0;

		bool g_wasRunning = false;
		bool g_wasStandby = false;      // the host was in standby when last seen (a crash restart keeps it so)
		std::string g_crashNote;
		std::deque<ULONGLONG> g_crashRestarts;   // when the host was restarted after a crash, for the loop guard
		constexpr size_t kMaxCrashRestarts = 3;      // within kCrashWindowMs; then it stays off and says so
		constexpr ULONGLONG kCrashWindowMs = 5 * 60 * 1000;
		// After a crash restart: what crashed, and since when the restarted host has been reloading. The note says
		// "loading again" until the host is idle, then whether the tone came back complete (otherwise a crash while
		// a big plugin loads leaves the note up long after the reload has finished, reading as a broken tone).
		std::string g_recoveringWhat;
		ULONGLONG g_recoveringSince = 0;
		enum class RecoveryResult : int { Idle, Running, Succeeded, Failed };
		std::atomic<int> g_recoveryResult{ static_cast<int>(RecoveryResult::Idle) };
		std::atomic<bool> g_recoveryRestart{ false };
		std::atomic<DWORD> g_recoveryExitedPid{ 0 };
		std::atomic<DWORD> g_launchError{ 0 };
		std::mutex g_recoveryLock;
		std::string g_recoveryError;

		bool g_autoStartChecked = false;
		bool g_mutedForTuning = false;
		ULONGLONG g_tuningPolledAt = 0;
		ULONGLONG g_songPolledAt = 0;
		// The player clicking or typing on the Tone designer page (NotePageInUse). "Always keep loaded" plugins wait
		// for a quiet page: one loading in the background takes the host's one thread for up to half a minute
		// (TH3-64 ~25 s), and New tone, Open tone or Add plugin clicked meanwhile waited behind it with nothing
		// seeming to happen.
		std::atomic<ULONGLONG> g_pageUsedAt{ 0 };
		constexpr ULONGLONG kPageQuietMs = 20000;
		std::string g_songKey;           // the song being played, empty outside songs
		std::string g_appliedSong;       // the song whose tone was last applied
		std::string g_songTarget;        // the tone the current song plays (its own or the default); empty: none set
		std::string g_switchTarget;      // released and loading: the readout names it until it is the loaded tone
		ULONGLONG g_switchAt = 0;
		std::string g_highlighted;       // the song list's highlighted song (its preview plays), and since when
		ULONGLONG g_highlightedAt = 0;
		std::string g_preparedSong;      // the last one whose tone's plugins the host was asked to get ready
		std::string g_lastMenu;
		std::string g_toneBeforeSong;    // the player's tone before a song tone replaced it
		bool g_overlayVisible = false;
		// A song's tone loading (FollowSong queued it): timed, with the plugins loaded from scratch, for SongLoads.ini
		// and the status line on the game screen.
		struct SongLoadWatch
		{
			bool active = false, seenLoading = false;
			std::string song, tone;
			std::vector<std::string> plugins;
			ULONGLONG queuedAt = 0, startedAt = 0;
		} g_watch;

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

		// Looked up once: SHGetKnownFolderPath goes through the shell, too slow to repeat on the render thread.
		std::wstring DataDirectory()
		{
			static const std::wstring dir = []
			{
				PWSTR appData = nullptr;
				if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData))) return std::wstring();
				std::wstring path = std::wstring(appData) + L"\\RocksmithAudioBridge\\VstHost";
				CoTaskMemFree(appData);
				return path;
			}();
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

		bool ProcessPathMatches(HANDLE process)
		{
			return HostProcessIdentity::MatchesProcess(process, HostPath());
		}

		DWORD VerifiedHostProcess(DWORD preferredPid, bool enumerate)
		{
			return HostProcessIdentity::FindProcess(preferredPid, enumerate, HostPath(), kHostWindowClass);
		}

		bool HostAlive(VstHostControl::Shared* shared);
		void Launch(const std::wstring& plugin, bool standby = false);

		void PollRecovery()
		{
			if (const DWORD launchError = g_launchError.exchange(0))
			{
				g_crashNote = "Tone designer could not be started (launch error " + std::to_string(launchError) + ").";
				LOG_ERROR("(VST HOST) launch failed, error " << launchError << std::endl);
				g_snapshotAt = 0;
			}
			const RecoveryResult result = static_cast<RecoveryResult>(g_recoveryResult.load());
			if (result == RecoveryResult::Running || result == RecoveryResult::Idle) return;
			const bool restart = g_recoveryRestart.load();
			const DWORD exitedPid = g_recoveryExitedPid.exchange(0);
			g_recoveryResult.store(static_cast<int>(RecoveryResult::Idle));
			if (result == RecoveryResult::Failed)
			{
				std::lock_guard<std::mutex> lock(g_recoveryLock);
				g_crashNote = g_recoveryError.empty() ? "Tone designer could not be restarted." : g_recoveryError;
				g_snapshotAt = 0;
				return;
			}
			g_commands.clear();
			g_sentCommand = 0;
			g_sentOp = Command::None;
			g_lastHostBeat = 0;
			g_hostBeatSeenAt = 0;
			g_wasRunning = false;
			g_recoveringSince = 0;
			if (exitedPid != 0)
			{
				if (VstHostControl::Shared* shared = Shared()) InterlockedCompareExchange(&shared->hostPid, 0, static_cast<LONG>(exitedPid));
			}
			g_snapshotAt = 0;
			if (restart)
			{
				g_crashNote = "Tone designer restart requested. Your last saved chain will load again.";
				Launch({});
			}
		}

		void BeginRecovery(bool restart, bool allowHealthy)
		{
			int expected = static_cast<int>(RecoveryResult::Idle);
			if (!g_recoveryResult.compare_exchange_strong(expected, static_cast<int>(RecoveryResult::Running))) return;
			VstHostControl::Shared* shared = Shared();
			if (!allowHealthy && shared && HostAlive(shared))
			{
				std::lock_guard<std::mutex> lock(g_recoveryLock);
				g_recoveryError = "Tone designer is responding; it was left running.";
				g_recoveryResult.store(static_cast<int>(RecoveryResult::Failed));
				return;
			}
			g_recoveryRestart.store(restart);
			const DWORD preferredPid = shared ? static_cast<DWORD>(shared->hostPid) : 0;
			std::thread([preferredPid]
			{
				const DWORD pid = VerifiedHostProcess(preferredPid, true);
				if (pid == 0)
				{
					std::lock_guard<std::mutex> lock(g_recoveryLock);
					if (HostProcessGone())
					{
						LOG_INFO("(VST HOST) recovery found no process and no single-instance mutex; treating host as already gone" << std::endl);
						g_recoveryResult.store(static_cast<int>(RecoveryResult::Succeeded));
					}
					else
					{
						g_recoveryError = "Tone designer is still running, but Windows could not identify its process. It could not be restarted.";
						LOG_ERROR("(VST HOST) recovery failed: no verified process while mutex is held (H001)" << std::endl);
						g_recoveryResult.store(static_cast<int>(RecoveryResult::Failed));
					}
					return;
				}
				HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
				if (!process || !ProcessPathMatches(process))
				{
					if (process) CloseHandle(process);
					std::lock_guard<std::mutex> lock(g_recoveryLock);
					g_recoveryError = "The Tone designer process changed before it could be safely closed.";
					LOG_ERROR("(VST HOST) recovery failed: process changed before termination (H002)" << std::endl);
					g_recoveryResult.store(static_cast<int>(RecoveryResult::Failed));
					return;
				}
				LOG_WARNING("(VST HOST) closing verified unresponsive host" << std::endl);
				const bool wasGone = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
				const bool terminated = wasGone || TerminateProcess(process, 1) != FALSE;
				const DWORD failure = terminated ? ERROR_SUCCESS : GetLastError();
				const bool exited = terminated && WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
				CloseHandle(process);
				if (!exited)
				{
					std::lock_guard<std::mutex> lock(g_recoveryLock);
					g_recoveryError = "Tone designer did not exit after the restart was requested.";
					LOG_ERROR("(VST HOST) recovery failed: verified process did not exit; Win32 error " << failure << std::endl);
					g_recoveryResult.store(static_cast<int>(RecoveryResult::Failed));
					return;
				}
				g_recoveryExitedPid.store(pid);
				g_recoveryResult.store(static_cast<int>(RecoveryResult::Succeeded));
			}).detach();
		}

		bool HostAlive(VstHostControl::Shared* shared)
		{
			const ULONGLONG now = GetTickCount64();
			const LONG beat = shared->hostBeat;
			if (beat != g_lastHostBeat) { g_lastHostBeat = beat; g_hostBeatSeenAt = now; }
			if (shared->hostPid == 0) return false;
			if (now - g_hostBeatSeenAt < static_cast<ULONGLONG>(VstHostControl::kAliveMs)) return true;
			// A heavy plugin can take seconds to load, and the host's beat pauses meanwhile; it is still there as
			// long as the process is. Opening a big plugin's window (AmpliTube) pauses it too, for a few seconds:
			// still running up to kHungMs, after which the page offers Force close (hostHung).
			if (HostProcessGone()) return false;
			return shared->loading != 0 || shared->preloading != 0 || now - g_hostBeatSeenAt <= kHungMs;
		}

		// standby: start it off (the game's amp plays), only to have the plugins loaded and ready.
		void Launch(const std::wstring& plugin, bool standby)
		{
			if (VstHostControl::Shared* shared = Shared()) InterlockedExchange(&shared->quitRequested, 0);
			const std::wstring executable = HostPath();
			const std::wstring args = L"--overlay" + std::wstring(standby ? L" --standby" : L"") + (plugin.empty() ? std::wstring() : L" \"" + plugin + L"\"");
			std::thread([executable, args]
			{
				std::wstring commandLine = L"\"" + executable + L"\" " + args;
				STARTUPINFOW startup{ sizeof(startup) };
				startup.dwFlags = STARTF_USESHOWWINDOW;
				startup.wShowWindow = SW_SHOWNOACTIVATE;
				PROCESS_INFORMATION process{};
				if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
				{
					g_launchError.store(GetLastError());
					return;
				}
				CloseHandle(process.hThread);
				CloseHandle(process.hProcess);
			}).detach();
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
			g_sentOp = next.op;
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

		// The value of the first "key": "value" pair after `from` in a moduleinfo.json (JSON5, written by the
		// SDK's moduleinfotool). Enough for the few fields the browser shows; not a JSON parser.
		std::string JsonString(const std::string& json, const char* key, size_t from = 0)
		{
			const size_t at = json.find(std::string("\"") + key + "\"", from);
			if (at == std::string::npos) return {};
			const size_t open = json.find('"', json.find(':', at) + 1);
			const size_t close = open == std::string::npos ? std::string::npos : json.find('"', open + 1);
			return close == std::string::npos ? std::string() : json.substr(open + 1, close - open - 1);
		}

		bool Contains(const std::string& text, const char* word)
		{
			std::string lower = text;
			for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			return lower.find(word) != std::string::npos;
		}

		// Vendor, version and kind. A bundle's Contents\Resources\moduleinfo.json says them; for older plugins the
		// vendor is the folder the plugin sits in (Common Files\VST3\<vendor>\...), and the kind is guessed from the
		// name or left Unknown.
		void Describe(Plugin& plugin, const std::filesystem::path& path, const std::wstring& root)
		{
			std::string json;
			std::error_code error;
			const std::filesystem::path info = path / L"Contents" / L"Resources" / L"moduleinfo.json";
			if (std::filesystem::is_regular_file(info, error) && std::filesystem::file_size(info, error) < (1u << 20))
			{
				if (FILE* file = _wfopen(info.c_str(), L"rb"))
				{
					char buffer[4096];
					size_t read = 0;
					while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) json.append(buffer, read);
					fclose(file);
				}
			}
			std::string categories;
			if (!json.empty())
			{
				plugin.vendor = JsonString(json, "Vendor", json.find("\"Factory Info\""));
				const size_t classes = json.find("\"Classes\"");
				if (classes != std::string::npos) plugin.version = JsonString(json, "Version", classes);
				for (size_t at = json.find("\"Sub Categories\""); at != std::string::npos; at = json.find("\"Sub Categories\"", at + 1))
					categories += json.substr(at, json.find(']', at) - at) + "|";
			}
			if (plugin.vendor.empty())
			{
				const std::filesystem::path parent = path.parent_path();
				if (_wcsicmp(parent.c_str(), root.c_str()) != 0) plugin.vendor = Utf8(parent.filename().wstring());
			}
			if (Contains(categories, "instrument") && !Contains(categories, "\"fx\"")) plugin.kind = Kind::Instrument;
			else if (Contains(categories, "amp") || Contains(categories, "distortion") || Contains(plugin.name, "amp")) plugin.kind = Kind::Amp;
			else if (Contains(categories, "dynamics") || Contains(plugin.name, "gate") || Contains(plugin.name, "comp")) plugin.kind = Kind::Dynamics;
			else if (Contains(categories, "fx")) plugin.kind = Kind::Effect;
		}

		std::vector<std::wstring> ReadList(const wchar_t* name)
		{
			std::vector<std::wstring> items;
			const std::wstring list = ReadString(name);
			size_t start = 0;
			while (start < list.size())
			{
				const size_t end = list.find(L';', start);
				const std::wstring item = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
				if (!item.empty()) items.push_back(item);
				if (end == std::wstring::npos) break;
				start = end + 1;
			}
			return items;
		}

		void WriteList(const wchar_t* name, const std::vector<std::wstring>& items)
		{
			std::wstring list;
			for (const std::wstring& item : items) list += (list.empty() ? L"" : L";") + item;
			WriteString(name, list);
		}

		bool ListHas(const std::vector<std::wstring>& items, const std::wstring& item)
		{
			return std::any_of(items.begin(), items.end(), [&](const std::wstring& i) { return _wcsicmp(i.c_str(), item.c_str()) == 0; });
		}

		std::vector<std::wstring> g_favourites, g_recent, g_favouriteTones;
		bool g_favouritesRead = false, g_recentRead = false, g_favouriteTonesRead = false;

		void RememberRecent(const std::wstring& path)
		{
			std::vector<std::wstring> recent = ReadList(L"RecentPlugins");
			recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; }), recent.end());
			recent.insert(recent.begin(), path);
			if (recent.size() > 8) recent.resize(8);
			WriteList(L"RecentPlugins", recent);
			g_recent = recent;
			g_recentRead = true;
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
					if (duplicate) continue;
					Plugin plugin{ Utf8(path.stem().wstring()), path.wstring() };
					Describe(plugin, path, root);
					plugins.push_back(std::move(plugin));
				}
			}
			std::sort(plugins.begin(), plugins.end(), [](const Plugin& a, const Plugin& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
			LOG_INFO("(VST HOST) found " << plugins.size() << " 64-bit VST3 plugin(s) in " << roots.size() << " folder(s)" << std::endl);
			g_plugins = std::move(plugins);
			g_scanned = true;
		}

		// Tones, and tones in folders one level down as "Folder/Name" (the host's ToneFile maps the same way).
		std::vector<std::string> ReadToneNames()
		{
			std::vector<std::string> tones;
			std::error_code error;
			for (const auto& entry : std::filesystem::directory_iterator(DataDirectory() + L"\\Tones", error))
			{
				if (entry.is_directory(error))
				{
					std::error_code inner;
					for (const auto& file : std::filesystem::directory_iterator(entry.path(), inner))
						if (_wcsicmp(file.path().extension().c_str(), L".rabtone") == 0)
							tones.push_back(Utf8(entry.path().filename().wstring()) + "/" + Utf8(file.path().stem().wstring()));
				}
				else if (_wcsicmp(entry.path().extension().c_str(), L".rabtone") == 0) tones.push_back(Utf8(entry.path().stem().wstring()));
			}
			std::sort(tones.begin(), tones.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
			return tones;
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

		std::wstring SongLoadsFile() { return DataDirectory() + L"\\SongLoads.ini"; }

		// key=value lines of an ini section, any size.
		std::vector<std::pair<std::wstring, std::wstring>> ReadSection(const std::wstring& file, const wchar_t* name)
		{
			std::vector<wchar_t> section(65536);
			DWORD size = 0;
			for (;;)
			{
				size = GetPrivateProfileSectionW(name, section.data(), static_cast<DWORD>(section.size()), file.c_str());
				if (size + 2 < section.size() || section.size() >= (1u << 22)) break;
				section.resize(section.size() * 4);
			}
			std::vector<std::pair<std::wstring, std::wstring>> lines;
			for (const wchar_t* line = section.data(); line < section.data() + size && *line; line += wcslen(line) + 1)
				if (const wchar_t* equals = wcschr(line, L'=')) lines.push_back({ std::wstring(line, equals), equals + 1 });
			return lines;
		}

		std::vector<std::pair<std::string, std::string>> ReadSongTones()
		{
			std::vector<std::pair<std::string, std::string>> songs;
			for (const auto& line : ReadSection(SongTonesFile(), L"Songs")) songs.push_back({ Utf8(line.first), Utf8(line.second) });
			return songs;
		}

		// SongLoads.ini [Loads]: key=seconds|tone|plugins loaded from scratch.
		std::vector<std::pair<std::string, SongLoad>> ReadSongLoads()
		{
			std::vector<std::pair<std::string, SongLoad>> loads;
			for (const auto& line : ReadSection(SongLoadsFile(), L"Loads"))
			{
				const std::string value = Utf8(line.second);
				const size_t a = value.find('|'), b = a == std::string::npos ? a : value.find('|', a + 1);
				if (b == std::string::npos) continue;
				SongLoad load;
				load.seconds = static_cast<float>(atof(value.substr(0, a).c_str()));
				load.tone = value.substr(a + 1, b - a - 1);
				load.plugins = value.substr(b + 1);
				loads.push_back({ Utf8(line.first), load });
			}
			return loads;
		}

		// ---- Slow reads, off the render thread ---------------------------------------------------------------
		// Everything the page needs from a file or the registry (the tones folder, SongTones.ini, Rocksmith.ini,
		// the settings) is read on a worker thread, twice a second or at once when a change kicks it. The render
		// thread only copies the result: a read held up by an antivirus scan must never cost the game a frame.
		struct SlowState
		{
			bool autoStart = true;
			bool muteWhileTuning = false;
			bool exclusiveFullscreen = false;
			std::string songKey;     // the song being played (set by the render thread), and its tone
			std::string songTone;
			std::vector<std::string> tones;
			std::vector<std::pair<std::string, std::string>> songTones;
			std::vector<std::pair<std::string, SongLoad>> songLoads;
			std::vector<std::wstring> scanFolders;
			std::string defaultTone;   // loads when a song without a tone of its own starts (empty: none)
			std::vector<std::wstring> alwaysLoaded;
			std::vector<std::wstring> preloadFailed;
			std::vector<std::pair<std::wstring, std::string>> preloadFailedWhy;
			unsigned version = 0;
		};
		std::mutex g_slowLock;
		SlowState g_slow;
		HANDLE g_slowKick = nullptr;
		std::once_flag g_slowStarted;

		void SlowWorker()
		{
			for (;;)
			{
				std::string songKey;
				{
					std::lock_guard<std::mutex> lock(g_slowLock);
					songKey = g_slow.songKey;
				}
				SlowState fresh;
				fresh.autoStart = ReadDword(L"AutoStart", 1) != 0;
				fresh.muteWhileTuning = ReadDword(L"MuteWhileTuning", 0) != 0;
				fresh.exclusiveFullscreen = ExclusiveFullscreen();
				fresh.songTone = SongTone(songKey);
				fresh.tones = ReadToneNames();
				// Logged when the count changes: an empty library while tone files exist is otherwise a mystery.
				static size_t loggedTones = static_cast<size_t>(-1);
				if (fresh.tones.size() != loggedTones)
				{
					loggedTones = fresh.tones.size();
					std::string names;
					for (const std::string& tone : fresh.tones) names += (names.empty() ? "" : ", ") + tone;
					LOG_INFO("(VST HOST) " << loggedTones << " tone(s) in " << Utf8(DataDirectory()) << "\\Tones: " << names << std::endl);
				}
				fresh.songTones = ReadSongTones();
				fresh.songLoads = ReadSongLoads();
				fresh.scanFolders = ExtraFolders();
				fresh.defaultTone = Utf8(ReadString(L"DefaultTone"));
				fresh.alwaysLoaded = ReadList(L"KeepLoadedPlugins");
				fresh.preloadFailed = ReadList(L"PreloadFailed");
				// The host writes "path|reason" for each: why the "Load failed" pill is showing.
				for (const std::wstring& item : ReadList(L"PreloadFailedWhy"))
				{
					const size_t bar = item.find(L'|');
					if (bar != std::wstring::npos) fresh.preloadFailedWhy.emplace_back(item.substr(0, bar), Utf8(item.substr(bar + 1)));
				}
				{
					std::lock_guard<std::mutex> lock(g_slowLock);
					fresh.songKey = g_slow.songKey;
					if (fresh.songKey != songKey) fresh.songTone = g_slow.songTone;   // the song changed meanwhile: next pass
					fresh.version = g_slow.version + 1;
					g_slow = std::move(fresh);
				}
				WaitForSingleObject(g_slowKick, 500);
			}
		}

		void StartSlowWorker()
		{
			std::call_once(g_slowStarted, []
			{
				g_slowKick = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				std::thread(SlowWorker).detach();
			});
		}

		void KickSlow()
		{
			StartSlowWorker();
			SetEvent(g_slowKick);
		}

		template <typename Read> auto ReadSlow(Read read)
		{
			StartSlowWorker();
			std::lock_guard<std::mutex> lock(g_slowLock);
			return read(g_slow);
		}

		void WatchSongLoad(const std::string& song, const std::string& tone)
		{
			g_watch = SongLoadWatch{};
			g_watch.active = true;
			g_watch.song = song;
			g_watch.tone = tone;
			g_watch.queuedAt = GetTickCount64();
		}

		// Every frame while a song's tone is on its way: the host's `loading` spans the whole LoadTone, and names
		// each plugin it loads from scratch (plugins kept loaded are not named). The result goes to SongLoads.ini.
		void FollowSongLoad(VstHostControl::Shared* shared)
		{
			if (!g_watch.active) return;
			const ULONGLONG now = GetTickCount64();
			if (shared->loading != 0)
			{
				if (!g_watch.seenLoading) { g_watch.seenLoading = true; g_watch.startedAt = now; }
				char name[VstHostControl::kNameChars + 1]{};
				memcpy(name, shared->loadingText, VstHostControl::kNameChars);
				if (name[0] && std::find(g_watch.plugins.begin(), g_watch.plugins.end(), name) == g_watch.plugins.end()) g_watch.plugins.push_back(name);
				return;
			}
			if (!g_watch.seenLoading)
			{
				if (now - g_watch.queuedAt > 20000) g_watch.active = false;   // never started: nothing to record
				return;
			}
			const float seconds = (now - g_watch.startedAt) / 1000.0f;
			std::string plugins;
			for (const std::string& plugin : g_watch.plugins) plugins += (plugins.empty() ? "" : ", ") + plugin;
			char value[64]{};
			sprintf_s(value, "%.1f", seconds);
			const std::string record = std::string(value) + "|" + g_watch.tone + "|" + plugins;
			SHCreateDirectoryExW(nullptr, DataDirectory().c_str(), nullptr);
			WritePrivateProfileStringW(L"Loads", Wide(g_watch.song).c_str(), Wide(record).c_str(), SongLoadsFile().c_str());
			LOG_INFO("(VST HOST) song " << g_watch.song << ": tone '" << g_watch.tone << "' ready in " << value << " s"
				<< (plugins.empty() ? std::string(", every plugin was already loaded") : ", loaded from scratch: " + plugins) << std::endl);
			g_watch.active = false;
			KickSlow();
		}

		// The song list's highlighted song (its preview playing gives its key): after a moment on it, the host loads
		// the plugins its tone (or the default tone) needs as spares, while the chain plays on unchanged. Starting the
		// song then switches at once. The key outside the song list is the last song previewed; asking again for the
		// same song does nothing, and the host skips plugins it has loaded already.
		void PrepareHighlighted(ULONGLONG now)
		{
			const std::string key = GameState::GetSongKey();
			if (key != g_highlighted) { g_highlighted = key; g_highlightedAt = now; return; }
			if (key.empty() || key == g_preparedSong || now - g_highlightedAt < kPrepareDwellMs) return;
			g_preparedSong = key;
			std::string tone = SongTone(key);
			if (tone.empty()) tone = ReadSlow([](SlowState& slow) { return slow.defaultTone; });
			if (tone.empty()) return;
			LOG_INFO("(VST HOST) song " << key << " highlighted: getting its tone '" << tone << "' ready" << std::endl);
			Queue(Command::PrepareTone, 0, 0, Wide(tone));
		}

		// Song tones: entering a song with an assigned tone loads it; entering one without, after a song tone was
		// loaded, goes back to the tone the player had before.
		// A song's tone switches as soon as the song starts (its plugins were loaded at the tuner, so it is quick); the
		// game's tone readout follows the switch (PublishPopupName), it never holds the switch back.
		std::string g_startTone;         // the song's switch, waiting for the song to be playing; empty: none
		bool g_startWatch = false;
		ULONGLONG g_startAt = 0;

		// Entering a song: the switch waits for the song to be playing (the highway moving), the moment the game's own
		// tone readout can show, instead of the game's "in song" flag, which comes while the song is still loading
		// (the tone would change at load and the readout come seconds later).
		void SwitchAtSongStart(const std::string& tone, bool watch)
		{
			g_startTone = tone;
			g_startWatch = watch;
			g_startAt = GetTickCount64();
		}

		void SwitchSongTone(const std::string& tone, bool watch)
		{
			Queue(Command::LoadTone, 0, 0, Wide(tone));
			if (watch) WatchSongLoad(g_songKey, tone);
			g_switchTarget = tone;
			g_switchAt = GetTickCount64();
		}

		void FollowSong(VstHostControl::Shared* shared, bool hostRunning)
		{
			const ULONGLONG now = GetTickCount64();
			if (now - g_songPolledAt < kSongPollMs) return;
			g_songPolledAt = now;
			const bool inSong = GameState::IsInSong();
			const std::string menu = GameState::GetCurrentMenu();
			// The tuner and calibration before a song: the song is chosen, so its tone can load now (the player tunes
			// meanwhile). SelectionListDialog is left out: the game uses it for other lists too.
			const bool aboutToPlay = !inSong && (menu.find("PreSongTuner") != std::string::npos || menu.find("CalibrationMeter_PreGame") != std::string::npos);
			// The host loads starred plugins in the background only outside songs (one starting up costs CPU), and
			// not while the player is using the page (g_pageUsedAt).
			const ULONGLONG usedAt = g_pageUsedAt.load();
			const bool pageBusy = usedAt != 0 && now - usedAt < kPageQuietMs;
			shared->preloadAllowed = inSong || pageBusy ? 0 : 1;
			if (menu != g_lastMenu)
			{
				g_lastMenu = menu;
				LOG_INFO("(VST HOST) menu " << menu << ", song key " << GameState::GetSongKey() << std::endl);
			}
			if (!inSong && !aboutToPlay && hostRunning) PrepareHighlighted(now);
			const std::string songKey = inSong || aboutToPlay ? GameState::GetSongKey() : std::string();
			if (songKey != g_songKey)
			{
				g_songKey = songKey;
				g_switchTarget.clear();
				g_startTone.clear();
				// The tone this song will play (its own, else the default; empty: whatever is loaded stays), named by the
				// game's own tone readout (ToneSwitchPopup). Read once per song.
				g_songTarget = songKey.empty() ? std::string() : SongTone(songKey);
				if (g_songTarget.empty() && !songKey.empty()) g_songTarget = ReadSlow([](SlowState& slow) { return slow.defaultTone; });
				ReadSlow([&](SlowState& slow) { slow.songKey = songKey; slow.songTone.clear(); return 0; });
				KickSlow();
			}
			// Out of the song: the next start of any song, the same one included, applies its tone again. (Without
			// this, a tone assigned during a song never loaded when that song was played again.)
			if (g_songKey.empty()) { g_appliedSong.clear(); return; }
			if (g_songKey == g_appliedSong || !hostRunning) return;
			if (!inSong)
			{
				// Tuner and calibration: only get the tone's plugins ready. The chain switches when the song starts, so
				// the sound and the game's "TONE SWITCH" readout change together (with the plugins loaded the switch
				// takes about 0.3 s).
				if (!g_songTarget.empty() && g_preparedSong != g_songKey)
				{
					g_preparedSong = g_songKey;
					LOG_INFO("(VST HOST) song " << g_songKey << " chosen: getting its tone '" << g_songTarget << "' ready for the start" << std::endl);
					Queue(Command::PrepareTone, 0, 0, Wide(g_songTarget));
				}
				return;
			}
			// Entering a song is rare: this one read stays here, so the song's tone loads without waiting a pass.
			g_appliedSong = g_songKey;
			const std::string tone = SongTone(g_songKey);
			LOG_INFO("(VST HOST) song " << g_songKey << " started: " << (tone.empty() ? std::string("no tone of its own") : "its tone is '" + tone + "'") << std::endl);
			char current[sizeof(shared->toneName) + 1]{};
			memcpy(current, shared->toneName, sizeof(shared->toneName));
			// A chain with unsaved changes is work in progress: it keeps playing through the song instead of the song's
			// tone or the default tone (a song start would load over it, skipping the page's "Discard changes?",
			// and the edits would be gone; restoring afterwards only knows the tone's name). The readout names the chain.
			if (shared->toneModified != 0 && shared->slotCount > 0)
			{
				const std::string skipped = !tone.empty() ? tone : ReadSlow([](SlowState& slow) { return slow.defaultTone; });
				if (!skipped.empty() && _stricmp(skipped.c_str(), current) != 0)
					LOG_INFO("(VST HOST) song " << g_songKey << ": the chain has unsaved changes, it keeps playing instead of '" << skipped << "'" << std::endl);
				g_songTarget.clear();
				g_toneBeforeSong.clear();   // the work in progress is the player's tone now: nothing to go back to
				return;
			}
			if (!tone.empty())
			{
				if (_stricmp(tone.c_str(), current) == 0) return;
				if (g_toneBeforeSong.empty()) g_toneBeforeSong = current;
				LOG_INFO("(VST HOST) song " << g_songKey << ": loading its tone '" << tone << "'" << std::endl);
				SwitchAtSongStart(tone, true);
			}
			else if (const std::string fallback = ReadSlow([](SlowState& slow) { return slow.defaultTone; }); !fallback.empty())
			{
				// A song without its own tone gets the default tone, when the player has set one.
				g_toneBeforeSong.clear();
				if (_stricmp(fallback.c_str(), current) == 0) return;
				LOG_INFO("(VST HOST) song " << g_songKey << " has no tone of its own: loading the default tone '" << fallback << "'" << std::endl);
				SwitchAtSongStart(fallback, true);
			}
			else if (!g_toneBeforeSong.empty())
			{
				LOG_INFO("(VST HOST) song " << g_songKey << " has no tone of its own: back to '" << g_toneBeforeSong << "'" << std::endl);
				SwitchAtSongStart(g_toneBeforeSong, false);
				g_toneBeforeSong.clear();
			}
		}

		// The name the game's tone readout shows while the Tone designer is what the player hears: the current song's
		// tone, else the chain loaded now. Empty (the game's own text) when the host is off or Game amp is chosen.
		void PublishPopupName(VstHostControl::Shared* shared, bool hostRunning)
		{
			const auto shortName = [](std::string name)
			{
				const size_t slash = name.find_last_of("/\\");   // "Tests/Test 1" reads as "Test 1"
				return slash == std::string::npos ? name : name.substr(slash + 1);
			};
			char loaded[VstHostControl::kNameChars + 1]{};
			if (shared) memcpy(loaded, shared->toneName, VstHostControl::kNameChars);
			const ULONGLONG now = GetTickCount64();
			if (!g_switchTarget.empty() && (_stricmp(g_switchTarget.c_str(), loaded) == 0 || now - g_switchAt > 10000)) g_switchTarget.clear();
			std::string name;
			if (shared && hostRunning && !ExternalAmp::GetSnapshot().comparing)
			{
				// The tone switching in now, else the chain loaded now.
				name = shortName(!g_switchTarget.empty() ? g_switchTarget : (loaded[0] ? std::string(loaded) : std::string("Unsaved tone")));
			}
			ToneSwitchPopup::SetToneName(name);

			// The game never shows its tone readout for our chain (it does only when its own tone slot changes), so it
			// is asked to (ToneSwitchPopup::ShowNow) as an event of each tone switch during a song: once the tone has
			// loaded and the song is playing (shown while the song is still loading, the game clears it at once). Checked
			// every 250 ms, up to 60 s per tone.
			static std::string announced, pending;
			static ULONGLONG checkedAt = 0, pendingSince = 0;
			if (now - checkedAt < 250) return;
			checkedAt = now;
			if (name.empty() || !GameState::IsInSong()) { announced.clear(); pending.clear(); return; }
			if (!g_startTone.empty())
			{
				// Song start: the switch goes once the song is playing (or after 30 s regardless); the readout then
				// follows the load below like any other switch.
				if (!ToneSwitchPopup::SongPlaying() && now - g_startAt < 30000) return;
				LOG_INFO("(VST HOST) song " << g_songKey << " is playing: switching to '" << g_startTone << "'" << std::endl);
				SwitchSongTone(g_startTone, g_startWatch);
				g_startTone.clear();
				return;
			}
			const bool idle = shared->loading == 0 && g_commands.empty() && shared->commandDone == shared->commandSeq;
			if (!idle || !loaded[0] || _stricmp(announced.c_str(), loaded) == 0) return;
			if (pending != loaded) { pending = loaded; pendingSince = now; }
			if (ToneSwitchPopup::ShowNow() || now - pendingSince > 60000) announced = loaded;
		}

		// A plugin that crashes takes the host with it. The host leaves a note as it goes (LastCrash = the module,
		// LastCrashAt = seconds since boot, Main.cpp OnCrash); seeing the host gone with a fresh note, the overlay
		// starts it again with the last autosaved chain and tells the player which plugin it was. After three
		// restarts within five minutes the next crash leaves it off, so a plugin that crashes on load cannot loop.
		void FollowCrashes(VstHostControl::Shared* shared, bool hostRunning)
		{
			if (g_recoveringSince && shared)
			{
				const ULONGLONG since = GetTickCount64() - g_recoveringSince;
				const bool idle = hostRunning && shared->loading == 0 && g_commands.empty() && shared->commandDone == shared->commandSeq;
				if (idle && since > 1500)
				{
					char tone[VstHostControl::kNameChars + 1]{};
					memcpy(tone, shared->toneName, VstHostControl::kNameChars);
					char error[sizeof(shared->loadError) + 1]{};
					memcpy(error, shared->loadError, sizeof(shared->loadError));
					const std::string name = tone[0] ? "'" + std::string(tone) + "'" : std::string("your last autosave");
					g_crashNote = g_recoveringWhat + ". Tone designer restarted and loaded " + name + " again"
						+ (error[0] ? std::string(", but: ") + error : std::string(": the tone is complete."));
					LOG_INFO("(VST HOST) back after the crash: " << name << (error[0] ? std::string(" with a problem: ") + error : std::string(" complete")) << std::endl);
					g_recoveringSince = 0;
					g_snapshotAt = 0;
				}
				else if (!hostRunning && since > 60000)
				{
					g_crashNote = g_recoveringWhat + ". Tone designer did not come back after restarting; start it again from the power button.";
					g_recoveringSince = 0;
					g_snapshotAt = 0;
				}
			}
			if (hostRunning) { g_wasRunning = true; g_wasStandby = shared && shared->standby != 0; return; }
			if (!g_wasRunning || !HostProcessGone()) return;
			g_wasRunning = false;
			const ULONGLONG now = GetTickCount64();
			const DWORD crashedAt = ReadDword(L"LastCrashAt", 0);
			if (crashedAt == 0 || now / 1000 - crashedAt > 15 || GameState::GameClosing) return;
			// LastCrash is the plugin the host could name (the faulting module, a plugin frame on the crashing
			// thread, or the plugin that started the thread); empty when the fault surfaced elsewhere with no plugin
			// in sight. Say only what is known.
			const std::string plugin = Utf8(ReadString(L"LastCrash"));
			const std::string module = Utf8(ReadString(L"LastCrashModule"));
			const std::string loading = Utf8(ReadString(L"LastCrashLoading"));
			const std::string graphics = Utf8(ReadString(L"LastCrashGraphics"));
			const std::string windows = Utf8(ReadString(L"LastCrashWindows"));
			WriteDword(L"LastCrashAt", 0);   // handled
			std::string what;
			if (!plugin.empty()) what = plugin + " crashed (a fault in that plugin's code)";
			else if (!graphics.empty())
				what = "Tone designer crashed in the graphics driver (" + graphics + ")" + (windows.empty() ? std::string()
					: ", most likely from " + windows + ": " + (windows.find(',') == std::string::npos ? "its window was the only one drawing with the graphics card" : "their windows were the only ones drawing with the graphics card"));
			else if (!loading.empty()) what = "Tone designer crashed while loading " + loading + " (the fault surfaced in " + (module.empty() ? std::string("Windows") : module) + ", so the plugin cannot be named for certain)";
			else what = "Tone designer crashed (in " + (module.empty() ? std::string("an unknown module") : module) + ")";
			while (!g_crashRestarts.empty() && now - g_crashRestarts.front() > kCrashWindowMs) g_crashRestarts.pop_front();
			if (g_crashRestarts.size() >= kMaxCrashRestarts) {
				g_crashNote = what + ", for the fourth time in five minutes, so Tone designer stays off. Start it again when ready; if one plugin keeps crashing, remove or replace it.";
				LOG_WARNING("(VST HOST) " << what << "; fourth crash in five minutes, not restarting" << std::endl);
				return;
			}
			g_crashRestarts.push_back(now);
			g_crashNote = what + ". Tone designer is restarting and loading your tone again...";
			LOG_WARNING("(VST HOST) " << what << "; restarting it" << std::endl);
			g_commands.clear();
			g_recoveringWhat = what;
			g_recoveringSince = now;
			// The restart brings back the last autosave, which may not be the song's tone (the crash can come while
			// that tone loads, and its queued load was just dropped): apply the song's tone again once the host runs.
			g_appliedSong.clear();
			g_preparedSong.clear();
			Launch({}, g_wasStandby);
		}

		// Starts the host once per game session, when the player has used it before and the amp link exists. With
		// "Start with the game" off but plugins marked "Always keep loaded", it starts in standby: off, the game's amp
		// playing, those plugins loading in the background so turning Tone designer on is instant.
		void AutoStart(bool hostRunning)
		{
			if (g_autoStartChecked || !GameState::GameLoaded) return;
			const ExternalAmp::Snapshot amp = ExternalAmp::GetSnapshot();
			if (!amp.proxyLoaded) return;
			g_autoStartChecked = true;
			const bool usedBefore = GetFileAttributesW((DataDirectory() + L"\\Current.rabtone").c_str()) != INVALID_FILE_ATTRIBUTES
				|| !ReadString(L"LastPlugin").empty();
			const bool on = usedBefore && ReadDword(L"AutoStart", 1) != 0;
			const bool standby = !on && !ReadList(L"KeepLoadedPlugins").empty();
			if (hostRunning || (!on && !standby)) return;
			if (GetFileAttributesW(HostPath().c_str()) == INVALID_FILE_ATTRIBUTES) return;
			LOG_INFO("(VST HOST) starting with the game" << (standby ? " in standby, to load the always-loaded plugins" : "") << std::endl);
			Launch({}, standby);
		}

		// Mute while tuning: the chain plays nothing in the tuner screens, so tuning is not through a wall of gain.
		void FollowTuner(bool hostRunning)
		{
			const ULONGLONG now = GetTickCount64();
			if (now - g_tuningPolledAt < kSongPollMs) return;
			g_tuningPolledAt = now;
			const bool wanted = hostRunning && ReadSlow([](SlowState& slow) { return slow.muteWhileTuning; }) && GameState::GameLoaded
				&& GameState::Menus::IsInTuningMenus();
			if (wanted == g_mutedForTuning) return;
			g_mutedForTuning = wanted;
			Queue(Command::SetMuted, wanted ? 1 : 0);
		}

		void Refresh(VstHostControl::Shared* shared)
		{
			Snapshot s;
			s.hostInstalled = GetFileAttributesW(HostPath().c_str()) != INVALID_FILE_ATTRIBUTES;
			ReadSlow([&](SlowState& slow)
			{
				s.autoStart = slow.autoStart;
				s.muteWhileTuning = slow.muteWhileTuning;
				s.exclusiveFullscreen = slow.exclusiveFullscreen;
				s.songTone = slow.songKey == g_songKey ? slow.songTone : std::string();
				s.defaultTone = slow.defaultTone;
				s.alwaysLoaded = slow.alwaysLoaded;
				s.preloadFailed = slow.preloadFailed;
				s.preloadFailedWhy = slow.preloadFailedWhy;
				return 0;
			});
			s.songKey = g_songKey;
		s.crashNote = g_crashNote;
			if (shared)
			{
				s.hostRunning = HostAlive(shared);
				s.standby = s.hostRunning && shared->standby != 0;
				s.hostHung = !s.hostRunning && shared->hostPid != 0 && shared->loading == 0 && shared->preloading == 0 && !HostProcessGone()
					&& GetTickCount64() - g_hostBeatSeenAt > kHungMs;
				s.hostRecovering = g_recoveryResult.load() == static_cast<int>(RecoveryResult::Running);
				s.hostDisconnected = !s.hostRunning && !s.hostHung
					&& (!HostProcessGone() || VerifiedHostProcess(static_cast<DWORD>(shared->hostPid), false) != 0);
				if (s.hostRunning)
				{
					const int count = std::clamp(static_cast<int>(shared->slotCount), 0, VstHostControl::kMaxSlots);
					for (int i = 0; i < count; ++i)
					{
						char name[VstHostControl::kNameChars + 1]{};
						memcpy(name, shared->slots[i].name, VstHostControl::kNameChars);
						wchar_t path[VstHostControl::kPathChars + 1]{};
						memcpy(path, const_cast<const wchar_t*>(shared->slotPaths[i]), sizeof(shared->slotPaths[i]));
						s.slots.push_back({ name, path, shared->slots[i].bypass != 0, (shared->openWindows & (1 << i)) != 0 });
					}
					s.chainBypassed = shared->chainBypassed != 0;
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
					const int spares = std::clamp(static_cast<int>(shared->spareCount), 0, VstHostControl::kMaxSpares);
					for (int i = 0; i < spares; ++i)
					{
						wchar_t path[VstHostControl::kPathChars + 1]{};
						memcpy(path, const_cast<const wchar_t*>(shared->sparePaths[i]), sizeof(shared->sparePaths[i]));
						s.spares.push_back(path);
					}
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
		PollRecovery();
		g_overlayVisible = overlayVisible;
		VstHostControl::Shared* shared = Shared();
		if (!shared) return;
		shared->overlayVisible = overlayVisible ? 1 : 0;
		FollowSongLoad(shared);
		const ULONGLONG now = GetTickCount64();
		if (now - g_lastBeat >= static_cast<ULONGLONG>(VstHostControl::kBeatMs) / 2)
		{
			g_lastBeat = now;
			InterlockedIncrement(&shared->overlayBeat);
		}
		const bool hostRunning = HostAlive(shared);
		// Standby (off, plugins kept loaded): the game's amp plays, so songs, the tone readout and the tuner mute
		// leave the chain alone. Turning it on applies the song's tone then (g_appliedSong was never set).
		const bool on = hostRunning && shared->standby == 0;
		// A plugin that did not load is marked in the browser for the rest of the session (or until a rescan).
		wchar_t failed[VstHostControl::kPathChars + 1]{};
		memcpy(failed, const_cast<const wchar_t*>(shared->failedPath), sizeof(shared->failedPath));
		if (failed[0] && g_lastFailed != failed)
		{
			g_lastFailed = failed;
			if (!ListHas(g_failed, failed)) g_failed.push_back(failed);
		}
		AutoStart(hostRunning);
		FollowCrashes(shared, hostRunning);
		FollowSong(shared, on);
		PublishPopupName(shared, on);
		FollowTuner(on);
		PumpCommands(shared, hostRunning);
		// The Noise gate follows the tone that plays: a saved tone's own gate, else the global one (ToneGates.hpp).
		// Off, standby or an unsaved chain: the global gate.
		std::string playingTone;
		if (on)
		{
			char tone[VstHostControl::kNameChars + 1]{};
			memcpy(tone, shared->toneName, VstHostControl::kNameChars);
			playingTone = tone;
		}
		ToneGates::SetActiveTone(playingTone);
	}

	bool IsLoading()
	{
		VstHostControl::Shared* shared = g_shared;
		return shared && (shared->loading != 0 || shared->commandDone != shared->commandSeq);
	}

	int ChainLatencySamples()
	{
		VstHostControl::Shared* shared = g_shared;
		return shared && shared->hostPid != 0 ? static_cast<int>(shared->chainLatencySamples) : -1;
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
		// Loading, every frame: a heavy plugin takes seconds and the page shows it moving the whole time.
		static ULONGLONG loadingSince = 0;
		if (shared && shared->loading != 0)
		{
			if (loadingSince == 0) loadingSince = GetTickCount64();
			char name[VstHostControl::kNameChars + 1]{};
			memcpy(name, shared->loadingText, VstHostControl::kNameChars);
			s.loading = true;
			s.loadingName = name;
			s.loadingStep = shared->loadingStep;
			s.loadingTotal = shared->loadingTotal;
			s.loadingSeconds = (GetTickCount64() - loadingSince) / 1000.0f;
		}
		else loadingSince = 0;
		if (shared && shared->preloading != 0)
		{
			char name[VstHostControl::kNameChars + 1]{};
			memcpy(name, shared->preloadText, VstHostControl::kNameChars);
			s.preloadingName = name;
			// The host loads it on its one thread, so commands wait meanwhile (one from the page, LoadSpare or
			// PrepareTone, is in flight for it; the "Always keep loaded" step runs with none in flight).
			s.backgroundOnly = shared->loading == 0;
			const bool sentWaits = shared->commandDone != shared->commandSeq && g_sentOp != Command::LoadSpare && g_sentOp != Command::PrepareTone;
			s.waitingOnBackground = !g_commands.empty() || sentWaits;
		}
		return s;
	}

	const std::vector<Plugin>& Plugins()
	{
		if (!g_scanned || g_rescanWanted.exchange(false)) Scan();
		return g_plugins;
	}

	void Rescan() { g_failed.clear(); Scan(); }

	std::vector<std::wstring> ScanFolders() { return ReadSlow([](SlowState& slow) { return slow.scanFolders; }); }

	void ShowPluginFile(const std::wstring& path)
	{
		ShellOpenAsync::Open(L"explorer.exe", L"/select,\"" + path + L"\"");
	}

	// The browser asks for every row every frame, so both lists are read from the registry once and kept.
	bool IsFavourite(const std::wstring& path)
	{
		if (!g_favouritesRead) { g_favourites = ReadList(L"FavouritePlugins"); g_favouritesRead = true; }
		return ListHas(g_favourites, path);
	}

	void SetFavourite(const std::wstring& path, bool favourite)
	{
		IsFavourite(path);
		g_favourites.erase(std::remove_if(g_favourites.begin(), g_favourites.end(), [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; }), g_favourites.end());
		if (favourite) g_favourites.push_back(path);
		WriteList(L"FavouritePlugins", g_favourites);
	}

	std::vector<std::wstring> RecentPlugins()
	{
		if (!g_recentRead) { g_recent = ReadList(L"RecentPlugins"); g_recentRead = true; }
		return g_recent;
	}

	bool FailedToLoad(const std::wstring& path) { return ListHas(g_failed, path); }

	bool IsHostGuest(const std::string& guestName) { return _stricmp(guestName.c_str(), "RocksmithAudioBridgeVstHost.exe") == 0; }

	std::vector<std::string> SongsUsingTone(const std::string& tone)
	{
		std::vector<std::string> songs;
		const std::wstring wanted = Wide(tone);
		for (const auto& line : ReadSection(SongTonesFile(), L"Songs"))
			if (_wcsicmp(line.second.c_str(), wanted.c_str()) == 0) songs.push_back(Utf8(line.first));
		return songs;
	}

	size_t SongToneCount() { return ReadSlow([](SlowState& slow) { return slow.songTones.size(); }); }

	std::vector<std::pair<std::string, std::string>> SongToneList() { return ReadSlow([](SlowState& slow) { return slow.songTones; }); }

	std::vector<std::pair<std::string, SongLoad>> SongLoads() { return ReadSlow([](SlowState& slow) { return slow.songLoads; }); }

	void SetToneForSong(const std::string& song, const std::string& tone)
	{
		if (song.empty()) return;
		SHCreateDirectoryExW(nullptr, DataDirectory().c_str(), nullptr);
		WritePrivateProfileStringW(L"Songs", Wide(song).c_str(), tone.empty() ? nullptr : Wide(tone).c_str(), SongTonesFile().c_str());
		ReadSlow([&](SlowState& slow)
		{
			slow.songTones.erase(std::remove_if(slow.songTones.begin(), slow.songTones.end(),
				[&](const auto& entry) { return _stricmp(entry.first.c_str(), song.c_str()) == 0; }), slow.songTones.end());
			if (!tone.empty()) slow.songTones.push_back({ song, tone });
			if (slow.songKey == song) slow.songTone = tone;
			return 0;
		});
		if (_stricmp(song.c_str(), g_songKey.c_str()) == 0) g_appliedSong.clear();   // playing now: apply it (FollowSong)
		if (_stricmp(song.c_str(), g_preparedSong.c_str()) == 0) g_preparedSong.clear();
		KickSlow();
		g_snapshotAt = 0;
	}

	bool HostProcessRunning() { return !HostProcessGone(); }

	void ReloadSettings()
	{
		g_favouritesRead = g_recentRead = g_favouriteTonesRead = false;
		g_appliedSong.clear();
		g_preparedSong.clear();
		KickSlow();
		g_snapshotAt = 0;
	}


	bool IsFavouriteTone(const std::string& tone)
	{
		if (!g_favouriteTonesRead) { g_favouriteTones = ReadList(L"FavouriteTones"); g_favouriteTonesRead = true; }
		return ListHas(g_favouriteTones, Wide(tone));
	}

	void SetFavouriteTone(const std::string& tone, bool favourite)
	{
		IsFavouriteTone(tone);
		const std::wstring name = Wide(tone);
		g_favouriteTones.erase(std::remove_if(g_favouriteTones.begin(), g_favouriteTones.end(), [&](const std::wstring& t) { return _wcsicmp(t.c_str(), name.c_str()) == 0; }), g_favouriteTones.end());
		if (favourite) g_favouriteTones.push_back(name);
		WriteList(L"FavouriteTones", g_favouriteTones);
	}

	unsigned SectionsOpen(unsigned fallback) { return ReadDword(L"ToneDesignerSections", fallback); }
	void SetSectionsOpen(unsigned bits) { WriteDword(L"ToneDesignerSections", bits); }
	int BottomPaneExtra() { return static_cast<int>(ReadDword(L"ToneDesignerPaneExtra", 0)); }
	void SetBottomPaneExtra(int pixels) { WriteDword(L"ToneDesignerPaneExtra", static_cast<DWORD>(pixels)); }

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
					if (!ListHas(ExtraFolders(), path)) WriteString(L"ScanFolders", list.empty() ? path : list + L";" + path);
					CoTaskMemFree(path);
					g_rescanWanted = true;
					KickSlow();
				}
				if (item) item->Release();
				dialog->Release();
			}
			CoUninitialize();
		}).detach();
	}

	const std::vector<std::string>& Tones()
	{
		ReadSlow([](SlowState& slow)
		{
			if (slow.version != g_tonesVersion) { g_tones = slow.tones; g_tonesVersion = slow.version; }
			return 0;
		});
		return g_tones;
	}

	std::wstring TonesFolder() { return DataDirectory() + L"\\Tones"; }
	std::wstring DataFolder() { return DataDirectory(); }

	void OpenTonesFolder()
	{
		const std::wstring folder = TonesFolder();
		SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
		ShellOpenAsync::Open(folder);
	}

	// A host in standby turns on at once; otherwise a new one starts (on, or in standby).
	void StartHost(bool standby)
	{
		g_crashRestarts.clear();
		g_crashNote.clear();
		g_recoveringSince = 0;
		VstHostControl::Shared* shared = Shared();
		if (shared && HostAlive(shared)) { if (!standby && shared->standby != 0) Queue(Command::SetStandby, 0); g_snapshotAt = 0; return; }
		if (!HostProcessGone() || VerifiedHostProcess(shared ? static_cast<DWORD>(shared->hostPid) : 0, false) != 0)
		{
			g_crashNote = "Tone designer is already running but is not connected. Use Restart Tone designer to recover it.";
			g_snapshotAt = 0;
			return;
		}
		Launch({}, standby);
	}

	void TurnOff()
	{
		Queue(Command::SetStandby, 1);
		g_snapshotAt = 0;
	}

	void StopHost()
	{
		g_commands.clear();
		if (VstHostControl::Shared* shared = Shared()) InterlockedExchange(&shared->quitRequested, 1);
	}

	void ForceCloseHost()
	{
		BeginRecovery(false, false);
	}

	void RestartHost()
	{
		g_crashRestarts.clear();
		g_crashNote.clear();
		g_recoveringSince = 0;
		BeginRecovery(true, true);
	}

	void DismissCrashNote() { g_crashNote.clear(); g_snapshotAt = 0; }

	void SetAutoStart(bool on)
	{
		WriteDword(L"AutoStart", on ? 1 : 0);
		ReadSlow([&](SlowState& slow) { slow.autoStart = on; return 0; });
		g_snapshotAt = 0;
	}

	namespace
	{
		void SetListed(const wchar_t* list, const std::wstring& path, bool on)
		{
			std::vector<std::wstring> items = ReadList(list);
			items.erase(std::remove_if(items.begin(), items.end(), [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; }), items.end());
			if (on) items.push_back(path);
			WriteList(list, items);
		}
	}

	// The player asking for a plugin also clears its PreloadFailed mark: they choose to try it again.
	void LoadInBackground(const std::wstring& path)
	{
		SetListed(L"PreloadFailed", path, false);
		Queue(Command::LoadSpare, 0, 0, path);
		KickSlow();
	}

	void SetAlwaysLoaded(const std::wstring& path, bool on)
	{
		SetListed(L"KeepLoadedPlugins", path, on);
		SetListed(L"PreloadFailed", path, false);
		VstHostControl::Shared* shared = Shared();
		if (on && shared && HostAlive(shared)) Queue(Command::LoadSpare, 0, 0, path);   // now, not at the next quiet moment
		KickSlow();
		g_snapshotAt = 0;
	}

	void Unload(const std::wstring& path)
	{
		SetListed(L"KeepLoadedPlugins", path, false);
		Queue(Command::UnloadSpare, 0, 0, path);
		KickSlow();
		g_snapshotAt = 0;
	}

	void Reload(const std::wstring& path)
	{
		SetListed(L"PreloadFailed", path, false);   // the player asked for it: a failed mark from before is cleared
		Queue(Command::ReloadPlugin, 0, 0, path);
		KickSlow();
		g_snapshotAt = 0;
	}

	void SetMuteWhileTuning(bool on)
	{
		WriteDword(L"MuteWhileTuning", on ? 1 : 0);
		ReadSlow([&](SlowState& slow) { slow.muteWhileTuning = on; return 0; });
		g_snapshotAt = 0;
	}

	void SetPopout(bool wanted)
	{
		if (VstHostControl::Shared* shared = Shared()) InterlockedExchange(&shared->popoutWanted, wanted ? 1 : 0);
	}

	void AddPlugin(const std::wstring& path, int position)
	{
		VstHostControl::Shared* shared = Shared();
		RememberRecent(path);
		if (!shared || !HostAlive(shared)) { if (HostProcessGone()) Launch(path); return; }
		Queue(Command::AddPlugin, position < 0 ? 0 : position + 1, 0, path);
	}

	void ReplaceSlot(int slot, const std::wstring& path) { RememberRecent(path); Queue(Command::ReplaceSlot, slot, 0, path); }
	void DuplicateSlot(int slot) { Queue(Command::DuplicateSlot, slot); }
	void SetChainBypass(bool bypass) { Queue(Command::SetChainBypass, bypass ? 1 : 0); }
	void RemoveSlot(int slot) { Queue(Command::RemoveSlot, slot); }
	void MoveSlot(int slot, int delta) { Queue(Command::MoveSlot, slot, delta); }
	void SetBypass(int slot, bool bypass) { Queue(Command::SetBypass, slot, bypass ? 1 : 0); }
	void ShowSlot(int slot) { Queue(Command::ShowSlot, slot); }
	void HideSlot(int slot) { Queue(Command::HideSlot, slot); }
	void ClearChain() { Queue(Command::ClearChain); }
	void NotePageInUse() { g_pageUsedAt = GetTickCount64(); }
	void SetLevels(int inputTenths, int outputTenths) { Queue(Command::SetLevels, inputTenths, outputTenths); }
	void LoadTone(const std::string& name) { g_toneBeforeSong.clear(); Queue(Command::LoadTone, 0, 0, Wide(name)); }
	void SaveTone(const std::string& name) { ToneGates::ToneSaved(name); Queue(Command::SaveTone, 0, 0, Wide(name)); KickSlow(); }
	std::string DefaultTone() { return ReadSlow([](SlowState& slow) { return slow.defaultTone; }); }

	void SetDefaultTone(const std::string& tone)
	{
		WriteString(L"DefaultTone", Wide(tone));
		ReadSlow([&](SlowState& slow) { slow.defaultTone = tone; return 0; });
		g_snapshotAt = 0;
	}

	void DeleteTone(const std::string& name)
	{
		if (_stricmp(DefaultTone().c_str(), name.c_str()) == 0) SetDefaultTone({});
		Queue(Command::DeleteTone, 0, 0, Wide(name));
		ToneGates::DeleteTone(name);
		// Songs that used it go back to the player's usual tone instead of failing to load a missing file.
		for (const std::string& song : SongsUsingTone(name))
			WritePrivateProfileStringW(L"Songs", Wide(song).c_str(), nullptr, SongTonesFile().c_str());
		KickSlow();
		g_snapshotAt = 0;
	}

	void RenameTone(const std::string& from, const std::string& to)
	{
		if (from.empty() || to.empty() || from == to) return;
		Queue(Command::RenameTone, 0, 0, Wide(from) + L"\n" + Wide(to));
		ToneGates::RenameTone(from, to);
		for (const std::string& song : SongsUsingTone(from))
			WritePrivateProfileStringW(L"Songs", Wide(song).c_str(), Wide(to).c_str(), SongTonesFile().c_str());
		if (_stricmp(g_toneBeforeSong.c_str(), from.c_str()) == 0) g_toneBeforeSong = to;
		if (IsFavouriteTone(from)) { SetFavouriteTone(from, false); SetFavouriteTone(to, true); }
		if (_stricmp(DefaultTone().c_str(), from.c_str()) == 0) SetDefaultTone(to);
		KickSlow();
		g_snapshotAt = 0;
	}

	void SetSongTone(const std::string& tone)
	{
		if (g_songKey.empty() || tone.empty()) return;
		SHCreateDirectoryExW(nullptr, DataDirectory().c_str(), nullptr);
		SetToneForSong(g_songKey, tone);
		g_appliedSong = g_songKey;   // it is the tone playing now
		KickSlow();
		g_snapshotAt = 0;
	}

	void ClearSongTone()
	{
		if (g_songKey.empty()) return;
		SetToneForSong(g_songKey, {});
		g_appliedSong = g_songKey;   // nothing to load: the tone playing stays
	}
}
