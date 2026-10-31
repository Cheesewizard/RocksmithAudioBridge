#include "PageCommon.hpp"

#include "../../AudioBridge/Engine/AudioBridgeEngine.hpp"
#include "../../Core/Text.hpp"

#include "Framework/Framework.hpp"
#include "SettingEdits.hpp"
#include "Settings.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <objbase.h>
#include <shellapi.h>
#include <thread>
#include <vector>

using namespace Overlay;

namespace AudioBridge::Pages
{
	namespace
	{
		Audio::ControlRequest Request(std::uint32_t op, const wchar_t* value)
		{
			Audio::ControlRequest request;
			request.operation = op;
			wcsncpy_s(request.value, value, _TRUNCATE);
			return request;
		}

		std::wstring ReadPassthroughOutputNameUncached()
		{
			const auto ini = GameFolder() / L"RS_ASIO.ini";
			wchar_t driver[256]{};
			GetPrivateProfileStringW(L"Asio.Output", L"Driver", L"", driver, 256, ini.c_str());
			std::wstring name = driver;
			// The proxy's ASIO name, or its older name.
			if (_wcsicmp(name.c_str(), L"Rocksmith Audio Bridge ASIO") == 0 || _wcsicmp(name.c_str(), L"Rocksmith Audio Bridge") == 0)
			{
				HKEY key{};
				if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, KEY_READ | KEY_WOW64_32KEY, &key) == ERROR_SUCCESS)
				{
					wchar_t target[256]{};
					DWORD size = sizeof(target);
					DWORD type = 0;
					if (RegQueryValueExW(key, L"Target", nullptr, &type, reinterpret_cast<LPBYTE>(target), &size) == ERROR_SUCCESS
						&& type == REG_SZ && target[0])
						name = target;
					RegCloseKey(key);
				}
			}
			return name;
		}

		// ---- Key bindings

		struct KeyBindInfo
		{
			std::string key;
			std::string section;
			std::string iniName;
			std::string label;
		};

		std::mutex keyBindsMutex;
		std::vector<KeyBindInfo> keyBinds;

		// Keys the game, Windows or Steam already own.
		const char* ReservedKeyReason(unsigned int vk)
		{
			switch (vk)
			{
			case VK_F8: return "F8 has known issues in Rocksmith (per the RSMods developer).";
			case VK_F10: return "F10 is a Windows menu key.";
			case VK_F11: return "F11 is Rocksmith's fullscreen key (Alt+Enter also sends it).";
			case VK_F12: return "F12 is Steam's screenshot key.";
			case VK_RETURN: case VK_SPACE: case VK_TAB: case VK_BACK:
			case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
				return "That key drives Rocksmith's menus.";
			default: return nullptr;
			}
		}

		// RSMods.ini's name for a key ("VK_INSERT"); empty when upstream's key table has none.
		std::string VkName(unsigned int vk)
		{
			for (const auto& [name, code] : Settings::keyMap)
				if (code == vk) return name;
			return {};
		}

		// The key a binding has now, or the one an edit is still saving.
		unsigned int BoundKey(const std::string& bindName)
		{
			if (const auto pending = SettingEdits::PendingValue(bindName))
			{
				const auto found = Settings::keyMap.find(*pending);
				return found != Settings::keyMap.end() ? found->second : 0;
			}
			return Settings::GetKeyBind(bindName);
		}

		// 4.x's pickers took the key from its own WndProc capture, which swallowed it. Upstream hands the overlay's ImGui
		// frame key presses only while its own menu is open, so the picker reads the keyboard itself: once every key is
		// up, the first key that goes down, taken when it is released again (so its release has already gone by when the
		// new binding lands, and a new overlay key does not close the overlay). Only while the game has focus.
		struct KeyCapture
		{
			std::string owner;      // the binding being changed; empty = not capturing
			bool armed = false;     // every key was up since the capture began
			unsigned int held = 0;  // the key pressed, waiting for its release

			void Begin(const char* bindName) { owner = bindName; armed = false; held = 0; }
			void Cancel() { owner.clear(); }

			// The key taken this frame, 0 = none yet. `cancelled` on Esc.
			unsigned int Poll(bool& cancelled)
			{
				cancelled = false;
				DWORD process = 0;
				GetWindowThreadProcessId(GetForegroundWindow(), &process);
				if (process != GetCurrentProcessId()) return 0;
				if (held)
				{
					if (GetAsyncKeyState(static_cast<int>(held)) & 0x8000) return 0;
					const unsigned int key = held;
					held = 0;
					if (key == VK_ESCAPE) { cancelled = true; return 0; }
					return key;
				}
				unsigned int pressed = 0;
				// From VK_BACK: 0x01..0x06 are the mouse buttons.
				for (unsigned int vk = VK_BACK; vk <= 0xFE && !pressed; ++vk)
					if (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) pressed = vk;
				if (!armed) { armed = pressed == 0; return 0; }
				held = pressed;
				return 0;
			}
		};

		KeyCapture capture;
	}

	Audio::ControlResponse BridgeControl(std::uint32_t op, const wchar_t* value)
	{
		return Audio::SharedOutput::DispatchControl(Request(op, value));
	}

	void SendInt(std::uint32_t op, int value)
	{
		wchar_t text[16];
		swprintf_s(text, L"%d", value);
		BridgeControl(op, text);
	}

	void PreviewControl(std::uint32_t op, const wchar_t* value)
	{
		Audio::SharedOutput::DispatchControlPreview(Request(op, value));
	}

	void PreviewInt(std::uint32_t op, int value)
	{
		wchar_t text[16];
		swprintf_s(text, L"%d", value);
		PreviewControl(op, text);
	}

	void SaveModInt(const char* key, const char* iniName, int value, const char* label)
	{
		SettingEdits::Apply({ key, "Mod Settings", iniName, std::to_string(value), label, true });
	}

	unsigned OutputStatusNumber(const LiveState& live, const wchar_t* field)
	{
		const wchar_t* value = wcsstr(live.output.file, field);
		return value ? static_cast<unsigned>(wcstoul(value + wcslen(field), nullptr, 10)) : 0;
	}

	const LiveState& Poll()
	{
		static LiveState state;
		static ULONGLONG last = 0;
		static ULONGLONG lastOutput = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last >= 100)
		{
			// Never wait on the render thread: while a slow op (e.g. the ASIO pick, op 25) runs, keep the last status.
			Audio::ControlResponse fresh;
			if (Audio::SharedOutput::TryDispatchControl(Request(1, L""), fresh)) state.status = fresh;
			state.input = Audio::CableInput::GetDiagnostics();
			last = now;
		}
		if (now - lastOutput >= 500)
		{
			Audio::ControlResponse fresh;
			if (Audio::SharedOutput::TryDispatchControl(Request(5, L""), fresh)) state.output = fresh;
			lastOutput = now;
		}
		return state;
	}

	std::filesystem::path GameFolder()
	{
		wchar_t exe[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		return std::filesystem::path(exe).parent_path();
	}

	// The Output page draws every frame; the ini + registry read behind this is refreshed at most once a second.
	std::wstring ReadPassthroughOutputName()
	{
		static std::wstring cached;
		static ULONGLONG readAt = 0;
		const ULONGLONG now = GetTickCount64();
		if (readAt == 0 || now - readAt >= 1000)
		{
			cached = ReadPassthroughOutputNameUncached();
			readAt = now;
		}
		return cached;
	}

	std::string Utf8(const std::wstring& text)
	{
		return Text::Utf8(text);
	}

	float MeterFraction(float peak)
	{
		if (peak <= 0.0f) return 0.0f;
		return std::clamp((20.0f * std::log10(peak) + 60.0f) / 60.0f, 0.0f, 1.0f);
	}

	std::string FormatTime(double seconds)
	{
		char text[32];
		sprintf_s(text, "%02d:%04.1f", static_cast<int>(seconds / 60.0), std::fmod(seconds, 60.0));
		return text;
	}

	void OpenAsync(std::wstring file)
	{
		std::thread([file = std::move(file)]
		{
			const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			if (SUCCEEDED(com)) CoUninitialize();
		}).detach();
	}

	void InfoRow(const char* label, const std::string& value, ImU32 valueColor)
	{
		const float startX = ImGui::GetCursorPosX();
		const float width = RowWidth();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Muted));
		ImGui::TextUnformatted(label);
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::SetCursorPosX(startX + width - ImGui::CalcTextSize(value.c_str()).x);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(valueColor));
		ImGui::TextUnformatted(value.c_str());
		ImGui::PopStyleColor();
	}

	void WarnCaption(const char* text)
	{
		ColorText(text, Color::Warn);
	}

	void ColorText(const char* text, ImU32 color)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	}

	std::string KeyName(unsigned int vk)
	{
		if (vk > 0)
		{
			const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
			wchar_t name[64]{};
			if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) return Utf8(name);
		}
		return "not set";
	}

	void SnapshotKeyBinds()
	{
		std::vector<KeyBindInfo> binds;
		for (const Framework::SettingDef& def : Framework::SettingsSchema().GetAll())
			if (def.type == Framework::SettingType::Key) binds.push_back({ def.key, def.ini.section, def.ini.name, def.label });
		std::lock_guard<std::mutex> lock(keyBindsMutex);
		keyBinds = std::move(binds);
	}

	void KeyBindPicker(const char* bindName, const char* label, const char* (*extraReason)(unsigned int))
	{
		static std::map<std::string, std::string, std::less<>> messages;
		std::string& message = messages[bindName];
		const bool mine = capture.owner == bindName;
		if (mine)
		{
			bool cancelled = false;
			const unsigned int vk = capture.Poll(cancelled);
			if (cancelled) capture.Cancel();
			else if (vk)
			{
				capture.Cancel();
				std::vector<KeyBindInfo> binds;
				{
					std::lock_guard<std::mutex> lock(keyBindsMutex);
					binds = keyBinds;
				}
				const KeyBindInfo* clash = nullptr;
				const KeyBindInfo* self = nullptr;
				for (const KeyBindInfo& bind : binds)
				{
					if (bind.key == bindName) self = &bind;
					else if (!clash && BoundKey(bind.key) == vk) clash = &bind;
				}
				const std::string vkName = VkName(vk);
				const char* extra = extraReason ? extraReason(vk) : nullptr;
				if (const char* reason = ReservedKeyReason(vk)) message = reason;
				else if (extra) message = extra;
				else if (clash && clash->key == "OverlayKey") message = "That key opens the overlay. Pick another, or change the overlay key on the General page.";
				else if (clash) message = "That key is already used by " + clash->label + ". Pick another, or rebind that one in RSMods, Keybindings.";
				else if (vkName.empty()) message = "That key can't be used as a hotkey.";
				else
				{
					SettingEdits::Apply({ bindName, self ? self->section : "Keybinds", self ? self->iniName : bindName, vkName,
						self ? self->label : label, false });
					message.clear();
				}
			}
		}
		const bool capturing = capture.owner == bindName;
		// Label on its own line and the button under it, so it lines up with the buttons above.
		const std::string text = std::string(label) + (capturing ? ": press a key (Esc cancels)" : ": " + KeyName(BoundKey(bindName)));
		ImGui::TextUnformatted(text.c_str());
		ImGui::PushID(bindName);
		if (capturing)
		{
			if (Button("Cancel")) capture.Cancel();
		}
		else if (Button("Change key", false, "Press this, then the key you want. Keys the game, Windows, Steam or another RSMods shortcut already use are refused."))
		{
			message.clear();
			capture.Begin(bindName);
		}
		ImGui::PopID();
		if (!message.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
			ImGui::TextWrapped("%s", message.c_str());
			ImGui::PopStyleColor();
		}
	}
}
