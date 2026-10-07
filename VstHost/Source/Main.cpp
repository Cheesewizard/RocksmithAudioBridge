// Rocksmith Audio Bridge VST host: plays the guitar through a chain of VST3 plugins (amp sims, pedals, effects)
// while Rocksmith keeps the clean signal for note detection. 64-bit, because Rocksmith is 32-bit and modern
// plugins are 64-bit only; audio goes through the bridge driver's external amp link, the same path an external
// amp sim uses.
//
// The overlay's External amp page drives it through a shared block (DLL/Audio/VstHostControl.hpp): build the
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
#include "public.sdk/source/vst/hosting/hostclasses.h"

#include <Windows.h>
#include <ShlObj.h>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
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

	Chain g_chain;
	BridgeAudio g_audio;
	HWND g_popout = nullptr;
	HWND g_editor = nullptr;
	HFONT g_font = nullptr;
	std::wstring g_title;
	int g_shown = -1;                 // slot whose editor the popout shows
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

	// The screen area the amp window must fit in: the game's monitor (or the popout's when there is no game).
	RECT WorkArea()
	{
		HWND anchor = FindGameWindow();
		MONITORINFO info{ sizeof(info) };
		GetMonitorInfoW(MonitorFromWindow(anchor ? anchor : g_popout, MONITOR_DEFAULTTOPRIMARY), &info);
		return info.rcWork;
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
		const RECT area = WorkArea();
		SetWindowPos(g_popout, nullptr, area.left + (area.right - area.left - width) / 2, area.top + (area.bottom - area.top - height) / 2,
			0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	// Keeps the whole window on its monitor after it changes size (a bigger plugin can push it off the edge).
	void KeepPopoutOnScreen()
	{
		RECT frame{};
		GetWindowRect(g_popout, &frame);
		const RECT area = WorkArea();
		const int width = static_cast<int>(frame.right - frame.left), height = static_cast<int>(frame.bottom - frame.top);
		int x = static_cast<int>(frame.left), y = static_cast<int>(frame.top);
		x = std::max(static_cast<int>(area.left), std::min(x, static_cast<int>(area.right) - width));
		y = std::max(static_cast<int>(area.top), std::min(y, static_cast<int>(area.bottom) - height));
		if (x != frame.left || y != frame.top) SetWindowPos(g_popout, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
		KeepPopoutOnScreen();
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

	// The chosen slot's editor in the popout, sized to it (and shrunk to fit the screen). The popout keeps its
	// place on screen.
	void ShowSlot(int index)
	{
		if (PluginHost* previous = g_chain.Plugin(g_shown)) previous->CloseEditor();
		g_shown = g_chain.Count() == 0 ? -1 : std::clamp(index, 0, g_chain.Count() - 1);
		PluginHost* plugin = g_chain.Plugin(g_shown);
		if (!plugin)
		{
			g_title = L"No plugins: add one on the External amp page";
			FitPopoutToEditor(Scale(460), Scale(60));
		}
		else
		{
			const RECT area = WorkArea();
			const int maxWidth = (area.right - area.left) * 9 / 10;
			const int maxHeight = (area.bottom - area.top) * 9 / 10 - StripHeight();
			g_title = std::to_wstring(g_shown + 1) + L"/" + std::to_wstring(g_chain.Count()) + L"  " + Wide(plugin->Name());
			if (g_chain.Bypassed(g_shown)) g_title += L"  (bypassed)";
			if (!plugin->OpenEditor(g_editor, FitPopoutToEditor, maxWidth, maxHeight))
			{
				g_title += L"  (no editor)";
				FitPopoutToEditor(Scale(460), Scale(60));
			}
		}
		SetWindowTextW(g_popout, (g_title + L" - Rocksmith Audio Bridge").c_str());
		InvalidateRect(g_popout, nullptr, TRUE);
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
		g_control->slotCount = count;
		g_control->shownSlot = g_shown;
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
	void EndLoading() { if (g_control) { InterlockedIncrement(&g_control->hostBeat); InterlockedExchange(&g_control->loading, 0); } }

	void MarkModified() { g_toneModified = true; }

	// One command from the External amp page.
	void RunCommand(VstHostControl::Command op, int arg, int arg2, const std::wstring& text)
	{
		using VstHostControl::Command;
		SetLoadError({});
		std::string error;
		switch (op)
		{
		case Command::AddPlugin:
			BeginLoading();
			if (g_chain.Add(text, error)) { MarkModified(); ShowSlot(g_chain.Count() - 1); }
			else SetLoadError(error.empty() ? "That plugin did not load." : error);
			EndLoading();
			break;
		case Command::ReplaceSlot:
		{
			BeginLoading();
			const bool shown = arg == g_shown;
			if (shown) if (PluginHost* plugin = g_chain.Plugin(g_shown)) plugin->CloseEditor();
			if (g_chain.Replace(arg, text, error)) MarkModified();
			else SetLoadError(error.empty() ? "That plugin did not load." : error);
			if (shown || g_shown < 0) ShowSlot(arg);
			EndLoading();
			break;
		}
		case Command::RemoveSlot:
			if (PluginHost* plugin = g_chain.Plugin(g_shown)) plugin->CloseEditor();
			g_chain.Remove(arg);
			MarkModified();
			ShowSlot(g_shown > arg ? g_shown - 1 : g_shown);
			break;
		case Command::MoveSlot:
			if (arg + arg2 >= 0 && arg + arg2 < g_chain.Count())
			{
				g_chain.Move(arg, arg2);
				if (g_shown == arg) g_shown = arg + arg2;
				else if (g_shown == arg + arg2) g_shown = arg;
				MarkModified();
				ShowSlot(g_shown);
			}
			break;
		case Command::SetBypass:
			g_chain.SetBypass(arg, arg2 != 0);
			MarkModified();
			if (arg == g_shown) ShowSlot(g_shown);
			break;
		case Command::ShowSlot:
			ShowSlot(arg);
			break;
		case Command::LoadTone:
		{
			const std::wstring file = ToneFile(text);
			BeginLoading();
			if (PluginHost* plugin = g_chain.Plugin(g_shown)) plugin->CloseEditor();
			g_shown = -1;
			std::string missing;
			if (g_chain.LoadTone(file, error, missing))
			{
				g_toneName = text;
				g_toneModified = false;
				SaveCurrentChain();
				if (!missing.empty()) SetLoadError("Not installed, skipped: " + missing);
			}
			else SetLoadError(error);
			ShowSlot(0);
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
		case Command::ClearChain:
			if (PluginHost* plugin = g_chain.Plugin(g_shown)) plugin->CloseEditor();
			g_chain.Clear();
			g_toneName.clear();
			g_toneModified = false;
			ShowSlot(-1);
			break;
		default:
			break;
		}
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
			if (g_chain.TakeRestartRequest()) { StartAudio(window); ShowSlot(g_shown); }
			if (g_chain.Dirty() && !g_toneModified) { g_toneModified = true; PublishState(); }
			if (++g_ticks >= kAutosaveTicks)
			{
				g_ticks = 0;
				if (g_chain.Dirty()) SaveCurrentChain();
			}
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
			// The window's close button only puts the amp away (the page's "Pop out amp" brings it back); without
			// an overlay to bring it back, it closes the host.
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
			SaveCurrentChain();
			if (PluginHost* plugin = g_chain.Plugin(g_shown)) plugin->CloseEditor();
			DestroyWindow(window);
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
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

	static Steinberg::Vst::HostApplication hostApplication;
	Steinberg::Vst::PluginContextFactory::instance().setPluginContext(&hostApplication);

	g_control = VstHostControl::Open(g_controlMapping);
	if (g_control) InterlockedExchange(&g_control->loading, 1);
	RestoreChain(plugin, fromOverlay);

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

	// Tool window: no taskbar button or Alt+Tab entry; the overlay drives it.
	g_popout = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass, L"Rocksmith Audio Bridge",
		WS_POPUP | WS_CLIPCHILDREN, 0, 0, 800, 600, nullptr, nullptr, instance, nullptr);
	g_editor = CreateWindowExW(0, kEditorClass, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, Scale(kStripDip), 800, 600 - Scale(kStripDip),
		g_popout, nullptr, instance, nullptr);
	g_font = CreateFontW(-Scale(13), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

	if (!StartAudio(g_popout)) { DestroyWindow(g_popout); g_chain.Clear(); OleUninitialize(); return 1; }
	DWORD shown = 0;
	ShowSlot(ReadDword(L"ShownSlot", shown) ? static_cast<int>(shown) : 0);
	PlacePopout();

	// Tell the overlay's External amp page we are here; the popout follows its "Pop out amp" switch (UpdatePopout).
	if (g_control)
	{
		DWORD wanted = 0;
		ReadDword(L"PopoutWanted", wanted);
		g_savedWanted = wanted != 0;
		InterlockedExchange(&g_control->popoutWanted, g_savedWanted ? 1 : 0);
		InterlockedExchange(&g_control->quitRequested, 0);
		// A command from before this host started was meant for a host that is gone.
		g_handledCommand = g_control->commandSeq;
		InterlockedExchange(&g_control->commandDone, g_handledCommand);
		PublishState();
		InterlockedExchange(&g_control->loading, 0);
		InterlockedExchange(&g_control->hostPid, static_cast<LONG>(GetCurrentProcessId()));
		Log("popout: %s", g_savedWanted ? "Pop out amp is on" : "waiting for Pop out amp on the External amp page");
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

	WriteDword(L"ShownSlot", static_cast<DWORD>(std::max(g_shown, 0)));
	if (g_control)
	{
		g_control->popoutShown = 0;
		InterlockedExchange(&g_control->loading, 0);
		InterlockedExchange(&g_control->hostPid, 0);
	}
	g_audio.Close();
	g_chain.Clear();
	if (g_font) DeleteObject(g_font);
	Log("host: closed");
	OleUninitialize();
	if (singleInstance) CloseHandle(singleInstance);
	return 0;
}
