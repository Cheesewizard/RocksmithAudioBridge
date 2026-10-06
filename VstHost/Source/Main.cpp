// Rocksmith Audio Bridge VST host: plays the guitar through a VST3 plugin (an amp sim) while Rocksmith keeps
// the clean signal for note detection. 64-bit, because Rocksmith is 32-bit and modern plugins are 64-bit only;
// audio goes through the bridge driver's external amp link, the same path an external amp sim uses.
//
// The plugin's editor lives in a borderless, always-on-top popout over the game that shows and hides with the
// overlay key (backslash), so the amp can be adjusted without leaving Rocksmith. This needs Rocksmith in
// windowed or borderless fullscreen; nothing can draw over exclusive fullscreen.
//
// Usage: RocksmithAudioBridgeVstHost.exe ["path\to\plugin.vst3"]
// Without a path it reopens the last plugin, or asks for one.

#include "BridgeAudio.hpp"
#include "HostLog.hpp"
#include "PluginHost.hpp"

#include "../../DLL/Audio/VstHostControl.hpp"
#include "public.sdk/source/vst/hosting/hostclasses.h"

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <string>

using namespace VstHost;

namespace
{
	constexpr wchar_t kWindowClass[] = L"RocksmithAudioBridgeVstHost";
	constexpr wchar_t kEditorClass[] = L"RocksmithAudioBridgeVstEditor";
	constexpr wchar_t kSettingsKey[] = L"Software\\RSMods\\VstHost";
	constexpr UINT_PTR kHousekeepingTimer = 1;
	constexpr UINT kHousekeepingMs = 500;
	constexpr int kAutosaveTicks = 60;   // 30 s
	constexpr UINT_PTR kKeyTimer = 2;
	constexpr UINT kKeyMs = 15;
	constexpr UINT kOverlayKey = VK_OEM_5;   // backslash, the game overlay's key (DLL/dllmain.cpp)
	constexpr int kStripDip = 26;            // drag bar above the editor

	PluginHost g_plugin;
	BridgeAudio g_audio;
	HWND g_popout = nullptr;
	HWND g_editor = nullptr;
	HFONT g_font = nullptr;
	std::wstring g_title;
	int g_ticks = 0;
	bool g_keyDown = false;
	HANDLE g_controlMapping = nullptr;
	VstHostControl::Shared* g_control = nullptr;
	LONG g_lastOverlayBeat = 0;
	ULONGLONG g_overlayBeatSeenAt = 0;
	bool g_savedWanted = false;

	int Scale(int dip) { return MulDiv(dip, g_popout ? static_cast<int>(GetDpiForWindow(g_popout)) : 96, 96); }
	int StripHeight() { return Scale(kStripDip); }
	int CloseButtonWidth() { return Scale(30); }

	bool ReadDword(const wchar_t* name, DWORD& value)
	{
		DWORD size = sizeof(value);
		return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
	}

	void WriteDword(const wchar_t* name, DWORD value)
	{
		RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value, sizeof(value));
	}

	std::wstring ReadLastPlugin()
	{
		wchar_t value[MAX_PATH * 2]{};
		DWORD size = sizeof(value);
		if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"LastPlugin", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
		return value;
	}

	void WriteLastPlugin(const std::wstring& path)
	{
		RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"LastPlugin", REG_SZ, path.c_str(),
			static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
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

	void PlacePopout()
	{
		RECT frame{};
		GetWindowRect(g_popout, &frame);
		const int width = frame.right - frame.left, height = frame.bottom - frame.top;
		DWORD x = 0, y = 0;
		if (ReadDword(L"PopoutX", x) && ReadDword(L"PopoutY", y))
		{
			const POINT topLeft{ static_cast<LONG>(static_cast<int>(x)), static_cast<LONG>(static_cast<int>(y)) };
			if (MonitorFromPoint(topLeft, MONITOR_DEFAULTTONULL))
			{
				SetWindowPos(g_popout, nullptr, topLeft.x, topLeft.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
				return;
			}
		}
		RECT area{};
		if (HWND game = FindGameWindow()) GetWindowRect(game, &area);
		else SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
		SetWindowPos(g_popout, nullptr, area.left + (area.right - area.left - width) / 2, area.top + (area.bottom - area.top - height) / 2,
			0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	void ShowPopout()
	{
		// Topmost but not activated: the game keeps the keyboard until the player clicks the amp.
		SetWindowPos(g_popout, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
		Log("popout: shown");
	}

	void HidePopout()
	{
		ShowWindow(g_popout, SW_HIDE);
		Log("popout: hidden");
	}

	// The overlay key while the amp has focus: the game never sees it, so hand focus back and pass the key on.
	// The game then closes its overlay, and the popout follows it (UpdatePopout).
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

	// The popout is on screen when the External amp page's "Pop out amp" is on and the overlay is open. Without
	// an overlay reporting in (Rocksmith closed, or an overlay that predates this), the switch alone decides.
	void UpdatePopout()
	{
		if (!g_control) return;
		const ULONGLONG now = GetTickCount64();
		const LONG overlayBeat = g_control->overlayBeat;
		if (overlayBeat != g_lastOverlayBeat) { g_lastOverlayBeat = overlayBeat; g_overlayBeatSeenAt = now; }
		const bool overlayReporting = g_overlayBeatSeenAt != 0 && now - g_overlayBeatSeenAt < static_cast<ULONGLONG>(VstHostControl::kAliveMs);

		const bool wanted = g_control->popoutWanted != 0;
		if (wanted != g_savedWanted) { g_savedWanted = wanted; WriteDword(L"PopoutWanted", wanted ? 1 : 0); }

		const bool show = wanted && (!overlayReporting || g_control->overlayVisible != 0);
		if (show == (IsWindowVisible(g_popout) != FALSE)) return;
		if (show) ShowPopout();
		else
		{
			const bool hadFocus = GetForegroundWindow() && IsOurWindow(GetForegroundWindow());
			HidePopout();
			if (hadFocus) if (HWND game = FindGameWindow()) SetForegroundWindow(game);
		}
		g_control->popoutShown = show ? 1 : 0;
	}

	void FitPopoutToEditor(int width, int height)
	{
		const int strip = StripHeight();
		SetWindowPos(g_editor, nullptr, 0, strip, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
		SetWindowPos(g_popout, nullptr, 0, 0, width, height + strip, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		InvalidateRect(g_popout, nullptr, TRUE);
	}

	void PaintStrip(HWND window)
	{
		PAINTSTRUCT paint;
		HDC dc = BeginPaint(window, &paint);
		RECT client{};
		GetClientRect(window, &client);
		RECT strip{ 0, 0, client.right, StripHeight() };
		HBRUSH background = CreateSolidBrush(RGB(28, 28, 32));
		FillRect(dc, &strip, background);
		DeleteObject(background);

		SetBkMode(dc, TRANSPARENT);
		HGDIOBJ oldFont = SelectObject(dc, g_font);
		RECT text = strip;
		text.left += Scale(10);
		text.right -= CloseButtonWidth() + Scale(8);
		SetTextColor(dc, RGB(225, 225, 230));
		DrawTextW(dc, g_title.c_str(), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		SetTextColor(dc, RGB(140, 140, 150));
		DrawTextW(dc, L"\\ hides", -1, &text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

		RECT close{ client.right - CloseButtonWidth(), 0, client.right, StripHeight() };
		SetTextColor(dc, RGB(200, 200, 205));
		DrawTextW(dc, L"\u2715", -1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dc, oldFont);
		EndPaint(window, &paint);
	}

	bool InCloseButton(HWND window, POINT client)
	{
		RECT rect{};
		GetClientRect(window, &rect);
		return client.y >= 0 && client.y < StripHeight() && client.x >= rect.right - CloseButtonWidth();
	}

	// Open (or reopen after the game changed its buffer size) the stream and run the plugin on it.
	bool StartAudio(HWND window)
	{
		g_audio.Stop();
		g_plugin.Suspend();
		std::string error;
		if (!g_audio.Open(window, &g_plugin, error) || !g_plugin.Prepare(g_audio.Frames(), error) || !g_audio.Start(error))
		{
			ShowError(error);
			return false;
		}
		return true;
	}

	LRESULT CALLBACK PopoutProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
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
				UpdatePopout();
				if (g_control && g_control->quitRequested)
				{
					InterlockedExchange(&g_control->quitRequested, 0);
					Log("host: closed from the overlay");
					PostMessageW(window, WM_CLOSE, 0, 0);
				}
				return 0;
			}
			if (g_control) InterlockedIncrement(&g_control->hostBeat);
			if (g_plugin.TakeRestartRequest()) StartAudio(window);
			if (++g_ticks >= kAutosaveTicks) { g_ticks = 0; g_plugin.SaveIfDirty(); }
			return 0;
		case WM_NCHITTEST:
		{
			const LRESULT hit = DefWindowProcW(window, message, wParam, lParam);
			POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
			ScreenToClient(window, &point);
			if (hit == HTCLIENT && point.y < StripHeight() && !InCloseButton(window, point)) return HTCAPTION;
			return hit;
		}
		case WM_LBUTTONUP:
		{
			const POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
			// The window's close button only puts the amp away (the page's "Pop out amp" brings it back); without a
			// overlay to bring it back, it closes the host.
			if (InCloseButton(window, point))
			{
				if (g_control && g_overlayBeatSeenAt) InterlockedExchange(&g_control->popoutWanted, 0);
				else PostMessageW(window, WM_CLOSE, 0, 0);
			}
			return 0;
		}
		case WM_EXITSIZEMOVE:
		{
			RECT frame{};
			GetWindowRect(window, &frame);
			WriteDword(L"PopoutX", static_cast<DWORD>(frame.left));
			WriteDword(L"PopoutY", static_cast<DWORD>(frame.top));
			return 0;
		}
		case WM_PAINT:
			PaintStrip(window);
			return 0;
		case WM_ERASEBKGND:
			return 1;
		case WM_CLOSE:
			KillTimer(window, kKeyTimer);
			KillTimer(window, kHousekeepingTimer);
			g_audio.Stop();
			g_plugin.SaveState();
			g_plugin.CloseEditor();
			DestroyWindow(window);
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(window, message, wParam, lParam);
	}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	LogOpen();

	// One host at a time: the external amp link carries one amp sim.
	HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\RocksmithAudioBridge.VstHost");
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		if (HWND existing = FindWindowW(kWindowClass, nullptr)) SetWindowPos(existing, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
		return 0;
	}

	OleInitialize(nullptr);   // plugin editors use drag and drop and COM on this thread

	std::wstring path;
	int argc = 0;
	if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
	{
		if (argc > 1) path = argv[1];
		LocalFree(argv);
	}
	if (path.empty()) path = ReadLastPlugin();
	if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) path = AskForPlugin();
	if (path.empty()) { OleUninitialize(); return 0; }
	Log("host: starting with %ls", path.c_str());

	static Steinberg::Vst::HostApplication hostApplication;
	Steinberg::Vst::PluginContextFactory::instance().setPluginContext(&hostApplication);

	std::string error;
	if (!g_plugin.Load(path, error))
	{
		ShowError("Could not load the plugin:\n" + error);
		OleUninitialize();
		return 1;
	}
	WriteLastPlugin(path);

	WNDCLASSEXW popoutClass{ sizeof(popoutClass) };
	popoutClass.lpfnWndProc = PopoutProc;
	popoutClass.hInstance = instance;
	popoutClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	popoutClass.lpszClassName = kWindowClass;
	RegisterClassExW(&popoutClass);

	WNDCLASSEXW editorClass{ sizeof(editorClass) };
	editorClass.lpfnWndProc = DefWindowProcW;
	editorClass.hInstance = instance;
	editorClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	editorClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	editorClass.lpszClassName = kEditorClass;
	RegisterClassExW(&editorClass);

	std::wstring name(g_plugin.Name().size(), wchar_t{});
	name.resize(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, g_plugin.Name().c_str(), static_cast<int>(g_plugin.Name().size()), name.data(), static_cast<int>(name.size()))));
	g_title = name;

	// Tool window: no taskbar button or Alt+Tab entry; the overlay key and the close button drive it.
	g_popout = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass, (name + L" - Rocksmith Audio Bridge").c_str(),
		WS_POPUP | WS_CLIPCHILDREN, 0, 0, 800, 600, nullptr, nullptr, instance, nullptr);
	g_editor = CreateWindowExW(0, kEditorClass, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, Scale(kStripDip), 800, 600 - Scale(kStripDip),
		g_popout, nullptr, instance, nullptr);
	g_font = CreateFontW(-Scale(13), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

	if (!StartAudio(g_popout)) { DestroyWindow(g_popout); g_plugin.Unload(); OleUninitialize(); return 1; }
	if (!g_plugin.OpenEditor(g_editor, FitPopoutToEditor))
	{
		g_title += L" (this plugin has no editor)";
		FitPopoutToEditor(Scale(420), Scale(60));
	}
	PlacePopout();

	// Tell the overlay's External amp page we are here; the popout follows its "Pop out amp" switch (UpdatePopout).
	g_control = VstHostControl::Open(g_controlMapping);
	if (g_control)
	{
		DWORD wanted = 0;
		ReadDword(L"PopoutWanted", wanted);
		g_savedWanted = wanted != 0;
		InterlockedExchange(&g_control->popoutWanted, g_savedWanted ? 1 : 0);
		InterlockedExchange(&g_control->quitRequested, 0);
		const std::string& plugin = g_plugin.Name();
		memset(g_control->pluginName, 0, sizeof(g_control->pluginName));
		memcpy(g_control->pluginName, plugin.c_str(), std::min(plugin.size(), sizeof(g_control->pluginName) - 1));
		InterlockedExchange(&g_control->hostPid, static_cast<LONG>(GetCurrentProcessId()));
		Log("popout: %s; it shows with the overlay once Pop out amp is on", g_savedWanted ? "Pop out amp is on" : "waiting for Pop out amp on the External amp page");
	}
	else ShowPopout();
	SetTimer(g_popout, kHousekeepingTimer, kHousekeepingMs, nullptr);
	SetTimer(g_popout, kKeyTimer, kKeyMs, nullptr);

	MSG message;
	while (GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	if (g_control)
	{
		g_control->popoutShown = 0;
		InterlockedExchange(&g_control->hostPid, 0);
	}
	g_audio.Close();
	g_plugin.Unload();
	if (g_font) DeleteObject(g_font);
	Log("host: closed");
	OleUninitialize();
	if (singleInstance) CloseHandle(singleInstance);
	return 0;
}
