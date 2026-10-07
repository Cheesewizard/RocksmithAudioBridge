// Rocksmith Audio Bridge VST host: plays the guitar through a chain of VST3 plugins (amp sims, pedals, effects)
// while Rocksmith keeps the clean signal for note detection. 64-bit, because Rocksmith is 32-bit and modern
// plugins are 64-bit only; audio goes through the bridge driver's external amp link, the same path an external
// amp sim uses.
//
// The overlay's Tone designer page drives it through a shared block (DLL/Audio/VstHostControl.hpp): build the
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

#include "../../DLL/Audio/VstHostControl.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <commdlg.h>
#include <shellapi.h>
#include <TlHelp32.h>
#include <DbgHelp.h>
#include <algorithm>
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
	constexpr UINT kOverlayKey = VK_OEM_5;   // backslash, the game overlay's key (DLL/dllmain.cpp)
	constexpr int kStripDip = 26;            // drag bar above the editor

	Chain g_chain;
	BridgeAudio g_audio;
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

	// Tone names become file names: keep them to characters every file system takes.
	std::wstring ToneFile(const std::wstring& name)
	{
		std::wstring safe;
		for (wchar_t c : name) safe += (c < 32 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
		while (!safe.empty() && (safe.back() == L' ' || safe.back() == L'.')) safe.pop_back();
		return safe.empty() ? std::wstring() : DataDirectory() + L"\\Tones\\" + safe + L".rabtone";
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

	bool RocksmithRunning()
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return true;   // cannot tell: assume it is, never close by mistake
		PROCESSENTRY32W entry{ sizeof(entry) };
		bool found = false;
		for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
			found = _wcsicmp(entry.szExeFile, L"Rocksmith2014.exe") == 0;
		CloseHandle(snapshot);
		return found;
	}

	// The host is the game's companion: once it has seen Rocksmith, it closes (saving the chain) when the game
	// does, instead of staying behind with nothing to play and its exe locked. Started by hand with no game, it
	// stays until closed. Checked every 2 s.
	bool GameClosed()
	{
		static bool seen = false;
		static int tick = 0;
		if (++tick % 4 != 0) return false;
		const bool running = RocksmithRunning();
		seen = seen || running;
		return seen && !running;
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

	void RememberOpenWindows()
	{
		WriteDword(L"OpenWindows", OpenWindowMask());
		WriteDword(L"ShownSlot", static_cast<DWORD>(std::max(g_shown, 0)));
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
		if (SlotOf(plugin) >= 0) window.plugin->CloseEditor();
		RememberPosition(window);
		const bool hadFocus = GetForegroundWindow() == window.frame;
		DestroyWindow(window.frame);
		g_windows.erase(it);
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

	void ReopenWindows(unsigned mask)
	{
		for (int i = 0; i < g_chain.Count() && i < 32; ++i) if (mask & (1u << i)) OpenWindow(i);
	}

	void CloseAllWindows()
	{
		while (!g_windows.empty()) CloseWindow(g_windows.back()->plugin);
	}

	// The overlay key while a plugin window has focus: the game never sees it, so hand focus back and pass the key on.
	// The game then closes its overlay, and the windows follow it (UpdateWindows).
	void OnOverlayKeyReleased()
	{
		const HWND foreground = GetForegroundWindow();
		if (!foreground || !IsOurWindow(foreground)) return;
		if (HWND game = FindGameWindow())
		{
			SetForegroundWindow(game);
			const UINT scan = MapVirtualKeyW(kOverlayKey, MAPVK_VK_TO_VSC);
			PostMessageW(game, WM_KEYDOWN, kOverlayKey, 1 | (scan << 16));
			PostMessageW(game, WM_KEYUP, kOverlayKey, 1 | (scan << 16) | (1u << 30) | (1u << 31));
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
		if (show == g_windowsShown) return;
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
		SetTextColor(dc, RGB(140, 140, 150));
		DrawTextW(dc, L"\\ hides", -1, &text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

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

	// One command from the Tone designer page.
	void RunCommand(VstHostControl::Command op, int arg, int arg2, const std::wstring& text)
	{
		using VstHostControl::Command;
		SetLoadError({});
		std::string error;
		const unsigned openBefore = OpenWindowMask();
		switch (op)
		{
		case Command::AddPlugin:
			BeginLoading();
			if (g_chain.Add(text, error)) { MarkModified(); OpenWindow(g_chain.Count() - 1); }
			else { SetLoadError(error.empty() ? "That plugin did not load." : error); SetFailedPath(text); }
			EndLoading();
			break;
		case Command::ReplaceSlot:
			BeginLoading();
			if (PluginHost* old = g_chain.Plugin(arg)) CloseWindow(old);   // before the plugin goes
			if (g_chain.Replace(arg, text, error)) MarkModified();
			else { SetLoadError(error.empty() ? "That plugin did not load." : error); SetFailedPath(text); }
			if (openBefore & (1u << arg)) OpenWindow(arg);   // the new pedal gets the old one's window
			EndLoading();
			break;
		case Command::RemoveSlot:
			if (PluginHost* old = g_chain.Plugin(arg)) CloseWindow(old);
			g_chain.Remove(arg);
			MarkModified();
			if (g_shown >= g_chain.Count()) g_shown = g_chain.Count() - 1;
			break;
		case Command::MoveSlot:
			if (arg + arg2 >= 0 && arg + arg2 < g_chain.Count())
			{
				g_chain.Move(arg, arg2);
				if (g_shown == arg) g_shown = arg + arg2;
				else if (g_shown == arg + arg2) g_shown = arg;
				MarkModified();
			}
			break;
		case Command::SetBypass:
			g_chain.SetBypass(arg, arg2 != 0);
			MarkModified();
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
				if (_wcsicmp(g_toneName.c_str(), text.c_str()) == 0) { g_toneName.clear(); RememberTone(); }
			}
			break;
		}
		case Command::SetLevels:
			g_chain.SetLevels(arg, arg2);
			MarkModified();
			break;
		case Command::SetMuted:
			g_chain.SetMuted(arg != 0);
			Log("chain: %s", arg ? "muted (tuning)" : "unmuted");
			break;
		case Command::DuplicateSlot:
			BeginLoading();
			if (g_chain.Duplicate(arg, error)) { MarkModified(); if (openBefore & (1u << arg)) OpenWindow(arg + 1); }
			else SetLoadError(error.empty() ? "That plugin did not load a second time." : error);
			EndLoading();
			break;
		case Command::SetChainBypass:
			g_chain.SetChainBypass(arg != 0);
			Log("chain: %s", arg ? "bypassed (clean guitar)" : "playing");
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
			if (!MoveFileExW(from.c_str(), to.c_str(), 0)) { SetLoadError("The tone could not be renamed."); break; }   // a change of case works too
			Log("tone: renamed %ls to %ls", from.c_str(), to.c_str());
			if (_wcsicmp(g_toneName.c_str(), oldName.c_str()) == 0) { g_toneName = newName; RememberTone(); }
			break;
		}
		case Command::ClearChain:
			CloseAllWindows();
			g_chain.Clear();
			g_toneName.clear();
			g_toneModified = false;
			g_shown = -1;
			break;
		default:
			break;
		}
		SyncWindows();
		PublishState();
	}

	void PollCommand()
	{
		if (!g_control || g_control->commandSeq == g_handledCommand) return;
		g_handledCommand = g_control->commandSeq;
		wchar_t text[VstHostControl::kPathChars]{};
		memcpy(text, const_cast<const wchar_t*>(g_control->commandText), sizeof(text) - sizeof(wchar_t));
		RunCommand(static_cast<VstHostControl::Command>(g_control->commandOp), g_control->commandArg, g_control->commandArg2, text);
		InterlockedExchange(&g_control->commandDone, g_handledCommand);
	}

	// Meters for the overlay: a peak with a short fall, in thousandths of full scale.
	void PublishMeters()
	{
		if (!g_control) return;
		g_inputMeter = std::max(g_chain.TakeInputPeak(), g_inputMeter * 0.85f);
		g_outputMeter = std::max(g_chain.TakeOutputPeak(), g_outputMeter * 0.85f);
		g_control->inputPeak = static_cast<LONG>(std::min(g_inputMeter, 4.0f) * 1000.0f);
		g_control->outputPeak = static_cast<LONG>(std::min(g_outputMeter, 4.0f) * 1000.0f);
	}

	// Open (or reopen after the game changed its buffer size) the stream and run the chain on it.
	bool StartAudio(HWND window)
	{
		g_audio.Stop();
		g_chain.Suspend();
		std::string error;
		if (!g_audio.Open(window, &g_chain, error) || !g_chain.Prepare(g_audio.Frames(), error) || !g_audio.Start(error))
		{
			ShowError(error);
			return false;
		}
		return true;
	}

	// The hidden main window: the timers, the audio link's reset message, and shutdown.
	LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case BridgeAudio::kResetMessage:
			Log("audio: the game changed its stream, restarting");
			StartAudio(window);
			return 0;
		case WM_TIMER:
			if (wParam == kKeyTimer)
			{
				const bool down = (GetAsyncKeyState(kOverlayKey) & 0x8000) != 0;
				if (g_keyDown && !down) OnOverlayKeyReleased();
				g_keyDown = down;
				UpdateWindows();
				PollCommand();
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
			InstallCrashReporter();   // a plugin may have replaced it with its own
			if (GameClosed())
			{
				Log("host: Rocksmith has closed, closing too");
				PostMessageW(window, WM_CLOSE, 0, 0);
				return 0;
			}
			if (g_chain.TakeRestartRequest()) StartAudio(window);
			if (g_chain.Dirty() && !g_toneModified) { g_toneModified = true; PublishState(); }
			if (++g_ticks >= kAutosaveTicks)
			{
				g_ticks = 0;
				if (g_chain.Dirty()) SaveCurrentChain();
			}
			return 0;
		case WM_CLOSE:
			KillTimer(window, kKeyTimer);
			KillTimer(window, kHousekeepingTimer);
			g_audio.Stop();
			SaveCurrentChain();
			RememberOpenWindows();   // reopened next time
			CloseAllWindows();
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
		using QueryThread = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
		if (auto query = reinterpret_cast<QueryThread>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")))
		{
			void* start = nullptr;
			if (query(GetCurrentThread(), 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr) == 0)
				plugin = PluginOf(ModuleAt(start));
		}
		return plugin;
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
		Log("host: crash attributed to %ls%ls%ls", plugin.empty() ? L"no plugin" : plugin.c_str(),
			g_loadingNow[0] ? L", while loading " : L"", g_loadingNow);
		WriteString(L"LastCrash", plugin);              // the plugin, or empty when none could be named
		WriteString(L"LastCrashModule", moduleName);    // where the fault surfaced
		WriteString(L"LastCrashLoading", g_loadingNow);
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
			else plugin = argv[i];
		}
		LocalFree(argv);
	}
	Log("host: starting%s%s%ls", fromOverlay ? " from the overlay" : "", plugin.empty() ? "" : ", adding ", plugin.c_str());

	g_control = VstHostControl::Open(g_controlMapping);
	if (g_control)
	{
		// Announce the host at once, loading: restoring a chain of heavy plugins can take most of a minute, and the
		// page shows the loading panel meanwhile instead of "off" (with a Start button that cannot start a second
		// host). The overlay counts a loading host as running while its process is there (VstHostLink HostAlive).
		InterlockedExchange(&g_control->loading, 1);
		CopyText(g_control->loadingText, sizeof(g_control->loadingText), "your chain");
		InterlockedExchange(&g_control->quitRequested, 0);
		// A command from before this host started was meant for a host that is gone; later ones wait in the queue
		// until the chain is restored.
		g_handledCommand = g_control->commandSeq;
		InterlockedExchange(&g_control->commandDone, g_handledCommand);
		InterlockedIncrement(&g_control->hostBeat);
		InterlockedExchange(&g_control->hostPid, static_cast<LONG>(GetCurrentProcessId()));
	}
	g_chain.SetLoadReporter(ReportLoading);
	RestoreChain(plugin, fromOverlay);

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

	if (!StartAudio(g_main))
	{
		if (g_control) { InterlockedExchange(&g_control->hostPid, 0); EndLoading(); }
		DestroyWindow(g_main);
		g_chain.Clear();
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

	MSG message;
	while (GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	if (g_control)
	{
		g_control->popoutShown = 0;
		InterlockedExchange(&g_control->loading, 0);
		InterlockedExchange(&g_control->hostPid, 0);
	}
	g_audio.Close();
	g_chain.Clear();
	ReleasePluginFormats();
	if (g_font) DeleteObject(g_font);
	Log("host: closed");
	OleUninitialize();
	if (singleInstance) CloseHandle(singleInstance);
	return 0;
}
