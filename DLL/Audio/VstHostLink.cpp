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
#include <mutex>

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
		std::vector<std::wstring> g_failed;   // plugins that did not load this session (the host's failedPath)
		std::wstring g_lastFailed;
		std::atomic<bool> g_rescanWanted{ false };   // set by the folder dialog's thread; the render thread rescans
		std::vector<std::string> g_tones;   // the render thread's copy of SlowState::tones
		unsigned g_tonesVersion = ~0u;

		Snapshot g_snapshot;
		ULONGLONG g_snapshotAt = 0;

		bool g_wasRunning = false;
		std::string g_crashNote;
		std::deque<ULONGLONG> g_crashRestarts;   // when the host was restarted after a crash, for the loop guard
		constexpr size_t kMaxCrashRestarts = 3;      // within kCrashWindowMs; then it stays off and says so
		constexpr ULONGLONG kCrashWindowMs = 5 * 60 * 1000;

		bool g_autoStartChecked = false;
		bool g_mutedForTuning = false;
		ULONGLONG g_tuningPolledAt = 0;
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

		size_t CountSongTones()
		{
			std::vector<wchar_t> section(32768);
			const DWORD size = GetPrivateProfileSectionW(L"Songs", section.data(), static_cast<DWORD>(section.size()), SongTonesFile().c_str());
			size_t count = 0;
			for (const wchar_t* line = section.data(); line < section.data() + size && *line; line += wcslen(line) + 1) ++count;
			return count;
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
			size_t songToneCount = 0;
			std::vector<std::wstring> scanFolders;
			std::string defaultTone;   // loads when a song without a tone of its own starts (empty: none)
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
				fresh.songToneCount = CountSongTones();
				fresh.scanFolders = ExtraFolders();
				fresh.defaultTone = Utf8(ReadString(L"DefaultTone"));
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

		// Song tones: entering a song with an assigned tone loads it; entering one without, after a song tone was
		// loaded, goes back to the tone the player had before.
		void FollowSong(VstHostControl::Shared* shared, bool hostRunning)
		{
			const ULONGLONG now = GetTickCount64();
			if (now - g_songPolledAt < kSongPollMs) return;
			g_songPolledAt = now;
			const std::string songKey = GameState::IsInSong() ? GameState::GetSongKey() : std::string();
			if (songKey != g_songKey)
			{
				g_songKey = songKey;
				ReadSlow([&](SlowState& slow) { slow.songKey = songKey; slow.songTone.clear(); return 0; });
				KickSlow();
			}
			// Out of the song: the next start of any song, the same one included, applies its tone again. (Without
			// this, a tone assigned during a song never loaded when that song was played again.)
			if (g_songKey.empty()) { g_appliedSong.clear(); return; }
			if (g_songKey == g_appliedSong || !hostRunning) return;
			// Entering a song is rare: this one read stays here, so the song's tone loads without waiting a pass.
			g_appliedSong = g_songKey;
			const std::string tone = SongTone(g_songKey);
			LOG_INFO("(VST HOST) song " << g_songKey << " started: " << (tone.empty() ? std::string("no tone of its own") : "its tone is '" + tone + "'") << std::endl);
			char current[sizeof(shared->toneName) + 1]{};
			memcpy(current, shared->toneName, sizeof(shared->toneName));
			if (!tone.empty())
			{
				if (_stricmp(tone.c_str(), current) == 0) return;
				if (g_toneBeforeSong.empty()) g_toneBeforeSong = current;
				LOG_INFO("(VST HOST) song " << g_songKey << ": loading its tone '" << tone << "'" << std::endl);
				Queue(Command::LoadTone, 0, 0, Wide(tone));
			}
			else if (const std::string fallback = ReadSlow([](SlowState& slow) { return slow.defaultTone; }); !fallback.empty())
			{
				// A song without its own tone gets the default tone, when the player has set one.
				g_toneBeforeSong.clear();
				if (_stricmp(fallback.c_str(), current) == 0) return;
				LOG_INFO("(VST HOST) song " << g_songKey << " has no tone of its own: loading the default tone '" << fallback << "'" << std::endl);
				Queue(Command::LoadTone, 0, 0, Wide(fallback));
			}
			else if (!g_toneBeforeSong.empty())
			{
				LOG_INFO("(VST HOST) song " << g_songKey << " has no tone of its own: back to '" << g_toneBeforeSong << "'" << std::endl);
				Queue(Command::LoadTone, 0, 0, Wide(g_toneBeforeSong));
				g_toneBeforeSong.clear();
			}
		}

		// A plugin that crashes takes the host with it. The host leaves a note as it goes (LastCrash = the module,
		// LastCrashAt = seconds since boot, Main.cpp OnCrash); seeing the host gone with a fresh note, the overlay
		// starts it again with the last autosaved chain and tells the player which plugin it was. Three crashes
		// within five minutes and it stays off, so a plugin that crashes on load cannot loop.
		void FollowCrashes(bool hostRunning)
		{
			if (hostRunning) { g_wasRunning = true; return; }
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
				g_crashNote = what + ", for the third time in five minutes, so Tone designer stays off. Start it again when ready; if one plugin keeps crashing, remove or replace it.";
				LOG_WARNING("(VST HOST) " << what << "; third crash in five minutes, not restarting" << std::endl);
				return;
			}
			g_crashRestarts.push_back(now);
			g_crashNote = what + ". Tone designer restarted with your last autosave (up to 30 s of changes may be lost).";
			LOG_WARNING("(VST HOST) " << what << "; restarting it" << std::endl);
			g_commands.clear();
			Launch({});
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
				return 0;
			});
			s.songKey = g_songKey;
		s.crashNote = g_crashNote;
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
		// A plugin that did not load is marked in the browser for the rest of the session (or until a rescan).
		wchar_t failed[VstHostControl::kPathChars + 1]{};
		memcpy(failed, const_cast<const wchar_t*>(shared->failedPath), sizeof(shared->failedPath));
		if (failed[0] && g_lastFailed != failed)
		{
			g_lastFailed = failed;
			if (!ListHas(g_failed, failed)) g_failed.push_back(failed);
		}
		AutoStart(hostRunning);
		FollowCrashes(hostRunning);
		FollowSong(shared, hostRunning);
		FollowTuner(hostRunning);
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
		ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
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
		std::vector<wchar_t> section(32768);
		const DWORD size = GetPrivateProfileSectionW(L"Songs", section.data(), static_cast<DWORD>(section.size()), SongTonesFile().c_str());
		for (const wchar_t* line = section.data(); line < section.data() + size && *line; line += wcslen(line) + 1)
		{
			const wchar_t* equals = wcschr(line, L'=');
			if (equals && _wcsicmp(equals + 1, Wide(tone).c_str()) == 0) songs.push_back(Utf8(std::wstring(line, equals)));
		}
		return songs;
	}

	size_t SongToneCount() { return ReadSlow([](SlowState& slow) { return slow.songToneCount; }); }

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

	void DismissCrashNote() { g_crashNote.clear(); g_snapshotAt = 0; }

	void SetAutoStart(bool on)
	{
		WriteDword(L"AutoStart", on ? 1 : 0);
		ReadSlow([&](SlowState& slow) { slow.autoStart = on; return 0; });
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

	void AddPlugin(const std::wstring& path)
	{
		VstHostControl::Shared* shared = Shared();
		RememberRecent(path);
		if (!shared || !HostAlive(shared)) { if (HostProcessGone()) Launch(path); return; }
		Queue(Command::AddPlugin, 0, 0, path);
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
	void SetLevels(int inputTenths, int outputTenths) { Queue(Command::SetLevels, inputTenths, outputTenths); }
	void LoadTone(const std::string& name) { g_toneBeforeSong.clear(); Queue(Command::LoadTone, 0, 0, Wide(name)); }
	void SaveTone(const std::string& name) { Queue(Command::SaveTone, 0, 0, Wide(name)); KickSlow(); }
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
		WritePrivateProfileStringW(L"Songs", Wide(g_songKey).c_str(), Wide(tone).c_str(), SongTonesFile().c_str());
		g_appliedSong = g_songKey;   // it is the tone playing now
		ReadSlow([&](SlowState& slow) { if (slow.songKey == g_songKey) slow.songTone = tone; return 0; });
		KickSlow();
		g_snapshotAt = 0;
	}

	void ClearSongTone()
	{
		if (g_songKey.empty()) return;
		WritePrivateProfileStringW(L"Songs", Wide(g_songKey).c_str(), nullptr, SongTonesFile().c_str());
		ReadSlow([&](SlowState& slow) { if (slow.songKey == g_songKey) slow.songTone.clear(); return 0; });
		KickSlow();
		g_snapshotAt = 0;
	}
}
