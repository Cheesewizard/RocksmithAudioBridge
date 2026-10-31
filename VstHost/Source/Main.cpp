// Rocksmith Audio Bridge VST host: plays the guitar through a chain of VST3 plugins (amp sims, pedals, effects)
// while Rocksmith keeps the clean signal for note detection. 64-bit, because Rocksmith is 32-bit and modern
// plugins are 64-bit only; audio goes through the bridge driver's external amp link, the same path an external
// amp sim uses.
//
// The overlay's Tone designer page drives it through a shared block (Mod/Shared/VstHostControl.hpp): build the
// chain, pick which plugin's window shows, save and load tones, set levels. The shown plugin's editor lives in a
// borderless, always-on-top popout over the game that shows with the overlay, so the amp can be adjusted without
// leaving Rocksmith. That needs Rocksmith in windowed or borderless fullscreen; nothing can draw over exclusive
// fullscreen.
//
// Files (%APPDATA%\RocksmithAudioBridge\VstHost): Current.rabtone (the chain as last played, restored at start),
// Tones\<name>.rabtone (saved tones), <plugin id>.state (each plugin's own settings, its starting point when added).
//
// Usage: RocksmithAudioBridgeVstHost.exe [--overlay] ["path\to\plugin.vst3"]
// Restores the last chain; a plugin path is added to it if it is not in it yet. Started by hand with nothing to
// play, it asks for a plugin (--overlay, used when the overlay starts it, never asks).

#include "BridgeAudio.hpp"
#include "Chain.hpp"
#include "HostLog.hpp"
#include "PedalEditor.hpp"
#include "PicturePublisher.hpp"
#include "SourceRouter.hpp"
#include "StompboxEngine.hpp"
#include "StompboxService.hpp"
#include "AudioFormat.hpp"

#include "../../Mod/Shared/VstHostControl.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <commdlg.h>
#include <shellapi.h>
#include <TlHelp32.h>
#include <DbgHelp.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

using namespace VstHost;

void InstallCrashReporter();

namespace
{
	constexpr wchar_t kWindowClass[] = L"RocksmithAudioBridgeVstHost";      // one per open plugin window
	constexpr wchar_t kEditorClass[] = L"RocksmithAudioBridgeVstEditor";    // the editor area inside it
	constexpr wchar_t kMainClass[] = L"RocksmithAudioBridgeVstHostMain";    // hidden: timers, audio resets, shutdown
	constexpr wchar_t kSettingsKey[] = L"Software\\RSMods\\VstHost";
	constexpr UINT_PTR kHousekeepingTimer = 1;
	constexpr UINT kHousekeepingMs = 500;
	constexpr int kAutosaveTicks = 60;   // 30 s
	constexpr UINT_PTR kKeyTimer = 2;
	constexpr UINT kKeyMs = 15;
	constexpr UINT kDefaultOverlayKey = VK_OEM_5;   // backslash, the game overlay's default key (DLL/dllmain.cpp)
	constexpr int kStripDip = 26;            // drag bar above the editor

	Chain g_chain;
	BridgeAudio g_audio;
	// The stream's sink: the VST chain, or the Stompbox engine while the Tone designer page asks for it
	// (StompboxService). The engine is loaded when the host starts and processes only while it plays.
	SourceRouter g_router;
	StompboxEngine g_stompbox;
	bool g_stompboxPlays = false;
	bool SetStompboxPlays(bool plays);
	StompboxService g_stompboxService{ g_stompbox, g_router, [](bool plays) { return SetStompboxPlays(plays); } };
	// The built-in pedals' window (the page's EditPedal). Made once JUCE runs (StartStompbox), released before it stops.
	std::unique_ptr<PedalEditor> g_pedalEditor;
	HWND g_main = nullptr;            // message-only window (kMainClass)
	HINSTANCE g_instance = nullptr;
	HFONT g_font = nullptr;
	int g_shown = -1;                 // the pedal last opened or selected (the page's starting selection)
	int g_ticks = 0;
	bool g_keyDown = false;
	HANDLE g_controlMapping = nullptr;
	VstHostControl::Shared* g_control = nullptr;
	LONG g_lastOverlayBeat = 0;
	ULONGLONG g_overlayBeatSeenAt = 0;
	bool g_savedWanted = false;
	LONG g_handledCommand = 0;
	std::wstring g_toneName;          // the tone last loaded or saved
	bool g_toneModified = false;
	float g_inputMeter = 0.0f, g_outputMeter = 0.0f;
	wchar_t g_loadingNow[128] = {};   // the plugin loading now, for a crash report (plain buffer: read in OnCrash)
	wchar_t g_openWindowNames[256] = {};   // plugins whose windows are open, for a crash report (OnCrash)
	ULONGLONG g_lastCommandAt = 0;    // the page's last command (background loading waits for a quiet moment)
	// A command is running. A plugin that pumps messages while it loads (a licence dialog) lets the timers run
	// meanwhile; the page's next command, and background loading, wait until the first is done.
	bool g_inCommand = false;
	// Standby: Tone designer is off, but the host stays running with its plugins loaded, so turning it on is instant
	// and starred plugins keep loading in the background. The audio link is released (the game's amp plays), the
	// chain processes nothing and the plugin windows are hidden. Started with --standby, or SetStandby from the page.
	bool g_standby = false;
	// Spares the player asked for (RefreshKeepSettings): KeepLoadedPlugins ("Always keep loaded"), and this run's
	// "Load in the background"; PreloadFailed, plugins that went down loading that way.
	std::vector<std::wstring> g_alwaysLoaded, g_sessionLoaded, g_preloadFailed;
	// The page's undo history (see Undo below). Made once JUCE runs (wWinMain), released before it stops.
	std::unique_ptr<juce::UndoManager> g_undo;

	bool ReadDword(const wchar_t* name, DWORD& value)
	{
		DWORD size = sizeof(value);
		return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
	}

	void WriteDword(const wchar_t* name, DWORD value)
	{
		RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value, sizeof(value));
	}

	std::wstring ReadString(const wchar_t* name)
	{
		wchar_t value[MAX_PATH * 2]{};
		DWORD size = sizeof(value);
		if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
		return value;
	}

	void WriteString(const wchar_t* name, const std::wstring& value)
	{
		RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
	}

	// A ';'-separated list (the page writes them: FavouritePlugins, PreloadFailed); any length.
	std::vector<std::wstring> ReadList(const wchar_t* name)
	{
		DWORD size = 0;
		if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0) return {};
		std::wstring text(size / sizeof(wchar_t) + 1, L'\0');
		if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_SZ, nullptr, text.data(), &size) != ERROR_SUCCESS) return {};
		text.resize(wcslen(text.c_str()));
		std::vector<std::wstring> items;
		for (size_t start = 0; start < text.size();)
		{
			const size_t end = text.find(L';', start);
			const std::wstring item = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
			if (!item.empty()) items.push_back(item);
			if (end == std::wstring::npos) break;
			start = end + 1;
		}
		return items;
	}

	bool ListHas(const std::vector<std::wstring>& items, const std::wstring& item)
	{
		return std::any_of(items.begin(), items.end(), [&](const std::wstring& i) { return _wcsicmp(i.c_str(), item.c_str()) == 0; });
	}

	std::wstring DataDirectory()
	{
		PWSTR appData = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData))) return {};
		std::wstring dir = std::wstring(appData) + L"\\RocksmithAudioBridge\\VstHost";
		CoTaskMemFree(appData);
		SHCreateDirectoryExW(nullptr, (dir + L"\\Tones").c_str(), nullptr);
		return dir;
	}

	std::wstring CurrentChainFile() { return DataDirectory() + L"\\Current.rabtone"; }

	// The Tone designer's engine, loaded as the host starts so its pedal catalogue is ready before the page opens.
	// Models, cabinets and presets live in %APPDATA%\RocksmithAudioBridge\Stompbox. HKCU\Software\RSMods\VstHost
	// StompboxDev = 1 is for development: Stompbox keeps its own layout and starts its local server for its desktop
	// remote (StompboxRemote, connect to 127.0.0.1).
	void StartStompbox()
	{
		PWSTR appData = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData)))
		{
			Log("stompbox: no AppData folder; the Tone designer engine is not started");
			return;
		}
		const std::wstring dataPath = std::wstring(appData) + L"\\RocksmithAudioBridge\\Stompbox";
		CoTaskMemFree(appData);
		SHCreateDirectoryExW(nullptr, dataPath.c_str(), nullptr);
		DWORD development = 0;
		ReadDword(L"StompboxDev", development);
		g_pedalEditor = std::make_unique<PedalEditor>(g_stompboxService);
		g_stompboxService.onEditPedal = [](const std::string& pedalId) { if (g_pedalEditor) g_pedalEditor->Open(pedalId); };
		g_stompboxService.Start(dataPath, development != 0);
	}

	// Tone names become file names: keep them to characters every file system takes.
	std::wstring SafeFilePart(const std::wstring& part)
	{
		std::wstring safe;
		for (wchar_t c : part) safe += (c < 32 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
		while (!safe.empty() && (safe.back() == L' ' || safe.back() == L'.')) safe.pop_back();
		while (!safe.empty() && safe.front() == L' ') safe.erase(safe.begin());
		return safe;
	}

	// A tone's name is "Name", or "Folder/Name" for a tone in a folder of the tones folder (one level; the page
	// groups them, "Bass tones", "Experimental"). Each part keeps to characters every file system takes.
	std::wstring ToneFile(const std::wstring& name)
	{
		const size_t slash = name.find(L'/');
		const std::wstring folder = slash == std::wstring::npos ? std::wstring() : SafeFilePart(name.substr(0, slash));
		const std::wstring base = SafeFilePart(slash == std::wstring::npos ? name : name.substr(slash + 1));
		if (base.empty()) return {};
		return DataDirectory() + L"\\Tones\\" + (folder.empty() ? std::wstring() : folder + L"\\") + base + L".rabtone";
	}

	void CreateFolderFor(const std::wstring& file) { SHCreateDirectoryExW(nullptr, file.substr(0, file.find_last_of(L'\\')).c_str(), nullptr); }

	// A tone folder left empty by a move or delete goes (RemoveDirectory refuses a folder that still has files).
	void RemoveFolderIfEmpty(const std::wstring& file)
	{
		const std::wstring folder = file.substr(0, file.find_last_of(L'\\'));
		if (_wcsicmp(folder.c_str(), (DataDirectory() + L"\\Tones").c_str()) != 0) RemoveDirectoryW(folder.c_str());
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

	// A plugin that failed to load in the background goes on PreloadFailed, so the background does not try it again
	// until the player asks for it, and PreloadFailedWhy ("path|reason;...") keeps why, for the page's "Load failed"
	// pill. A ';' or '|' in the reason would break the list, so they become ','.
	void RecordPreloadFailure(const std::wstring& path, std::wstring reason)
	{
		std::vector<std::wstring> failed = ReadList(L"PreloadFailed");
		if (!ListHas(failed, path)) failed.push_back(path);
		std::wstring list;
		for (const std::wstring& item : failed) list += (list.empty() ? L"" : L";") + item;
		WriteString(L"PreloadFailed", list);

		std::replace_if(reason.begin(), reason.end(), [](wchar_t c) { return c == L';' || c == L'|'; }, L',');
		std::wstring why;
		for (const std::wstring& item : ReadList(L"PreloadFailedWhy"))
		{
			const std::wstring itemPath = item.substr(0, item.find(L'|'));
			if (_wcsicmp(itemPath.c_str(), path.c_str()) != 0) why += (why.empty() ? L"" : L";") + item;
		}
		why += (why.empty() ? L"" : L";") + path + L"|" + reason;
		WriteString(L"PreloadFailedWhy", why);
	}

	void CopyText(char* destination, size_t capacity, const std::string& text)
	{
		memset(destination, 0, capacity);
		memcpy(destination, text.c_str(), std::min(text.size(), capacity - 1));
	}

	std::wstring AskForPlugin()
	{
		wchar_t file[MAX_PATH * 2]{};
		OPENFILENAMEW dialog{};
		dialog.lStructSize = sizeof(dialog);
		dialog.lpstrFilter = L"VST3 plugins (*.vst3)\0*.vst3\0";
		dialog.lpstrFile = file;
		dialog.nMaxFile = static_cast<DWORD>(std::size(file));
		dialog.lpstrInitialDir = L"C:\\Program Files\\Common Files\\VST3";
		dialog.lpstrTitle = L"Pick a VST3 plugin";
		dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
		return GetOpenFileNameW(&dialog) ? file : std::wstring();
	}

	void ShowError(const std::string& text)
	{
		Log("error: %s", text.c_str());
		MessageBoxA(nullptr, text.c_str(), "Rocksmith Audio Bridge VST host", MB_OK | MB_ICONWARNING | MB_TOPMOST);
	}

	void SetLoadError(const std::string& text)
	{
		if (!text.empty()) Log("problem: %s", text.c_str());
		if (g_control) CopyText(g_control->loadError, sizeof(g_control->loadError), text);
	}

	// The plugin browser marks this plugin "Failed to load".
	void SetFailedPath(const std::wstring& path)
	{
		if (!g_control) return;
		wchar_t* destination = const_cast<wchar_t*>(g_control->failedPath);
		memset(destination, 0, sizeof(g_control->failedPath));
		wcsncpy_s(destination, VstHostControl::kPathChars, path.c_str(), _TRUNCATE);
	}

	bool IsRocksmithProcess(DWORD pid)
	{
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process) return false;
		wchar_t path[MAX_PATH]{};
		DWORD size = MAX_PATH;
		const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
		CloseHandle(process);
		if (!ok) return false;
		const wchar_t* name = wcsrchr(path, L'\\');
		return _wcsicmp(name ? name + 1 : path, L"Rocksmith2014.exe") == 0;
	}

	// ---- Closing with the game --------------------------------------------------------------------------------
	// The host is the game's companion: once it has seen Rocksmith, it closes (saving the chain) when the game does.
	// The game starts it, so Steam counts it as part of the game: a host left behind keeps Steam showing Rocksmith
	// as running. This used to be a 2 s check on the window thread's timer, which asked for WM_CLOSE and then
	// trusted the close to finish; when it did not (a plugin's teardown or its DLL unloading at exit can hang, as
	// can a window thread already stuck in a plugin), the host stayed. Now a thread of its own waits on the game's
	// process, asks the window thread to close, and ends the process itself if the close does not finish in time.
	// Started by hand with no game, the host stays until closed.
	std::atomic<bool> g_closeHandled{ false };   // the window thread runs its timers, so it handles WM_CLOSE
	std::atomic<bool> g_chainSaved{ false };     // WM_CLOSE saved the chain; only teardown is left
	constexpr ULONGLONG kCloseSaveMs = 30000;     // a heavy chain's plugin states can take many seconds to save
	constexpr ULONGLONG kCloseTeardownMs = 5000;  // after the save: releasing the plugins and the audio link

	DWORD ParentProcessId()
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return 0;
		PROCESSENTRY32W entry{ sizeof(entry) };
		DWORD parent = 0;
		for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
			if (entry.th32ProcessID == GetCurrentProcessId()) { parent = entry.th32ParentProcessID; break; }
		CloseHandle(snapshot);
		return parent;
	}

	// The game's process: the parent when the overlay started the host (its ShellExecute runs in the game), else
	// the first Rocksmith2014.exe running.
	HANDLE OpenGameProcess()
	{
		const DWORD parent = ParentProcessId();
		if (parent && IsRocksmithProcess(parent))
			if (HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, parent)) return game;
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
		PROCESSENTRY32W entry{ sizeof(entry) };
		HANDLE game = nullptr;
		for (BOOL more = Process32FirstW(snapshot, &entry); more && !game; more = Process32NextW(snapshot, &entry))
			if (_wcsicmp(entry.szExeFile, L"Rocksmith2014.exe") == 0) game = OpenProcess(SYNCHRONIZE, FALSE, entry.th32ProcessID);
		CloseHandle(snapshot);
		return game;
	}

	DWORD WINAPI WatchGame(void*)
	{
		HANDLE game = nullptr;
		while (!(game = OpenGameProcess())) Sleep(2000);
		WaitForSingleObject(game, INFINITE);
		CloseHandle(game);
		Log("host: Rocksmith has closed, closing too");
		// Still restoring the chain (no timers yet): nothing has changed that needs saving.
		if (!g_closeHandled) { Log("host: closed while starting"); TerminateProcess(GetCurrentProcess(), 0); }
		PostMessageW(g_main, WM_CLOSE, 0, 0);
		const ULONGLONG start = GetTickCount64();
		ULONGLONG savedAt = 0;
		for (;;)
		{
			Sleep(100);
			const ULONGLONG now = GetTickCount64();
			if (g_chainSaved && savedAt == 0) savedAt = now;
			if (savedAt ? now - savedAt >= kCloseTeardownMs : now - start >= kCloseSaveMs) break;
		}
		Log(savedAt ? "host: saved, but releasing the plugins did not finish; ending the process"
			: "host: the chain did not save in time; ending the process");
		TerminateProcess(GetCurrentProcess(), 0);
		return 0;
	}

	// The game's main window: its largest visible top-level window.
	HWND FindGameWindow()
	{
		struct Search { HWND best = nullptr; LONG area = 0; } search;
		EnumWindows([](HWND window, LPARAM param) -> BOOL
		{
			auto* s = reinterpret_cast<Search*>(param);
			DWORD pid = 0;
			GetWindowThreadProcessId(window, &pid);
			if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) || !IsRocksmithProcess(pid)) return TRUE;
			RECT rect{};
			GetWindowRect(window, &rect);
			const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
			if (area > s->area) { s->area = area; s->best = window; }
			return TRUE;
		}, reinterpret_cast<LPARAM>(&search));
		return search.best;
	}

	bool IsOurWindow(HWND window)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(window, &pid);
		return pid == GetCurrentProcessId();
	}

	// ---- Plugin windows ---------------------------------------------------------------------------------------
	// Each pedal can have its own window, so several plugins can be tweaked at once: a borderless, topmost tool
	// window with a drag bar and a close button, the plugin's editor under it. Which pedals have a window open is the
	// player's choice (the page's double-click, Open window / Close window, the window's own close button); all of
	// them show only while "Show plugin windows" (popoutWanted) is on and the overlay is open.
	// A window belongs to a plugin instance, not a slot number: after any chain change (SyncWindows), windows whose
	// plugin left the chain close and the rest are renumbered; a replaced pedal and a reloaded tone get their windows
	// back (RunCommand). Positions are remembered per plugin file (WindowPos<hash>X/Y), the open slots as a bit mask
	// (OpenWindows), both in HKCU\Software\RSMods\VstHost.
	struct AmpWindow
	{
		PluginHost* plugin = nullptr;   // compared only once the plugin may have left the chain, never used then
		std::wstring path;
		HWND frame = nullptr;           // strip + editor area
		HWND area = nullptr;            // the editor's parent
		std::wstring title;
	};
	std::vector<std::unique_ptr<AmpWindow>> g_windows;
	bool g_windowsShown = false;

	int Scale(HWND window, int dip) { return MulDiv(dip, window ? static_cast<int>(GetDpiForWindow(window)) : 96, 96); }
	int StripHeight(HWND window) { return Scale(window, kStripDip); }
	int CloseButtonWidth(HWND window) { return Scale(window, 30); }

	// The screen area windows must fit in: the game's monitor, else the window's own.
	RECT WorkArea(HWND nearWindow)
	{
		HWND anchor = FindGameWindow();
		MONITORINFO info{ sizeof(info) };
		GetMonitorInfoW(MonitorFromWindow(anchor ? anchor : nearWindow, MONITOR_DEFAULTTOPRIMARY), &info);
		return info.rcWork;
	}

	AmpWindow* WindowForFrame(HWND frame)
	{
		for (auto& window : g_windows) if (window->frame == frame) return window.get();
		return nullptr;
	}

	AmpWindow* WindowForPlugin(const PluginHost* plugin)
	{
		for (auto& window : g_windows) if (window->plugin == plugin) return window.get();
		return nullptr;
	}

	int SlotOf(const PluginHost* plugin)
	{
		for (int i = 0; i < g_chain.Count(); ++i) if (g_chain.Plugin(i) == plugin) return i;
		return -1;
	}

	unsigned OpenWindowMask()
	{
		unsigned mask = 0;
		for (auto& window : g_windows)
		{
			const int slot = SlotOf(window->plugin);
			if (slot >= 0 && slot < 32) mask |= 1u << slot;
		}
		return mask;
	}

	// FNV-1a of the lower-cased plugin path: one remembered spot per plugin file.
	std::wstring PositionKey(const std::wstring& path)
	{
		uint32_t hash = 2166136261u;
		for (wchar_t c : path) { hash ^= static_cast<uint32_t>(towlower(c)); hash *= 16777619u; }
		wchar_t key[32];
		swprintf_s(key, L"WindowPos%08X", hash);
		return key;
	}

	void RememberPosition(const AmpWindow& window)
	{
		RECT frame{};
		GetWindowRect(window.frame, &frame);
		const std::wstring key = PositionKey(window.path);
		WriteDword((key + L"X").c_str(), static_cast<DWORD>(frame.left));
		WriteDword((key + L"Y").c_str(), static_cast<DWORD>(frame.top));
	}

	// Keeps the whole window on its monitor after it changes size (a bigger plugin can push it off the edge).
	void KeepOnScreen(HWND frameWindow)
	{
		RECT frame{};
		GetWindowRect(frameWindow, &frame);
		const RECT area = WorkArea(frameWindow);
		const int width = static_cast<int>(frame.right - frame.left), height = static_cast<int>(frame.bottom - frame.top);
		int x = static_cast<int>(frame.left), y = static_cast<int>(frame.top);
		x = std::max(static_cast<int>(area.left), std::min(x, static_cast<int>(area.right) - width));
		y = std::max(static_cast<int>(area.top), std::min(y, static_cast<int>(area.bottom) - height));
		if (x != frame.left || y != frame.top) SetWindowPos(frameWindow, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	// Its remembered spot, else the middle of the game's monitor, each further window a step down and right.
	void PlaceWindow(AmpWindow& window)
	{
		const std::wstring key = PositionKey(window.path);
		DWORD x = 0, y = 0;
		if (ReadDword((key + L"X").c_str(), x) && ReadDword((key + L"Y").c_str(), y))
		{
			const POINT topLeft{ static_cast<LONG>(static_cast<int>(x)), static_cast<LONG>(static_cast<int>(y)) };
			if (MonitorFromPoint(topLeft, MONITOR_DEFAULTTONULL))
			{
				SetWindowPos(window.frame, nullptr, topLeft.x, topLeft.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
				KeepOnScreen(window.frame);
				return;
			}
		}
		RECT frame{};
		GetWindowRect(window.frame, &frame);
		const RECT area = WorkArea(window.frame);
		const int step = Scale(window.frame, 32) * static_cast<int>(g_windows.size() - 1);
		SetWindowPos(window.frame, nullptr, area.left + (area.right - area.left - (frame.right - frame.left)) / 2 + step,
			area.top + (area.bottom - area.top - (frame.bottom - frame.top)) / 2 + step, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		KeepOnScreen(window.frame);
	}

	void FitWindow(AmpWindow& window, int width, int height)
	{
		const int strip = StripHeight(window.frame);
		SetWindowPos(window.area, nullptr, 0, strip, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
		SetWindowPos(window.frame, nullptr, 0, 0, width, height + strip, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		KeepOnScreen(window.frame);
		InvalidateRect(window.frame, nullptr, TRUE);
	}

	void UpdateTitle(AmpWindow& window)
	{
		const int slot = SlotOf(window.plugin);
		if (slot < 0) return;
		window.title = std::to_wstring(slot + 1) + L"/" + std::to_wstring(g_chain.Count()) + L"  " + Wide(window.plugin->Name());
		if (g_chain.Bypassed(slot)) window.title += L"  (bypassed)";
		SetWindowTextW(window.frame, (window.title + L" - Rocksmith Audio Bridge").c_str());
		InvalidateRect(window.frame, nullptr, TRUE);
	}

	// Topmost but not activated: the game keeps the keyboard until the player clicks a plugin.
	void ShowFrame(HWND frame) { SetWindowPos(frame, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE); }

	void RememberOpenWindowNames()
	{
		std::wstring names;
		for (auto& window : g_windows)
			if (SlotOf(window->plugin) >= 0) names += (names.empty() ? L"" : L", ") + Wide(window->plugin->Name());
		wcsncpy_s(g_openWindowNames, names.c_str(), _TRUNCATE);
	}

	void RememberOpenWindows()
	{
		RememberOpenWindowNames();
		WriteDword(L"OpenWindows", OpenWindowMask());
		WriteDword(L"ShownSlot", static_cast<DWORD>(std::max(g_shown, 0)));
	}

	// ---- Inspector picture -----------------------------------------------------------------------------------------
	// The page's Inspector shows a picture of the inspected plugin's own window (Mod/Shared/PluginPicture.hpp),
	// printed kPictureMs apart while the overlay is on screen: a picture, so four or five a second is plenty, and the
	// print runs here on the main thread, never near the stream.
	// A plugin has one editor, and it must exist to be printed:
	//   - the plugin's window is open and on screen: that is printed;
	//   - its window is open but hidden ("Show plugin windows" off, standby): it is shown cloaked (DWM keeps drawing
	//     it but puts nothing on screen), lent to the picture, and hidden and uncloaked again before anything else
	//     shows or hides it (ReturnLentFrame), so the player's window behaves exactly as before;
	//   - no window: the editor opens in a window of our own (g_pictureEditor), a cloaked tool window on the game's
	//     monitor, which is never on screen, in the taskbar or in Alt+Tab. It is released when the inspected slot
	//     changes, the plugin reloads, or nothing printed it for kPictureIdleMs, and at once when the player opens
	//     the plugin's real window (OpenWindow), which then makes its editor exactly as it always has. Hiding the
	//     overlay only stops the printing: a heavy editor (AmpliTube takes about a second to open) is not made again
	//     each time the player toggles the overlay.
	// Cloaked rather than off screen: JUCE (ours, and every JUCE-built plugin's own copy) repaints on the vertical
	// blank of the monitor its window is on, and a window on no monitor would never repaint after its first frame.
	// Tried standalone: a cloaked window with a GDI child and a flip-model Direct3D 11 child prints both, before and
	// after they redraw.
	constexpr ULONGLONG kPictureMs = 220;
	constexpr ULONGLONG kPictureIdleMs = 30000;
	constexpr ULONGLONG kPictureSettleMs = 1500;   // a black print this soon after the editor opened is not a failure yet
	PicturePublisher g_picture;
	bool g_pictureOpen = false;
	ULONGLONG g_pictureAt = 0;
	struct PictureEditor
	{
		PluginHost* plugin = nullptr;   // compared only once the plugin may have left the chain, like AmpWindow
		HWND frame = nullptr;
		ULONGLONG openedAt = 0;
		ULONGLONG usedAt = 0;
	};
	PictureEditor g_pictureEditor;
	HWND g_lentFrame = nullptr;     // a hidden plugin window shown cloaked for the picture
	ULONGLONG g_lentAt = 0;

	bool Cloak(HWND window, bool cloaked)
	{
		const BOOL value = cloaked ? TRUE : FALSE;
		return SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_CLOAK, &value, sizeof(value)));
	}

	void ReleasePictureEditor()
	{
		if (!g_pictureEditor.frame) return;
		// The plugin may have left the chain (its editor went with it), or been reloaded in place (its editor closed).
		PluginHost* plugin = g_pictureEditor.plugin;
		if (SlotOf(plugin) >= 0 && plugin->EditorParent() == g_pictureEditor.frame) plugin->CloseEditor();
		DestroyWindow(g_pictureEditor.frame);
		g_pictureEditor = PictureEditor{};
		Log("picture: hidden editor released");
	}

	bool OpenPictureEditor(PluginHost* plugin)
	{
		ReleasePictureEditor();
		const RECT area = WorkArea(nullptr);
		const HWND frame = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kEditorClass, L"Rocksmith Audio Bridge picture",
			WS_POPUP | WS_CLIPCHILDREN, area.left, area.top, 800, 600, nullptr, nullptr, g_instance, nullptr);
		if (!frame) return false;
		if (!Cloak(frame, true))
		{
			// Without cloaking it would show over the game; there is nothing to print then.
			DestroyWindow(frame);
			Log("picture: could not cloak a window for the hidden editor");
			return false;
		}
		// The same limits as the plugin's own window (OpenWindow), so the picture shows the size the player gets.
		const int maxWidth = (area.right - area.left) * 9 / 10;
		const int maxHeight = (area.bottom - area.top) * 9 / 10 - StripHeight(frame);
		if (!plugin->OpenEditor(frame, [](int, int) {}, maxWidth, maxHeight))
		{
			DestroyWindow(frame);
			return false;
		}
		ShowWindow(frame, SW_SHOWNOACTIVATE);   // cloaked: drawn by DWM, never on screen
		const ULONGLONG now = GetTickCount64();
		g_pictureEditor = PictureEditor{ plugin, frame, now, now };
		Log("picture: hidden editor opened for %s", plugin->Name().c_str());
		return true;
	}

	// Hands a lent window back the way it was: hidden (unless the windows are on screen) and uncloaked. Before
	// UpdateWindows shows or hides the windows, and whenever the picture no longer needs it.
	void ReturnLentFrame()
	{
		if (!g_lentFrame) return;
		if (IsWindow(g_lentFrame))
		{
			if (!g_windowsShown) ShowWindow(g_lentFrame, SW_HIDE);
			Cloak(g_lentFrame, false);   // hidden by now, so this never flashes it
		}
		g_lentFrame = nullptr;
	}

	bool LendFrame(HWND frame)
	{
		if (g_lentFrame == frame) return true;
		ReturnLentFrame();
		if (!Cloak(frame, true)) return false;
		SetWindowPos(frame, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
		g_lentFrame = frame;
		g_lentAt = GetTickCount64();
		return true;
	}

	void StopPicture()
	{
		ReleasePictureEditor();
		ReturnLentFrame();
		if (g_pictureOpen) g_picture.Report(PluginPicture::State::None, -1);
	}

	// The overlay reports in and is on screen. Unlike OverlayOnScreen, an overlay that does not report counts as
	// hidden: nobody looks at the picture then.
	bool OverlayShowing()
	{
		if (!g_control) return false;
		const bool overlayReporting = g_overlayBeatSeenAt != 0 && GetTickCount64() - g_overlayBeatSeenAt < static_cast<ULONGLONG>(VstHostControl::kAliveMs);
		return overlayReporting && g_control->overlayVisible != 0;
	}

	void UpdatePicture()
	{
		if (!g_pictureOpen || g_inCommand) return;   // a command may be swapping the very plugin
		const ULONGLONG now = GetTickCount64();
		if (g_pictureEditor.frame && now - g_pictureEditor.usedAt > kPictureIdleMs) ReleasePictureEditor();
		const LONG slot = g_control ? g_control->inspectSlot : -1;
		PluginHost* plugin = g_chain.Plugin(slot);   // none for -1 or a slot past the chain's end
		if (!plugin) { StopPicture(); return; }
		if (g_pictureEditor.frame && g_pictureEditor.plugin != plugin) ReleasePictureEditor();   // another slot now
		if (!OverlayShowing())
		{
			// Nobody looks: stop printing, but keep the hidden editor until kPictureIdleMs (see above).
			ReturnLentFrame();
			g_picture.Report(PluginPicture::State::None, -1);
			return;
		}
		if (now - g_pictureAt < kPictureMs) return;
		g_pictureAt = now;
		if (!plugin->HasOwnEditor())
		{
			ReleasePictureEditor();
			ReturnLentFrame();
			g_picture.Report(PluginPicture::State::NoEditor, slot);
			return;
		}

		HWND top = nullptr, content = nullptr;
		ULONGLONG openedAt = 0;
		if (AmpWindow* window = WindowForPlugin(plugin))
		{
			if (g_lentFrame && g_lentFrame != window->frame) ReturnLentFrame();
			if (!IsWindowVisible(window->frame) && !LendFrame(window->frame))
			{
				g_picture.Report(PluginPicture::State::Failed, slot, "The plugin's window is open but hidden, and could not be drawn unseen.");
				return;
			}
			top = window->frame;
			content = window->area;
			openedAt = g_lentFrame == window->frame ? g_lentAt : 0;
		}
		else
		{
			ReturnLentFrame();
			// Reloaded in place (a replaced pedal, a tone): the editor in our window closed with the old plugin.
			if (g_pictureEditor.frame && plugin->EditorParent() != g_pictureEditor.frame) ReleasePictureEditor();
			if (!g_pictureEditor.frame && !OpenPictureEditor(plugin))
			{
				g_picture.Report(PluginPicture::State::Failed, slot, "The plugin's window could not be opened for the picture.");
				return;
			}
			g_pictureEditor.usedAt = now;
			top = content = g_pictureEditor.frame;
			openedAt = g_pictureEditor.openedAt;
		}
		g_picture.Capture(slot, top, content, openedAt != 0 && now - openedAt < kPictureSettleMs);
	}

	// Opens the window of a slot's plugin, or brings it to the front when it is open already.
	void OpenWindow(int slot)
	{
		PluginHost* plugin = g_chain.Plugin(slot);
		if (!plugin) return;
		g_shown = slot;
		if (AmpWindow* existing = WindowForPlugin(plugin))
		{
			if (g_windowsShown) ShowFrame(existing->frame);
			return;
		}
		auto created = std::make_unique<AmpWindow>();
		AmpWindow& window = *created;
		window.plugin = plugin;
		window.path = g_chain.Path(slot);
		// Tool window: no taskbar button or Alt+Tab entry; the overlay drives it.
		window.frame = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass, L"Rocksmith Audio Bridge",
			WS_POPUP | WS_CLIPCHILDREN, 0, 0, 800, 600, nullptr, nullptr, g_instance, nullptr);
		window.area = CreateWindowExW(0, kEditorClass, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, StripHeight(window.frame),
			800, 600, window.frame, nullptr, g_instance, nullptr);
		g_windows.push_back(std::move(created));

		const RECT area = WorkArea(window.frame);
		const int maxWidth = (area.right - area.left) * 9 / 10;
		const int maxHeight = (area.bottom - area.top) * 9 / 10 - StripHeight(window.frame);
		const HWND frame = window.frame;
		const auto onResized = [frame](int width, int height) { if (AmpWindow* found = WindowForFrame(frame)) FitWindow(*found, width, height); };
		// The Inspector's picture may hold this plugin's editor in its hidden window; the real window takes it over.
		if (g_pictureEditor.plugin == plugin) ReleasePictureEditor();
		if (!plugin->OpenEditor(window.area, onResized, maxWidth, maxHeight))
			FitWindow(window, Scale(window.frame, 460), Scale(window.frame, 60));
		UpdateTitle(window);
		PlaceWindow(window);
		if (g_windowsShown) ShowFrame(window.frame);
		RememberOpenWindows();
		Log("window: opened %s (%zu open)", plugin->Name().c_str(), g_windows.size());
	}

	// Closes a plugin's window. The plugin may already have left the chain (its editor went with it).
	void CloseWindow(const PluginHost* plugin)
	{
		const auto it = std::find_if(g_windows.begin(), g_windows.end(), [&](const auto& window) { return window->plugin == plugin; });
		if (it == g_windows.end()) return;
		AmpWindow& window = **it;
		const bool inChain = SlotOf(plugin) >= 0;
		Log("window: closing %s", inChain ? window.plugin->Name().c_str() : "a window whose plugin left the chain");
		if (inChain) window.plugin->CloseEditor();
		RememberPosition(window);
		const bool hadFocus = GetForegroundWindow() == window.frame;
		if (g_lentFrame == window.frame) g_lentFrame = nullptr;   // going anyway
		DestroyWindow(window.frame);
		g_windows.erase(it);
		RememberOpenWindowNames();
		if (hadFocus) if (HWND game = FindGameWindow()) SetForegroundWindow(game);
	}

	// After any chain change: windows whose plugin left the chain close, the others get their new number.
	void SyncWindows()
	{
		for (size_t i = g_windows.size(); i-- > 0;)
			if (SlotOf(g_windows[i]->plugin) < 0) CloseWindow(g_windows[i]->plugin);
		for (auto& window : g_windows) UpdateTitle(*window);
		RememberOpenWindows();
	}

	// A slot number comes from the page through shared memory: out of the mask's range means no window.
	bool InMask(unsigned mask, int slot) { return slot >= 0 && slot < 32 && (mask & (1u << slot)) != 0; }

	void ReopenWindows(unsigned mask)
	{
		for (int i = 0; i < g_chain.Count() && i < 32; ++i) if (InMask(mask, i)) OpenWindow(i);
	}

	void CloseAllWindows()
	{
		while (!g_windows.empty()) CloseWindow(g_windows.back()->plugin);
	}

	// The overlay's key, as the overlay last published it ([Keybinds] OverlayKey); backslash before it has.
	UINT OverlayKey()
	{
		const LONG key = g_control ? g_control->overlayKey : 0;
		return key > 0 && key < 256 ? static_cast<UINT>(key) : kDefaultOverlayKey;
	}

	// The overlay key while a plugin window has focus: the game never sees it, so hand focus back and pass the key on.
	// The game then closes its overlay, and the windows follow it (UpdateWindows).
	void OnOverlayKeyReleased(UINT key)
	{
		const HWND foreground = GetForegroundWindow();
		if (!foreground || !IsOurWindow(foreground)) return;
		if (HWND game = FindGameWindow())
		{
			SetForegroundWindow(game);
			const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
			PostMessageW(game, WM_KEYDOWN, key, 1 | (scan << 16));
			PostMessageW(game, WM_KEYUP, key, 1 | (scan << 16) | (1u << 30) | (1u << 31));
		}
	}

	// The open windows are on screen when the page's "Show plugin windows" is on and the overlay is open. Without
	// an overlay reporting in (Rocksmith closed, or an overlay that predates this), the switch alone decides.
	void UpdateWindows()
	{
		bool show = true;
		if (g_control)
		{
			const ULONGLONG now = GetTickCount64();
			const LONG overlayBeat = g_control->overlayBeat;
			if (overlayBeat != g_lastOverlayBeat) { g_lastOverlayBeat = overlayBeat; g_overlayBeatSeenAt = now; }
			const bool overlayReporting = g_overlayBeatSeenAt != 0 && now - g_overlayBeatSeenAt < static_cast<ULONGLONG>(VstHostControl::kAliveMs);
			const bool wanted = g_control->popoutWanted != 0;
			if (wanted != g_savedWanted) { g_savedWanted = wanted; WriteDword(L"PopoutWanted", wanted ? 1 : 0); }
			show = wanted && (!overlayReporting || g_control->overlayVisible != 0);
		}
		show = show && !g_standby;
		if (show == g_windowsShown) return;
		ReturnLentFrame();   // the Inspector's picture gives a hidden window back before the windows change
		g_windowsShown = show;
		const bool hadFocus = GetForegroundWindow() && IsOurWindow(GetForegroundWindow());
		for (auto& window : g_windows)
		{
			if (show) ShowFrame(window->frame);
			else ShowWindow(window->frame, SW_HIDE);
		}
		if (!show && hadFocus) if (HWND game = FindGameWindow()) SetForegroundWindow(game);
		if (g_control) g_control->popoutShown = show ? 1 : 0;
		Log("windows: %s (%zu open)", show ? "shown" : "hidden", g_windows.size());
	}

	// The overlay is on screen, or none reports in (Rocksmith closed, or an older overlay). The pedal window follows
	// this alone: the page opened it, so "Show plugin windows" (the VST rig's switch) does not apply.
	bool OverlayOnScreen()
	{
		if (!g_control) return true;
		const bool overlayReporting = g_overlayBeatSeenAt != 0 && GetTickCount64() - g_overlayBeatSeenAt < static_cast<ULONGLONG>(VstHostControl::kAliveMs);
		return !overlayReporting || g_control->overlayVisible != 0;
	}

	void PaintStrip(HWND frame)
	{
		PAINTSTRUCT paint;
		HDC dc = BeginPaint(frame, &paint);
		RECT client{};
		GetClientRect(frame, &client);
		RECT strip{ 0, 0, client.right, StripHeight(frame) };
		HBRUSH background = CreateSolidBrush(RGB(28, 28, 32));
		FillRect(dc, &strip, background);
		DeleteObject(background);

		SetBkMode(dc, TRANSPARENT);
		HGDIOBJ oldFont = SelectObject(dc, g_font);
		RECT text = strip;
		text.left += Scale(frame, 10);
		text.right -= CloseButtonWidth(frame) + Scale(frame, 8);
		SetTextColor(dc, RGB(225, 225, 230));
		const AmpWindow* window = WindowForFrame(frame);
		DrawTextW(dc, window ? window->title.c_str() : L"", -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

		RECT close{ client.right - CloseButtonWidth(frame), 0, client.right, StripHeight(frame) };
		SetTextColor(dc, RGB(200, 200, 205));
		DrawTextW(dc, L"✕", -1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dc, oldFont);
		EndPaint(frame, &paint);
	}

	bool InCloseButton(HWND frame, POINT client)
	{
		RECT rect{};
		GetClientRect(frame, &rect);
		return client.y >= 0 && client.y < StripHeight(frame) && client.x >= rect.right - CloseButtonWidth(frame);
	}

	// Everything the overlay shows about the chain.
	void PublishState()
	{
		if (!g_control) return;
		const int count = std::min(g_chain.Count(), VstHostControl::kMaxSlots);
		for (int i = 0; i < VstHostControl::kMaxSlots; ++i)
		{
			PluginHost* plugin = i < count ? g_chain.Plugin(i) : nullptr;
			CopyText(g_control->slots[i].name, sizeof(g_control->slots[i].name), plugin ? plugin->Name() : std::string());
			g_control->slots[i].bypass = plugin && g_chain.Bypassed(i) ? 1 : 0;
		}
		for (int i = 0; i < VstHostControl::kMaxSlots; ++i)
		{
			wchar_t* destination = const_cast<wchar_t*>(g_control->slotPaths[i]);
			memset(destination, 0, sizeof(g_control->slotPaths[i]));
			if (i < count) wcsncpy_s(destination, VstHostControl::kPathChars, g_chain.Path(i).c_str(), _TRUNCATE);
		}
		g_control->slotCount = count;
		g_control->chainBypassed = g_chain.ChainBypassed() ? 1 : 0;
		g_control->shownSlot = g_shown;
		g_control->openWindows = static_cast<LONG>(OpenWindowMask());
		CopyText(g_control->toneName, sizeof(g_control->toneName), Utf8(g_toneName));
		g_control->toneModified = g_toneModified ? 1 : 0;
		g_control->inputTenths = g_chain.InputTenths();
		g_control->outputTenths = g_chain.OutputTenths();
		const int spares = std::min(g_chain.SpareCount(), VstHostControl::kMaxSpares);
		for (int i = 0; i < VstHostControl::kMaxSpares; ++i)
		{
			wchar_t* destination = const_cast<wchar_t*>(g_control->sparePaths[i]);
			memset(destination, 0, sizeof(g_control->sparePaths[i]));
			if (i < spares) wcsncpy_s(destination, VstHostControl::kPathChars, g_chain.SparePath(i).c_str(), _TRUNCATE);
		}
		g_control->spareCount = spares;
		CopyText(g_control->undoLabel, sizeof(g_control->undoLabel), g_undo ? g_undo->getUndoDescription().toStdString() : std::string());
		CopyText(g_control->redoLabel, sizeof(g_control->redoLabel), g_undo ? g_undo->getRedoDescription().toStdString() : std::string());
		InterlockedIncrement(&g_control->stateSeq);
	}

	void RememberTone()
	{
		WriteString(L"ToneName", g_toneName);
		WriteDword(L"ToneModified", g_toneModified ? 1 : 0);
	}

	void SaveCurrentChain()
	{
		g_chain.SavePluginSettings();
		g_chain.SaveTone(CurrentChainFile());
		RememberTone();
	}

	void BeginLoading() { if (g_control) InterlockedExchange(&g_control->loading, 1); }
	void EndLoading()
	{
		if (!g_control) return;
		InterlockedIncrement(&g_control->hostBeat);
		g_loadingNow[0] = 0;
		CopyText(g_control->loadingText, sizeof(g_control->loadingText), {});
		InterlockedExchange(&g_control->loading, 0);
	}

	// What the page shows while plugins load (a heavy one takes seconds, a tone several of them): the plugin's file
	// name and the step. Called by the chain just before each plugin loads.
	void ReportLoading(const std::wstring& path, int step, int total)
	{
		std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
		if (name.size() > 5 && _wcsicmp(name.c_str() + name.size() - 5, L".vst3") == 0) name.resize(name.size() - 5);
		wcsncpy_s(g_loadingNow, name.c_str(), _TRUNCATE);
		Log("load: %ls (%d of %d)", name.c_str(), step, total);
		if (!g_control) return;
		CopyText(g_control->loadingText, sizeof(g_control->loadingText), Utf8(name));
		InterlockedExchange(&g_control->loadingStep, step);
		InterlockedExchange(&g_control->loadingTotal, total);
		InterlockedExchange(&g_control->loading, 1);
	}

	void MarkModified() { g_toneModified = true; }

	// Spares (Chain.hpp), checked every couple of seconds. How many plugins that left the chain stay loaded is
	// automatic, not a setting (a menu for it confused players): kKeepRecent, or none while the PC is short of
	// memory (they are reloaded from scratch then, as without spares). It also caps how many plugins the highlighted
	// song's tone loads ahead (PrepareTone). KeepLoadedPlugins: plugins marked "Always keep loaded" in the browser,
	// loaded in the background when they are not (PreloadStep), kept whatever the memory; g_sessionLoaded: "Load in
	// the background", kept until the host closes.
	constexpr int kKeepRecent = 4;            // a few amps and pedals to go back and forth between
	constexpr DWORD kMemoryTightPercent = 85; // system memory in use at which recent spares are let go

	void RefreshKeepSettings()
	{
		MEMORYSTATUSEX memory = { sizeof(memory) };
		const bool tight = GlobalMemoryStatusEx(&memory) && memory.dwMemoryLoad >= kMemoryTightPercent;
		const int recent = tight ? 0 : kKeepRecent;
		g_alwaysLoaded = ReadList(L"KeepLoadedPlugins");
		g_preloadFailed = ReadList(L"PreloadFailed");
		std::vector<std::wstring> keep = g_alwaysLoaded;
		for (const std::wstring& path : g_sessionLoaded) if (!ListHas(keep, path)) keep.push_back(path);
		const int before = g_chain.SpareCount();
		g_chain.SetKeep(static_cast<int>(recent), keep);
		if (g_chain.SpareCount() != before) PublishState();
	}

	// Loads one plugin as a spare now; the beat pauses meanwhile and `preloading` tells the page which. PreloadingNow
	// names it in the registry: if it takes the host down, the next start puts it on PreloadFailed (main) and the
	// background never tries it again until the player asks for it on the page.
	bool LoadSpareNow(const std::wstring& path, const char* why, bool recent)
	{
		std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
		if (name.size() > 5 && _wcsicmp(name.c_str() + name.size() - 5, L".vst3") == 0) name.resize(name.size() - 5);
		Log("spares: loading %ls in the background (%s)", name.c_str(), why);
		WriteString(L"PreloadingNow", path);
		wcsncpy_s(g_loadingNow, name.c_str(), _TRUNCATE);
		CopyText(g_control->preloadText, sizeof(g_control->preloadText), Utf8(name));
		InterlockedExchange(&g_control->preloading, 1);
		std::string error;
		const bool ok = g_chain.PreloadOne(path, error, recent);
		if (!ok)
		{
			Log("spares: %ls did not load (%s)", name.c_str(), error.c_str());
			if (!ListHas(g_preloadFailed, path)) g_preloadFailed.push_back(path);
			// Saved too: RefreshKeepSettings rereads the list from the registry, which dropped this in-memory mark
			// and retried the plugin at the next quiet moment.
			RecordPreloadFailure(path, error.empty() ? L"The plugin refused to load." : Wide(error));
		}
		WriteString(L"PreloadingNow", {});
		g_loadingNow[0] = 0;
		InterlockedIncrement(&g_control->hostBeat);
		InterlockedExchange(&g_control->preloading, 0);
		CopyText(g_control->preloadText, sizeof(g_control->preloadText), {});
		PublishState();
		return ok;
	}

	// One "Always keep loaded" plugin that is not loaded yet. Only while no song plays (the page says so; a plugin
	// starting up can cost the stream late buffers), with no command waiting and a few seconds after the last one,
	// so it never gets in the way of the player. One per call; the call comes again.
	void PreloadStep()
	{
		if (g_alwaysLoaded.empty() || !g_control || !g_control->preloadAllowed || g_inCommand) return;
		if (g_control->commandSeq != g_handledCommand || GetTickCount64() - g_lastCommandAt < 4000) return;
		for (const std::wstring& path : g_alwaysLoaded)
		{
			if (g_chain.IsLoaded(path) || ListHas(g_preloadFailed, path) || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
			LoadSpareNow(path, "always kept loaded", false);
			return;
		}
	}

	void SetStandby(bool on);

	// ---- Undo -------------------------------------------------------------------------------------------------
	// The page's chain edits can be taken back and done again (Command::Undo, Redo), the JUCE way: each edit is a
	// juce::UndoableAction (the Command pattern) kept by one juce::UndoManager on this thread. An action does its edit
	// through the Chain's own methods, so the stream pauses exactly as for the page's command, and knows how to take it
	// back. Only an edit that takes a plugin out of the chain (Remove, Replace, Clear) keeps that plugin's settings
	// (getStateInformation, a memento) to put it back as it was; nothing snapshots the whole tone. A plugin that comes
	// back is taken from the spares when it is still kept loaded (Chain::Insert), so undo is quick.
	// One page action is one undo step; a dragged level slider or a dragged parameter is one step too (coalesced).
	// Edits made in a plugin's own window are the plugin's business and are not recorded, as in any DAW. Loading a
	// tone or starting a new one clears the history. Each step carries a short name for the page's Edit menu
	// (undoLabel, redoLabel). If an undo or redo cannot be done (the chain is not what the step expects, a plugin
	// no longer loads), JUCE drops the whole history rather than leave steps that no longer fit.
	std::string g_undoKey;           // the last recorded edit, for coalescing ("levels", "param:2:14:7"); empty = none
	ULONGLONG g_undoAt = 0;
	int g_gesture = 0;               // SetParam gestures so far: each Begin or Set starts a new undo step
	constexpr ULONGLONG kLevelsJoinMs = 1500;   // level changes this close together are one step (one slider drag)
	constexpr int kUndoUnitsKept = 64 * 1024;   // units are kilobytes of kept plugin settings: about 64 MB
	constexpr int kUndoStepsKept = 30;          // at least this many steps whatever their size

	bool SlotIs(int slot, const std::wstring& path)
	{
		return slot >= 0 && slot < g_chain.Count() && _wcsicmp(g_chain.Path(slot).c_str(), path.c_str()) == 0;
	}

	void KeepShownInRange()
	{
		if (g_shown >= g_chain.Count()) g_shown = g_chain.Count() - 1;
	}

	// Two slots changed places: the page's selection follows its pedal.
	void SwapShown(int a, int b)
	{
		if (g_shown == a) g_shown = b;
		else if (g_shown == b) g_shown = a;
	}

	std::string SlotName(int slot)
	{
		PluginHost* plugin = g_chain.Plugin(slot);
		return plugin ? plugin->Name() : std::string("plugin");
	}

	std::string PluginFileName(const std::wstring& path)
	{
		std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
		if (name.size() > 5 && _wcsicmp(name.c_str() + name.size() - 5, L".vst3") == 0) name.resize(name.size() - 5);
		return Utf8(name);
	}

	// The undo history's size is counted in kilobytes of kept settings, so a few heavy amp states do not crowd out
	// many light steps.
	int SizeInUnits(size_t bytes) { return 1 + static_cast<int>(bytes / 1024); }

	// AddPlugin, `position` = the slot it goes in at (-1: the end). Undo takes it out again (kept as a spare); redo
	// puts it back with the settings it had then.
	class AddAction final : public juce::UndoableAction
	{
	public:
		AddAction(std::wstring path, int position) : m_path(std::move(path)), m_position(position) {}

		bool perform() override
		{
			std::string error;
			BeginLoading();
			bool ok;
			if (m_first)
			{
				m_first = false;
				ok = g_chain.Add(m_path, error);
				if (ok)
				{
					// Added last, then stepped back to its place (a plugin dropped between two pedals).
					int at = g_chain.Count() - 1;
					if (m_position >= 0)
						for (const int position = std::min(m_position, at); at > position; --at) g_chain.Move(at, -1);
					m_slot = at;
					OpenWindow(at);
				}
				else SetFailedPath(m_path);
			}
			else ok = g_chain.Insert(m_slot, m_path, m_state, m_bypass, error);
			if (ok) MarkModified();
			else SetLoadError(error.empty() ? "That plugin did not load." : error);
			EndLoading();
			return ok;
		}

		bool undo() override
		{
			if (!SlotIs(m_slot, m_path)) return false;
			PluginHost* plugin = g_chain.Plugin(m_slot);
			plugin->GetState(m_state);
			m_bypass = g_chain.Bypassed(m_slot);
			CloseWindow(plugin);
			g_chain.Remove(m_slot);
			MarkModified();
			KeepShownInRange();
			return true;
		}

		int getSizeInUnits() override { return SizeInUnits(m_state.size()); }

	private:
		std::wstring m_path;
		int m_position;
		int m_slot = -1;
		bool m_first = true;
		std::vector<char> m_state;   // as it was when undo took it out
		bool m_bypass = false;
	};

	// ReplaceSlot. The plugin that goes keeps its settings here, so undo brings it back exactly; redo gives the new one
	// back the settings it had when undo took it out. The slot keeps its bypass, and a slot with a window gets one for
	// its new plugin.
	class ReplaceAction final : public juce::UndoableAction
	{
	public:
		ReplaceAction(int slot, std::wstring path) : m_slot(slot), m_newPath(std::move(path)) {}

		bool perform() override
		{
			if (m_first) { if (!g_chain.Plugin(m_slot)) return false; m_oldPath = g_chain.Path(m_slot); }
			else if (!SlotIs(m_slot, m_oldPath)) return false;
			std::vector<char> before;
			g_chain.Plugin(m_slot)->GetState(before);
			const bool ok = Swap(m_newPath, m_first ? std::vector<char>() : m_newState);
			if (ok) m_oldState = std::move(before);
			else if (m_first) SetFailedPath(m_newPath);
			m_first = false;
			return ok;
		}

		bool undo() override
		{
			if (!SlotIs(m_slot, m_newPath)) return false;
			std::vector<char> after;
			g_chain.Plugin(m_slot)->GetState(after);
			if (!Swap(m_oldPath, m_oldState)) return false;
			m_newState = std::move(after);
			return true;
		}

		int getSizeInUnits() override { return SizeInUnits(m_oldState.size() + m_newState.size()); }

	private:
		bool Swap(const std::wstring& path, const std::vector<char>& state)
		{
			PluginHost* old = g_chain.Plugin(m_slot);
			const bool hadWindow = WindowForPlugin(old) != nullptr;
			BeginLoading();
			CloseWindow(old);   // before the plugin goes
			std::string error;
			const bool ok = g_chain.Replace(m_slot, path, error);
			if (ok)
			{
				// The plugin plays already; JUCE keeps state calls and processing apart (as LoadTone does for kept ones).
				if (!state.empty()) g_chain.Plugin(m_slot)->SetState(state);
				MarkModified();
			}
			else SetLoadError(error.empty() ? "That plugin did not load." : error);
			if (hadWindow) OpenWindow(m_slot);   // the new pedal gets the old one's window
			EndLoading();
			return ok;
		}

		int m_slot;
		std::wstring m_newPath, m_oldPath;
		std::vector<char> m_oldState, m_newState;
		bool m_first = true;
	};

	// RemoveSlot. The plugin's settings, bypass and window are kept, so undo puts it back as it was.
	class RemoveAction final : public juce::UndoableAction
	{
	public:
		explicit RemoveAction(int slot) : m_slot(slot) {}

		bool perform() override
		{
			if (m_first) { if (!g_chain.Plugin(m_slot)) return false; m_path = g_chain.Path(m_slot); m_first = false; }
			else if (!SlotIs(m_slot, m_path)) return false;
			PluginHost* plugin = g_chain.Plugin(m_slot);
			plugin->GetState(m_state);
			m_bypass = g_chain.Bypassed(m_slot);
			m_hadWindow = WindowForPlugin(plugin) != nullptr;
			CloseWindow(plugin);
			g_chain.Remove(m_slot);
			MarkModified();
			KeepShownInRange();
			return true;
		}

		bool undo() override
		{
			std::string error;
			BeginLoading();
			const bool ok = g_chain.Insert(m_slot, m_path, m_state, m_bypass, error);
			if (ok)
			{
				MarkModified();
				if (m_hadWindow) OpenWindow(m_slot);
			}
			else SetLoadError(error.empty() ? "That plugin did not load again." : error);
			EndLoading();
			return ok;
		}

		int getSizeInUnits() override { return SizeInUnits(m_state.size()); }

	private:
		int m_slot;
		std::wstring m_path;
		std::vector<char> m_state;
		bool m_bypass = false;
		bool m_hadWindow = false;
		bool m_first = true;
	};

	// MoveSlot: one step earlier (-1) or later (+1); undo moves it back.
	class MoveAction final : public juce::UndoableAction
	{
	public:
		MoveAction(int slot, int delta) : m_slot(slot), m_delta(delta) {}

		bool perform() override { return Move(m_slot, m_delta); }
		bool undo() override { return Move(m_slot + m_delta, -m_delta); }

	private:
		static bool Move(int slot, int delta)
		{
			if (slot < 0 || slot >= g_chain.Count() || slot + delta < 0 || slot + delta >= g_chain.Count()) return false;
			g_chain.Move(slot, delta);
			SwapShown(slot, slot + delta);
			MarkModified();
			return true;
		}

		int m_slot, m_delta;
	};

	// SetBypass on one slot.
	class BypassAction final : public juce::UndoableAction
	{
	public:
		BypassAction(int slot, bool bypass) : m_slot(slot), m_bypass(bypass) {}

		bool perform() override
		{
			if (m_first) { if (!g_chain.Plugin(m_slot)) return false; m_path = g_chain.Path(m_slot); m_was = g_chain.Bypassed(m_slot); m_first = false; }
			return Set(m_bypass);
		}

		bool undo() override { return Set(m_was); }

	private:
		bool Set(bool bypass)
		{
			if (!SlotIs(m_slot, m_path)) return false;
			g_chain.SetBypass(m_slot, bypass);
			MarkModified();
			return true;
		}

		int m_slot;
		bool m_bypass, m_was = false, m_first = true;
		std::wstring m_path;
	};

	// DuplicateSlot: the copy goes right after it. Undo takes the copy out (its settings kept for redo).
	class DuplicateAction final : public juce::UndoableAction
	{
	public:
		explicit DuplicateAction(int slot) : m_slot(slot) {}

		bool perform() override
		{
			std::string error;
			BeginLoading();
			bool ok;
			if (m_first)
			{
				m_first = false;
				const bool hadWindow = WindowForPlugin(g_chain.Plugin(m_slot)) != nullptr;
				ok = g_chain.Duplicate(m_slot, error);
				if (ok)
				{
					m_path = g_chain.Path(m_slot + 1);
					if (hadWindow) OpenWindow(m_slot + 1);
				}
			}
			else ok = SlotIs(m_slot, m_path) && g_chain.Insert(m_slot + 1, m_path, m_state, m_bypass, error);
			if (ok) MarkModified();
			else SetLoadError(error.empty() ? "That plugin did not load a second time." : error);
			EndLoading();
			return ok;
		}

		bool undo() override
		{
			if (!SlotIs(m_slot + 1, m_path)) return false;
			PluginHost* copy = g_chain.Plugin(m_slot + 1);
			copy->GetState(m_state);
			m_bypass = g_chain.Bypassed(m_slot + 1);
			CloseWindow(copy);
			g_chain.Remove(m_slot + 1);
			MarkModified();
			KeepShownInRange();
			return true;
		}

		int getSizeInUnits() override { return SizeInUnits(m_state.size()); }

	private:
		int m_slot;
		std::wstring m_path;
		std::vector<char> m_state;
		bool m_bypass = false;
		bool m_first = true;
	};

	// ClearChain as a step ("clear board"): every plugin with its settings, bypass and window, and the tone's name,
	// are kept so undo puts the board back as it was.
	class ClearAction final : public juce::UndoableAction
	{
	public:
		bool perform() override
		{
			if (g_chain.Count() == 0 && g_toneName.empty()) return false;
			m_slots.clear();
			for (int i = 0; i < g_chain.Count(); ++i)
			{
				Entry entry{ g_chain.Path(i), {}, g_chain.Bypassed(i) };
				g_chain.Plugin(i)->GetState(entry.state);
				m_slots.push_back(std::move(entry));
			}
			m_windows = OpenWindowMask();
			m_toneName = g_toneName;
			m_toneModified = g_toneModified;
			m_shown = g_shown;
			CloseAllWindows();
			g_chain.Clear();
			g_toneName.clear();
			g_toneModified = false;
			g_shown = -1;
			return true;
		}

		bool undo() override
		{
			if (g_chain.Count() != 0) return false;
			bool ok = true;
			std::string firstError;
			BeginLoading();
			for (size_t i = 0; i < m_slots.size(); ++i)
			{
				std::string error;
				if (!g_chain.Insert(static_cast<int>(i), m_slots[i].path, m_slots[i].state, m_slots[i].bypass, error))
				{
					ok = false;
					if (firstError.empty()) firstError = PluginFileName(m_slots[i].path) + ": " + (error.empty() ? "it did not load again." : error);
				}
			}
			g_toneName = m_toneName;
			g_toneModified = m_toneModified || !ok;
			g_shown = std::min(m_shown, g_chain.Count() - 1);
			RememberTone();
			if (ok) ReopenWindows(m_windows);   // slot numbers only match when every plugin came back
			else SetLoadError(firstError);
			EndLoading();
			return ok;
		}

		int getSizeInUnits() override
		{
			size_t bytes = 0;
			for (const Entry& entry : m_slots) bytes += entry.state.size();
			return SizeInUnits(bytes);
		}

	private:
		struct Entry
		{
			std::wstring path;
			std::vector<char> state;
			bool bypass = false;
		};
		std::vector<Entry> m_slots;
		unsigned m_windows = 0;
		std::wstring m_toneName;
		bool m_toneModified = false;
		int m_shown = -1;
	};

	// SetChainBypass (every plugin skipped). Not part of the tone, so the tone is not marked changed.
	class ChainBypassAction final : public juce::UndoableAction
	{
	public:
		explicit ChainBypassAction(bool bypass) : m_bypass(bypass) {}

		bool perform() override
		{
			if (m_first) { m_was = g_chain.ChainBypassed(); m_first = false; if (m_was == m_bypass) return false; }
			Set(m_bypass);
			return true;
		}

		bool undo() override { Set(m_was); return true; }

	private:
		static void Set(bool bypass)
		{
			g_chain.SetChainBypass(bypass);
			Log("chain: %s", bypass ? "bypassed (clean guitar)" : "playing");
		}

		bool m_bypass, m_was = false, m_first = true;
	};

	// SetLevels. Global, not part of the tone: saved for every session and every tone; the tone is not marked
	// changed. A slider drag sends many; consecutive ones coalesce into one step (Record's key, createCoalescedAction).
	class LevelsAction final : public juce::UndoableAction
	{
	public:
		LevelsAction(int input, int output) : m_input(input), m_output(output) {}

		bool perform() override
		{
			if (m_first) { m_oldInput = g_chain.InputTenths(); m_oldOutput = g_chain.OutputTenths(); m_first = false; }
			Apply(m_input, m_output);
			return true;
		}

		bool undo() override { Apply(m_oldInput, m_oldOutput); return true; }

		juce::UndoableAction* createCoalescedAction(juce::UndoableAction* next) override
		{
			const auto* later = dynamic_cast<LevelsAction*>(next);
			if (!later) return nullptr;
			auto* joined = new LevelsAction(later->m_input, later->m_output);
			joined->m_oldInput = m_oldInput;
			joined->m_oldOutput = m_oldOutput;
			joined->m_first = false;
			return joined;
		}

	private:
		static void Apply(int input, int output)
		{
			g_chain.SetLevels(input, output);
			WriteDword(L"InputTrimTenths", static_cast<DWORD>(input));
			WriteDword(L"OutputTrimTenths", static_cast<DWORD>(output));
		}

		int m_input, m_output, m_oldInput = 0, m_oldOutput = 0;
		bool m_first = true;
	};

	// SetParam from the Inspector. The first perform follows the gesture (a drag: beginChangeGesture on Begin, the
	// value on every Change, endChangeGesture on End); undo and redo set the value as one complete gesture each. The
	// steps of one gesture coalesce into one undo step that goes from the value before the drag to the last one.
	class ParamAction final : public juce::UndoableAction
	{
	public:
		ParamAction(int slot, int index, float value, VstHostControl::ParamPhase phase) : m_slot(slot), m_index(index), m_value(value), m_phase(phase) {}

		bool perform() override
		{
			using VstHostControl::ParamPhase;
			if (m_first)
			{
				PluginHost* plugin = g_chain.Plugin(m_slot);
				if (!plugin || !plugin->HasParameter(m_index)) return false;
				m_path = g_chain.Path(m_slot);
				m_old = plugin->ParameterValue(m_index);
				m_first = false;
				if (m_phase == ParamPhase::Begin) plugin->BeginGesture(m_index);
				plugin->SetParameter(m_index, m_value, m_phase == ParamPhase::Set);
				if (m_phase == ParamPhase::End) plugin->EndGesture(m_index);
				return true;
			}
			return Set(m_value);
		}

		bool undo() override { return Set(m_old); }

		juce::UndoableAction* createCoalescedAction(juce::UndoableAction* next) override
		{
			const auto* later = dynamic_cast<ParamAction*>(next);
			if (!later || later->m_slot != m_slot || later->m_index != m_index) return nullptr;
			auto* joined = new ParamAction(m_slot, m_index, later->m_value, later->m_phase);
			joined->m_path = m_path;
			joined->m_old = m_old;
			joined->m_first = false;
			return joined;
		}

	private:
		bool Set(float value)
		{
			if (!SlotIs(m_slot, m_path)) return false;
			g_chain.Plugin(m_slot)->SetParameter(m_index, value, true);
			return true;
		}

		int m_slot, m_index;
		float m_value, m_old = 0.0f;
		VstHostControl::ParamPhase m_phase;
		std::wstring m_path;
		bool m_first = true;
	};

	// Does an edit and records it as one undo step named `name`. An edit with the same non-empty `key` as the last
	// one recorded joins that step instead (coalesced), within `joinMs` of it when that is not 0. False when the edit
	// could not be done (nothing is recorded then).
	bool Record(juce::UndoableAction* action, const std::string& name, const std::string& key = {}, ULONGLONG joinMs = 0)
	{
		const ULONGLONG now = GetTickCount64();
		const bool joins = !key.empty() && key == g_undoKey && (joinMs == 0 || now - g_undoAt < joinMs);
		if (!joins) g_undo->beginNewTransaction(juce::String::fromUTF8(name.c_str()));
		const bool done = g_undo->perform(action);
		g_undoKey = done ? key : std::string();
		g_undoAt = now;
		return done;
	}

	void ClearUndo(const char* why)
	{
		if (!g_undo->canUndo() && !g_undo->canRedo()) return;
		g_undo->clearUndoHistory();
		g_undoKey.clear();
		Log("undo: history cleared (%s)", why);
	}

	// ---- Inspector ------------------------------------------------------------------------------------------------
	// The page's Inspector shows the parameters of one slot (inspectSlot). They are published under a sequence lock
	// when the plugin reports a change (its window, automation, the page's own SetParam), at most every
	// kParamsMinMs, and every kBeatMs regardless, so a value a plugin changes without telling still shows.
	constexpr ULONGLONG kParamsMinMs = 40;
	LONG g_paramsSlot = -2;
	const PluginHost* g_paramsPlugin = nullptr;
	uint32_t g_paramsChanges = 0;
	ULONGLONG g_paramsAt = 0;

	void PublishParams()
	{
		if (!g_control) return;
		const LONG wanted = g_control->inspectSlot;
		PluginHost* plugin = g_chain.Plugin(wanted);   // none for -1 or a slot past the chain's end
		const LONG slot = plugin ? wanted : -1;
		const ULONGLONG now = GetTickCount64();
		const bool moved = slot != g_paramsSlot || plugin != g_paramsPlugin || (plugin && plugin->ParameterChanges() != g_paramsChanges);
		if (!(moved && now - g_paramsAt >= kParamsMinMs) && now - g_paramsAt < static_cast<ULONGLONG>(VstHostControl::kBeatMs)) return;
		g_paramsSlot = slot;
		g_paramsPlugin = plugin;
		g_paramsChanges = plugin ? plugin->ParameterChanges() : 0;
		g_paramsAt = now;
		int total = 0;
		const std::vector<PluginHost::ParameterInfo> list = plugin ? plugin->ListParameters(VstHostControl::kMaxParams, total) : std::vector<PluginHost::ParameterInfo>();
		InterlockedIncrement(&g_control->paramSeq);   // odd: being written
		g_control->paramSlot = slot;
		g_control->paramTotal = total;
		g_control->paramCount = static_cast<LONG>(list.size());
		for (size_t i = 0; i < list.size(); ++i)
		{
			const PluginHost::ParameterInfo& info = list[i];
			VstHostControl::Param& param = g_control->params[i];
			param.index = info.index;
			param.value = info.value;
			param.steps = info.steps;
			param.flags = (info.discrete ? VstHostControl::kParamDiscrete : 0) | (info.boolean ? VstHostControl::kParamBoolean : 0);
			CopyText(param.name, sizeof(param.name), info.name);
			CopyText(param.text, sizeof(param.text), info.text);
			CopyText(param.label, sizeof(param.label), info.label);
		}
		InterlockedIncrement(&g_control->paramSeq);   // even: complete
	}

	// SetParam: a drag (Begin, Change..., End) is one gesture and one undo step; Set is both on its own.
	void RunSetParam(int slot, int index, float value, VstHostControl::ParamPhase phase)
	{
		using VstHostControl::ParamPhase;
		if (!std::isfinite(value)) return;
		if (phase == ParamPhase::Begin || phase == ParamPhase::Set) ++g_gesture;
		PluginHost* plugin = g_chain.Plugin(slot);
		if (!plugin) return;
		const std::string key = "param:" + std::to_string(slot) + ":" + std::to_string(index) + ":" + std::to_string(g_gesture);
		Record(new ParamAction(slot, index, value, phase), "change " + plugin->ParameterName(index), key);
		if (phase == ParamPhase::End || phase == ParamPhase::Set) g_undoKey.clear();   // the next edit is a step of its own
		g_paramsAt = 0;   // the Inspector shows the new value at once
	}

	// One command from the Tone designer page.
	void RunCommand(VstHostControl::Command op, int arg, int arg2, const std::wstring& text, float value, int phase)
	{
		using VstHostControl::Command;
		SetLoadError({});
		std::string error;
		const unsigned openBefore = OpenWindowMask();
		switch (op)
		{
		case Command::AddPlugin:
		{
			// arg = position + 1 to insert it there (a plugin dropped between two pedals); 0 appends. The step is
			// named after the plugin once it has loaded (its file name until then).
			const int position = arg > 0 ? arg - 1 : -1;
			if (Record(new AddAction(text, position), "add " + PluginFileName(text)))
			{
				const int at = position >= 0 ? std::min(position, g_chain.Count() - 1) : g_chain.Count() - 1;
				g_undo->setCurrentTransactionName(juce::String::fromUTF8(("add " + SlotName(at)).c_str()));
			}
			break;
		}
		case Command::ReplaceSlot:
			if (g_chain.Plugin(arg)) Record(new ReplaceAction(arg, text), "replace " + SlotName(arg));
			break;
		case Command::RemoveSlot:
			Record(new RemoveAction(arg), "remove " + SlotName(arg));
			break;
		case Command::MoveSlot:
			Record(new MoveAction(arg, arg2), "move " + SlotName(arg));
			break;
		case Command::SetBypass:
			Record(new BypassAction(arg, arg2 != 0), (arg2 != 0 ? "bypass " : "turn on ") + SlotName(arg));
			break;
		case Command::ShowSlot:
			OpenWindow(arg);
			break;
		case Command::HideSlot:
			if (PluginHost* plugin = g_chain.Plugin(arg)) CloseWindow(plugin);
			break;
		case Command::LoadTone:
		{
			const std::wstring file = ToneFile(text);
			BeginLoading();
			// Plugins the tone shares with the chain are kept (Chain::LoadTone), and so are their windows; windows
			// of plugins that go close in SyncWindows, and slots that had a window get one for their new plugin.
			std::string missing;
			if (g_chain.LoadTone(file, error, missing))
			{
				g_toneName = text;
				g_toneModified = false;
				// The tone file is the chain as it now plays: copy it rather than asking every plugin for its
				// settings again (TONE3000's are close to a megabyte).
				if (!CopyFileW(file.c_str(), CurrentChainFile().c_str(), FALSE)) SaveCurrentChain();
				else RememberTone();
				if (!missing.empty()) SetLoadError("Not installed, skipped: " + missing);
				ClearUndo("a tone was loaded");
			}
			else SetLoadError(error);
			ReopenWindows(openBefore);
			EndLoading();
			break;
		}
		case Command::SaveTone:
		{
			const std::wstring file = ToneFile(text);
			if (file.empty()) { SetLoadError("Give the tone a name."); break; }
			CreateFolderFor(file);
			g_chain.SavePluginSettings();
			if (g_chain.SaveTone(file))
			{
				g_toneName = text;
				g_toneModified = false;
				SaveCurrentChain();
			}
			else SetLoadError("The tone could not be saved.");
			break;
		}
		case Command::DeleteTone:
		{
			const std::wstring file = ToneFile(text);
			if (!file.empty() && DeleteFileW(file.c_str()))
			{
				Log("tone: deleted %ls", file.c_str());
				RemoveFolderIfEmpty(file);
				if (_wcsicmp(g_toneName.c_str(), text.c_str()) == 0) { g_toneName.clear(); RememberTone(); }
			}
			break;
		}
		case Command::SetLevels:
			// Global, not part of the tone (LevelsAction); a slider drag is one step.
			Record(new LevelsAction(arg, arg2), "levels", "levels", kLevelsJoinMs);
			break;
		case Command::SetMuted:
			g_chain.SetMuted(arg != 0);
			Log("chain: %s", arg ? "muted (tuning)" : "unmuted");
			break;
		case Command::DuplicateSlot:
			if (g_chain.Plugin(arg)) Record(new DuplicateAction(arg), "duplicate " + SlotName(arg));
			break;
		case Command::SetChainBypass:
			Record(new ChainBypassAction(arg != 0), arg != 0 ? "bypass whole chain" : "turn on whole chain");
			break;
		case Command::SetParam:
			RunSetParam(arg, arg2, value, static_cast<VstHostControl::ParamPhase>(phase));
			break;
		case Command::Undo:
			if (g_undo->canUndo())
			{
				Log("undo: %s", g_undo->getUndoDescription().toRawUTF8());
				g_undo->undo();
			}
			g_undoKey.clear();
			KeepShownInRange();
			break;
		case Command::Redo:
			if (g_undo->canRedo())
			{
				Log("redo: %s", g_undo->getRedoDescription().toRawUTF8());
				g_undo->redo();
			}
			g_undoKey.clear();
			KeepShownInRange();
			break;
		case Command::RenameTone:
		{
			const size_t split = text.find(L'\n');
			if (split == std::wstring::npos) break;
			const std::wstring oldName = text.substr(0, split), newName = text.substr(split + 1);
			const std::wstring from = ToneFile(oldName), to = ToneFile(newName);
			if (from.empty() || to.empty()) { SetLoadError("Give the tone a name."); break; }
			if (GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES && _wcsicmp(from.c_str(), to.c_str()) != 0)
			{
				SetLoadError("A tone with that name already exists.");
				break;
			}
			CreateFolderFor(to);   // a rename can move it into a folder
			if (!MoveFileExW(from.c_str(), to.c_str(), 0)) { SetLoadError("The tone could not be renamed."); break; }   // a change of case works too
			RemoveFolderIfEmpty(from);
			Log("tone: renamed %ls to %ls", from.c_str(), to.c_str());
			if (_wcsicmp(g_toneName.c_str(), oldName.c_str()) == 0) { g_toneName = newName; RememberTone(); }
			break;
		}
		case Command::PrepareTone:
		{
			// The song list's highlighted song (the page asks after a moment on it): its tone's plugins are loaded
			// as spares while the chain plays on, so starting the song switches at once. No more than the recent
			// limit keeps, or Trim would unload them again straight away.
			const std::vector<std::wstring> needed = g_chain.NotLoaded(Chain::TonePlugins(ToneFile(text)));
			int room = g_chain.KeepRecent();
			for (const std::wstring& path : needed)
			{
				if (room-- <= 0) break;
				if (ListHas(g_preloadFailed, path) || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
				LoadSpareNow(path, "the highlighted song's tone", true);
			}
			break;
		}
		case Command::LoadSpare:
			// The player's own choice: loaded even if it failed that way before, and kept while the host runs.
			if (!ListHas(g_sessionLoaded, text)) g_sessionLoaded.push_back(text);
			RefreshKeepSettings();
			if (!g_chain.IsLoaded(text)) LoadSpareNow(text, "asked for on the page", true);
			break;
		case Command::UnloadSpare:
			g_sessionLoaded.erase(std::remove_if(g_sessionLoaded.begin(), g_sessionLoaded.end(),
				[&](const std::wstring& p) { return _wcsicmp(p.c_str(), text.c_str()) == 0; }), g_sessionLoaded.end());
			RefreshKeepSettings();
			g_chain.DropSpares(text);
			break;
		case Command::SetStandby:
			SetStandby(arg != 0);
			break;
		case Command::ReloadPlugin:
		{
			// The plugin browser's or a pedal's Reload: its windows close before the plugin goes and reopen after.
			// They are matched by copy, not slot number: a copy that does not come back moves the ones after it up.
			// Other plugins' windows stay open.
			std::vector<bool> hadWindow;   // per copy in the chain, in chain order
			for (int i = 0; i < g_chain.Count(); ++i)
				if (_wcsicmp(g_chain.Path(i).c_str(), text.c_str()) == 0)
				{
					hadWindow.push_back(WindowForPlugin(g_chain.Plugin(i)) != nullptr);
					CloseWindow(g_chain.Plugin(i));
				}
			BeginLoading();
			int sparesDropped = 0;
			std::vector<int> slots;
			const int countBefore = g_chain.Count();
			const bool reloaded = g_chain.Reload(text, error, sparesDropped, slots);
			if (!reloaded) { SetLoadError(error); SetFailedPath(text); }
			// A spare (Always keep loaded, Load in the background, or a recent one) comes back as a spare, unless the
			// chain's copy now covers it. Not after a failed reload: it would only fail again.
			else if (sparesDropped > 0 && slots.empty()) LoadSpareNow(text, "reloaded", true);
			for (size_t k = 0; k < slots.size() && k < hadWindow.size(); ++k)
				if (hadWindow[k] && slots[k] >= 0) OpenWindow(slots[k]);
			// A copy that did not come back moved the slots after it: the history's steps no longer fit the chain.
			if (g_chain.Count() != countBefore) { MarkModified(); ClearUndo("a plugin did not reload"); }
			if (g_shown >= g_chain.Count()) g_shown = g_chain.Count() - 1;
			EndLoading();
			break;
		}
		case Command::ClearChain:
			// arg = 1: a new tone, which starts a fresh history; 0: the board cleared, one undo step.
			if (arg == 0) { Record(new ClearAction(), "clear board"); break; }
			CloseAllWindows();
			g_chain.Clear();
			g_toneName.clear();
			g_toneModified = false;
			g_shown = -1;
			ClearUndo("a new tone");
			break;
		default:
			break;
		}
		SyncWindows();
		PublishState();
	}

	void PollCommand()
	{
		if (!g_control || g_inCommand || g_control->commandSeq == g_handledCommand) return;
		g_handledCommand = g_control->commandSeq;
		g_lastCommandAt = GetTickCount64();
		wchar_t text[VstHostControl::kPathChars]{};
		memcpy(text, const_cast<const wchar_t*>(g_control->commandText), sizeof(text) - sizeof(wchar_t));
		g_inCommand = true;
		RunCommand(static_cast<VstHostControl::Command>(g_control->commandOp), g_control->commandArg, g_control->commandArg2, text,
			g_control->commandValue, g_control->commandPhase);
		g_inCommand = false;
		InterlockedExchange(&g_control->commandDone, g_handledCommand);
	}

	// Meters for the overlay: a peak with a short fall, in thousandths of full scale.
	void PublishMeters()
	{
		if (!g_control) return;
		const float inputPeak = g_stompboxPlays ? g_stompboxService.TakeInputPeak() : g_chain.TakeInputPeak();
		const float outputPeak = g_stompboxPlays ? g_stompboxService.TakeOutputPeak() : g_chain.TakeOutputPeak();
		g_inputMeter = std::max(inputPeak, g_inputMeter * 0.85f);
		g_outputMeter = std::max(outputPeak, g_outputMeter * 0.85f);
		g_control->inputPeak = static_cast<LONG>(std::min(g_inputMeter, 4.0f) * 1000.0f);
		g_control->outputPeak = static_cast<LONG>(std::min(g_outputMeter, 4.0f) * 1000.0f);
	}

	// Once a second: how long the stream's blocks took against the block period, measured on this PC while the
	// player plays. The Tone designer page shows it as the tone's load (StompboxService::PublishLoad). The worst
	// second of every 30 goes to the log while the Tone designer engine plays.
	BlockTiming g_worstTiming;

	void MeasureStreamLoad()
	{
		const BlockTiming timing = g_router.TakeTiming();
		if (timing.blocks == 0) return;
		g_stompboxService.PublishLoad(timing, g_audio.Frames());
		g_worstTiming.blocks += timing.blocks;
		g_worstTiming.medianMicroseconds = std::max(g_worstTiming.medianMicroseconds, timing.medianMicroseconds);
		g_worstTiming.percentile99Microseconds = std::max(g_worstTiming.percentile99Microseconds, timing.percentile99Microseconds);
		g_worstTiming.percentile999Microseconds = std::max(g_worstTiming.percentile999Microseconds, timing.percentile999Microseconds);
		g_worstTiming.maximumMicroseconds = std::max(g_worstTiming.maximumMicroseconds, timing.maximumMicroseconds);
	}

	void LogStreamTiming()
	{
		const BlockTiming timing = g_worstTiming;
		g_worstTiming = {};
		if (timing.blocks == 0 || !g_stompboxPlays) return;
		const double periodMicroseconds = 1000000.0 * g_audio.Frames() / SAMPLE_RATE;
		Log("stompbox: %llu blocks of %d frames (%.0f us each), worst second: median %.0f us, p99 %.0f us, p99.9 %.0f us, max %.0f us, limiter %.1f dB on the last block",
			static_cast<unsigned long long>(timing.blocks), g_audio.Frames(), periodMicroseconds, timing.medianMicroseconds,
			timing.percentile99Microseconds, timing.percentile999Microseconds, timing.maximumMicroseconds,
			20.0 * std::log10(std::max(1e-6, 1.0 - g_router.LimiterReduction())));
	}

	// Open (or reopen after the game changed its buffer size) the stream and run the chain on it.
	bool StartAudio(HWND window)
	{
		g_audio.Stop();
		g_chain.Suspend();
		std::string error;
		if (!g_audio.Open(window, &g_router, error) || !g_chain.Prepare(g_audio.Frames(), error))
		{
			ShowError(error);
			return false;
		}
		if (g_stompboxPlays) g_stompboxService.PrepareStream(g_audio.Frames());   // the stream is stopped: Stompbox may allocate
		g_router.SetSource(g_stompboxPlays ? g_stompboxService.Sink() : &g_chain, g_stompboxPlays);
		if (!g_audio.Start(error))
		{
			ShowError(error);
			return false;
		}
		return true;
	}

	// The Tone designer page chose its engine (true) or the VST chain (false). Reopens the stream so the engine is
	// prepared while it is stopped; in standby the stream opens with this source when the player turns it on.
	bool SetStompboxPlays(bool plays)
	{
		if (plays == g_stompboxPlays) return true;
		g_stompboxPlays = plays;
		Log("stompbox: the stream plays through %s", plays ? "the Tone designer engine" : "the VST chain");
		return g_standby || StartAudio(g_main);
	}

	// Off (standby) and back on. Stopping the stream releases the amp link, so the overlay's ExternalAmp brings the
	// game's amp back exactly as when the host closes; the plugins stay loaded. On reopens the stream (the game may
	// have changed its buffer size meanwhile); if that fails the host stays in standby and the page shows why.
	void SetStandby(bool on)
	{
		if (on == g_standby) return;
		if (on)
		{
			g_audio.Stop();
			g_chain.Suspend();
			g_standby = true;
			Log("host: standby (off, plugins kept loaded)");
		}
		else
		{
			g_standby = false;
			if (!StartAudio(g_main)) { g_audio.Stop(); g_standby = true; SetLoadError("Tone designer could not reconnect to the game's audio, so it stayed off."); }
			else Log("host: on");
		}
		if (g_control) InterlockedExchange(&g_control->standby, g_standby ? 1 : 0);
		UpdateWindows();
	}

	// The hidden main window: the timers, the audio link's reset message, and shutdown.
	LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case BridgeAudio::kResetMessage:
			if (g_standby) return 0;   // turning on opens it fresh
			Log("audio: the game changed its stream, restarting");
			StartAudio(window);
			return 0;
		case WM_TIMER:
			if (wParam == kKeyTimer)
			{
				// Only while one of our windows has the keyboard (otherwise the game sees the key itself): the key
				// read goes through Steam's overlay hook, which Steam injects into this process, so it is not free.
				const HWND foreground = GetForegroundWindow();
				const UINT overlayKey = OverlayKey();
				const bool down = foreground && IsOurWindow(foreground) && (GetAsyncKeyState(overlayKey) & 0x8000) != 0;
				if (g_keyDown && !down) OnOverlayKeyReleased(overlayKey);
				g_keyDown = down;
				UpdateWindows();
				PollCommand();
				PublishParams();
				UpdatePicture();
				g_stompboxService.Poll();
				if (g_pedalEditor) g_pedalEditor->Poll(OverlayOnScreen());
				PublishMeters();
				if (g_control && g_control->quitRequested)
				{
					InterlockedExchange(&g_control->quitRequested, 0);
					Log("host: closed from the overlay");
					PostMessageW(window, WM_CLOSE, 0, 0);
				}
				return 0;
			}
			if (g_control) InterlockedIncrement(&g_control->hostBeat);
			if (g_control) InterlockedExchange(&g_control->chainLatencySamples, g_chain.LatencySamples());
			InstallCrashReporter();   // a plugin may have replaced it with its own
			if (g_chain.TakeRestartRequest() && !g_standby) StartAudio(window);
			if (g_ticks % 4 == 0) RefreshKeepSettings();
			PreloadStep();
			if (g_chain.Dirty() && !g_toneModified) { g_toneModified = true; PublishState(); }
			if (g_ticks % 2 == 0) MeasureStreamLoad();
			if (++g_ticks >= kAutosaveTicks)
			{
				g_ticks = 0;
				LogStreamTiming();
				if (g_chain.Dirty()) SaveCurrentChain();
			}
			return 0;
		case WM_CLOSE:
			KillTimer(window, kKeyTimer);
			KillTimer(window, kHousekeepingTimer);
			g_audio.Stop();
			SaveCurrentChain();
			g_chainSaved = true;     // WatchGame: only teardown is left
			RememberOpenWindows();   // reopened next time
			StopPicture();
			CloseAllWindows();
			if (g_pedalEditor) g_pedalEditor->Close();
			DestroyWindow(window);
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(window, message, wParam, lParam);
	}

	// One plugin window: drag by its strip, close with its button.
	LRESULT CALLBACK AmpWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case WM_NCHITTEST:
		{
			const LRESULT hit = DefWindowProcW(window, message, wParam, lParam);
			POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
			ScreenToClient(window, &point);
			if (hit == HTCLIENT && point.y < StripHeight(window) && !InCloseButton(window, point)) return HTCAPTION;
			return hit;
		}
		case WM_LBUTTONUP:
		{
			const POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
			// Closes this plugin's window; the page opens it again. Without an overlay to do that, closing the last
			// window closes the host.
			if (InCloseButton(window, point))
				if (AmpWindow* amp = WindowForFrame(window))
				{
					CloseWindow(amp->plugin);
					RememberOpenWindows();
					PublishState();
					if (g_windows.empty() && !(g_control && g_overlayBeatSeenAt)) PostMessageW(g_main, WM_CLOSE, 0, 0);
				}
			return 0;
		}
		case WM_EXITSIZEMOVE:
			if (AmpWindow* amp = WindowForFrame(window)) RememberPosition(*amp);
			return 0;
		case WM_PAINT:
			PaintStrip(window);
			return 0;
		case WM_ERASEBKGND:
			return 1;
		case WM_CLOSE:
			return 0;   // closed through its button or the page, never Alt+F4 on a tool window
		}
		return DefWindowProcW(window, message, wParam, lParam);
	}

	// The chain as last played; for an older install with only one remembered plugin, that plugin.
	void RestoreChain(const std::wstring& extraPlugin, bool fromOverlay)
	{
		const std::wstring current = CurrentChainFile();
		std::string error, missing;
		if (GetFileAttributesW(current.c_str()) != INVALID_FILE_ATTRIBUTES && g_chain.LoadTone(current, error, missing))
		{
			g_toneName = ReadString(L"ToneName");
			DWORD modified = 0;
			g_toneModified = ReadDword(L"ToneModified", modified) && modified != 0;
			if (!missing.empty()) SetLoadError("Not installed, skipped: " + missing);
		}
		else if (!error.empty()) Log("restore: %s", error.c_str());
		else
		{
			const std::wstring last = ReadString(L"LastPlugin");
			if (!last.empty() && GetFileAttributesW(last.c_str()) != INVALID_FILE_ATTRIBUTES && !g_chain.Add(last, error))
				Log("restore: %ls did not load (%s)", last.c_str(), error.c_str());
		}
		// The player's global input and output trims (0 dB until they set them): a tone's own levels are its author's
		// job, set inside its plugins, so loading a tone never moves these.
		DWORD input = 0, output = 0;
		ReadDword(L"InputTrimTenths", input);
		ReadDword(L"OutputTrimTenths", output);
		g_chain.SetLevels(static_cast<int>(input), static_cast<int>(output));

		std::wstring add = extraPlugin;
		if (add.empty() && g_chain.Count() == 0 && !fromOverlay) add = AskForPlugin();
		bool already = false;
		for (int i = 0; i < g_chain.Count(); ++i) already = already || _wcsicmp(g_chain.Path(i).c_str(), add.c_str()) == 0;
		if (!add.empty() && !already)
		{
			if (g_chain.Add(add, error)) { WriteString(L"LastPlugin", add); g_toneModified = true; }
			else ShowError("Could not load the plugin:\n" + error);
		}
	}
}

namespace
{
	DWORD g_mainThread = 0;

	// Plugins run inside this process, so a plugin that crashes ends the host. Before Windows ends it: log which
	// module crashed and on which thread, keep one minidump (vst-host-crash.dmp in
	// %APPDATA%\RocksmithAudioBridge\VstHost, replaced each time) for diagnosis, and leave the overlay a note (LastCrash, LastCrashAt
	// in HKCU\Software\RSMods\VstHost) so it restarts the host and tells the player which plugin it was.
	// The plugin a module belongs to: anything loaded from inside a .vst3 bundle (the plugin itself or a library
	// it ships, Helix's HelixCoreWin_x64.dll for one), named by the bundle. Empty for Windows, runtime and our own.
	std::wstring PluginOf(HMODULE module)
	{
		wchar_t path[MAX_PATH]{};
		if (!module || !GetModuleFileNameW(module, path, MAX_PATH)) return {};
		std::wstring lower = path;
		for (wchar_t& c : lower) c = towlower(c);
		const size_t bundle = lower.find(L".vst3");
		if (bundle == std::wstring::npos) return {};
		const std::wstring upToBundle(path, path + bundle);
		return upToBundle.substr(upToBundle.find_last_of(L"\\/") + 1);
	}

	HMODULE ModuleAt(const void* address)
	{
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module);
		return module;
	}

	// A graphics driver's user-mode part (AMD, NVIDIA, Intel): its worker threads serve whoever draws with the GPU,
	// which in this process is only the plugins' own interfaces (PluginHost draws JUCE's part in software).
	bool IsGraphicsDriver(const wchar_t* moduleName)
	{
		static const wchar_t* const prefixes[] = { L"amdxx", L"amdxc", L"atio", L"atidxx", L"aticfx", L"nvwgf", L"nvoglv", L"nvd3dum", L"nvldumd", L"igd", L"igc", L"igxe", L"ig9icd", L"ig11icd" };
		for (const wchar_t* prefix : prefixes) if (_wcsnicmp(moduleName, prefix, wcslen(prefix)) == 0) return true;
		return false;
	}

	std::wstring ModuleName(HMODULE module)
	{
		wchar_t path[MAX_PATH]{};
		if (!module || !GetModuleFileNameW(module, path, MAX_PATH)) return {};
		const wchar_t* slash = wcsrchr(path, L'\\');
		return slash ? slash + 1 : path;
	}

	// The module that started the current thread (NtQueryInformationThread, ThreadQuerySetWin32StartAddress).
	HMODULE ThreadStartModule()
	{
		using QueryThread = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
		auto query = reinterpret_cast<QueryThread>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
		void* start = nullptr;
		if (!query || query(GetCurrentThread(), 9, &start, sizeof(start), nullptr) != 0) return nullptr;
		return ModuleAt(start);
	}

	// Which plugin a crash belongs to, when the fault itself is in Windows or the C runtime (a plugin's bad pointer
	// often surfaces in ntdll): the first plugin frame on the crashing thread's stack, else the plugin that started
	// the thread. Empty when neither names one; the report then says which plugin was loading, if any.
	std::wstring CrashingPlugin(const void* faultAddress)
	{
		std::wstring plugin = PluginOf(ModuleAt(faultAddress));
		if (!plugin.empty()) return plugin;
		void* frames[62]{};
		const USHORT count = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
		for (USHORT i = 0; i < count && plugin.empty(); ++i) plugin = PluginOf(ModuleAt(frames[i]));
		if (!plugin.empty()) return plugin;
		return PluginOf(ThreadStartModule());
	}

	LONG WINAPI OnCrash(EXCEPTION_POINTERS* info)
	{
		static LONG reported = 0;
		if (InterlockedExchange(&reported, 1)) return EXCEPTION_CONTINUE_SEARCH;
		void* address = info->ExceptionRecord->ExceptionAddress;
		HMODULE module = nullptr;
		wchar_t modulePath[MAX_PATH] = L"unknown";
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module))
			GetModuleFileNameW(module, modulePath, MAX_PATH);
		const wchar_t* slash = wcsrchr(modulePath, L'\\');
		const wchar_t* moduleName = slash ? slash + 1 : modulePath;
		const unsigned long long offset = module ? static_cast<unsigned long long>(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)) : 0;
		const bool windowThread = GetCurrentThreadId() == g_mainThread;
		Log("host: CRASH 0x%08X in %ls+0x%llX on the %s thread", static_cast<unsigned>(info->ExceptionRecord->ExceptionCode), moduleName, offset,
			windowThread ? "window (plugin editor)" : "audio or plugin worker");

		const std::wstring plugin = CrashingPlugin(address);
		const std::wstring starter = ModuleName(ThreadStartModule());
		const bool graphics = plugin.empty() && IsGraphicsDriver(starter.c_str());
		Log("host: crash attributed to %ls%ls%ls%ls%ls", plugin.empty() ? L"no plugin" : plugin.c_str(),
			g_loadingNow[0] ? L", while loading " : L"", g_loadingNow,
			graphics ? L", on a graphics-driver thread; windows open: " : L"", graphics ? g_openWindowNames : L"");
		WriteString(L"LastCrash", plugin);              // the plugin, or empty when none could be named
		WriteString(L"LastCrashModule", moduleName);    // where the fault surfaced
		WriteString(L"LastCrashLoading", g_loadingNow);
		// A graphics-driver thread: the driver module, and the plugins drawing with the GPU (their windows were open).
		WriteString(L"LastCrashGraphics", graphics ? starter : std::wstring());
		WriteString(L"LastCrashWindows", graphics ? std::wstring(g_openWindowNames) : std::wstring());
		WriteDword(L"LastCrashAt", static_cast<DWORD>(GetTickCount64() / 1000));   // seconds since boot, same clock in the game

		const std::wstring folder = DataDirectory();
		SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
		HANDLE dump = CreateFileW((folder + L"\\vst-host-crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (dump != INVALID_HANDLE_VALUE)
		{
			MINIDUMP_EXCEPTION_INFORMATION exception{ GetCurrentThreadId(), info, FALSE };
			MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
				static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &exception, nullptr, nullptr);
			CloseHandle(dump);
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

// Plugins may install their own filter when they load; the host's is put back after every command.
void InstallCrashReporter() { SetUnhandledExceptionFilter(OnCrash); }

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	LogOpen();
	// JUCE hosts the plugins (PluginHost.cpp). This thread is its message thread; the Win32 loop below dispatches
	// its messages too. Plugins and the format list are released before this goes out of scope.
	juce::ScopedJuceInitialiser_GUI juce;
	g_mainThread = GetCurrentThreadId();
	InstallCrashReporter();
	g_undo = std::make_unique<juce::UndoManager>(kUndoUnitsKept, kUndoStepsKept);

	// One host at a time: the external amp link carries one amp sim.
	HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\RocksmithAudioBridge.VstHost");
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		Log("host: already running, leaving it to the overlay");
		return 0;
	}

	OleInitialize(nullptr);   // plugin editors use drag and drop and COM on this thread

	std::wstring plugin;
	bool fromOverlay = false;
	int argc = 0;
	if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
	{
		for (int i = 1; i < argc; ++i)
		{
			if (_wcsicmp(argv[i], L"--overlay") == 0) fromOverlay = true;
			else if (_wcsicmp(argv[i], L"--standby") == 0) g_standby = true;
			else plugin = argv[i];
		}
		LocalFree(argv);
	}
	Log("host: starting%s%s%s%ls", fromOverlay ? " from the overlay" : "", g_standby ? " in standby" : "", plugin.empty() ? "" : ", adding ", plugin.c_str());

	g_control = VstHostControl::Open(g_controlMapping);
	if (g_control)
	{
		// Announce the host at once, loading: restoring a chain of heavy plugins can take most of a minute, and the
		// page shows the loading panel meanwhile instead of "off" (with a Start button that cannot start a second
		// host). The overlay counts a loading host as running while its process is there (VstHostLink HostAlive).
		InterlockedExchange(&g_control->loading, 1);
		CopyText(g_control->loadingText, sizeof(g_control->loadingText), "your chain");
		InterlockedExchange(&g_control->quitRequested, 0);
		InterlockedExchange(&g_control->standby, g_standby ? 1 : 0);
		// A command from before this host started was meant for a host that is gone; later ones wait in the queue
		// until the chain is restored.
		g_handledCommand = g_control->commandSeq;
		InterlockedExchange(&g_control->commandDone, g_handledCommand);
		InterlockedIncrement(&g_control->hostBeat);
		InterlockedExchange(&g_control->hostPid, static_cast<LONG>(GetCurrentProcessId()));
	}
	// Before the chain restores: a game that closes meanwhile ends a host that is still loading.
	if (HANDLE watcher = CreateThread(nullptr, 0, WatchGame, nullptr, 0, nullptr)) CloseHandle(watcher);
	g_chain.SetLoadReporter(ReportLoading);
	// A starred plugin that took the host down while it loaded in the background (PreloadStep) is not tried again.
	if (const std::wstring crashed = ReadString(L"PreloadingNow"); !crashed.empty())
	{
		RecordPreloadFailure(crashed, L"The plugin host closed while this loaded in the background, most likely because the plugin crashed it.");
		WriteString(L"PreloadingNow", {});
		Log("spares: %ls went down with the host last time while loading in the background; not tried again", crashed.c_str());
	}
	RefreshKeepSettings();
	RestoreChain(plugin, fromOverlay);
	g_lastCommandAt = GetTickCount64();

	g_instance = instance;
	WNDCLASSEXW mainClass{ sizeof(mainClass) };
	mainClass.lpfnWndProc = MainProc;
	mainClass.hInstance = instance;
	mainClass.lpszClassName = kMainClass;
	RegisterClassExW(&mainClass);

	WNDCLASSEXW windowClass{ sizeof(windowClass) };
	windowClass.lpfnWndProc = AmpWindowProc;
	windowClass.hInstance = instance;
	windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	windowClass.lpszClassName = kWindowClass;
	RegisterClassExW(&windowClass);

	WNDCLASSEXW editorClass{ sizeof(editorClass) };
	editorClass.lpfnWndProc = DefWindowProcW;
	editorClass.hInstance = instance;
	editorClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	editorClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	editorClass.lpszClassName = kEditorClass;
	RegisterClassExW(&editorClass);

	g_main = CreateWindowExW(0, kMainClass, L"Rocksmith Audio Bridge VST host", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
	g_font = CreateFontW(-MulDiv(13, static_cast<int>(GetDpiForSystem()), 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

	g_pictureOpen = g_picture.Open();
	StartStompbox();
	// In standby the stream opens when the player turns Tone designer on.
	if (!g_standby && !StartAudio(g_main))
	{
		if (g_control) { InterlockedExchange(&g_control->hostPid, 0); EndLoading(); }
		g_picture.Close();
		DestroyWindow(g_main);
		g_undo.reset();
		g_chain.ReleaseAll();
		ReleasePluginFormats();
		OleUninitialize();
		return 1;
	}

	// The windows that were open last time. Before per-plugin windows there was one, showing ShownSlot.
	DWORD openWindows = 0, shown = 0;
	if (!ReadDword(L"OpenWindows", openWindows)) openWindows = ReadDword(L"ShownSlot", shown) ? (1u << std::min<DWORD>(shown, 31)) : 1u;
	ReopenWindows(openWindows);

	// Tell the overlay's Tone designer page we are here; the windows follow its "Show plugin windows" switch.
	if (g_control)
	{
		DWORD wanted = 0;
		ReadDword(L"PopoutWanted", wanted);
		g_savedWanted = wanted != 0;
		InterlockedExchange(&g_control->popoutWanted, g_savedWanted ? 1 : 0);
		PublishState();
		EndLoading();
		Log("windows: %zu open, %s", g_windows.size(), g_savedWanted ? "shown with the overlay" : "waiting for Show plugin windows on the Tone designer page");
	}
	UpdateWindows();
	SetTimer(g_main, kHousekeepingTimer, kHousekeepingMs, nullptr);
	SetTimer(g_main, kKeyTimer, kKeyMs, nullptr);
	g_closeHandled = true;

	MSG message;
	while (GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	if (g_control)
	{
		g_control->popoutShown = 0;
		InterlockedExchange(&g_control->standby, 0);
		InterlockedExchange(&g_control->loading, 0);
		InterlockedExchange(&g_control->hostPid, 0);
	}
	g_audio.Close();
	g_picture.Close();
	g_stompboxService.onEditPedal = nullptr;
	g_pedalEditor.reset();   // a JUCE window: before JUCE shuts down
	g_stompboxService.Stop();
	g_stompbox.Unload();
	g_undo.reset();   // its steps hold plugin settings only, but it is a JUCE object: before JUCE shuts down
	g_chain.ReleaseAll();
	ReleasePluginFormats();
	if (g_font) DeleteObject(g_font);
	Log("host: closed");
	OleUninitialize();
	if (singleInstance) CloseHandle(singleInstance);
	return 0;
}
