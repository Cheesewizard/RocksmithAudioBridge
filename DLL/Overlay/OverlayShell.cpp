#include "stdafx.h"
#include "OverlayShell.hpp"
#include "OverlayUi.hpp"
#include "Lib/ImGUI/imgui_internal.h"   // SetItemUsingMouseWheel (the pedalboard scrolls sideways)
#include "NoteByNote/Ui/NoteByNoteHudLayout.hpp"
#include "AudioBridge/Engine/AudioBridgeEngine.hpp"
#include "AudioBridge/Cable/CableInput.hpp"
#include "AudioBridge/Cable/CableCaptureSession.hpp"
#include "AudioBridge/Engine/OutputDevices.hpp"
#include "AudioBridge/Engine/OutputTap.hpp"
#include "Recorder/TakeRecorder.hpp"
#include "NoteByNote/Ml/MlServiceLauncher.hpp"
#include "ExternalAmp/ExternalAmp.hpp"
#include "ExternalAmp/CableAmpLink.hpp"
#include "ToneDesigner/VstHostLink.hpp"
#include "ToneDesigner/ToneGates.hpp"
#include "Shared/VstHostControl.hpp"
#include "Shared/ShellOpenAsync.hpp"
#include "AudioBridge/Engine/AsioOutputDevice.hpp"
#include "AudioBridge/Engine/ExclusiveOutputDevice.hpp"
#include "AudioBridge/Engine/DeviceVolume.hpp"
#include "AudioBridge/Engine/AppAudioMix.hpp"
#include "AudioBridge/Engine/GameToneLatency.hpp"
#include "AudioBridge/Engine/LatencyCalibration.hpp"
#include "AudioBridge/Input/InputCaptureHook.hpp"
#include "AudioBridge/Devices/DeviceEnumeratorHook.hpp"
#include "AudioBridge/Engine/CableGamePeriod.hpp"
#include "ToneDesigner/SongLibrary.hpp"
#include "ToneDesigner/ToneBackup.hpp"
#include "Core/Extensions.hpp"
#include "NoteByNote/Host/ControllerBridge.hpp"
#include "DropPedal/DropPedal.hpp"
#include "NoteByNote/Ui/NoteByNoteMenu.hpp"
#include "DropPedal/DropPedalOverlay.hpp"
#include "GameState.hpp"
#include "Keybindings.hpp"
#include "Shared/ProductVersion.hpp"
#include <filesystem>
#include <cmath>
#include <shellapi.h>
#include <mutex>
#include <thread>
#include <optional>
#include <atomic>
#include <set>
#include <cwctype>

namespace Overlay
{
	namespace
	{
		std::atomic_bool& LatencyReadoutSetting()
		{
			static std::atomic_bool enabled{ []()
			{
				DWORD value = 0;
				DWORD size = sizeof(value);
				const LSTATUS result = RegGetValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\Overlay", L"LatencyReadout",
					RRF_RT_REG_DWORD, nullptr, &value, &size);
				if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND && result != ERROR_PATH_NOT_FOUND)
				{
					LOG_WARNING("(OVERLAY) Could not read latency readout setting, error " << result << std::endl);
				}
				return result == ERROR_SUCCESS && value != 0;
			}() };
			return enabled;
		}

		enum class Page { NoteByNote, Mixer, Input, Output, Record, General, Debug, DropPedal, AmpSim };

		// ---- Engine access ------------------------------------------------------------------------------
		// Same operation codes the desktop bridge sends over the pipe; the overlay calls the engine directly.

		Audio::ControlResponse BridgeControl(uint32_t op, const wchar_t* value = L"")
		{
			Audio::ControlRequest request;
			request.operation = op;
			wcsncpy_s(request.value, value, _TRUNCATE);
			return Audio::SharedOutput::DispatchControl(request);
		}

		void SendInt(uint32_t op, int value)
		{
			wchar_t text[16]; swprintf_s(text, L"%d", value);
			BridgeControl(op, text);
		}

		// Slider drags apply live but only save on release (SendInt / BridgeControl then), instead of rewriting
		// RSMods.ini on every frame of a drag from the render thread.
		void PreviewControl(uint32_t op, const wchar_t* value)
		{
			Audio::ControlRequest request;
			request.operation = op;
			wcsncpy_s(request.value, value, _TRUNCATE);
			Audio::SharedOutput::DispatchControlPreview(request);
		}

		void PreviewInt(uint32_t op, int value)
		{
			wchar_t text[16]; swprintf_s(text, L"%d", value);
			PreviewControl(op, text);
		}

		// Live status (op 1) and guitar-input diagnostics, polled ~10 Hz rather than every frame.
		struct LiveState
		{
			Audio::ControlResponse status{};
			Audio::ControlResponse output{};
			Audio::CableInput::Diagnostics input{};
		};

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
			if (now - last >= 100) {
				// Never wait on the render thread: while a slow op (e.g. the ASIO pick, op 25) runs, keep the last status.
				Audio::ControlRequest request;
				request.operation = 1;
				Audio::ControlResponse fresh;
				if (Audio::SharedOutput::TryDispatchControl(request, fresh)) state.status = fresh;
				state.input = Audio::CableInput::GetDiagnostics();
				last = now;
			}
			if (now - lastOutput >= 500)
			{
				Audio::ControlRequest request;
				request.operation = 5;
				Audio::ControlResponse fresh;
				if (Audio::SharedOutput::TryDispatchControl(request, fresh)) state.output = fresh;
				lastOutput = now;
			}
			return state;
		}

		// Set by a page that sizes itself to fill the window (Tone designer's panel) rather than to its content. The
		// window's height normally follows the page's content; a page that also follows the window's height would
		// feed back and grow every frame, so DrawShell gives such a page the full allowed height instead.
		bool g_pageFillsHeight = false;
		float g_pageNaturalHeight = 0.0f;   // a fill page's preferred height (page padding included), set while it draws

		// Linear peak (guitar input or game output) to meter fraction on a -60..0 dBFS scale, as the D3D diagnostics HUD does.
		float MeterFraction(float peak)
		{
			if (peak <= 0.0f) return 0.0f;
			return std::clamp((20.0f * std::log10(peak) + 60.0f) / 60.0f, 0.0f, 1.0f);
		}

		std::filesystem::path GameFolder()
		{
			wchar_t exe[MAX_PATH]{};
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			return std::filesystem::path(exe).parent_path();
		}

		// Names the device the game plays straight out of in passthrough, from the same source the desktop bridge
		// reports: RS_ASIO.ini [Asio.Output] Driver, resolved through the proxy's HKCU Target when it is our proxy.
		// Read only; RSMods never writes RS_ASIO.ini.
		std::wstring ReadPassthroughOutputNameUncached();

		// The Output page draws every frame; the ini + registry read behind this is refreshed at most once a second.
		std::wstring ReadPassthroughOutputName()
		{
			static std::wstring cached;
			static ULONGLONG readAt = 0;
			const ULONGLONG now = GetTickCount64();
			if (readAt == 0 || now - readAt >= 1000) { cached = ReadPassthroughOutputNameUncached(); readAt = now; }
			return cached;
		}

		std::wstring ReadPassthroughOutputNameUncached()
		{
			const auto ini = GameFolder() / L"RS_ASIO.ini";
			wchar_t driver[256]{};
			GetPrivateProfileStringW(L"Asio.Output", L"Driver", L"", driver, 256, ini.c_str());
			std::wstring name = driver;
			// The proxy's ASIO name, or its older name.
			if (_wcsicmp(name.c_str(), L"Rocksmith Audio Bridge ASIO") == 0 || _wcsicmp(name.c_str(), L"Rocksmith Audio Bridge") == 0) {
				HKEY key{};
				if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, KEY_READ | KEY_WOW64_32KEY, &key) == ERROR_SUCCESS) {
					wchar_t target[256]{}; DWORD size = sizeof(target); DWORD type = 0;
					if (RegQueryValueExW(key, L"Target", nullptr, &type, reinterpret_cast<LPBYTE>(target), &size) == ERROR_SUCCESS
						&& type == REG_SZ && target[0])
						name = target;
					RegCloseKey(key);
				}
			}
			return name;
		}

		std::string Utf8(const std::wstring& text)
		{
			if (text.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
			std::string out(size > 0 ? size - 1 : 0, '\0');
			if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
			return out;
		}

		std::string RecordHotkeyName()
		{
			const int vk = static_cast<int>(Settings::GetKeyBind("RecordingHotkey"));
			if (vk > 0) {
				const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
				wchar_t name[64]{};
				if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) return Utf8(name);
			}
			return "not set";
		}

		// Record hotkey picker. Keys the game, Windows or Steam already own are refused, and so is any key another
		// RSMods binding uses (a shared key fires both actions).
		const char* ReservedKeyReason(unsigned int vk)
		{
			switch (vk) {
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

		// "DropPedalBaseTuningKey" -> "Drop Pedal base tuning": the ini name without "Key", split into words, with
		// the few names whose split reads badly spelled out.
		std::string KeyBindDisplayName(const std::string& name)
		{
			static const std::map<std::string, std::string, std::less<>> spelled = {
				{ "DropPedalBaseTuningKey", "Drop Pedal base tuning" }, { "DropPedalToggleKey", "Drop Pedal on/off" },
				{ "DropPedalPitchDownKey", "Drop Pedal pitch down" }, { "DropPedalPitchUpKey", "Drop Pedal pitch up" },
				{ "RRSpeedKey", "Riff Repeater speed" }, { "SFXVolumeKey", "Effects volume" },
				{ "CustomSongListTitles", "custom song list titles" }, { "MenuToggleKey", "the RSMods menu" },
			};
			if (const auto found = spelled.find(name); found != spelled.end()) return found->second;
			std::string base = name.size() > 3 && name.compare(name.size() - 3, 3, "Key") == 0 ? name.substr(0, name.size() - 3) : name;
			std::string words;
			for (size_t i = 0; i < base.size(); ++i) {
				const char c = base[i];
				if (i > 0 && std::isupper(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(base[i - 1]))) words += ' ';
				words += (i > 0 && std::isupper(static_cast<unsigned char>(c))) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : c;
			}
			return words;
		}

		void RecordHotkeyPicker()
		{
			static std::string message;
			unsigned int vk = 0;
			if (Keybindings::TakeCapturedKey(vk)) {
				const std::string clash = Settings::FindKeyBindUsing(vk, "RecordingHotkey");
				if (const char* reason = ReservedKeyReason(vk)) message = reason;
				else if (!clash.empty()) message = "That key is already used by " + KeyBindDisplayName(clash) + ". Pick another, or rebind that one in RSMods, Keybindings.";
				else if (!Settings::SetKeyBind("RecordingHotkey", vk, true)) message = "That key can't be used as a hotkey.";
				else message.clear();
			}
			const bool capturing = Keybindings::IsCapturingKey();
			// Label on its own line and the button under it, so it lines up with Open folder above.
			ImGui::TextUnformatted(capturing ? "Record hotkey: press a key (Esc cancels)" : ("Record hotkey: " + RecordHotkeyName()).c_str());
			if (capturing) {
				if (Button("Cancel")) Keybindings::CancelKeyCapture();
			}
			else if (Button("Change key", false, "Press this, then the key you want. Keys the game, Windows, Steam or another RSMods shortcut already use are refused.")) {
				message.clear();
				Keybindings::BeginKeyCapture();
			}
			if (!message.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
				ImGui::TextWrapped("%s", message.c_str());
				ImGui::PopStyleColor();
			}
		}

		std::string FormatTime(double seconds)
		{
			char text[32];
			sprintf_s(text, "%02d:%04.1f", static_cast<int>(seconds / 60.0), std::fmod(seconds, 60.0));
			return text;
		}

		uint32_t PackRgb(const float rgb[3])
		{
			auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
			return (channel(rgb[0]) << 16) | (channel(rgb[1]) << 8) | channel(rgb[2]);
		}

		ImU32 ToImColor(uint32_t rgb)
		{
			return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
		}

		// ---- Pages ------------------------------------------------------------------------------------------

		void NoteByNotePage()
		{
			PageHeader("Note by Note", "Practice readout and target display. Changes apply instantly and are saved for the next launch.");

			if (NoteByNoteMenu::IsMenuRowMissing()) {
				// The NOTE BY NOTE row in Riff Repeater Advanced Settings is what switches the feature on, and it lives in
				// cache.psarc, which the game keeps open: it can only be put back with the game closed.
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
				ImGui::TextWrapped("The NOTE BY NOTE switch is missing from Riff Repeater Advanced Settings (the game's menu file was "
					"reset, for example by a Steam file check). Close Rocksmith and run the Rocksmith Audio Bridge installer's "
					"Reinstall / Repair to add it back.");
				ImGui::PopStyleColor();
			}

			using Block = Settings::NoteByNoteHudBlock;

			BeginCard("Detection readout");
			bool visible = Settings::IsNoteByNoteDetectionVisible();
			if (ToggleRow("Show detection readout", "Native, Enhanced and ML lines while you practise. Detection keeps running when hidden.", &visible))
				BridgeControl(29, visible ? L"1" : L"0");

			bool committed = false;
			float uiSize = static_cast<float>(Settings::GetNoteByNoteUiSize());
			if (SliderRow("Text size", &uiSize, 50.0f, 300.0f, "%.0f%%", &committed, "Size of the Native, Enhanced and ML lines. 100% is the original size. The target has its own size below."))
				Settings::SetNoteByNoteUiSize(static_cast<int>(uiSize + 0.5f), false);
			if (committed) Settings::SetNoteByNoteUiSize(static_cast<int>(uiSize + 0.5f), true);

			committed = false;
			float spacing = static_cast<float>(Settings::GetNoteByNoteLineSpacing());
			if (SliderRow("Line spacing", &spacing, 50.0f, 300.0f, "%.0f%%", &committed, "Vertical gap between the Native, Enhanced and ML lines. 100% is the original spacing."))
				Settings::SetNoteByNoteLineSpacing(static_cast<int>(spacing + 0.5f), false);
			if (committed) Settings::SetNoteByNoteLineSpacing(static_cast<int>(spacing + 0.5f), true);
			EndCard();

			BeginCard("Target");
			committed = false;
			float targetSize = static_cast<float>(Settings::GetNoteByNoteTargetSize());
			if (SliderRow("Size", &targetSize, 50.0f, 300.0f, "%.0f%%", &committed, "Size of the target note or chord and its string colour."))
				Settings::SetNoteByNoteTargetSize(static_cast<int>(targetSize + 0.5f), false);
			if (committed) Settings::SetNoteByNoteTargetSize(static_cast<int>(targetSize + 0.5f), true);

			static const char* const styles[] = { "Detailed", "Simple", "Tab", "Look ahead" };
			int style = static_cast<int>(Settings::GetNoteByNoteTargetStyle());
			if (SegmentedRow("Style", &style, styles, 4, "Detailed names the string, fret and note. Simple shows just the fret next to the string colour, plus the bend (half bend, full bend). Tab shows the current note or chord. Look ahead opens a resizable song tab; scrolling never changes the current target."))
				Settings::SetNoteByNoteTargetStyle(static_cast<Settings::NoteByNoteTargetStyle>(style));

			// Custom is the only draggable position. Its spot is remembered while Under readout or Center is chosen,
			// so cycling back to Custom puts the target where it was left.
			using Position = Settings::NoteByNoteTargetPosition;
			static const char* const positions[] = { "Under readout", "Center", "Custom" };
			const Position current = Settings::GetNoteByNoteTargetPosition();
			int position = current == Position::Custom ? 2 : current == Position::Center ? 1 : 0;
			ImGui::BeginDisabled(style == static_cast<int>(Settings::NoteByNoteTargetStyle::TabTimeline));
			if (SegmentedRow("Position", &position, positions, 3, "Under readout stacks it below the detection lines. Center puts it near the gameplay area. Custom is wherever you drag it, and is remembered.")) {
				if (position == 2 && !Settings::GetNoteByNoteHudPlacement(Block::Target).IsSet()) {
					// First time in Custom: start from where the target is drawn now.
					const HudLayout::Block& drawn = HudLayout::blocks[static_cast<int>(Block::Target)];
					if (HudLayout::IsLive(Block::Target) && HudLayout::frameWidth > 0 && HudLayout::frameHeight > 0)
						Settings::SetNoteByNoteHudPlacement(Block::Target, { drawn.x0 / HudLayout::frameWidth, drawn.y0 / HudLayout::frameHeight }, true);
				}
				Settings::SetNoteByNoteTargetPosition(position == 2 ? Position::Custom : position == 1 ? Position::Center : Position::Left);
			}
			ImGui::EndDisabled();
			if (style == static_cast<int>(Settings::NoteByNoteTargetStyle::TabTimeline))
				Caption("Drag the tab window by its title and resize it using its edges. Scroll to peek ahead; Back to current resumes following.");
			EndCard();

			BeginCard("Layout");
			Caption("While this page is open, drag the readout or the target on screen to move it (dragging the target sets its Position to Custom). Scroll over either to resize it, double-click to put it back.");
			if (!HudLayout::IsLive(Block::Readout) && !HudLayout::IsLive(Block::Target))
				Caption("Start a song with Note by Note on to see them.");
			if (Button("Reset layout", false, "Puts both back in their built-in places (target Under readout), forgets the Custom spot and restores text size, target size and line spacing to 100%. Colours are kept.")) {
				Settings::SetNoteByNoteHudPlacement(Block::Readout, {}, true);
				Settings::SetNoteByNoteHudPlacement(Block::Target, {}, true);
				Settings::SetNoteByNoteTargetPosition(Position::Left);
				Settings::SetNoteByNoteUiSize(100, true);
				Settings::SetNoteByNoteTargetSize(100, true);
				Settings::SetNoteByNoteLineSpacing(100, true);
			}
			EndCard();

			BeginCard("Colours");
			bool custom = Settings::IsNoteByNoteCustomColours();
			if (ToggleRow("Custom colours", custom ? "Your colours are active." : "Using the default colours.", &custom))
				Settings::SetNoteByNoteCustomColours(custom);

			ImGui::BeginDisabled(!custom);
			// Three colours are drawn: text, green when a readout line shows the target note, and
			// Partial after a pass when that detector counted toward it but read a different note (native's +-1
			// tolerance). No red is ever drawn; the Rejected key stays in RSMods.ini, unused, so older settings
			// files still load.
			static const char* const names[] = { "Text / target", "Confirmed", "Counted, read off" };
			static const char* const hints[] = {
				"The colour of the target and of a readout line that has not heard the target note.",
				"A readout line turns this colour while it hears the target note, or after a pass when it read the passed note.",
				"After a pass, a readout line turns this colour when that detector helped pass the note but read a slightly different pitch.",
			};
			for (int i = 0; i < 3; ++i) {
				const uint32_t rgb = Settings::GetNoteByNoteColor(i);
				float color[3] = { ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f };
				bool saved = false;
				if (ColorSwatch(names[i], color, &saved, hints[i])) Settings::SetNoteByNoteColor(i, PackRgb(color), false);
				if (saved) Settings::SetNoteByNoteColor(i, PackRgb(color), true);
				if (i < 2) ImGui::SameLine(0.0f, 18.0f);
			}

			// Preview on a game-black strip, so colours are judged against what they will sit on.
			const ImVec2 start = ImGui::GetCursorScreenPos();
			const float width = RowWidth(), height = 38.0f;
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->AddRectFilled(start, ImVec2(start.x + width, start.y + height), IM_COL32(0, 0, 0, 255), 4.0f);
			const NoteByNote::DetectionPalette palette = custom ? Settings::GetNoteByNoteDetectionPalette() : NoteByNote::DetectionPalette{};
			const struct { const char* text; uint32_t argb; } samples[] = {
				{ "[x/3/3/x/x/x]", palette.neutral }, { "Native:  E2", palette.confirmed }, { "Native:  F2", palette.partial },
			};
			float x = start.x + 14.0f;
			for (const auto& sample : samples) {
				const ImVec2 size = ImGui::CalcTextSize(sample.text);
				list->AddText(ImVec2(x, start.y + (height - size.y) * 0.5f), ToImColor(sample.argb & 0xFFFFFF), sample.text);
				x += size.x + 26.0f;
			}
			ImGui::Dummy(ImVec2(width, height));

			if (Button("Reset colours", false, "Puts the colours back to the defaults."))
				Settings::ResetNoteByNoteColors();
			ImGui::EndDisabled();
			EndCard();

			Caption("Turn on Note by Note from the Riff Repeater menu in Rocksmith.");
		}

		// ---- Note by Note HUD layout editor ---------------------------------------------------------------
		// Runs while the Note by Note page is open. The HUD is D3DX text drawn after ImGui, so the blocks are not
		// ImGui items: the HUD publishes last frame's rectangles (HudLayout) and this hit-tests the mouse against
		// them, outlining each block on the background draw list (under the HUD text, over the game). The shell
		// window wins: nothing here starts while the mouse is over any ImGui window. Dragging writes the
		// placement live and saves once on release; scroll steps and double-click resets save immediately.

		void HudEditor()
		{
			using Block = Settings::NoteByNoteHudBlock;
			ImGuiIO& io = ImGui::GetIO();
			static int dragging = -1;
			static ImVec2 grabOffset;
			const float frameW = HudLayout::frameWidth, frameH = HudLayout::frameHeight;
			if (frameW <= 0.0f || frameH <= 0.0f) { dragging = -1; return; }
			// The target's dragged spot is its Custom position: dragging it while Under readout or Center is chosen
			// switches the position to Custom (from where it is drawn now). Double-click reset only applies in Custom.
			const bool targetIsCustom = Settings::GetNoteByNoteTargetPosition() == Settings::NoteByNoteTargetPosition::Custom;
			auto resettable = [&](int i) { return i != static_cast<int>(Block::Target) || targetIsCustom; };

			constexpr float pad = 6.0f;
			const ImVec2 mouse = io.MousePos;
			const bool overWindow = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
			int hovered = -1;
			// Target first: it is the smaller block and can sit on top of the readout.
			for (const Block block : { Block::Target, Block::Readout }) {
				const int i = static_cast<int>(block);
				const HudLayout::Block& r = HudLayout::blocks[i];
				if (hovered < 0 && !overWindow && HudLayout::IsLive(block)
					&& mouse.x >= r.x0 - pad && mouse.x <= r.x1 + pad && mouse.y >= r.y0 - pad && mouse.y <= r.y1 + pad)
					hovered = i;
			}

			if (dragging >= 0) {
				const Block block = static_cast<Block>(dragging);
				const HudLayout::Block& r = HudLayout::blocks[dragging];
				const float w = r.x1 - r.x0, h = r.y1 - r.y0;
				const float x = std::clamp(mouse.x - grabOffset.x, 0.0f, std::max(0.0f, frameW - w));
				const float y = std::clamp(mouse.y - grabOffset.y, 0.0f, std::max(0.0f, frameH - h));
				const bool released = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
				// A few pixels of travel before anything moves, so a plain click (or the first half of a
				// double-click) never turns a built-in position into a custom one.
				static bool moved = false;
				if (!moved && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) moved = true;
				if (moved) {
					Settings::SetNoteByNoteHudPlacement(block, { x / frameW, y / frameH }, released);
					if (block == Block::Target && !targetIsCustom)
						Settings::SetNoteByNoteTargetPosition(Settings::NoteByNoteTargetPosition::Custom);
				}
				if (released) { dragging = -1; moved = false; }
			}
			else if (hovered >= 0) {
				const Block block = static_cast<Block>(hovered);
				const HudLayout::Block& r = HudLayout::blocks[hovered];
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					if (resettable(hovered)) Settings::SetNoteByNoteHudPlacement(block, {}, true);
				}
				else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
					dragging = hovered;
					grabOffset = ImVec2(mouse.x - r.x0, mouse.y - r.y0);
				}
				else if (io.MouseWheel != 0.0f) {
					const int step = io.MouseWheel > 0.0f ? 10 : -10;
					if (block == Block::Readout) Settings::SetNoteByNoteUiSize(Settings::GetNoteByNoteUiSize() + step, true);
					else Settings::SetNoteByNoteTargetSize(Settings::GetNoteByNoteTargetSize() + step, true);
				}
			}

			// Keep the click, drag and wheel away from the game while the editor owns them (takes effect next frame,
			// which the hover before any click covers).
			const int active = dragging >= 0 ? dragging : hovered;
			if (active >= 0) {
				ImGui::CaptureMouseFromApp(true);
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
			}

			ImDrawList* list = ImGui::GetBackgroundDrawList();
			static const char* const names[] = { "Readout", "Target" };
			for (const Block block : { Block::Readout, Block::Target }) {
				const int i = static_cast<int>(block);
				if (!HudLayout::IsLive(block)) continue;
				const HudLayout::Block& r = HudLayout::blocks[i];
				const bool hot = i == active;
				const ImVec2 a(r.x0 - pad, r.y0 - pad), b(r.x1 + pad, r.y1 + pad);
				list->AddRectFilled(a, b, hot ? IM_COL32(59, 139, 235, 46) : IM_COL32(59, 139, 235, 18), 4.0f);
				list->AddRect(a, b, hot ? Color::AccentHi : IM_COL32(96, 165, 250, 120), 4.0f, 0, hot ? 2.0f : 1.0f);
				const char* label = !hot ? names[i]
					: block == Block::Readout ? "Readout: drag to move, scroll to resize, double-click to reset"
					: targetIsCustom ? "Target: drag to move, scroll to resize, double-click to reset"
					: "Target: drag to move (switches Position to Custom), scroll to resize";
				ImFont* font = GetFonts().caption ? GetFonts().caption : ImGui::GetFont();
				const float size = font->FontSize;
				const ImVec2 textSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
				const ImVec2 at(a.x, a.y - textSize.y - 6.0f >= 0.0f ? a.y - textSize.y - 6.0f : b.y + 4.0f);
				list->AddRectFilled(ImVec2(at.x - 4.0f, at.y - 2.0f), ImVec2(at.x + textSize.x + 4.0f, at.y + textSize.y + 2.0f), IM_COL32(11, 17, 30, 220), 3.0f);
				list->AddText(font, size, at, hot ? Color::Text : Color::Muted, label);
			}
		}

		// ---- Drop Pedal ---------------------------------------------------------------------------------------
		// Colours reload live: DropPedal::Overlay::LoadSettings re-reads modSettings and bumps its colour revision,
		// so the in-game Drop Pedal readout repaints on the next frame. The on/off switch is saved for the next
		// launch only, because the pedal's input hooks are installed when the game starts.

		std::string KeyName(const char* bind, const char* fallback)
		{
			const int vk = static_cast<int>(Settings::GetKeyBind(bind));
			if (vk > 0) {
				const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
				wchar_t name[64]{};
				if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) return Utf8(name);
			}
			return fallback;
		}

		uint32_t ParseHex(const std::string& text, uint32_t fallback)
		{
			if (text.size() != 6 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return fallback;
			return static_cast<uint32_t>(std::stoul(text, nullptr, 16));
		}

		std::string Hex(uint32_t rgb)
		{
			char text[8]{};
			sprintf_s(text, "%06X", rgb & 0xFFFFFFu);
			return text;
		}

		void DropPedalPage()
		{
			PageHeader("Drop Pedal", "Shift your guitar's tuning without retuning the strings.");

			struct ColourKey { const char* mod; const char* ini; uint32_t fallback; const char* label; const char* hint; };
			static const ColourKey colours[] = {
				{ "DropPedalOverlayDownColor", "OverlayDownColor", 0x6BE06B, "Pitch down", "Colour of the readout while the pedal shifts down." },
				{ "DropPedalOverlayUpColor", "OverlayUpColor", 0xFFC24D, "Pitch up", "Colour of the readout while the pedal shifts up." },
				{ "DropPedalOverlayStatusColor", "OverlayStatusColor", 0xFFFFFF, "Status", "Colour of the pedal's status text." },
			};
			const bool configured = DropPedal::IsConfiguredEnabled();
			const bool custom = Settings::ReturnSettingValue("DropPedalCustomOverlayColors") == "on";
			uint32_t rgb[3];
			for (int i = 0; i < 3; ++i) rgb[i] = custom ? ParseHex(Settings::ReturnSettingValue(colours[i].mod), colours[i].fallback) : colours[i].fallback;

			BeginCard("Now");
			if (!configured) {
				Caption("Drop Pedal is switched off in RSMods.ini (EnableDropPedal = off). Set it back to on and restart Rocksmith to use it.");
			}
			else {
				const DropPedal::PitchMode mode = DropPedal::GetPitchMode();
				ImGui::PushFont(GetFonts().title);
				ImGui::TextUnformatted(DropPedal::GetTuningName(DropPedal::Player::One).c_str());
				ImGui::PopFont();
				const int shift = DropPedal::GetShiftSemitones();
				const std::string modeName = DropPedal::GetPitchModeName();
				StatusPill(modeName.c_str(), mode == DropPedal::PitchMode::Off ? Color::Muted : Color::AccentHi);
				if (mode != DropPedal::PitchMode::Off) {
					ImGui::SameLine(0.0f, 10.0f);
					char text[48];
					sprintf_s(text, "%+d semitone%s", shift, (shift == 1 || shift == -1) ? "" : "s");
					StatusPill(text, shift < 0 ? ToImColor(rgb[0]) : shift > 0 ? ToImColor(rgb[1]) : Color::Muted);
				}
			}
			Caption(("Shortcuts: " + KeyName("DropPedalToggleKey", "F7") + " cycles Drop Pedal, Speaker Mode and Off. "
				+ KeyName("DropPedalPitchDownKey", ",") + " and " + KeyName("DropPedalPitchUpKey", ".") + " shift down and up. "
				+ KeyName("DropPedalBaseTuningKey", "F9") + " sets the base tuning. Change them in RSMods, Keybindings.").c_str());
			EndCard();

			// The pedal itself always comes with the mod and is switched in game (F7); this only hides its readout,
			// for players who want a clean screen.
			BeginCard("Display");
			bool shown = Settings::ReturnSettingValue("DropPedalShowOverlay") != "off";
			if (ToggleRow("Show pedal readout", "The Pitch line in the top left corner. The pedal keeps working while it is hidden.", &shown))
				Settings::SetDropPedalSetting("DropPedalShowOverlay", "ShowOverlay", shown ? "on" : "off", true);
			EndCard();

			BeginCard("Colours");
			bool useCustom = custom;
			if (ToggleRow("Custom colours", custom ? "Your colours are active." : "Using the default colours.", &useCustom)) {
				Settings::SetDropPedalSetting("DropPedalCustomOverlayColors", "CustomOverlayColors", useCustom ? "on" : "off", true);
				DropPedal::Overlay::LoadSettings();
			}
			ImGui::BeginDisabled(!custom);
			for (int i = 0; i < 3; ++i) {
				float color[3] = { ((rgb[i] >> 16) & 0xFF) / 255.0f, ((rgb[i] >> 8) & 0xFF) / 255.0f, (rgb[i] & 0xFF) / 255.0f };
				bool saved = false;
				const bool changed = ColorSwatch(colours[i].label, color, &saved, colours[i].hint);
				if (changed || saved) {
					rgb[i] = PackRgb(color);
					Settings::SetDropPedalSetting(colours[i].mod, colours[i].ini, Hex(rgb[i]), saved);
					DropPedal::Overlay::LoadSettings();
				}
				if (i < 2) ImGui::SameLine(0.0f, 18.0f);
			}

			// Preview on game black.
			const ImVec2 start = ImGui::GetCursorScreenPos();
			const float width = RowWidth(), height = 38.0f;
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->AddRectFilled(start, ImVec2(start.x + width, start.y + height), IM_COL32(0, 0, 0, 255), 4.0f);
			const struct { const char* text; uint32_t rgb; } samples[] = {
				{ "Drop D  -2", rgb[0] }, { "Eb Standard  +1", rgb[1] }, { "Drop Pedal", rgb[2] },
			};
			float x = start.x + 14.0f;
			for (const auto& sample : samples) {
				const ImVec2 size = ImGui::CalcTextSize(sample.text);
				list->AddText(ImVec2(x, start.y + (height - size.y) * 0.5f), ToImColor(sample.rgb), sample.text);
				x += size.x + 30.0f;
			}
			ImGui::Dummy(ImVec2(width, height));
			if (Button("Reset colours", false, "Puts the three colours back to the defaults.")) {
				for (const ColourKey& key : colours) Settings::SetDropPedalSetting(key.mod, key.ini, Hex(key.fallback), true);
				DropPedal::Overlay::LoadSettings();
			}
			ImGui::EndDisabled();
			EndCard();
		}

		// Mixer buses in control-channel order (op 7 + index), matching ControlResponse::volumes[].
		const char* const kChannels[7] = { "Song", "Player 1", "Master", "Player 2", "Mic", "Voice", "Effects" };
		// Display order, same grouping as the desktop mixer: Master | Player 1, Player 2 | Song, Effects, Voice, Mic.
		const int kChannelOrder[7] = { 2, 1, 3, 0, 6, 5, 4 };

		void MixerPage(const LiveState& live)
		{
			PageHeader("Mixer", "What you hear. Recordings of the game mix and Note by Note are not affected.");
			static float volumes[7] = {};
			static bool seeded = false;
			if (!seeded) { for (int i = 0; i < 7; ++i) volumes[i] = live.status.volumes[i]; seeded = true; }

			BeginCard();
			const float width = RowWidth();
			const float gap = 8.0f, groupGap = 22.0f;   // wider gaps after Master and after the players
			const float strip = (width - gap * 4.0f - groupGap * 2.0f) / 7.0f;
			const float height = 240.0f;
			for (int slot = 0; slot < 7; ++slot) {
				const int channel = kChannelOrder[slot];
				char id[8]; sprintf_s(id, "ch%d", channel);
				if (FaderStrip(id, kChannels[channel], &volumes[channel], ImVec2(strip, height)))
					SendInt(7 + channel, static_cast<int>(volumes[channel] + 0.5f));
				Hint(channel == 2 ? "Master playback volume over everything. Drag, or scroll for 1% steps."
					: "Playback volume for this bus. Drag, or scroll for 1% steps.");
				if (slot == 6) break;
				const bool groupEnd = slot == 0 || slot == 2;
				ImGui::SameLine(0.0f, groupEnd ? groupGap : gap);
				if (groupEnd) {
					// Thin divider centred in the group gap.
					const ImVec2 at = ImGui::GetCursorScreenPos();
					ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x - groupGap * 0.5f, at.y + 6.0f),
						ImVec2(at.x - groupGap * 0.5f, at.y + height - 6.0f), Color::CardEdge, 1.0f);
				}
			}
			EndCard();
			Caption("Mixer levels last for this session.");
		}

		// ---- External amp ---------------------------------------------------------------------------------
		// Play through an amp sim on the bridge's ASIO driver (ExternalAmp/ExternalAmp.hpp, AsioProxy/AmpLink.hpp).

		// Label on the left, value on the right of the card row.
		void InfoRow(const char* label, const std::string& value, ImU32 valueColor = Color::Text)
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
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
			ImGui::TextUnformatted(text);
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
		}

		// ---- Tone designer --------------------------------------------------------------------------------
		// The VST host (VstHost/, ToneDesigner/VstHostLink.hpp) plays the guitar through a chain of VST3 plugins and shows the
		// selected pedal's own window over the game. This page is laid out like a chain loader, in one framed panel:
		//   - a bar: File / Edit / View, the tone's name between previous / next arrows (amber dot = unsaved),
		//     VST | Game amp (A/B), the amp window and power buttons;
		//   - the tone library on the left (favourites, all tones, song tones);
		//   - the pedalboard: In jack, cables, one pedal per plugin coloured by kind (LED, knobs, footswitch), Add,
		//     Out jack. Click selects, double-click opens the plugin's window, the footswitch bypasses, drag
		//     reorders, right-click a pedal, a tone or the board for its actions; IN / OUT meters at its ends;
		//   - under it, one fixed-height pane: Plugins | Songs switches it between the plugin browser and the song
		//     browser (the song playing now is first there; its tone is changed with a right-click).
		// View shows or hides the library, the browser and the meters (remembered). Commands are queued to the host;
		// the page shows its state a few times a second. It shares the amp link with the External app tab, which
		// carries one app at a time: each page says when the other one has it.
		namespace ToneDesigner
		{
			enum Section : unsigned { Library = 2, Browser = 4, Meters = 8 };
			enum class Dialog { None, SaveAs, Rename, Delete, NewTone, Discard, Levels, NewFolder, DeleteTones, Restore };
			enum Filter { All, Favourites, Amps, Effects, Dynamics, Instruments, Recent, FilterCount };

			// The panel's own colours: neutral greys, so it reads as a separate app inside the overlay.
			namespace Ink
			{
				constexpr ImU32 App = IM_COL32(21, 23, 28, 255);
				constexpr ImU32 Edge = IM_COL32(42, 46, 55, 255);
				constexpr ImU32 Bar = IM_COL32(28, 31, 38, 255);
				constexpr ImU32 Field = IM_COL32(14, 16, 20, 255);
				constexpr ImU32 FieldEdge = IM_COL32(52, 57, 71, 255);
				constexpr ImU32 Text = IM_COL32(217, 220, 227, 255);
				constexpr ImU32 Soft = IM_COL32(184, 189, 200, 255);
				constexpr ImU32 Muted = IM_COL32(138, 144, 158, 255);
				constexpr ImU32 Dim = IM_COL32(125, 132, 148, 255);
				constexpr ImU32 Blue = IM_COL32(47, 111, 208, 255);
				constexpr ImU32 Amber = IM_COL32(240, 180, 41, 255);
				constexpr ImU32 Library = IM_COL32(24, 26, 32, 255);
				constexpr ImU32 RowHover = IM_COL32(34, 38, 46, 255);
				constexpr ImU32 RowSelected = IM_COL32(36, 58, 92, 255);
				constexpr ImU32 Rack = IM_COL32(15, 17, 21, 255);
				constexpr ImU32 RackEdge = IM_COL32(38, 42, 51, 255);
				constexpr ImU32 Jack = IM_COL32(74, 80, 96, 255);
				constexpr ImU32 Cable = IM_COL32(58, 64, 77, 255);
				constexpr ImU32 Outline = IM_COL32(79, 143, 240, 255);
				constexpr ImU32 Led = IM_COL32(229, 72, 77, 255);
				constexpr ImU32 LedOff = IM_COL32(61, 37, 38, 255);
				constexpr ImU32 Sub = IM_COL32(154, 161, 176, 255);
				constexpr ImU32 Knob = IM_COL32(20, 23, 29, 255);
				constexpr ImU32 KnobEdge = IM_COL32(92, 100, 117, 255);
				constexpr ImU32 Switch = IM_COL32(155, 163, 179, 255);
				constexpr ImU32 SwitchEdge = IM_COL32(98, 106, 122, 255);
				constexpr ImU32 AddText = IM_COL32(122, 167, 245, 255);
				constexpr ImU32 Meter = IM_COL32(48, 164, 108, 255);
				constexpr ImU32 Tag = IM_COL32(36, 58, 92, 255);
				constexpr ImU32 TagText = IM_COL32(156, 194, 255, 255);
				constexpr ImU32 Good = IM_COL32(74, 199, 132, 255);
				constexpr ImU32 Bad = IM_COL32(229, 72, 77, 255);
			}

			// Pedal body and edge by kind: dynamics teal, amps brown and wider, effects purple, others slate.
			struct PedalLook { ImU32 body, edge; float width; int knobs; };
			PedalLook LookFor(VstHostLink::Kind kind)
			{
				switch (kind) {
				case VstHostLink::Kind::Amp: return { IM_COL32(58, 42, 34, 255), IM_COL32(90, 64, 51, 255), 150.0f, 5 };
				case VstHostLink::Kind::Dynamics: return { IM_COL32(38, 53, 58, 255), IM_COL32(53, 80, 88, 255), 96.0f, 2 };
				case VstHostLink::Kind::Effect: return { IM_COL32(44, 43, 68, 255), IM_COL32(69, 67, 106, 255), 96.0f, 2 };
				default: return { IM_COL32(43, 50, 64, 255), IM_COL32(60, 69, 87, 255), 96.0f, 3 };
				}
			}

			struct State
			{
				bool loaded = false;
				unsigned open = 0;              // Section bits, remembered
				int selected = -1;              // the pedal selected on the board (not necessarily in the amp window)
				std::wstring selectedPlugin;    // the browser's selected row
				char search[64] = {};
				char toneSearch[48] = {};
				std::string selectedTone;       // the library's selected row (double-click or Load loads it)
				std::vector<std::string> foldedFolders;   // tone folders folded away in the library (this session)
				int filter = All;
				Dialog dialog = Dialog::None;
				bool openDialog = false;
				std::string dialogTone;         // the tone a dialog is about
				std::vector<std::string> dialogTones;   // a batch dialog's tones (New folder, Delete for a selection)
				std::wstring dialogPath;        // Restore: the backup
				// The library's selection: Ctrl+click adds or takes one away, Shift+click takes the range from the
				// anchor (in the order the rows were drawn last frame). Right-click on a selection gives batch actions.
				std::vector<std::string> selectedTones;
				std::string toneAnchor;
				std::vector<std::string> toneOrder, toneOrderNext;
				// The bottom pane: the plugin browser, or the songs (give songs a tone without playing them). The
				// pedalboard stays on top either way; Plugins | Songs in the pane's title row switches it.
				bool songsMode = false;
				float libraryContentH = 0.0f;   // the tone list's full height last frame, for the window's natural height
				char songSearch[64] = {};
				int songFilter = 0;
				std::vector<std::string> selectedSongs;   // song keys, chosen the same way as tones
				std::string songAnchor;
				std::vector<ToneBackup::Entry> backups;   // File, Backups: listed when the menu opens
				size_t dialogSongs = 0;         // Delete: songs that use it (read once, when the dialog opens)
				char name[64] = {};
				char pickSearch[48] = {};       // "Give it a tone": filters the tone choices (ToneChoices)
				float paneExtra = 0.0f;         // the divider under the board, dragged: added to the bottom pane's height
			};

			State& S()
			{
				static State state;
				if (!state.loaded) {
					state.open = VstHostLink::SectionsOpen(Library | Browser | Meters);
					state.paneExtra = static_cast<float>(VstHostLink::BottomPaneExtra());
					state.loaded = true;
				}
				return state;
			}

			bool IsOpen(Section section) { return (S().open & section) != 0; }

			// Tone names are "Name" or "Folder/Name" (VstHostLink.hpp): one folder level in the tones folder.
			std::string FolderOf(const std::string& tone)
			{
				const size_t slash = tone.find('/');
				return slash == std::string::npos ? std::string() : tone.substr(0, slash);
			}

			std::string BaseName(const std::string& tone)
			{
				const size_t slash = tone.find('/');
				return slash == std::string::npos ? tone : tone.substr(slash + 1);
			}

			bool InList(const std::vector<std::string>& list, const std::string& item)
			{
				return std::any_of(list.begin(), list.end(), [&](const std::string& i) { return _stricmp(i.c_str(), item.c_str()) == 0; });
			}

			// A click in a list with a selection: Ctrl toggles the row, Shift takes the range from the anchor (in
			// `order`, the rows as shown), a plain click selects only that row and makes it the anchor.
			void SelectClick(std::vector<std::string>& selection, std::string& anchor, const std::vector<std::string>& order, const std::string& item)
			{
				const ImGuiIO& io = ImGui::GetIO();
				if (io.KeyCtrl) {
					const auto it = std::find_if(selection.begin(), selection.end(), [&](const std::string& i) { return _stricmp(i.c_str(), item.c_str()) == 0; });
					if (it != selection.end()) selection.erase(it);
					else selection.push_back(item);
					anchor = item;
					return;
				}
				auto indexOf = [&](const std::string& value) {
					for (size_t i = 0; i < order.size(); ++i) if (_stricmp(order[i].c_str(), value.c_str()) == 0) return static_cast<int>(i);
					return -1;
				};
				const int from = indexOf(anchor), to = indexOf(item);
				if (io.KeyShift && from >= 0 && to >= 0) {
					selection.clear();
					for (int i = std::min(from, to); i <= std::max(from, to); ++i) selection.push_back(order[static_cast<size_t>(i)]);
					return;
				}
				selection = { item };
				anchor = item;
			}


			void Toggle(Section section)
			{
				State& s = S();
				s.open ^= section;
				VstHostLink::SetSectionsOpen(s.open);
			}

			// The bottom pane (View > Plugin browser) shows the song browser instead of the plugins.
			bool SongsShown() { return S().songsMode && IsOpen(Browser); }

			// Dialogs open from inside menus and popups, whose ID stack differs from the page's; the request is kept
			// here and the modal opened at page level (Dialogs).
			void Ask(Dialog dialog, const std::string& tone = {})
			{
				State& s = S();
				s.dialog = dialog;
				s.openDialog = true;
				s.dialogTone = tone;
				s.dialogTones.clear();
				s.dialogSongs = dialog == Dialog::Delete ? VstHostLink::SongsUsingTone(tone).size() : 0;
				strncpy_s(s.name, (dialog == Dialog::Rename ? BaseName(tone) : dialog == Dialog::NewFolder ? std::string() : tone).c_str(), _TRUNCATE);
			}

			const VstHostLink::Plugin* FindPlugin(const std::wstring& path)
			{
				for (const VstHostLink::Plugin& plugin : VstHostLink::Plugins())
					if (_wcsicmp(plugin.path.c_str(), path.c_str()) == 0) return &plugin;
				return nullptr;
			}

			const char* KindName(VstHostLink::Kind kind)
			{
				switch (kind) {
				case VstHostLink::Kind::Amp: return "Amp";
				case VstHostLink::Kind::Effect: return "Effect";
				case VstHostLink::Kind::Dynamics: return "Dynamics";
				case VstHostLink::Kind::Instrument: return "Instrument";
				default: return "Plugin";
				}
			}

			ImU32 KindColor(VstHostLink::Kind kind)
			{
				switch (kind) {
				case VstHostLink::Kind::Amp: return IM_COL32(230, 160, 120, 255);
				case VstHostLink::Kind::Effect: return IM_COL32(170, 160, 240, 255);
				case VstHostLink::Kind::Dynamics: return IM_COL32(120, 200, 210, 255);
				default: return Ink::Muted;
				}
			}

			bool InChain(const VstHostLink::Snapshot& vst, const std::wstring& path)
			{
				return std::any_of(vst.slots.begin(), vst.slots.end(),
					[&](const VstHostLink::SlotView& slot) { return _wcsicmp(slot.path.c_str(), path.c_str()) == 0; });
			}

			bool Listed(const std::vector<std::wstring>& list, const std::wstring& path)
			{
				return std::any_of(list.begin(), list.end(), [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; });
			}

			bool KeptLoaded(const VstHostLink::Snapshot& vst, const std::wstring& path) { return Listed(vst.spares, path); }

			// Loading in the background now: the host names it by its file name without .vst3.
			bool Loading(const VstHostLink::Snapshot& vst, const std::wstring& path)
			{
				if (vst.preloadingName.empty()) return false;
				std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
				if (name.size() > 5 && _wcsicmp(name.c_str() + name.size() - 5, L".vst3") == 0) name.resize(name.size() - 5);
				return _stricmp(Utf8(name).c_str(), vst.preloadingName.c_str()) == 0;
			}

			bool HasUnsavedChanges(const VstHostLink::Snapshot& vst) { return vst.toneModified && !vst.slots.empty(); }

			// Loading a tone or starting a new one replaces the chain: ask first when it has changes that are not saved.
			void RequestLoad(const VstHostLink::Snapshot& vst, const std::string& tone)
			{
				if (HasUnsavedChanges(vst)) Ask(Dialog::Discard, tone);
				else VstHostLink::LoadTone(tone);
			}

			void RequestNew(const VstHostLink::Snapshot& vst)
			{
				if (HasUnsavedChanges(vst)) Ask(Dialog::NewTone);
				else VstHostLink::ClearChain();
			}

			// Tone file actions (new, open, save, rename) only wait behind a plugin loading in the background: they
			// queue and run once it has loaded, rather than greying out with no reason given.
			bool FileIdle(const VstHostLink::Snapshot& vst) { return !vst.busy || vst.backgroundOnly; }

			std::string WaitNote(const VstHostLink::Snapshot& vst)
			{
				return vst.backgroundOnly ? vst.preloadingName + " is loading in the background; this happens once it is ready." : std::string();
			}

			void Save(const VstHostLink::Snapshot& vst)
			{
				if (vst.toneName.empty()) Ask(Dialog::SaveAs);
				else VstHostLink::SaveTone(vst.toneName);
			}

			// The previous or next saved tone (the arrows beside the tone's name), wrapping around.
			void StepTone(const VstHostLink::Snapshot& vst, int delta)
			{
				const std::vector<std::string>& tones = VstHostLink::Tones();
				if (tones.empty()) return;
				int index = -1;
				for (int i = 0; i < static_cast<int>(tones.size()); ++i)
					if (_stricmp(tones[i].c_str(), vst.toneName.c_str()) == 0) index = i;
				const int count = static_cast<int>(tones.size());
				const int next = index < 0 ? (delta > 0 ? 0 : count - 1) : ((index + delta) % count + count) % count;
				if (next != index) RequestLoad(vst, tones[next]);
			}

			// The host moves a pedal one place at a time; a drag across several is queued as adjacent swaps.
			void MovePedal(int from, int to)
			{
				for (int i = from; i < to; ++i) VstHostLink::MoveSlot(i, +1);
				for (int i = from; i > to; --i) VstHostLink::MoveSlot(i, -1);
				S().selected = to;
			}

			// Each pedal has its own plugin window (VstHost Main.cpp). Opening one also turns "Show plugin windows" on.
			// A plugin window can take seconds to appear (a big amp sim builds its editor first): the pedal shows a
			// spinner from the request until its window is open, or for 15 s at most.
			int g_openingSlot = -1;
			double g_openingSince = 0.0;

			void OpenWindow(int slot)
			{
				VstHostLink::ShowSlot(slot);
				VstHostLink::SetPopout(true);
				g_openingSlot = slot;
				g_openingSince = ImGui::GetTime();
			}

			// The bar's button: all open windows on or off. Turned on with none open, it opens the selected pedal's.
			void ToggleWindows(const VstHostLink::Snapshot& vst)
			{
				const bool anyOpen = std::any_of(vst.slots.begin(), vst.slots.end(), [](const VstHostLink::SlotView& slot) { return slot.windowOpen; });
				if (!vst.popoutWanted && !anyOpen && !vst.slots.empty())
					VstHostLink::ShowSlot(S().selected >= 0 && S().selected < static_cast<int>(vst.slots.size()) ? S().selected : 0);
				VstHostLink::SetPopout(!vst.popoutWanted);
			}

			// Popups and menus get room around their items (the overlay's windows have no padding of their own).
			void PushPopupPadding() { ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8)); }

			ImFont* CaptionFont() { return GetFonts().caption ? GetFonts().caption : ImGui::GetFont(); }

			void Text(ImDrawList* list, ImFont* font, ImVec2 at, ImU32 color, const char* text)
			{
				list->AddText(font, font->FontSize, at, color, text);
			}

			float TextWidth(ImFont* font, const char* text) { return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text).x; }

			// The plugins as menu items, vendor on the right. Instruments are left out (they make no sound from a
			// guitar); the browser still lists them.
			// Dragging: a pedal carries its slot ("RAB_PEDAL", an int); a plugin from the browser carries its path
			// ("RAB_PLUGIN"). Either can be dropped between two pedals to go in there.
			struct PluginDrag { wchar_t path[VstHostControl::kPathChars]; };

			template <typename Pick> void PluginMenuItems(Pick pick)
			{
				const std::vector<VstHostLink::Plugin>& plugins = VstHostLink::Plugins();
				if (plugins.empty()) ImGui::TextDisabled("No 64-bit VST3 plugins found");
				// A dot before each loaded plugin, as in the browser: green in the chain, blue kept loaded outside it
				// (adding that one is instant).
				const VstHostLink::Snapshot vst = VstHostLink::GetSnapshot();
				for (const VstHostLink::Plugin& plugin : plugins) {
					if (plugin.kind == VstHostLink::Kind::Instrument) continue;
					const std::string label = "    " + plugin.name;   // room for the dot
					if (ImGui::MenuItem(label.c_str(), plugin.vendor.c_str())) pick(plugin);
					const bool inChain = InChain(vst, plugin.path), loaded = KeptLoaded(vst, plugin.path);
					if (inChain || loaded) {
						const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
						ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(min.x + 7.0f, (min.y + max.y) * 0.5f), 3.5f, inChain ? Ink::Good : Ink::AddText, 12);
						Hint(inChain ? "In the chain now." : "Kept loaded: adding it is instant.");
					}
				}
			}

			// One pedal's actions: the pedal's right-click menu, and Edit for the selected pedal.
			void PedalActions(const VstHostLink::Snapshot& vst, int i)
			{
				const int count = static_cast<int>(vst.slots.size());
				const bool valid = i >= 0 && i < count;
				const bool idle = !vst.busy;
				const bool windowOpen = valid && vst.slots[i].windowOpen;
				if (ImGui::MenuItem(windowOpen ? "Bring window to front" : "Open window", "or double-click it", false, valid)) OpenWindow(i);
				if (windowOpen && ImGui::MenuItem("Close window")) VstHostLink::HideSlot(i);
				if (ImGui::MenuItem(valid && vst.slots[i].bypass ? "Turn on" : "Bypass", "or click its footswitch", false, valid && idle))
					VstHostLink::SetBypass(i, !vst.slots[i].bypass);
				ImGui::Separator();
				if (ImGui::BeginMenu("Replace with", valid && idle)) {
					PluginMenuItems([&](const VstHostLink::Plugin& plugin) { VstHostLink::ReplaceSlot(i, plugin.path); });
					ImGui::EndMenu();
				}
				if (ImGui::MenuItem("Duplicate", nullptr, false, valid && idle && count < VstHostControl::kMaxSlots)) VstHostLink::DuplicateSlot(i);
				if (ImGui::MenuItem("Reload plugin", nullptr, false, valid && idle)) VstHostLink::Reload(vst.slots[i].path);
				Hint("Closes the plugin completely and starts it again, keeping its settings: for when it needs a fresh start to see something new, such as tones you imported into it. Every copy of it reloads; the rest of the chain keeps playing meanwhile.");
				if (ImGui::MenuItem("Move left", nullptr, false, valid && idle && i > 0)) { VstHostLink::MoveSlot(i, -1); S().selected = i - 1; }
				if (ImGui::MenuItem("Move right", nullptr, false, valid && idle && i + 1 < count)) { VstHostLink::MoveSlot(i, +1); S().selected = i + 1; }
				ImGui::Separator();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
				if (ImGui::MenuItem("Remove", nullptr, false, valid && idle)) VstHostLink::RemoveSlot(i);
				ImGui::PopStyleColor();
			}

			void FileMenu(const VstHostLink::Snapshot& vst)
			{
				if (!ImGui::BeginMenu("File")) return;
				const bool idle = FileIdle(vst);
				const std::string wait = WaitNote(vst);
				if (!wait.empty()) { ImGui::TextDisabled("Loading %s in the background", vst.preloadingName.c_str()); ImGui::Separator(); }
				if (ImGui::MenuItem("New tone", nullptr, false, idle)) RequestNew(vst);
				Hint(wait.c_str());
				if (ImGui::BeginMenu("Open tone", idle)) {
					const std::vector<std::string>& tones = VstHostLink::Tones();
					if (tones.empty()) ImGui::TextDisabled("No saved tones yet");
					for (const std::string& tone : tones)
						if (ImGui::MenuItem(tone.c_str(), nullptr, _stricmp(tone.c_str(), vst.toneName.c_str()) == 0)) RequestLoad(vst, tone);
					ImGui::EndMenu();
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Save", nullptr, false, idle && (vst.toneName.empty() || vst.toneModified))) Save(vst);
				Hint(wait.c_str());
				if (ImGui::MenuItem("Save as...", nullptr, false, idle)) Ask(Dialog::SaveAs, vst.toneName);
				Hint(wait.c_str());
				if (ImGui::MenuItem("Revert to saved", nullptr, false, idle && !vst.toneName.empty() && vst.toneModified)) VstHostLink::LoadTone(vst.toneName);
				Hint(wait.c_str());
				if (ImGui::MenuItem("Rename...", nullptr, false, idle && !vst.toneName.empty())) Ask(Dialog::Rename, vst.toneName);
				Hint(wait.c_str());
				ImGui::Separator();
				if (ImGui::MenuItem("Show tones folder")) VstHostLink::OpenTonesFolder();
				// Backups: one zip with tones, song tones, plugin settings and settings (ToneBackup.hpp).
				if (ImGui::BeginMenu("Backups")) {
					State& s = S();
					if (ImGui::IsWindowAppearing()) s.backups = ToneBackup::List();
					if (ImGui::MenuItem("Back up now", nullptr, false, !ToneBackup::Busy())) ToneBackup::BackupNow();
					Hint("Your tones, song tones, plugin settings and Tone designer settings in one zip. One is also made each day on its own; the last 7 of those are kept.");
					ImGui::Separator();
					ImGui::TextDisabled(s.backups.empty() ? "No backups yet" : "Restore");
					for (size_t i = 0; i < s.backups.size() && i < 14; ++i)
						if (ImGui::MenuItem(s.backups[i].name.c_str(), nullptr, false, !ToneBackup::Busy())) {
							Ask(Dialog::Restore, s.backups[i].name);
							s.dialogPath = s.backups[i].path;
						}
					ImGui::Separator();
					if (ImGui::MenuItem("Open backups folder")) ToneBackup::OpenFolder();
					ImGui::EndMenu();
				}
				ImGui::Separator();
				// The power button only turns it off (standby, plugins kept loaded); this closes the program and frees them.
				if (ImGui::MenuItem("Close Tone designer")) VstHostLink::StopHost();
				ImGui::EndMenu();
			}

			void EditMenu(const VstHostLink::Snapshot& vst)
			{
				if (!ImGui::BeginMenu("Edit")) return;
				const int selected = S().selected;
				const bool valid = selected >= 0 && selected < static_cast<int>(vst.slots.size());
				ImGui::TextDisabled("%s", valid ? vst.slots[selected].name.c_str() : "No pedal selected");
				ImGui::Separator();
				PedalActions(vst, selected);
				ImGui::Separator();
				if (ImGui::MenuItem("Bypass whole chain", nullptr, vst.chainBypassed, !vst.slots.empty()))
					VstHostLink::SetChainBypass(!vst.chainBypassed);
				ImGui::Separator();
				if (ImGui::MenuItem("Levels...")) Ask(Dialog::Levels);
				ImGui::EndMenu();
			}

			void ViewMenu(const VstHostLink::Snapshot& vst, const ExternalAmp::Snapshot& amp)
			{
				if (!ImGui::BeginMenu("View")) return;
				if (ImGui::MenuItem("Plugin windows", nullptr, vst.popoutWanted)) ToggleWindows(vst);
				if (ImGui::MenuItem("Tone library", nullptr, IsOpen(Library))) Toggle(Library);
				if (ImGui::MenuItem("Plugin browser", nullptr, IsOpen(Browser))) Toggle(Browser);
				if (ImGui::MenuItem("Meters", nullptr, IsOpen(Meters))) Toggle(Meters);
				ImGui::Separator();
				if (ImGui::MenuItem("Compare with the game's amp", nullptr, amp.comparing)) ExternalAmp::SetCompare(!amp.comparing);
				if (ImGui::MenuItem("Start with the game", nullptr, vst.autoStart)) VstHostLink::SetAutoStart(!vst.autoStart);
				// No "Fast tone changes" menu: the host keeps recent plugins loaded on its own (RefreshKeepSettings);
				// pinning one is the browser's right-click, and a failed background load shows on its status pill.
				ImGui::Separator();
				if (ImGui::MenuItem("Rescan plugins")) VstHostLink::Rescan();
				if (ImGui::MenuItem("Add plugin folder...")) VstHostLink::AddScanFolder();
				ImGui::EndMenu();
			}

			// A square icon button drawn on the bar; `on` lights it blue.
			bool IconSquare(const char* id, unsigned glyph, bool on, const char* hint, float size)
			{
				const ImVec2 at = ImGui::GetCursorScreenPos();
				const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
				const bool hovered = ImGui::IsItemHovered();
				ImDrawList* list = ImGui::GetWindowDrawList();
				const ImVec2 end(at.x + size, at.y + size);
				if (on) list->AddRectFilled(at, end, Ink::Blue, 6.0f);
				else if (hovered) list->AddRectFilled(at, end, Ink::Edge, 6.0f);
				list->AddRect(at, end, on ? Ink::Blue : Ink::FieldEdge, 6.0f);
				DrawIcon(list, ImVec2(at.x + size * 0.5f, at.y + size * 0.5f), glyph, on || hovered ? IM_COL32_WHITE : Ink::Soft);
				Hint(hint);
				return pressed;
			}

			// The bar: menus, the tone's name between arrows, A/B, the amp window and power.
			void Bar(const VstHostLink::Snapshot& vst, const ExternalAmp::Snapshot& amp, float width, float height)
			{
				ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0, 0, 0, 0));
				ImGui::PushStyleColor(ImGuiCol_Header, ImGui::ColorConvertU32ToFloat4(Ink::Edge));
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::ColorConvertU32ToFloat4(Ink::Edge));
				// The bar's height and its labels' vertical place both come from FramePadding (Begin, BeginMenuBar);
				// it is popped right after, so the menus' popups keep normal item heights.
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9, (height - ImGui::GetFontSize()) * 0.5f));
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 0));
				ImGui::BeginChild("##toneBar", ImVec2(width, height), false,
					ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
				ImGui::PopStyleVar();
				const bool bar = ImGui::BeginMenuBar();
				ImGui::PopStyleVar();
				PushPopupPadding();
				if (bar) {
					FileMenu(vst);
					EditMenu(vst);
					ViewMenu(vst, amp);
					float menusEnd = ImGui::GetCursorPosX();
					ImDrawList* list = ImGui::GetWindowDrawList();
					const ImVec2 origin = ImGui::GetWindowPos();
					const float button = height - 12.0f;
					const float y = (height - button) * 0.5f;

					// Right: VST | Game amp, the amp window, power.
					ImFont* caption = CaptionFont();
					const char* sides[2] = { "VST", "Game amp" };
					const float sideW[2] = { TextWidth(caption, sides[0]) + 18.0f, TextWidth(caption, sides[1]) + 18.0f };
					float x = width - 10.0f - button - 6.0f - button - 10.0f - sideW[0] - sideW[1];
					const ImVec2 ab(origin.x + x, origin.y + y);
					list->AddRect(ab, ImVec2(ab.x + sideW[0] + sideW[1], ab.y + button), Ink::FieldEdge, 6.0f);
					for (int side = 0; side < 2; ++side) {
						const bool on = (side == 1) == amp.comparing;
						const ImVec2 at(ab.x + (side ? sideW[0] : 0.0f), ab.y);
						ImGui::SetCursorScreenPos(at);
						ImGui::PushID(side);
						if (ImGui::InvisibleButton("##ab", ImVec2(sideW[side], button)) && !on) ExternalAmp::SetCompare(side == 1);
						ImGui::PopID();
						Hint("A/B: hear Tone designer, or Rocksmith's own amp for now (not saved).");
						if (on) list->AddRectFilled(at, ImVec2(at.x + sideW[side], at.y + button), Ink::Blue, 6.0f,
							side ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersLeft);
						Text(list, caption, ImVec2(at.x + 9.0f, at.y + (button - caption->FontSize) * 0.5f), on ? IM_COL32_WHITE : Ink::Muted, sides[side]);
					}
					x += sideW[0] + sideW[1] + 10.0f;
					ImGui::SetCursorScreenPos(ImVec2(origin.x + x, origin.y + y));
					if (IconSquare("##ampWindow", Glyph::NewWindow, vst.popoutWanted,
						vst.popoutWanted ? "Hide the plugin windows." : "Show the plugin windows over the game whenever this overlay is open. Each pedal has its own: double-click a pedal to open it. Drag a window's top bar to move it.", button))
						ToggleWindows(vst);
					x += button + 6.0f;
					ImGui::SetCursorScreenPos(ImVec2(origin.x + x, origin.y + y));
					if (IconSquare("##power", Glyph::Power, false, "Turn Tone designer off. The game's amp comes back; your plugins stay loaded, so turning it on again is instant. File > Close Tone designer closes it completely.", button))
						VstHostLink::TurnOff();

					// Middle: < tone name >, centred in the space between the menus and A/B.
					const float left = menusEnd + 12.0f, right = ab.x - origin.x - 12.0f;
					const float nameW = std::clamp(right - left - 2.0f * (button + 6.0f), 80.0f, 240.0f);
					const float groupW = nameW + 2.0f * (button + 6.0f);
					float gx = left + std::max(0.0f, (right - left - groupW) * 0.5f);
					ImGui::SetCursorScreenPos(ImVec2(origin.x + gx, origin.y + y));
					if (IconSquare("##prevTone", Glyph::ChevronLeft, false, "Previous saved tone.", button) && FileIdle(vst)) StepTone(vst, -1);
					gx += button + 6.0f;
					const ImVec2 box(origin.x + gx, origin.y + y);
					ImGui::SetCursorScreenPos(box);
					if (ImGui::InvisibleButton("##toneName", ImVec2(nameW, button))) ImGui::OpenPopup("##toneList");
					Hint("Your tones. Click to pick one; File has Save and Save as.");
					list->AddRectFilled(box, ImVec2(box.x + nameW, box.y + button), Ink::Field, 6.0f);
					list->AddRect(box, ImVec2(box.x + nameW, box.y + button), Ink::FieldEdge, 6.0f);
					const std::string name = vst.loading ? "Loading..." : vst.toneName.empty() ? (vst.slots.empty() ? "New tone" : "Unsaved tone") : BaseName(vst.toneName);
					const bool dot = HasUnsavedChanges(vst);
					const float textW = ImGui::CalcTextSize(name.c_str()).x + (dot ? 13.0f : 0.0f);
					const float tx = box.x + std::max(8.0f, (nameW - textW) * 0.5f);
					const float ty = box.y + (button - ImGui::GetFontSize()) * 0.5f;
					list->PushClipRect(box, ImVec2(box.x + nameW - 6.0f, box.y + button), true);
					list->AddText(ImVec2(tx, ty), IM_COL32_WHITE, name.c_str());
					list->PopClipRect();
					if (dot) list->AddCircleFilled(ImVec2(std::min(tx + textW - 3.5f, box.x + nameW - 8.0f), ty + ImGui::GetFontSize() * 0.55f), 3.5f, Ink::Amber, 12);
					gx += nameW + 6.0f;
					ImGui::SetCursorScreenPos(ImVec2(origin.x + gx, origin.y + y));
					if (IconSquare("##nextTone", Glyph::ChevronRight, false, "Next saved tone.", button) && FileIdle(vst)) StepTone(vst, +1);
					if (ImGui::BeginPopup("##toneList")) {
						const std::vector<std::string>& tones = VstHostLink::Tones();
						if (tones.empty()) ImGui::TextDisabled("No saved tones yet");
						for (const std::string& tone : tones)
							if (ImGui::MenuItem(tone.c_str(), nullptr, _stricmp(tone.c_str(), vst.toneName.c_str()) == 0) && FileIdle(vst)) RequestLoad(vst, tone);
						ImGui::Separator();
						if (ImGui::MenuItem("Save as...", nullptr, false, FileIdle(vst))) Ask(Dialog::SaveAs, vst.toneName);
						ImGui::EndPopup();
					}
					ImGui::EndMenuBar();
				}
				ImGui::PopStyleVar();
				ImGui::EndChild();
				ImGui::PopStyleColor(3);
			}

			std::vector<std::string> ToneFolders(const std::vector<std::string>& tones)
			{
				std::vector<std::string> folders;
				for (const std::string& tone : tones) {
					const std::string folder = FolderOf(tone);
					if (!folder.empty() && std::find(folders.begin(), folders.end(), folder) == folders.end()) folders.push_back(folder);
				}
				std::sort(folders.begin(), folders.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
				return folders;
			}

			void MoveToFolder(const std::string& tone, const std::string& folder)
			{
				const std::string to = folder.empty() ? BaseName(tone) : folder + "/" + BaseName(tone);
				if (to != tone) VstHostLink::RenameTone(tone, to);
				State& s = S();
				if (_stricmp(s.selectedTone.c_str(), tone.c_str()) == 0) s.selectedTone = to;
				for (std::string& selected : s.selectedTones) if (_stricmp(selected.c_str(), tone.c_str()) == 0) selected = to;
				if (_stricmp(s.toneAnchor.c_str(), tone.c_str()) == 0) s.toneAnchor = to;
			}

			// Right-click on a selection of several tones.
			void BatchToneMenu(const VstHostLink::Snapshot& vst)
			{
				State& s = S();
				const std::vector<std::string> batch = s.selectedTones;
				ImGui::TextDisabled("%d tones", static_cast<int>(batch.size()));
				ImGui::Separator();
				if (ImGui::BeginMenu("Move to folder", FileIdle(vst))) {
					if (ImGui::MenuItem("No folder")) for (const std::string& t : batch) MoveToFolder(t, {});
					ImGui::Separator();
					for (const std::string& folder : ToneFolders(VstHostLink::Tones()))
						if (ImGui::MenuItem(folder.c_str())) for (const std::string& t : batch) MoveToFolder(t, folder);
					ImGui::Separator();
					if (ImGui::MenuItem("New folder...")) { Ask(Dialog::NewFolder); s.dialogTones = batch; }
					ImGui::EndMenu();
				}
				bool allFavourite = true;
				for (const std::string& t : batch) allFavourite = allFavourite && VstHostLink::IsFavouriteTone(t);
				if (ImGui::MenuItem("Favourite", nullptr, allFavourite)) for (const std::string& t : batch) VstHostLink::SetFavouriteTone(t, !allFavourite);
				if (ImGui::MenuItem("Select none")) { s.selectedTones.clear(); s.toneAnchor.clear(); }
				ImGui::Separator();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
				if (ImGui::MenuItem(("Delete " + std::to_string(batch.size()) + " tones...").c_str(), nullptr, false, FileIdle(vst))) {
					Ask(Dialog::DeleteTones);
					s.dialogTones = batch;
				}
				ImGui::PopStyleColor();
			}

			// One row of the tone library: click loads, the star marks a favourite, right-click for the rest.
			// `section` keeps rows apart: a favourite is listed twice (Favourites and its own place), and two rows with
			// one ImGui ID share their click and hover, so neither could be selected.
			void ToneRow(const VstHostLink::Snapshot& vst, const std::string& tone, float width, const char* section)
			{
				ImGui::PushID(section);
				ImGui::PushID(tone.c_str());
				const bool current = _stricmp(tone.c_str(), vst.toneName.c_str()) == 0;
				const bool favourite = VstHostLink::IsFavouriteTone(tone);
				const float height = ImGui::GetFontSize() + 10.0f;
				const ImVec2 at = ImGui::GetCursorScreenPos();
				const bool pressed = ImGui::InvisibleButton("##tone", ImVec2(width, height));
				const bool hovered = ImGui::IsItemHovered();
				const bool onStar = ImGui::GetIO().MousePos.x > at.x + width - 24.0f;
				// Click selects, double-click loads (like the plugin browser); the star toggles a favourite.
				State& s = S();
				if (!InList(s.toneOrderNext, tone)) s.toneOrderNext.push_back(tone);
				if (pressed) {
					if (onStar) VstHostLink::SetFavouriteTone(tone, !favourite);
					else { SelectClick(s.selectedTones, s.toneAnchor, s.toneOrder, tone); s.selectedTone = tone; }
				}
				if (hovered && !onStar && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !current && FileIdle(vst)) RequestLoad(vst, tone);
				// Drag onto songs (the Songs pane) to give them this tone.
				if (SongsShown() && ImGui::BeginDragDropSource()) {
					ImGui::SetDragDropPayload("RAB_TONE", tone.c_str(), tone.size() + 1);
					ImGui::Text("%s  (drop on songs)", BaseName(tone).c_str());
					ImGui::EndDragDropSource();
				}
				const bool selected = InList(s.selectedTones, tone);
				Hint(current ? "The tone playing now. Right-click for more. Ctrl or Shift+click to select several."
					: "Double-click to load it. Right-click for more; Ctrl or Shift+click to select several for a batch. The star keeps it at the top.");
				ImDrawList* list = ImGui::GetWindowDrawList();
				const ImVec2 end(at.x + width, at.y + height);
				if (selected) list->AddRectFilled(at, end, Ink::RowSelected, 5.0f);
				else if (hovered) list->AddRectFilled(at, end, Ink::RowHover, 5.0f);
				// The tone playing now: a green dot before its name.
				if (current) list->AddCircleFilled(ImVec2(at.x + 9.0f, at.y + height * 0.5f), 3.5f, Ink::Good, 12);
				list->PushClipRect(at, ImVec2(end.x - 24.0f, end.y), true);
				const std::string shown = BaseName(tone);
				list->AddText(ImVec2(at.x + 18.0f, at.y + 5.0f), current || selected ? IM_COL32_WHITE : IM_COL32(195, 200, 210, 255), shown.c_str());
				list->PopClipRect();
				if (_stricmp(vst.defaultTone.c_str(), tone.c_str()) == 0 && !hovered && !favourite) {
					ImFont* caption = CaptionFont();
					const float w = TextWidth(caption, "default");
					Text(list, caption, ImVec2(end.x - w - 6.0f, at.y + (height - caption->FontSize) * 0.5f), Ink::Dim, "default");
				}
				if (favourite || hovered)
					DrawIcon(list, ImVec2(end.x - 12.0f, at.y + height * 0.5f), favourite ? Glyph::StarFilled : Glyph::Star, favourite ? Ink::Amber : Ink::Dim);

				PushPopupPadding();
				if (ImGui::BeginPopupContextItem("##toneMenu")) {
					// Right-click outside the selection: that tone alone. On a selection of several: the batch.
					if (!InList(s.selectedTones, tone)) { s.selectedTones = { tone }; s.toneAnchor = tone; }
					if (s.selectedTones.size() > 1) BatchToneMenu(vst);
					else {
					s.selectedTone = tone;
					ImGui::TextDisabled("%s", tone.c_str());
					ImGui::Separator();
					if (ImGui::MenuItem("Load", "or double-click it", false, !current && FileIdle(vst))) RequestLoad(vst, tone);
					if (SongsShown() && !s.selectedSongs.empty()) {
						const std::string label = "Give to the " + (s.selectedSongs.size() == 1 ? std::string("selected song") : std::to_string(s.selectedSongs.size()) + " selected songs");
						if (ImGui::MenuItem(label.c_str())) for (const std::string& key : s.selectedSongs) VstHostLink::SetToneForSong(key, tone);
					}
					if (ImGui::MenuItem("Favourite", nullptr, favourite)) VstHostLink::SetFavouriteTone(tone, !favourite);
					const bool songTone = _stricmp(vst.songTone.c_str(), tone.c_str()) == 0;
					const bool isDefault = _stricmp(vst.defaultTone.c_str(), tone.c_str()) == 0;
					if (ImGui::MenuItem("Use as default tone", nullptr, isDefault))
						VstHostLink::SetDefaultTone(isDefault ? std::string() : tone);
					Hint("Loads whenever a song without a tone of its own starts.");
					if (ImGui::MenuItem("Use for the current song", nullptr, songTone, !vst.songKey.empty() && !songTone && !vst.busy)) {
						VstHostLink::SetSongTone(tone);
						if (!current) RequestLoad(vst, tone);
					}
					ImGui::Separator();
					if (ImGui::MenuItem("Rename...", nullptr, false, FileIdle(vst))) Ask(Dialog::Rename, tone);
					if (ImGui::BeginMenu("Move to folder", FileIdle(vst))) {
						if (ImGui::MenuItem("No folder", nullptr, FolderOf(tone).empty())) MoveToFolder(tone, {});
						ImGui::Separator();
						for (const std::string& folder : ToneFolders(VstHostLink::Tones()))
							if (ImGui::MenuItem(folder.c_str(), nullptr, folder == FolderOf(tone))) MoveToFolder(tone, folder);
						ImGui::Separator();
						if (ImGui::MenuItem("New folder...")) Ask(Dialog::NewFolder, tone);
						ImGui::EndMenu();
					}
					if (ImGui::MenuItem("Show in folder")) VstHostLink::OpenTonesFolder();
					ImGui::Separator();
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
					if (ImGui::MenuItem("Delete...", nullptr, false, FileIdle(vst))) Ask(Dialog::Delete, tone);
					ImGui::PopStyleColor();
					}
					ImGui::EndPopup();
				}
				ImGui::PopStyleVar();
				ImGui::PopID();
				ImGui::PopID();
			}

			void FoldLabel(const char* text)
			{
				ImGui::Dummy(ImVec2(0, 2));
				ImGui::PushFont(CaptionFont());
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Dim));
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
				ImGui::TextUnformatted(text);
				ImGui::PopStyleColor();
				ImGui::PopFont();
			}

			// Left column: search, favourites, all tones, song tones.
			void LibraryColumn(const VstHostLink::Snapshot& vst, float width, float height)
			{
				State& s = S();
				s.toneOrder.swap(s.toneOrderNext);
				s.toneOrderNext.clear();
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 10));
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 2));
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
				ImGui::BeginChild("##toneLibrary", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
				ImGui::PopStyleColor();
				const float rowW = ImGui::GetContentRegionAvail().x;
				ImGui::SetNextItemWidth(rowW);
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
				ImGui::InputTextWithHint("##toneSearch", "Search tones", s.toneSearch, sizeof(s.toneSearch));
				ImGui::PopStyleVar();

				const std::vector<std::string>& tones = VstHostLink::Tones();
				auto matches = [&](const std::string& tone) {
					if (!s.toneSearch[0]) return true;
					const char* needle = s.toneSearch;
					return std::search(tone.begin(), tone.end(), needle, needle + strlen(needle),
						[](char a, char b) { return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b)); }) != tone.end();
				};
				bool anyFavourite = false;
				for (const std::string& tone : tones) if (VstHostLink::IsFavouriteTone(tone) && matches(tone)) { anyFavourite = true; break; }
				if (anyFavourite) {
					FoldLabel("Favourites");
					for (const std::string& tone : tones) if (VstHostLink::IsFavouriteTone(tone) && matches(tone)) ToneRow(vst, tone, rowW, "favourites");
				}
				FoldLabel(ToneFolders(tones).empty() ? "All tones" : "Tones");
				if (tones.empty()) {
					ImGui::PushFont(CaptionFont());
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowW);
					ImGui::TextUnformatted("None yet. File, Save as keeps the chain with every plugin's settings.");
					// Where they live, so a tone saved elsewhere can be copied in (installing never touches this folder).
					ImGui::TextUnformatted(("Saved in " + Utf8(VstHostLink::TonesFolder())).c_str());
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
					if (ImGui::SmallButton("Open tones folder")) VstHostLink::OpenTonesFolder();
					ImGui::PopFont();
				}
				for (const std::string& tone : tones) if (FolderOf(tone).empty() && matches(tone)) ToneRow(vst, tone, rowW, "tones");
				// Folders: a header that folds the folder away (click), its tones under it.
				for (const std::string& folder : ToneFolders(tones)) {
					size_t count = 0;
					for (const std::string& tone : tones) if (FolderOf(tone) == folder && matches(tone)) ++count;
					if (count == 0) continue;
					auto& folded = s.foldedFolders;
					const auto it = std::find(folded.begin(), folded.end(), folder);
					const bool open = it == folded.end();
					ImGui::PushID(folder.c_str());
					const ImVec2 at = ImGui::GetCursorScreenPos();
					const float headerH = ImGui::GetFontSize() + 8.0f;
					if (ImGui::InvisibleButton("##folder", ImVec2(rowW, headerH))) {
						if (open) folded.push_back(folder);
						else folded.erase(it);
					}
					const bool hovered = ImGui::IsItemHovered();
					ImGui::PopID();
					ImDrawList* list = ImGui::GetWindowDrawList();
					if (hovered) list->AddRectFilled(at, ImVec2(at.x + rowW, at.y + headerH), Ink::RowHover, 5.0f);
					const ImU32 color = hovered ? Ink::Text : Ink::Soft;
					const float cx = at.x + 8.0f, cy = at.y + headerH * 0.5f;
					if (open) list->AddTriangleFilled(ImVec2(cx - 4.0f, cy - 2.0f), ImVec2(cx + 4.0f, cy - 2.0f), ImVec2(cx, cy + 3.0f), color);
					else list->AddTriangleFilled(ImVec2(cx - 2.0f, cy - 4.0f), ImVec2(cx + 3.0f, cy), ImVec2(cx - 2.0f, cy + 4.0f), color);
					list->AddText(ImVec2(at.x + 18.0f, at.y + 4.0f), color, folder.c_str());
					ImFont* caption = CaptionFont();
					const std::string number = std::to_string(count);
					Text(list, caption, ImVec2(at.x + rowW - TextWidth(caption, number.c_str()) - 8.0f, at.y + (headerH - caption->FontSize) * 0.5f), Ink::Dim, number.c_str());
					if (open)
						for (const std::string& tone : tones) if (FolderOf(tone) == folder && matches(tone)) {
							ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
							ToneRow(vst, tone, rowW - 10.0f, "folders");
						}
				}
				FoldLabel("Song tones");
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
				const size_t songs = VstHostLink::SongToneCount();
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
				ImGui::TextUnformatted(songs == 0 ? "None yet" : songs == 1 ? "1 song" : (std::to_string(songs) + " songs").c_str());
				if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				if (ImGui::IsItemClicked()) {
					s.songsMode = true;
					s.songFilter = songs == 0 ? 0 : 1;
					if (!IsOpen(Browser)) Toggle(Browser);
				}
				Hint("Songs that load their own tone when they start. Click to see them (Songs, in the pane under the board), or give more songs a tone there.");
				ImGui::PopStyleColor();
				s.libraryContentH = ImGui::GetCursorPosY() + 10.0f;   // plus the bottom padding

				// Right-click on the empty part of the column.
				PushPopupPadding();
				if (ImGui::BeginPopupContextWindow("##libraryMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
					ImGui::TextDisabled("Tones");
					ImGui::Separator();
					if (ImGui::MenuItem("New tone", nullptr, false, FileIdle(vst))) RequestNew(vst);
					if (ImGui::MenuItem("Save current as...", nullptr, false, FileIdle(vst))) Ask(Dialog::SaveAs, vst.toneName);
					if (ImGui::MenuItem("Open tones folder")) VstHostLink::OpenTonesFolder();
					ImGui::EndPopup();
				}
				ImGui::PopStyleVar();
				ImGui::EndChild();
				ImGui::PopStyleVar(2);
			}

			void Jack(ImDrawList* list, ImVec2 center, const char* label)
			{
				list->AddCircle(center, 7.0f, Ink::Jack, 16, 3.0f);
				ImFont* caption = CaptionFont();
				Text(list, caption, ImVec2(center.x - TextWidth(caption, label) * 0.5f, center.y + 12.0f), Ink::Dim, label);
			}

			// A vertical segmented meter in a column `width` wide whose left edge is at `at.x`, centred on `at.y`:
			// green, then amber, then red at the top; all lit with a red top while it clips. Click for the levels.
			void MeterColumn(ImDrawList* list, ImVec2 at, float width, const char* label, float peak, const char* id, const VstHostLink::Snapshot& vst)
			{
				constexpr int kSegments = 6;
				const float segH = 8.0f, gap = 3.0f, barW = 10.0f;
				const float barH = kSegments * (segH + gap) - gap;
				ImFont* caption = CaptionFont();
				const float labelTop = at.y - (barH + caption->FontSize + 6.0f) * 0.5f;
				const float barTop = labelTop + caption->FontSize + 6.0f;
				const float cx = at.x + width * 0.5f;
				ImGui::SetCursorScreenPos(ImVec2(at.x, labelTop));
				if (ImGui::InvisibleButton(id, ImVec2(width, barTop + barH - labelTop))) Ask(Dialog::Levels);
				Hint(vst.outputPeak > 1.0f ? "The chain's output is clipping. Click to lower the output trim, or lower the last plugin's volume."
					: "Your guitar into the chain (IN) and the chain's output (OUT). Click for the levels.");
				Text(list, caption, ImVec2(cx - TextWidth(caption, label) * 0.5f, labelTop), Ink::Dim, label);
				const bool clipping = peak > 1.0f;
				const int lit = clipping ? kSegments : static_cast<int>(std::lround(MeterFraction(peak) * kSegments));
				for (int i = 0; i < kSegments; ++i) {
					const float y = barTop + barH - (i + 1) * (segH + gap) + gap;
					const ImVec2 a(cx - barW * 0.5f, y), b(cx + barW * 0.5f, y + segH);
					const ImU32 on = i == kSegments - 1 ? Ink::Bad : i == kSegments - 2 ? Ink::Amber : Ink::Meter;
					list->AddRectFilled(a, b, i < lit ? on : Ink::Edge, 2.0f);   // unlit segments stay visible on the rack
				}
			}

			// Over the rack while plugins load: which plugin, the step of a tone, the seconds so far, and a bar that keeps
			// moving. The overlay draws it, so it moves even while the host is busy inside a slow plugin's loader; a
			// load that is truly stuck shows as the seconds climbing (the page offers Force close once the host stops
			// answering).
			void LoadingPanel(const VstHostLink::Snapshot& vst)
			{
				ImDrawList* list = ImGui::GetWindowDrawList();
				const ImVec2 min = ImGui::GetWindowPos();
				const ImVec2 size = ImGui::GetWindowSize();
				const ImVec2 max(min.x + size.x, min.y + size.y);
				list->PushClipRect(min, max, false);
				list->AddRectFilled(min, max, IM_COL32(15, 17, 21, 215), 10.0f);
				ImFont* caption = CaptionFont();
				const float centerX = min.x + size.x * 0.5f;
				const float top = min.y + size.y * 0.5f - 34.0f;

				const std::string title = "Loading " + (vst.loadingName.empty() ? std::string("plugins") : vst.loadingName);
				const float titleW = ImGui::CalcTextSize(title.c_str()).x;
				list->AddText(ImVec2(centerX - titleW * 0.5f, top), IM_COL32_WHITE, title.c_str());

				char detail[96];
				const int seconds = static_cast<int>(vst.loadingSeconds);
				if (vst.loadingTotal > 1) sprintf_s(detail, "%d of %d, %d s", std::max(vst.loadingStep, 1), vst.loadingTotal, seconds);
				else sprintf_s(detail, "%d s", seconds);
				std::string line = detail;
				if (seconds >= 8) line += ". Big plugins (AmpliTube, Helix) can take a while";
				const float lineW = TextWidth(caption, line.c_str());
				Text(list, caption, ImVec2(centerX - lineW * 0.5f, top + ImGui::GetFontSize() + 6.0f), Ink::Muted, line.c_str());

				// The bar: the plugins done so far filled, and a block sweeping through the one loading now.
				const float barW = std::min(320.0f, size.x * 0.6f), barH = 6.0f;
				const ImVec2 bar(centerX - barW * 0.5f, top + ImGui::GetFontSize() + caption->FontSize + 20.0f);
				list->AddRectFilled(bar, ImVec2(bar.x + barW, bar.y + barH), Ink::Field, 3.0f);
				const int total = std::max(vst.loadingTotal, 1);
				const int done = std::clamp(vst.loadingStep - 1, 0, total);
				const float segment = barW / total;
				if (done > 0) list->AddRectFilled(bar, ImVec2(bar.x + segment * done, bar.y + barH), Ink::Blue, 3.0f);
				const float sweep = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
				const float blockW = std::max(segment * 0.35f, 12.0f);
				const float blockX = bar.x + segment * done + (segment - blockW) * sweep;
				list->AddRectFilled(ImVec2(blockX, bar.y), ImVec2(blockX + blockW, bar.y + barH), Ink::AddText, 3.0f);
				list->PopClipRect();
			}

			// The rack: In, the pedals joined by cables, Add, Out. Scrolls sideways when the chain is longer than the panel.
			void Board(const VstHostLink::Snapshot& vst, float width, float height)
			{
				State& s = S();
				const int count = static_cast<int>(vst.slots.size());
				if (s.selected >= count) s.selected = count - 1;
				if (s.selected < 0 && vst.shownSlot >= 0 && vst.shownSlot < count) s.selected = vst.shownSlot;

				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 14));
				ImGui::BeginChild("##board", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);
				const float boardW = ImGui::GetContentRegionAvail().x;
				// The hint under the rack wraps to as many lines as the board's width needs; its room is measured, not
				// guessed, so it never runs into the pane below (plus the item spacing above it and a little air).
				const char* boardHint = count == 0 ? "The chain is empty, so you hear your clean guitar. Click Add for an amp sim, then pedals before it (gate, drive) or after it (delay, reverb)."
					: vst.chainBypassed ? "The whole chain is bypassed (Edit menu): you hear your clean guitar."
					: "Right-click a pedal, a tone, or the board. Double-click a pedal to open its window. Click a footswitch to bypass. Drag a pedal or a plugin from the browser between two pedals to put it there; scroll or middle-drag to move along a long chain.";
				const float hintH = CaptionFont()->CalcTextSizeA(CaptionFont()->FontSize, FLT_MAX, boardW, boardHint).y + ImGui::GetStyle().ItemSpacing.y + 4.0f;
				const float rackH = std::max(150.0f, ImGui::GetContentRegionAvail().y - hintH);
				const ImVec2 board = ImGui::GetCursorScreenPos();
				ImDrawList* outer = ImGui::GetWindowDrawList();
				outer->AddRectFilled(board, ImVec2(board.x + boardW, board.y + rackH), Ink::Rack, 10.0f);
				outer->AddRect(board, ImVec2(board.x + boardW, board.y + rackH), Ink::RackEdge, 10.0f);
				const float pedalH = 124.0f;

				// IN and OUT meters stand at the board's two ends, outside the scrolling rack so a long chain never
				// scrolls them away (View > Meters hides them). Click either for the levels.
				const float meterCol = IsOpen(Meters) ? 44.0f : 0.0f;
				if (meterCol > 0.0f) {
					const float midY = board.y + std::max(0.0f, (rackH - 6.0f - pedalH) * 0.5f) + pedalH * 0.5f;
					MeterColumn(outer, ImVec2(board.x, midY), meterCol, "IN", vst.inputPeak, "##inMeter", vst);
					MeterColumn(outer, ImVec2(board.x + boardW - meterCol, midY), meterCol, "OUT", vst.outputPeak, "##outMeter", vst);
				}
				const float rackW = boardW - 2.0f * meterCol;
				ImGui::SetCursorScreenPos(ImVec2(board.x + meterCol, board.y));

				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 0));
				ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
				ImGui::BeginChild("##rack", ImVec2(rackW, rackH), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysUseWindowPadding);
				ImDrawList* list = ImGui::GetWindowDrawList();
				ImFont* caption = CaptionFont();
				const float top = ImGui::GetCursorScreenPos().y + std::max(0.0f, (rackH - 6.0f - pedalH) * 0.5f);
				const float midY = top + pedalH * 0.5f;
				float x = ImGui::GetCursorScreenPos().x;
				const ImVec2 rackStart = ImGui::GetCursorScreenPos();
				auto cable = [&] { list->AddLine(ImVec2(x, midY), ImVec2(x + 14.0f, midY), Ink::Cable, 3.0f); x += 22.0f; };
				bool overTile = false;   // the mouse is on a pedal (its menu, not the board's)

				Jack(list, ImVec2(x + 21.0f, midY - 6.0f), "In");
				x += 42.0f + 8.0f;
				cable();
				// The gaps before each pedal and after the last: drop targets that insert there (see the drop
				// handling after the Add tile). Each spans the cable into the pedal, a little into the pedal too.
				std::vector<ImRect> gaps;
				for (int i = 0; i < count; ++i) {
					gaps.emplace_back(ImVec2(x - 26.0f, top), ImVec2(x + 6.0f, top + pedalH));
					const VstHostLink::SlotView& slot = vst.slots[i];
					const VstHostLink::Plugin* plugin = FindPlugin(slot.path);
					const PedalLook look = LookFor(plugin ? plugin->kind : VstHostLink::Kind::Unknown);
					const bool playing = !slot.bypass && !vst.chainBypassed;
					const bool selected = i == s.selected;
					const ImVec2 at(x, top);
					const ImVec2 end(x + look.width, top + pedalH);
					const ImVec2 footswitch(x + look.width * 0.5f, end.y - 20.0f);

					ImGui::PushID(i);
					ImGui::SetCursorScreenPos(at);
					const bool pressed = ImGui::InvisibleButton("##pedal", ImVec2(look.width, pedalH));
					ImGui::SetItemUsingMouseWheel();   // the wheel scrolls the rack, not the page
					const bool hovered = ImGui::IsItemHovered();
					overTile |= hovered;
					const ImVec2 mouse = ImGui::GetIO().MousePos;
					const bool onSwitch = (mouse.x - footswitch.x) * (mouse.x - footswitch.x) + (mouse.y - footswitch.y) * (mouse.y - footswitch.y) < 15.0f * 15.0f;
					if (pressed) {
						if (onSwitch) { if (!vst.busy) VstHostLink::SetBypass(i, !slot.bypass); }
						else s.selected = i;
					}
					if (hovered && !onSwitch && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenWindow(i);
					Hint(onSwitch ? (slot.bypass ? "Footswitch: turn it on." : "Footswitch: bypass it.")
						: "Double-click to open its window. Right-click for more. Drag to move it.");
					if (ImGui::BeginDragDropSource()) {
						ImGui::SetDragDropPayload("RAB_PEDAL", &i, sizeof(i));
						ImGui::TextUnformatted(slot.name.c_str());
						ImGui::EndDragDropSource();
					}
					if (ImGui::BeginDragDropTarget()) {
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAB_PEDAL")) {
							const int from = *static_cast<const int*>(payload->Data);
							if (from != i && !vst.busy) MovePedal(from, i);
						}
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAB_PLUGIN"))
							if (!vst.busy) VstHostLink::ReplaceSlot(i, static_cast<const PluginDrag*>(payload->Data)->path);
						ImGui::EndDragDropTarget();
					}
					PushPopupPadding();
					if (ImGui::BeginPopupContextItem("##pedalMenu")) {
						s.selected = i;
						ImGui::TextDisabled("%s", slot.name.c_str());
						ImGui::Separator();
						PedalActions(vst, i);
						ImGui::EndPopup();
					}
					ImGui::PopStyleVar();

					// A bypassed pedal is drawn faded into the rack.
					auto fade = [&](ImU32 color) { return playing ? color : Mix(Ink::Rack, color, 0.55f); };
					if (selected) list->AddRect(ImVec2(at.x - 3.0f, at.y - 3.0f), ImVec2(end.x + 3.0f, end.y + 3.0f), Ink::Outline, 11.0f, 0, 2.0f);
					list->AddRectFilled(at, end, fade(hovered ? Mix(look.body, IM_COL32_WHITE, 0.06f) : look.body), 9.0f);
					list->AddRect(at, end, fade(look.edge), 9.0f);
					list->AddCircleFilled(ImVec2(x + look.width * 0.5f, at.y + 12.0f), 4.0f, playing ? Ink::Led : Ink::LedOff, 12);
					if (slot.windowOpen)   // its plugin window is open
						DrawIcon(list, ImVec2(end.x - 13.0f, at.y + 13.0f), Glyph::NewWindow, fade(vst.popoutWanted ? Ink::AddText : Ink::Dim));
					list->PushClipRect(ImVec2(at.x + 4.0f, at.y), ImVec2(end.x - 4.0f, end.y), true);
					const float nameW = ImGui::CalcTextSize(slot.name.c_str()).x;
					list->AddText(ImVec2(x + std::max(5.0f, (look.width - nameW) * 0.5f), at.y + 22.0f), fade(IM_COL32_WHITE), slot.name.c_str());
					const std::string sub = plugin && !plugin->vendor.empty() ? plugin->vendor : (slot.bypass ? "bypassed" : "");
					const float subW = TextWidth(caption, sub.c_str());
					Text(list, caption, ImVec2(x + std::max(5.0f, (look.width - subW) * 0.5f), at.y + 24.0f + ImGui::GetFontSize()), fade(Ink::Sub), sub.c_str());
					list->PopClipRect();
					const float knobsW = look.knobs * 16.0f + (look.knobs - 1) * 7.0f;
					for (int k = 0; k < look.knobs; ++k) {
						const ImVec2 knob(x + (look.width - knobsW) * 0.5f + 8.0f + k * 23.0f, at.y + 68.0f);
						list->AddCircleFilled(knob, 8.0f, fade(Ink::Knob), 16);
						list->AddCircle(knob, 8.0f, fade(Ink::KnobEdge), 16, 2.0f);
						list->AddLine(knob, ImVec2(knob.x - 3.0f, knob.y - 5.0f), fade(Ink::KnobEdge), 1.5f);
					}
					list->AddCircleFilled(footswitch, 12.0f, fade(hovered && onSwitch ? IM_COL32(190, 198, 212, 255) : Ink::Switch), 20);
					list->AddCircle(footswitch, 12.0f, fade(Ink::SwitchEdge), 20, 2.0f);
					// Its window was asked for and is not open yet: dim the pedal and turn a spinner over it.
					if (i == g_openingSlot) {
						if (slot.windowOpen || ImGui::GetTime() - g_openingSince > 15.0) g_openingSlot = -1;
						else {
							list->AddRectFilled(at, end, IM_COL32(0, 0, 0, 120), 9.0f);
							const ImVec2 c(x + look.width * 0.5f, top + pedalH * 0.5f - 6.0f);
							const float a0 = static_cast<float>(ImGui::GetTime()) * 6.0f;
							list->PathArcTo(c, 14.0f, a0, a0 + 4.2f, 24);
							list->PathStroke(Ink::AddText, 0, 3.0f);
							Text(list, caption, ImVec2(c.x - TextWidth(caption, "Opening") * 0.5f, c.y + 20.0f), IM_COL32_WHITE, "Opening");
						}
					}
					ImGui::PopID();
					x = end.x + 8.0f;
					cable();
				}

				gaps.emplace_back(ImVec2(x - 26.0f, top), ImVec2(x + 6.0f, top + pedalH));
				// Add: a dashed tile, opening the plugin list.
				if (count < VstHostControl::kMaxSlots) {
					const float addW = 70.0f;
					const ImVec2 at(x, top), end(x + addW, top + pedalH);
					ImGui::SetCursorScreenPos(at);
					ImGui::BeginDisabled(vst.busy);
					if (ImGui::InvisibleButton("##addPedal", ImVec2(addW, pedalH))) ImGui::OpenPopup("##addMenu");
					ImGui::SetItemUsingMouseWheel();
					const bool hovered = ImGui::IsItemHovered();
					ImGui::EndDisabled();
					Hint("Add a plugin at the end of the chain. The plugin browser below has search; drag one from it onto the board.");
					if (ImGui::BeginDragDropTarget()) {
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAB_PLUGIN"))
							if (!vst.busy) VstHostLink::AddPlugin(static_cast<const PluginDrag*>(payload->Data)->path);
						ImGui::EndDragDropTarget();
					}
					const ImU32 dash = hovered ? Ink::AddText : Ink::Jack;
					for (float d = at.x + 9.0f; d < end.x - 9.0f; d += 8.0f) {
						list->AddLine(ImVec2(d, at.y), ImVec2(std::min(d + 4.0f, end.x - 9.0f), at.y), dash);
						list->AddLine(ImVec2(d, end.y), ImVec2(std::min(d + 4.0f, end.x - 9.0f), end.y), dash);
					}
					for (float d = at.y + 9.0f; d < end.y - 9.0f; d += 8.0f) {
						list->AddLine(ImVec2(at.x, d), ImVec2(at.x, std::min(d + 4.0f, end.y - 9.0f)), dash);
						list->AddLine(ImVec2(end.x, d), ImVec2(end.x, std::min(d + 4.0f, end.y - 9.0f)), dash);
					}
					DrawIcon(list, ImVec2(at.x + addW * 0.5f, midY - 8.0f), Glyph::Add, Ink::AddText);
					Text(list, caption, ImVec2(at.x + (addW - TextWidth(caption, "Add")) * 0.5f, midY + 6.0f), Ink::AddText, "Add");
					x = end.x + 8.0f;
					cable();
				}

				// Dropping into a gap inserts there: a dragged pedal moves to that place (the pedals after it shift
				// along, rather than two pedals swapping), a plugin from the browser goes in at that slot. While
				// something is dragged, every gap shows a faint marker and the one under the mouse a bright bar.
				if (const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
					dragging && (dragging->IsDataType("RAB_PEDAL") || dragging->IsDataType("RAB_PLUGIN"))) {
					for (int g = 0; g < static_cast<int>(gaps.size()); ++g) {
						const ImRect& gap = gaps[g];
						const float gx = gap.Min.x + 14.0f;
						// A pedal's own two sides are no move: no marker there.
						const int from = dragging->IsDataType("RAB_PEDAL") ? *static_cast<const int*>(dragging->Data) : -1;
						if (from >= 0 && (g == from || g == from + 1)) continue;
						if (from < 0 && count >= VstHostControl::kMaxSlots) continue;   // a full chain takes no more
						list->AddLine(ImVec2(gx, gap.Min.y + 18.0f), ImVec2(gx, gap.Max.y - 18.0f), Ink::Jack, 2.0f);
						if (!ImGui::BeginDragDropTargetCustom(gap, ImGui::GetID(("##gap" + std::to_string(g)).c_str())))
							continue;
						const ImGuiDragDropFlags peek = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAB_PEDAL", peek)) {
							list->AddLine(ImVec2(gx, gap.Min.y), ImVec2(gx, gap.Max.y), Ink::Outline, 4.0f);
							if (payload->IsDelivery() && !vst.busy) MovePedal(from, g > from ? g - 1 : g);
						}
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAB_PLUGIN", peek)) {
							list->AddLine(ImVec2(gx, gap.Min.y), ImVec2(gx, gap.Max.y), Ink::Outline, 4.0f);
							if (payload->IsDelivery() && !vst.busy && count < VstHostControl::kMaxSlots) {
								VstHostLink::AddPlugin(static_cast<const PluginDrag*>(payload->Data)->path, g);
								S().selected = g;
							}
						}
						ImGui::EndDragDropTarget();
					}
				}
				Jack(list, ImVec2(x + 21.0f, midY - 6.0f), "Out");
				x += 42.0f;
				// The empty part of the board: one item behind the pedals (submitted last, so the pedals keep the
				// hover), for its right-click menu and to claim the mouse wheel. It also sets the rack's scroll width.
				ImGui::SetCursorScreenPos(rackStart);
				ImGui::InvisibleButton("##board", ImVec2(x + 10.0f - rackStart.x, std::max(rackH - 6.0f, 1.0f)));
				ImGui::SetItemUsingMouseWheel();
				// Not over a pedal: IsItemHovered here is true under the pedals too (ImGui 1.85 checks only the
				// rectangle), and opening this menu after a pedal's would replace the pedal's menu.
				if (!overTile && ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) ImGui::OpenPopup("##boardMenu");

				// A long chain: the wheel scrolls it sideways (the page does not scroll under the rack), and a
				// middle-button drag anywhere on it pans it. Shift+wheel and a touchpad's sideways scroll work as usual.
				static bool panning = false;
				const ImGuiIO& io = ImGui::GetIO();
				if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
				{
					if (io.MouseWheel != 0.0f) ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseWheel * 90.0f);
					if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) panning = true;
				}
				if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) panning = false;
				if (panning && io.MouseDelta.x != 0.0f) ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
				if (vst.loading) LoadingPanel(vst);

				PushPopupPadding();
				if (ImGui::BeginPopup("##addMenu") || ImGui::BeginPopup("##boardMenu")) {
					ImGui::TextDisabled("Add plugin");
					ImGui::Separator();
					if (count >= VstHostControl::kMaxSlots) ImGui::TextDisabled("The chain is full (8 plugins)");
					else PluginMenuItems([](const VstHostLink::Plugin& plugin) { VstHostLink::AddPlugin(plugin.path); });
					ImGui::Separator();
					if (ImGui::MenuItem("Bypass whole chain", nullptr, vst.chainBypassed, count > 0)) VstHostLink::SetChainBypass(!vst.chainBypassed);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
					if (ImGui::MenuItem("Clear chain", nullptr, false, count > 0 && !vst.busy)) RequestNew(vst);
					ImGui::PopStyleColor();
					ImGui::EndPopup();
				}
				ImGui::PopStyleVar();
				ImGui::EndChild();
				ImGui::PopStyleVar(2);

				// Under the rack: what to do, or why it is quiet.
				ImGui::PushFont(caption);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Dim));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + boardW);
				ImGui::TextUnformatted(boardHint);
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
				ImGui::PopFont();
				ImGui::EndChild();
				ImGui::PopStyleVar();
			}

			bool MatchesSearch(const VstHostLink::Plugin& plugin, const char* search)
			{
				if (!search[0]) return true;
				auto contains = [&](const std::string& text) {
					return std::search(text.begin(), text.end(), search, search + strlen(search),
						[](char a, char b) { return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b)); }) != text.end();
				};
				return contains(plugin.name) || contains(plugin.vendor);
			}

			bool MatchesFilter(const VstHostLink::Plugin& plugin, int filter)
			{
				switch (filter) {
				case Favourites: return VstHostLink::IsFavourite(plugin.path);
				case Amps: return plugin.kind == VstHostLink::Kind::Amp;
				case Effects: return plugin.kind == VstHostLink::Kind::Effect || plugin.kind == VstHostLink::Kind::Unknown;
				case Dynamics: return plugin.kind == VstHostLink::Kind::Dynamics;
				case Instruments: return plugin.kind == VstHostLink::Kind::Instrument;
				default: return true;
				}
			}

			// A status pill's text and colours.
			struct PillLook { const char* text; ImU32 fill, edge, ink; };
			PillLook TintPill(const char* text, ImU32 hue) { return { text, Mix(Ink::App, hue, 0.18f), Mix(Ink::App, hue, 0.55f), hue }; }

			// A plugin's status is two separate things, each its own pill: where it stands with the chain (in it,
			// ready to add, or why not), and whether a copy is kept loaded outside it for fast tone changes. One
			// word for both hid "In chain" behind "Always loaded".
			PillLook ChainPill(const VstHostLink::Snapshot& vst, const VstHostLink::Plugin& plugin)
			{
				if (VstHostLink::FailedToLoad(plugin.path)) return TintPill("Failed to load", Ink::Bad);
				if (InChain(vst, plugin.path)) return TintPill("In chain", Ink::Good);
				if (plugin.kind == VstHostLink::Kind::Instrument) return TintPill("Not for guitar", Ink::Amber);
				if (Loading(vst, plugin.path)) return TintPill("Loading...", Ink::Amber);
				return { "Ready", IM_COL32(40, 45, 55, 255), Ink::FieldEdge, Ink::Soft };
			}

			bool MemoryPill(const VstHostLink::Snapshot& vst, const VstHostLink::Plugin& plugin, PillLook& look)
			{
				// "Always keep loaded" on a plugin in the chain: the chain's copy is the loaded one (the host keeps no
				// spare copy beside it), so it is loaded now, not waiting. "Loads soon" is only for one that is not
				// loaded yet: the host loads always-loaded plugins in the background outside songs.
				const bool always = Listed(vst.alwaysLoaded, plugin.path);
				if (KeptLoaded(vst, plugin.path) || (always && InChain(vst, plugin.path)))
					look = { always ? "Always loaded" : "Loaded", Ink::Blue, Ink::Blue, IM_COL32_WHITE };
				else if (always) look = TintPill(Listed(vst.preloadFailed, plugin.path) ? "Load failed" : "Loads soon", Ink::Amber);
				else return false;
				return true;
			}

			// Why a starred plugin shows "Load failed". Marks from before the host saved a reason only ever came from
			// the host closing mid-load, so that is the answer for those too.
			std::string PreloadFailedWhy(const VstHostLink::Snapshot& vst, const std::wstring& path)
			{
				for (const auto& [failedPath, why] : vst.preloadFailedWhy)
					if (_wcsicmp(failedPath.c_str(), path.c_str()) == 0 && !why.empty()) return why;
				return "The plugin host closed while this loaded in the background, most likely because the plugin crashed it.";
			}

			float PillWidth(ImFont* font, const char* text, bool dot = false) { return TextWidth(font, text) + 22.0f + (dot ? 14.0f : 0.0f); }

			// The status column's two pill widths: each as wide as its longest label, so every row's pills line up.
			float ChainPillWidth(ImFont* font)
			{
				float w = 0.0f;
				for (const char* t : { "In chain", "Ready", "Not for guitar", "Failed to load", "Loading..." }) w = std::max(w, PillWidth(font, t));
				return w;
			}
			float MemoryPillWidth(ImFont* font)
			{
				float w = 0.0f;
				for (const char* t : { "Always loaded", "Loaded", "Loads soon", "Load failed" }) w = std::max(w, PillWidth(font, t));
				return w;
			}

			// Draws a pill at `at`, `height` tall, with an optional status dot before the text; returns its width.
			// A `fixedWidth` makes it that wide with the text centred (the status column).
			float DrawPill(ImDrawList* list, ImFont* font, ImVec2 at, float height, const PillLook& look, ImU32 dot = 0, float fixedWidth = 0.0f)
			{
				const float width = fixedWidth > 0.0f ? fixedWidth : PillWidth(font, look.text, dot != 0);
				const ImVec2 end(at.x + width, at.y + height);
				list->AddRectFilled(at, end, look.fill, height * 0.5f);
				list->AddRect(at, end, look.edge, height * 0.5f);
				float x = fixedWidth > 0.0f ? at.x + (width - TextWidth(font, look.text)) * 0.5f : at.x + 11.0f;
				if (dot) { list->AddCircleFilled(ImVec2(x + 4.0f, at.y + height * 0.5f), 4.0f, dot, 12); x += 14.0f; }
				Text(list, font, ImVec2(x, at.y + (height - font->FontSize) * 0.5f), look.ink, look.text);
				return width;
			}

			bool Chip(const char* label, bool on)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(on ? Ink::Blue : Ink::RowHover));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(on ? Ink::Blue : Ink::Edge));
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(on ? IM_COL32_WHITE : Ink::Soft));
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 3));
				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
				const bool pressed = ImGui::Button(label);
				ImGui::PopStyleVar(2);
				ImGui::PopStyleColor(3);
				return pressed;
			}

			// Tone choices for a menu: a search box, then tones outside folders and a submenu per folder. While the
			// search has text, every matching tone is listed flat, folder included ("Tests/Test 1").
			template <typename Pick> void ToneChoices(Pick pick)
			{
				const std::vector<std::string>& tones = VstHostLink::Tones();
				if (tones.empty()) { ImGui::TextDisabled("No saved tones yet"); return; }
				char* search = S().pickSearch;
				if (ImGui::IsWindowAppearing()) { search[0] = 0; ImGui::SetKeyboardFocusHere(); }
				ImGui::SetNextItemWidth(std::max(180.0f, ImGui::GetContentRegionAvail().x));
				ImGui::InputTextWithHint("##pickSearch", "Search tones", search, sizeof(S().pickSearch));
				if (search[0]) {
					const std::string needle = search;
					bool any = false;
					for (const std::string& tone : tones) {
						const bool hit = std::search(tone.begin(), tone.end(), needle.begin(), needle.end(),
							[](char x, char y) { return tolower(static_cast<unsigned char>(x)) == tolower(static_cast<unsigned char>(y)); }) != tone.end();
						if (!hit) continue;
						any = true;
						if (ImGui::MenuItem(tone.c_str())) pick(tone);
					}
					if (!any) ImGui::TextDisabled("No tone matches");
					return;
				}
				for (const std::string& tone : tones) if (FolderOf(tone).empty() && ImGui::MenuItem(tone.c_str())) pick(tone);
				for (const std::string& folder : ToneFolders(tones))
					if (ImGui::BeginMenu(folder.c_str())) {
						for (const std::string& tone : tones) if (FolderOf(tone) == folder && ImGui::MenuItem(BaseName(tone).c_str())) pick(tone);
						ImGui::EndMenu();
					}
			}

			// Plugins | Songs: what the pane under the board shows. It is the pane's title, so it sits at the start of
			// that pane's first row (PluginBrowser, SongsView). The choice is kept for the session (State::songsMode).
			void PaneToggle()
			{
				ImFont* font = CaptionFont();
				static const char* const modes[2] = { "Plugins", "Songs" };
				State& s = S();
				ImDrawList* list = ImGui::GetWindowDrawList();
				const float height = ImGui::GetFrameHeight();
				const ImVec2 start = ImGui::GetCursorScreenPos();
				float x = start.x;
				for (int i = 0; i < 2; ++i) {
					const float w = TextWidth(font, modes[i]) + 22.0f;
					const ImVec2 at(x, start.y);
					const bool on = s.songsMode == (i == 1);
					ImGui::SetCursorScreenPos(at);
					ImGui::PushID(i);
					if (ImGui::InvisibleButton("##pane", ImVec2(w, height))) s.songsMode = i == 1;
					ImGui::PopID();
					const bool hovered = ImGui::IsItemHovered();
					Hint(i == 1 ? "Your songs: give them tones of their own without playing them, and see which wait for plugins to load. Drag a tone from the left onto them."
						: "Every plugin found: drag one onto the board to use it.");
					if (on) list->AddRectFilled(at, ImVec2(at.x + w, at.y + height), Ink::Blue, 6.0f, i ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersLeft);
					Text(list, font, ImVec2(at.x + 11.0f, at.y + (height - font->FontSize) * 0.5f), on ? IM_COL32_WHITE : hovered ? Ink::Text : Ink::Muted, modes[i]);
					x += w;
				}
				list->AddRect(start, ImVec2(x, start.y + height), Ink::FieldEdge, 6.0f);
				ImGui::SetCursorScreenPos(start);
				ImGui::Dummy(ImVec2(x - start.x, height));
			}

			// The Songs pane under the board: every song in the library (SongLibrary) with its tone and how long its
			// tone took to load when it last started (VstHostLink::SongLoads, the audit). Select songs (Ctrl or Shift
			// for several), then drag a tone from the library onto them, right-click, or use the buttons below.
			// Drawn inside BottomPane's fixed-height child, on the same first row as its Plugins | Songs toggle.
			void SongsView(const VstHostLink::Snapshot& vst)
			{
				State& s = S();
				SongLibrary::Refresh();
				const std::shared_ptr<const SongLibrary::Library> library = SongLibrary::Get();
				const std::vector<std::pair<std::string, std::string>> songTones = VstHostLink::SongToneList();
				const std::vector<std::pair<std::string, VstHostLink::SongLoad>> loads = VstHostLink::SongLoads();
				auto toneOf = [&](const std::string& key) -> std::string {
					for (const auto& entry : songTones) if (_stricmp(entry.first.c_str(), key.c_str()) == 0) return entry.second;
					return {};
				};
				auto loadOf = [&](const std::string& key) -> const VstHostLink::SongLoad* {
					for (const auto& entry : loads) if (_stricmp(entry.first.c_str(), key.c_str()) == 0) return &entry.second;
					return nullptr;
				};
				auto slow = [](const VstHostLink::SongLoad* load) { return load && load->seconds >= 3.0f && !load->plugins.empty(); };
				auto playing = [&](const std::string& key) { return !vst.songKey.empty() && _stricmp(key.c_str(), vst.songKey.c_str()) == 0; };
				// The song playing now goes through SetSongTone / ClearSongTone as well, so its tone loads at once (or
				// the one playing stays when it is taken away).
				auto assignTo = [&](const std::vector<std::string>& keys, const std::string& tone) {
					for (const std::string& key : keys) {
						if (!playing(key)) VstHostLink::SetToneForSong(key, tone);
						else if (tone.empty()) VstHostLink::ClearSongTone();
						else {
							VstHostLink::SetSongTone(tone);
							if (_stricmp(tone.c_str(), vst.toneName.c_str()) != 0) RequestLoad(vst, tone);
						}
					}
				};

				ImFont* caption = CaptionFont();
				const float left = ImGui::GetCursorStartPos().x;
				const float inner = ImGui::GetWindowContentRegionMax().x - left;

				// Title row (after the toggle), laid out like the plugin browser's: the count, search on the right.
				// While a tone's plugins load in the background, that shows in place of the count; amber while a change waits for it.
				const SongLibrary::State state = SongLibrary::GetState();
				const std::string count = !vst.preloadingName.empty()
					? (vst.waitingOnBackground ? "Waiting for " + vst.preloadingName + " to finish loading, then your change goes through..."
						: "Getting " + vst.preloadingName + " ready in the background...")
					: state == SongLibrary::State::Reading && library->songs.empty() ? std::string("reading your songs...")
					: std::to_string(library->songs.size()) + " songs, " + std::to_string(songTones.size()) + " with a tone";
				const float searchW = std::min(240.0f, inner * 0.4f);
				ImGui::SameLine(0.0f, 12.0f);
				if (ImGui::GetCursorPosX() + TextWidth(caption, count.c_str()) + 12.0f < left + inner - searchW) {
					ImGui::PushFont(caption);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(vst.waitingOnBackground && !vst.preloadingName.empty() ? Ink::Amber : Ink::Dim));
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(count.c_str());
					ImGui::PopStyleColor();
					ImGui::PopFont();
				}
				ImGui::SameLine(left + inner - searchW);
				ImGui::SetNextItemWidth(searchW);
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
				ImGui::InputTextWithHint("##songSearch", "Search songs or artists", s.songSearch, sizeof(s.songSearch));
				ImGui::PopStyleVar();
				ImGui::PopStyleColor();

				// Filter chips.
				enum { AllSongs, WithTone, SlowToLoad, SongFilterCount };
				static const char* const chips[SongFilterCount] = { "All", "With a tone", "Slow to load" };
				ImGui::SetCursorPosX(left);
				for (int c = 0; c < SongFilterCount; ++c) {
					if (c > 0) ImGui::SameLine(0.0f, 6.0f);
					if (Chip(chips[c], s.songFilter == c)) s.songFilter = c;
					if (c == SlowToLoad) Hint("Songs whose tone waited 3 s or more for plugins to load when they last started. Always keep loaded (right-click the plugin in the plugin browser) makes them instant; songs you rarely play can stay as they are.");
				}

				// Rows: the library's songs, and song keys with a tone or a load record the library does not have.
				struct Row { std::string key, title, artist; const SongLibrary::Song* song; };
				std::vector<Row> rows;
				auto matches = [&](const std::string& title, const std::string& artist, const std::string& key) {
					if (!s.songSearch[0]) return true;
					const std::string needle = s.songSearch;
					auto has = [&](const std::string& text) {
						return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
							[](char x, char y) { return tolower(static_cast<unsigned char>(x)) == tolower(static_cast<unsigned char>(y)); }) != text.end();
					};
					return has(title) || has(artist) || has(key);
				};
				auto wanted = [&](const std::string& key) {
					if (s.songFilter == WithTone) return !toneOf(key).empty();
					if (s.songFilter == SlowToLoad) return slow(loadOf(key));
					return true;
				};
				for (const SongLibrary::Song& song : library->songs)
					if (wanted(song.key) && matches(song.title, song.artist, song.key)) rows.push_back({ song.key, song.title, song.artist, &song });
				auto addOrphan = [&](const std::string& key) {
					if (library->Find(key) || std::any_of(rows.begin(), rows.end(), [&](const Row& r) { return _stricmp(r.key.c_str(), key.c_str()) == 0; })) return;
					if (wanted(key) && matches(key, std::string(), key)) rows.push_back({ key, key, std::string(), nullptr });
				};
				for (const auto& entry : songTones) addOrphan(entry.first);
				for (const auto& entry : loads) addOrphan(entry.first);
				if (s.songFilter == SlowToLoad)
					std::sort(rows.begin(), rows.end(), [&](const Row& x, const Row& y) { return loadOf(x.key)->seconds > loadOf(y.key)->seconds; });
				// The song playing now goes first, with a green dot: its tone is changed here (right-click it), now
				// that the strip under the board is gone.
				std::stable_partition(rows.begin(), rows.end(), [&](const Row& r) { return playing(r.key); });
				std::vector<std::string> order;
				order.reserve(rows.size());
				for (const Row& row : rows) order.push_back(row.key);

				// Column widths: the Tone column fits the longest tone name (capped), the slow-load seconds keep their
				// own narrow column at the end, and Song and Artist share the rest, Song the larger part.
				float toneColW = TextWidth(caption, "Tone") + 16.0f;
				for (const auto& entry : songTones) toneColW = std::max(toneColW, TextWidth(caption, BaseName(entry.second).c_str()) + 24.0f);
				toneColW = std::min(toneColW, inner * 0.28f);
				bool anySlow = false;
				for (const Row& row : rows) anySlow = anySlow || slow(loadOf(row.key));
				const float lastColW = anySlow ? TextWidth(caption, "99 s") + 18.0f : 0.0f;
				const float titleX = 8.0f, toneX = std::floor(inner - 8.0f - lastColW - toneColW);
				const float artistX = std::floor(titleX + (toneX - titleX) * 0.58f);
				ImDrawList* list = ImGui::GetWindowDrawList();
				{
					const ImVec2 at = ImGui::GetCursorScreenPos();
					Text(list, caption, ImVec2(at.x + titleX, at.y), Ink::Dim, "Song");
					Text(list, caption, ImVec2(at.x + artistX, at.y), Ink::Dim, "Artist");
					Text(list, caption, ImVec2(at.x + toneX, at.y), Ink::Dim, "Tone");
					ImGui::Dummy(ImVec2(inner, caption->FontSize + 2.0f));
					list->AddLine(ImVec2(at.x, at.y + caption->FontSize + 4.0f), ImVec2(at.x + inner, at.y + caption->FontSize + 4.0f), Ink::Edge);
				}

				// The foot row holds Buttons (FramePadding 7 each side, see Button): its full height, the gap above it
				// and the item spacing on both sides, so nothing is cut off at the pane's edge (the 14 inset is the
				// child's window padding, outside the content region).
				const float itemGap = ImGui::GetStyle().ItemSpacing.y;
				const float footH = ImGui::GetFontSize() + 14.0f + 4.0f + 2.0f * itemGap;
				const float listH = std::max(ImGui::GetContentRegionAvail().y - footH, 40.0f);
				const float rowH = ImGui::GetFontSize() + 10.0f;   // the plugin browser's row height
				if (rows.empty()) {
					ImGui::PushFont(caption);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
					ImGui::PushTextWrapPos(left + inner);
					ImGui::TextUnformatted(state == SongLibrary::State::Failed && library->songs.empty()
						? "Your songs could not be read: RSMods.exe needs to be in the game's RSMods folder (the installer's Repair puts it back)."
						: state == SongLibrary::State::Reading && library->songs.empty() ? "Reading your songs. The first time takes a few seconds, longer for a big library."
						: s.songFilter == WithTone ? "No song has a tone of its own yet. Select songs, then drag a tone from the left onto them."
						: s.songFilter == SlowToLoad ? "No song has waited for its tone. Songs show here after they start."
						: "Nothing matches.");
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
					ImGui::PopFont();
					ImGui::Dummy(ImVec2(0, std::max(listH - caption->FontSize * 2.0f, 0.0f)));
				}
				else {
					ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
					ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
					ImGui::BeginChild("##songRows", ImVec2(inner, listH), false);
					ImDrawList* rowsList = ImGui::GetWindowDrawList();
					const float rowW = ImGui::GetContentRegionAvail().x;
					ImGuiListClipper clipper;   // thousands of songs: only the visible rows are drawn
					clipper.Begin(static_cast<int>(rows.size()), rowH);
					while (clipper.Step()) {
						for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
							const Row& row = rows[static_cast<size_t>(r)];
							ImGui::PushID(row.key.c_str());
							const bool selected = InList(s.selectedSongs, row.key);
							const ImVec2 at = ImGui::GetCursorScreenPos();
							if (ImGui::InvisibleButton("##song", ImVec2(rowW, rowH))) SelectClick(s.selectedSongs, s.songAnchor, order, row.key);
							const bool hovered = ImGui::IsItemHovered();
							const std::string tone = toneOf(row.key);
							const VstHostLink::SongLoad* load = loadOf(row.key);
							// A tone dragged from the library: onto a selected song, the whole selection gets it.
							if (ImGui::BeginDragDropTarget()) {
								if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAB_TONE")) {
									const std::string dropped(static_cast<const char*>(payload->Data));
									if (selected) assignTo(s.selectedSongs, dropped);
									else { s.selectedSongs = { row.key }; s.songAnchor = row.key; assignTo(s.selectedSongs, dropped); }
								}
								ImGui::EndDragDropTarget();
							}
							// Details in the tooltip, not on the page.
							if (hovered) {
								std::string about = row.song ? row.song->album + (row.song->year > 0 ? " (" + std::to_string(row.song->year) + ")" : std::string())
									+ "\n" + row.song->arrangements + ", in " + row.song->file : std::string("Not in your song library (key ") + row.key + ")";
								if (load) {
									char seconds[16]{};
									sprintf_s(seconds, "%.1f", load->seconds);
									about += std::string("\nLast start: \"") + load->tone + "\" ready in " + seconds + " s"
										+ (load->plugins.empty() ? std::string(", every plugin already loaded.") : ", loading " + load->plugins + " from scratch.");
								}
								if (playing(row.key)) about = "Playing now.\n" + about;
								about += "\n\nDrag a tone from the left onto it, or right-click.";
								Hint(about.c_str());
							}
							PushPopupPadding();
							if (ImGui::BeginPopupContextItem("##songMenu")) {
								if (!selected && ImGui::IsWindowAppearing()) { s.selectedSongs = { row.key }; s.songAnchor = row.key; }
								const std::vector<std::string> keys = s.selectedSongs;
								ImGui::TextDisabled("%s", keys.size() == 1 ? row.title.c_str() : (std::to_string(keys.size()) + " songs").c_str());
								ImGui::Separator();
								if (ImGui::BeginMenu("Give it a tone")) { ToneChoices([&](const std::string& t) { assignTo(keys, t); }); ImGui::EndMenu(); }
								bool anyTone = false;
								for (const std::string& key : keys) anyTone = anyTone || !toneOf(key).empty();
								if (ImGui::MenuItem("Take its tone away", nullptr, false, anyTone)) assignTo(keys, {});
								ImGui::EndPopup();
							}
							ImGui::PopStyleVar();

							const ImVec2 end(at.x + rowW, at.y + rowH);
							if (selected) rowsList->AddRectFilled(at, end, Ink::RowSelected, 5.0f);
							else if (hovered) rowsList->AddRectFilled(at, end, Ink::RowHover, 5.0f);
							const float textY = at.y + (rowH - ImGui::GetFontSize()) * 0.5f;
							const float captionY = at.y + (rowH - caption->FontSize) * 0.5f;
							float nameX = titleX;
							if (playing(row.key)) {
								rowsList->AddCircleFilled(ImVec2(at.x + titleX + 4.0f, at.y + rowH * 0.5f), 3.5f, Ink::Good, 12);
								nameX += 14.0f;
							}
							rowsList->PushClipRect(ImVec2(at.x + titleX, at.y), ImVec2(at.x + artistX - 8.0f, end.y), true);
							rowsList->AddText(ImVec2(at.x + nameX, textY), IM_COL32_WHITE, row.title.c_str());
							rowsList->PopClipRect();
							rowsList->PushClipRect(ImVec2(at.x + artistX, at.y), ImVec2(at.x + toneX - 8.0f, end.y), true);
							Text(rowsList, caption, ImVec2(at.x + artistX, captionY), Ink::Muted, row.artist.c_str());
							rowsList->PopClipRect();
							// Last load, short: an amber "14 s" when it waited for plugins (the tooltip says which).
							const std::string last = slow(load) ? std::to_string(static_cast<int>(load->seconds + 0.5f)) + " s" : std::string();
							const float lastW = last.empty() ? 0.0f : TextWidth(caption, last.c_str()) + 14.0f;
							if (!last.empty()) Text(rowsList, caption, ImVec2(end.x - lastW + 4.0f, captionY), Ink::Amber, last.c_str());
							if (!tone.empty()) {
								const std::string tag = BaseName(tone);
								const float tagW = std::min(TextWidth(caption, tag.c_str()) + 16.0f, end.x - lastW - 6.0f - (at.x + toneX));
								rowsList->AddRectFilled(ImVec2(at.x + toneX, at.y + 3.0f), ImVec2(at.x + toneX + tagW, end.y - 3.0f), Ink::Tag, 5.0f);
								rowsList->PushClipRect(ImVec2(at.x + toneX, at.y), ImVec2(at.x + toneX + tagW - 4.0f, end.y), true);
								Text(rowsList, caption, ImVec2(at.x + toneX + 8.0f, captionY), Ink::TagText, tag.c_str());
								rowsList->PopClipRect();
							}
							else Text(rowsList, caption, ImVec2(at.x + toneX + 8.0f, captionY), Ink::Dim, "-");   // no tone of its own
							ImGui::PopID();
						}
					}
					clipper.End();
					ImGui::EndChild();
					ImGui::PopStyleVar(2);
				}

				// Foot: actions while songs are selected, a short how-to otherwise.
				ImGui::Dummy(ImVec2(0, 4));
				if (!s.selectedSongs.empty()) {
					const std::vector<std::string> keys = s.selectedSongs;
					ImGui::PushFont(caption);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(keys.size() == 1 ? "1 song:" : (std::to_string(keys.size()) + " songs:").c_str());
					ImGui::PopStyleColor();
					ImGui::PopFont();
					ImGui::SameLine();
					if (Button("Give it a tone...", true)) ImGui::OpenPopup("##songTonePick");
					bool anyTone = false;
					for (const std::string& key : keys) anyTone = anyTone || !toneOf(key).empty();
					if (anyTone) {
						ImGui::SameLine();
						if (Button("Take its tone away")) assignTo(keys, {});
					}
					PushPopupPadding();
					if (ImGui::BeginPopup("##songTonePick")) { ToneChoices([&](const std::string& t) { assignTo(keys, t); }); ImGui::EndPopup(); }
					ImGui::PopStyleVar();
				}
				else {
					ImGui::PushFont(caption);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Dim));
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted("Select songs (Ctrl or Shift+click for several), then drag a tone from the left onto them.");
					ImGui::PopStyleColor();
					ImGui::PopFont();
				}
			}

			// Under the board: every plugin found, with search, filters, type and status.
			// Drawn inside BottomPane's fixed-height child, on the same first row as its Plugins | Songs toggle.
			void PluginBrowser(const VstHostLink::Snapshot& vst)
			{
				State& s = S();
				const std::vector<VstHostLink::Plugin>& plugins = VstHostLink::Plugins();
				const bool canAdd = !vst.busy && vst.slots.size() < static_cast<size_t>(VstHostControl::kMaxSlots);
				const bool canReplace = !vst.busy && s.selected >= 0 && s.selected < static_cast<int>(vst.slots.size());
				ImFont* caption = CaptionFont();
				const float left = ImGui::GetCursorStartPos().x;
				const float inner = ImGui::GetWindowContentRegionMax().x - left;

				// Title row (after the toggle): count and search.
				// Two summary chips: what was found and how much of it is in the chain, then how many plugins are
				// loaded outside the chain (the host's spares), so the speed of a tone change is not a mystery.
				const float chipH = ImGui::GetFrameHeight();
				auto summaryChip = [&](const char* id, const std::string& text, ImU32 hue, const char* hint) {
					ImGui::SameLine(0.0f, 10.0f);
					const PillLook look = { text.c_str(), Mix(Ink::App, hue, 0.12f), Mix(Ink::App, hue, 0.6f), Ink::Text };
					const ImVec2 at = ImGui::GetCursorScreenPos();
					ImGui::InvisibleButton(id, ImVec2(PillWidth(caption, look.text, true), chipH));
					Hint(hint);
					DrawPill(ImGui::GetWindowDrawList(), caption, at, chipH, look, hue);
				};
				const size_t folders = VstHostLink::ScanFolders().size() + 1;
				const std::string found = std::to_string(plugins.size()) + (plugins.size() == 1 ? " plugin" : " plugins") + " found, "
					+ std::to_string(vst.slots.size()) + " in chain";
				const std::string foundHint = "Found in " + std::to_string(folders) + (folders == 1 ? " folder" : " folders") + ". View > Add plugin folder for more; Rescan after installing.";
				summaryChip("##foundChip", found, Ink::Good, foundHint.c_str());
				// The plugins kept loaded outside the chain work behind the scenes (no count shown); only a background
				// load shows, since tone file actions wait for it.
				if (!vst.preloadingName.empty()) {
					const std::string loading = "Loading " + vst.preloadingName + " in the background...";
					summaryChip("##keptChip", loading, Ink::Blue, "Getting it ready so a tone that uses it switches at once. Saving or opening a tone waits until it has loaded.");
				}
				const float searchW = std::min(240.0f, inner * 0.4f);
				ImGui::SameLine(left + inner - searchW);
				ImGui::SetNextItemWidth(searchW);
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
				ImGui::InputTextWithHint("##pluginSearch", "Search plugins or vendors", s.search, sizeof(s.search));
				ImGui::PopStyleVar();
				ImGui::PopStyleColor();

				// Filter chips.
				static const char* const chips[FilterCount] = { "All", "Favourites", "Amps", "Effects", "Dynamics", "Instruments", "Recent" };
				ImGui::SetCursorPosX(left);
				for (int c = 0; c < FilterCount; ++c) {
					if (c > 0) {
						ImGui::SameLine(0.0f, 6.0f);
						if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(chips[c]).x + 20.0f > left + inner) { ImGui::NewLine(); ImGui::SetCursorPosX(left); }
					}
					if (Chip(chips[c], s.filter == c)) s.filter = c;
				}

				std::vector<const VstHostLink::Plugin*> rows;
				if (s.filter == Recent) {
					for (const std::wstring& path : VstHostLink::RecentPlugins())
						if (const VstHostLink::Plugin* plugin = FindPlugin(path))
							if (MatchesSearch(*plugin, s.search)) rows.push_back(plugin);
				}
				else {
					for (const VstHostLink::Plugin& plugin : plugins)
						if (MatchesFilter(plugin, s.filter) && MatchesSearch(plugin, s.search)) rows.push_back(&plugin);
				}

				// Table.
				// Status holds two pills ("In chain" + "Always loaded" is the widest pair), so it starts where they fit.
				const float chainW = ChainPillWidth(caption), memoryW = MemoryPillWidth(caption);
				const float statusW = chainW + 6.0f + memoryW + 16.0f;   // 16: clear of the list's scrollbar
				const float nameX = 28.0f, vendorX = inner * 0.38f, kindX = inner * 0.58f;
				const float statusX = std::max(kindX + 80.0f, inner - statusW);
				ImDrawList* list = ImGui::GetWindowDrawList();
				ImGui::SetCursorPosX(left);
				{
					const ImVec2 at = ImGui::GetCursorScreenPos();
					Text(list, caption, ImVec2(at.x + nameX, at.y), Ink::Dim, "Plugin");
					Text(list, caption, ImVec2(at.x + vendorX, at.y), Ink::Dim, "Vendor");
					Text(list, caption, ImVec2(at.x + kindX, at.y), Ink::Dim, "Type");
					Text(list, caption, ImVec2(at.x + statusX, at.y), Ink::Dim, "Status");
					ImGui::Dummy(ImVec2(inner, caption->FontSize + 2.0f));
					list->AddLine(ImVec2(at.x, at.y + caption->FontSize + 4.0f), ImVec2(at.x + inner, at.y + caption->FontSize + 4.0f), Ink::Edge);
				}
				const float rowH = ImGui::GetFontSize() + 10.0f;
				// The list fills the pane down to the foot (buttons, then the selected plugin's line and any warning),
				// so the pane keeps one height whatever is listed or selected; the list is what gives way.
				const VstHostLink::Plugin* picked = FindPlugin(s.selectedPlugin);
				static const char* const instrumentWarning = "This is an instrument: it makes its own sound from MIDI notes and does nothing with a guitar.";
				const std::string selectedLine = !picked ? std::string() : picked->name + (picked->vendor.empty() ? "" : ", " + picked->vendor)
					+ (picked->version.empty() ? "" : " " + picked->version) + ":  " + Utf8(picked->path);
				const float itemGap = ImGui::GetStyle().ItemSpacing.y;
				float footH = 2.0f + 2.0f * itemGap + ImGui::GetFontSize() + 14.0f;   // Dummy(2), Button's 7 + 7 padding
				if (picked) footH += itemGap + caption->CalcTextSizeA(caption->FontSize, FLT_MAX, inner, selectedLine.c_str()).y;
				if (picked && picked->kind == VstHostLink::Kind::Instrument)
					footH += itemGap + ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, inner, instrumentWarning).y;
				const float listH = std::max(rowH * 2.0f, ImGui::GetContentRegionAvail().y - footH);
				if (rows.empty()) {
					ImGui::SetCursorPosX(left);
					ImGui::BeginChild("##plugins", ImVec2(inner, listH), false);
					ImGui::PushFont(caption);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
					ImGui::PushTextWrapPos(0.0f);
					ImGui::TextUnformatted(plugins.empty() ? "No 64-bit VST3 plugins found. Install some, or View, Add plugin folder if they are somewhere else."
						: s.filter == Favourites ? "No favourites yet: click the star next to a plugin."
						: s.filter == Recent ? "Plugins you add show here."
						: "Nothing matches.");
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
					ImGui::PopFont();
					ImGui::EndChild();
				}
				else {
					ImGui::SetCursorPosX(left);
					ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
					ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
					ImGui::BeginChild("##plugins", ImVec2(inner, listH), false);
					ImDrawList* rowsList = ImGui::GetWindowDrawList();
					const float rowW = ImGui::GetContentRegionAvail().x;
					for (size_t r = 0; r < rows.size(); ++r) {
						const VstHostLink::Plugin& plugin = *rows[r];
						ImGui::PushID(static_cast<int>(r));
						const bool selected = _wcsicmp(s.selectedPlugin.c_str(), plugin.path.c_str()) == 0;
						const bool favourite = VstHostLink::IsFavourite(plugin.path);
						const ImVec2 at = ImGui::GetCursorScreenPos();
						const bool pressed = ImGui::InvisibleButton("##plugin", ImVec2(rowW, rowH));
						const bool hovered = ImGui::IsItemHovered();
						if (pressed) {
							if (ImGui::GetIO().MousePos.x < at.x + nameX - 4.0f) VstHostLink::SetFavourite(plugin.path, !favourite);
							else s.selectedPlugin = plugin.path;
						}
						if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && canAdd) VstHostLink::AddPlugin(plugin.path);
						Hint("Double-click to add it to the end of the chain, or drag it onto the board: between two pedals to put it there, onto a pedal to replace it. Right-click to keep it loaded, and more. The star marks a favourite.");
						if (plugin.kind != VstHostLink::Kind::Instrument && ImGui::BeginDragDropSource()) {
							PluginDrag drag{};
							wcsncpy_s(drag.path, plugin.path.c_str(), _TRUNCATE);
							ImGui::SetDragDropPayload("RAB_PLUGIN", &drag, sizeof(drag));
							ImGui::TextUnformatted(plugin.name.c_str());
							ImGui::EndDragDropSource();
						}
						PushPopupPadding();
						if (ImGui::BeginPopupContextItem("##pluginMenu")) {
							s.selectedPlugin = plugin.path;
							ImGui::TextDisabled("%s", plugin.name.c_str());
							ImGui::Separator();
							if (ImGui::MenuItem("Add to chain", "or double-click it", false, canAdd)) VstHostLink::AddPlugin(plugin.path);
							if (ImGui::MenuItem("Replace selected pedal", nullptr, false, canReplace)) VstHostLink::ReplaceSlot(s.selected, plugin.path);
							ImGui::Separator();
							// Fast tone changes: a plugin kept loaded outside the chain is in a tone in a moment.
							const bool inChain = InChain(vst, plugin.path), loaded = KeptLoaded(vst, plugin.path);
							const bool always = Listed(vst.alwaysLoaded, plugin.path);
							if (ImGui::MenuItem("Load in the background", nullptr, false, vst.hostRunning && !inChain && !loaded && !Loading(vst, plugin.path)))
								VstHostLink::LoadInBackground(plugin.path);
							Hint("Loads it now without adding it, and keeps it loaded until the game closes: tones that use it switch at once.");
							if (ImGui::MenuItem("Always keep loaded", nullptr, always)) VstHostLink::SetAlwaysLoaded(plugin.path, !always);
							Hint("Loads it in the background every time Tone designer starts (outside songs), so tones that use it always switch at once. Costs memory while loaded.");
							if (ImGui::MenuItem("Unload", nullptr, false, vst.hostRunning && loaded)) VstHostLink::Unload(plugin.path);
							Hint("Frees its memory. In the chain it stays; this only unloads the spare copy.");
							// A plugin whose background load failed has no copy to reload, so Reload is the retry: it loads it
							// again in the background (which clears the failed mark), the same as Load in the background.
							const bool failed = !inChain && !loaded && Listed(vst.preloadFailed, plugin.path);
							if (ImGui::MenuItem("Reload", nullptr, false, vst.hostRunning && !vst.busy && (inChain || loaded || failed) && !Loading(vst, plugin.path))) {
								if (failed) VstHostLink::LoadInBackground(plugin.path);
								else VstHostLink::Reload(plugin.path);
							}
							Hint("Closes the plugin completely and starts it again, keeping each pedal's settings: for when it needs a fresh start to see something new, such as tones you imported into it. The rest of the chain keeps playing while it loads. If it failed to load, this tries again.");
							ImGui::Separator();
							if (ImGui::MenuItem("Favourite", nullptr, favourite)) VstHostLink::SetFavourite(plugin.path, !favourite);
							if (ImGui::MenuItem("Show the file")) VstHostLink::ShowPluginFile(plugin.path);
							ImGui::EndPopup();
						}
						ImGui::PopStyleVar();

						const ImVec2 end(at.x + rowW, at.y + rowH);
						if (selected) rowsList->AddRectFilled(at, end, Ink::RowSelected, 5.0f);
						else if (hovered) rowsList->AddRectFilled(at, end, Ink::RowHover, 5.0f);
						const float textY = at.y + (rowH - ImGui::GetFontSize()) * 0.5f;
						const float captionY = at.y + (rowH - caption->FontSize) * 0.5f;
						DrawIcon(rowsList, ImVec2(at.x + 13.0f, at.y + rowH * 0.5f), favourite ? Glyph::StarFilled : Glyph::Star, favourite ? Ink::Amber : Ink::Jack);
						rowsList->PushClipRect(ImVec2(at.x + nameX, at.y), ImVec2(at.x + vendorX - 8.0f, end.y), true);
						rowsList->AddText(ImVec2(at.x + nameX, textY), IM_COL32_WHITE, plugin.name.c_str());
						rowsList->PopClipRect();
						rowsList->PushClipRect(ImVec2(at.x + vendorX, at.y), ImVec2(at.x + kindX - 8.0f, end.y), true);
						Text(rowsList, caption, ImVec2(at.x + vendorX, captionY), Ink::Muted, plugin.vendor.empty() ? "-" : plugin.vendor.c_str());
						rowsList->PopClipRect();
						Text(rowsList, caption, ImVec2(at.x + kindX, captionY), KindColor(plugin.kind), KindName(plugin.kind));
						const float pillH = rowH - 8.0f;
						float px = at.x + statusX;
						rowsList->PushClipRect(ImVec2(px, at.y), end, true);
						px += DrawPill(rowsList, caption, ImVec2(px, at.y + 4.0f), pillH, ChainPill(vst, plugin), 0, chainW) + 6.0f;
						PillLook memory{};
						if (MemoryPill(vst, plugin, memory)) DrawPill(rowsList, caption, ImVec2(px, at.y + 4.0f), pillH, memory, 0, memoryW);
						rowsList->PopClipRect();
						// "Load failed" on its own says nothing: hovering it says why, and how to try again.
						if (memory.text && strcmp(memory.text, "Load failed") == 0 && ImGui::IsWindowHovered()
							&& ImGui::IsMouseHoveringRect(ImVec2(px, at.y + 4.0f), ImVec2(px + memoryW, at.y + 4.0f + pillH)))
						{
							const std::string why = PreloadFailedWhy(vst, plugin.path);
							ImGui::BeginTooltip();
							ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
							ImGui::TextUnformatted(why.c_str());
							ImGui::TextDisabled("It is not loaded in the background again until you right-click it and pick Reload.");
							ImGui::PopTextWrapPos();
							ImGui::EndTooltip();
						}
						ImGui::PopID();
					}
					ImGui::EndChild();
					ImGui::PopStyleVar(2);
				}

				// The selected plugin, and what can be done with it.
				const VstHostLink::Plugin* selected = picked;
				ImGui::Dummy(ImVec2(0, 2));
				ImGui::SetCursorPosX(left);
				ImGui::BeginDisabled(!selected || !canAdd);
				if (Button("Add to chain", selected != nullptr && canAdd, "Adds it at the end of the chain.")) VstHostLink::AddPlugin(selected->path);
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::BeginDisabled(!selected || !canReplace);
				if (Button("Replace selected pedal", false, "Puts it in place of the pedal selected on the board.")) VstHostLink::ReplaceSlot(s.selected, selected->path);
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (Button("Rescan", false, "Looks for newly installed plugins, and tries plugins that failed to load again.")) VstHostLink::Rescan();
				ImGui::SameLine();
				if (Button("Plugin folders...", false, "Also look for plugins in another folder (opens behind the game, so alt-tab to see it)."))
					VstHostLink::AddScanFolder();
				if (selected) {
					ImGui::SetCursorPosX(left);
					ImGui::PushFont(caption);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Dim));
					ImGui::PushTextWrapPos(left + inner);
					ImGui::TextUnformatted(selectedLine.c_str());
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
					ImGui::PopFont();
					if (selected->kind == VstHostLink::Kind::Instrument) {
						ImGui::SetCursorPosX(left);
						WarnCaption(instrumentWarning);
					}
				}
			}

			void LevelsDialogBody(const VstHostLink::Snapshot& vst)
			{
				// Trims around the chain: the player's own, the same for every tone (a tone's levels are its author's job, set in
				// its plugins; loading one never moves these). Dragging sends the new level as it moves (the overlay coalesces them).
				static float inputDb = 0.0f, outputDb = 0.0f;
				static bool dragging = false;
				if (!dragging) { inputDb = vst.inputTenths / 10.0f; outputDb = vst.outputTenths / 10.0f; }
				bool inputDone = false, outputDone = false;
				const bool inputMoved = SliderRow("Into the chain", &inputDb, -24.0f, 24.0f, "%+.1f dB", &inputDone,
					"Level of your guitar going into the first plugin, the same for every tone. Amp sims expect a real guitar's level; raise it if tones sound thin or quiet.");
				const bool outputMoved = SliderRow("Out of the chain", &outputDb, -24.0f, 24.0f, "%+.1f dB", &outputDone,
					"Level of the whole chain going to the game, the same for every tone.");
				if (inputMoved || outputMoved) {
					dragging = true;
					VstHostLink::SetLevels(static_cast<int>(std::lround(inputDb * 10.0f)), static_cast<int>(std::lround(outputDb * 10.0f)));
				}
				if (inputDone || outputDone) dragging = false;
				Meter("Input", MeterFraction(vst.inputPeak), "After the input trim.");
				Meter("Output", MeterFraction(vst.outputPeak), "After the output trim, before the game's mix.");
				if (vst.outputPeak > 1.0f) WarnCaption("The chain's output is clipping. Lower the output trim or the last plugin's volume.");
			}

			std::string Trimmed(const char* text)
			{
				std::string out = text;
				while (!out.empty() && isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
				size_t first = 0;
				while (first < out.size() && isspace(static_cast<unsigned char>(out[first]))) ++first;
				return out.substr(first);
			}

			bool DangerButton(const char* label)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(IM_COL32(185, 28, 28, 255)));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(Color::Bad));
				const bool pressed = Button(label);
				ImGui::PopStyleColor(2);
				return pressed;
			}

			// Save as, Rename, Delete, Levels and the "changes are not saved" questions, as one modal at page level.
			void Dialogs(const VstHostLink::Snapshot& vst)
			{
				State& s = S();
				const char* id = "##toneDialog";
				if (s.openDialog) { ImGui::OpenPopup(id); s.openDialog = false; }
				ImGui::SetNextWindowSize(ImVec2(s.dialog == Dialog::Levels ? 520.0f : 440.0f, 0.0f), ImGuiCond_Always);
				// Opens over the middle of the overlay window (wherever it has been dragged), not the screen, and can then be dragged
				// by its background like any other window.
				const ImGuiWindow* overlay = ImGui::GetCurrentWindow()->RootWindow;
				ImGui::SetNextWindowPos(ImVec2(overlay->Pos.x + overlay->Size.x * 0.5f, overlay->Pos.y + overlay->Size.y * 0.5f),
					ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 18));
				if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
					auto heading = [](const std::string& text) {
						ImGui::PushFont(GetFonts().title ? GetFonts().title : ImGui::GetFont());
						ImGui::TextUnformatted(text.c_str());
						ImGui::PopFont();
					};
					bool close = false;
					const std::string current = vst.toneName.empty() ? "your chain" : "\"" + vst.toneName + "\"";
					switch (s.dialog) {
					case Dialog::SaveAs:
					case Dialog::Rename: {
						const bool rename = s.dialog == Dialog::Rename;
						heading(rename ? "Rename tone" : "Save tone as");
						Caption(rename ? "Songs that use it keep using it under its new name."
							: "Saves the chain with every plugin's settings. Folder/Name saves it into a folder (Bass tones/Fuzz).");
						if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
						ImGui::SetNextItemWidth(-1.0f);
						const bool enter = ImGui::InputTextWithHint("##name", "Tone name", s.name, sizeof(s.name), ImGuiInputTextFlags_EnterReturnsTrue);
						const std::string typed = Trimmed(s.name);
						// Renaming keeps the tone in its folder (Move to folder moves it).
						const std::string name = rename && !typed.empty() && !FolderOf(s.dialogTone).empty() ? FolderOf(s.dialogTone) + "/" + typed : typed;
						const std::vector<std::string>& tones = VstHostLink::Tones();
						const bool taken = _stricmp(name.c_str(), s.dialogTone.c_str()) != 0
							&& std::any_of(tones.begin(), tones.end(), [&](const std::string& t) { return _stricmp(t.c_str(), name.c_str()) == 0; });
						if (taken) WarnCaption(rename ? "A tone with that name already exists." : "A tone with that name already exists; saving replaces it.");
						const bool ok = !name.empty() && !(rename && taken) && !(rename && name == s.dialogTone);
						ImGui::BeginDisabled(!ok);
						if (Button(rename ? "Rename" : "Save", true) || (enter && ok)) {
							if (rename) VstHostLink::RenameTone(s.dialogTone, name);
							else VstHostLink::SaveTone(name);
							close = true;
						}
						ImGui::EndDisabled();
						ImGui::SameLine();
						if (Button("Cancel")) close = true;
						break;
					}
					case Dialog::Delete: {
						heading("Delete \"" + s.dialogTone + "\"?");
						const size_t songs = s.dialogSongs;
						std::string text = "The tone file is deleted.";
						if (songs > 0) text += " " + std::to_string(songs) + (songs == 1 ? " song uses it; it goes" : " songs use it; they go") + " back to your usual tone.";
						if (_stricmp(s.dialogTone.c_str(), vst.toneName.c_str()) == 0) text += " The chain you hear stays as it is, unsaved.";
						Caption(text.c_str());
						if (DangerButton("Delete")) { VstHostLink::DeleteTone(s.dialogTone); close = true; }
						ImGui::SameLine();
						if (Button("Cancel")) close = true;
						break;
					}
					case Dialog::NewTone:
					case Dialog::Discard: {
						const bool fresh = s.dialog == Dialog::NewTone;
						heading(fresh ? "Start a new tone?" : "Load \"" + s.dialogTone + "\"?");
						Caption(("Changes to " + current + " are not saved.").c_str());
						// The host is busy loading a plugin in the background: say the choice waits for it, rather
						// than the box closing with nothing seeming to happen.
						if (const std::string wait = WaitNote(vst); !wait.empty()) WarnCaption(wait.c_str());
						auto proceed = [&] { if (fresh) VstHostLink::ClearChain(); else VstHostLink::LoadTone(s.dialogTone); };
						if (!vst.toneName.empty()) {
							if (Button("Save, then continue", true)) { VstHostLink::SaveTone(vst.toneName); proceed(); close = true; }
							ImGui::SameLine();
						}
						if (Button("Don't save")) { proceed(); close = true; }
						ImGui::SameLine();
						if (Button("Cancel")) close = true;
						break;
					}
					case Dialog::NewFolder: {
						heading("New folder");
						Caption((s.dialogTones.empty() ? "Moves \"" + BaseName(s.dialogTone) + "\" into a new folder, for example Bass tones or Experimental."
							: "Moves the " + std::to_string(s.dialogTones.size()) + " selected tones into a new folder, for example Bass tones or Experimental.").c_str());
						if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
						ImGui::SetNextItemWidth(-1.0f);
						const bool enter = ImGui::InputTextWithHint("##folder", "Folder name", s.name, sizeof(s.name), ImGuiInputTextFlags_EnterReturnsTrue);
						std::string folder = Trimmed(s.name);
						folder.erase(std::remove(folder.begin(), folder.end(), '/'), folder.end());
						ImGui::BeginDisabled(folder.empty());
						if (Button("Move", true) || (enter && !folder.empty())) {
							if (s.dialogTones.empty()) MoveToFolder(s.dialogTone, folder);
							else for (const std::string& t : std::vector<std::string>(s.dialogTones)) MoveToFolder(t, folder);
							close = true;
						}
						ImGui::EndDisabled();
						ImGui::SameLine();
						if (Button("Cancel")) close = true;
						break;
					}
					case Dialog::DeleteTones: {
						heading("Delete " + std::to_string(s.dialogTones.size()) + " tones?");
						std::string names;
						for (size_t i = 0; i < s.dialogTones.size() && i < 6; ++i) names += (i ? ", " : "") + BaseName(s.dialogTones[i]);
						if (s.dialogTones.size() > 6) names += " and " + std::to_string(s.dialogTones.size() - 6) + " more";
						Caption((names + ". The tone files are deleted; songs that use them go back to your usual tone. A backup (File, Backups) can bring them back.").c_str());
						if (DangerButton(("Delete " + std::to_string(s.dialogTones.size()) + " tones").c_str())) {
							for (const std::string& t : s.dialogTones) VstHostLink::DeleteTone(t);
							s.selectedTones.clear();
							close = true;
						}
						ImGui::SameLine();
						if (Button("Cancel")) close = true;
						break;
					}
					case Dialog::Restore: {
						heading("Restore \"" + s.dialogTone + "\"?");
						Caption("Your tones, song tones, plugin settings and Tone designer settings are replaced by the ones in this backup. What you have now is backed up first, so this can be undone. Tone designer closes for a moment and starts again.");
						if (DangerButton("Restore")) { ToneBackup::Restore(s.dialogPath); close = true; }
						ImGui::SameLine();
						if (Button("Cancel")) close = true;
						break;
					}
					case Dialog::Levels:
						heading("Levels");
						LevelsDialogBody(vst);
						if (Button("Done", true)) close = true;
						break;
					default:
						close = true;
						break;
					}
					if (close) { s.dialog = Dialog::None; ImGui::CloseCurrentPopup(); }
					ImGui::EndPopup();
				}
				ImGui::PopStyleVar();
			}

			// The pane under the board has one fixed height, worked out from the fonts alone (title row, chips, column
			// heads, seven list rows, the foot), never from what either pane drew last: switching Plugins | Songs, picking
			// a plugin or filtering moves nothing, and the page and the overlay window keep their size. Inside, each
			// pane's list takes what its foot leaves (PluginBrowser, SongsView).
			float BottomPaneHeight()
			{
				const ImGuiStyle& style = ImGui::GetStyle();
				const float pad = 14.0f, gap = style.ItemSpacing.y;
				const float frame = ImGui::GetFrameHeight();
				const float rowH = ImGui::GetFontSize() + 10.0f;
				const float buttonH = ImGui::GetFontSize() + 14.0f;
				const float caption = CaptionFont()->FontSize;
				return std::floor(pad + frame + gap + frame + gap + caption + 2.0f + gap + 7.0f * rowH
					+ gap + 2.0f + gap + buttonH + gap + caption * 2.0f + pad);
			}

			// The bottom pane: one child of that fixed height with the same insets for both panes; the Plugins | Songs
			// toggle is drawn here, first on the title row, so it is at the same place whichever pane is showing.
			void BottomPane(const VstHostLink::Snapshot& vst, float width, float height)
			{
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 14));
				ImGui::BeginChild("##bottomPane", ImVec2(width, height), false,
					ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
				ImGui::PopStyleVar();
				PaneToggle();
				if (SongsShown()) SongsView(vst);
				else PluginBrowser(vst);
				ImGui::EndChild();
			}

			// The framed panel: bar, library + board, then the bottom pane. Backgrounds are drawn on the page's draw
			// list first; the parts are child windows on top.
			void Panel(const VstHostLink::Snapshot& vst, const ExternalAmp::Snapshot& amp)
			{
				const float width = ImGui::GetContentRegionAvail().x;
				const float barH = ImGui::GetFontSize() + 20.0f;
				// The board takes whatever height the page has left, never less than kMinBoardH. The bottom pane's height
				// is BottomPaneHeight (from the fonts) plus what the divider between them was dragged by (paneExtra,
				// remembered), so nothing here depends on the last frame. The divider trades height between the two:
				// the page's natural height uses the undragged pane, so dragging never grows or shrinks the window.
				constexpr float kMinBoardH = 160.0f;
				// The divider has its own strip between the board and the pane: over either of them, those child windows
				// would take the mouse and it could never be grabbed.
				constexpr float kDividerH = 8.0f;
				State& s = S();
				const float basePaneH = BottomPaneHeight();
				const float rowH = ImGui::GetFontSize() + 10.0f;
				const float minPaneH = basePaneH - 4.0f * rowH;   // three list rows left
				const float dividerH = IsOpen(Browser) ? kDividerH : 0.0f;
				const float maxPaneH = std::max(minPaneH, std::floor(ImGui::GetContentRegionAvail().y - barH - dividerH - 12.0f - kMinBoardH));
				const float paneH = IsOpen(Browser) ? std::clamp(std::floor(basePaneH + s.paneExtra), minPaneH, maxPaneH) : 0.0f;
				g_pageFillsHeight = true;
				// The height the page would like until the window is dragged: the board's own, or the tone list's when
				// that is taller, so a short library does not open as an empty column (the shell caps it to the screen).
				const float naturalMainH = std::max(300.0f, IsOpen(Library) ? s.libraryContentH : 0.0f);
				g_pageNaturalHeight = ImGui::GetCursorPosY() + barH + naturalMainH + (IsOpen(Browser) ? basePaneH : 0.0f) + dividerH + 12.0f + 20.0f;
				const float mainH = std::max(kMinBoardH, std::floor(ImGui::GetContentRegionAvail().y - barH - dividerH - paneH - 12.0f));
				const float libraryW = IsOpen(Library) ? std::min(240.0f, width * 0.32f) : 0.0f;
				const ImVec2 at = ImGui::GetCursorScreenPos();
				ImDrawList* list = ImGui::GetWindowDrawList();
				// Split the draw list so the panel's background (channel 0) goes under everything drawn after it (channel 1).
				list->ChannelsSplit(2);
				list->ChannelsSetCurrent(1);

				Bar(vst, amp, width, barH);
				ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + barH));
				if (libraryW > 0.0f) {
					LibraryColumn(vst, libraryW, mainH);
					ImGui::SameLine(0.0f, 0.0f);
				}
				Board(vst, width - libraryW, mainH);
				const float dividerTop = at.y + barH + mainH;
				const float browserTop = dividerTop + dividerH;
				ImGui::SetCursorScreenPos(ImVec2(at.x, browserTop));
				if (IsOpen(Browser)) BottomPane(vst, width, paneH);
				const float bottom = browserTop + paneH;
				// The divider: drag the line between the board and the pane under it; double-click puts it back.
				bool dividerHot = false;
				if (IsOpen(Browser)) {
					ImGui::SetCursorScreenPos(ImVec2(at.x + 12.0f, dividerTop));
					ImGui::InvisibleButton("##paneDivider", ImVec2(width - 24.0f, dividerH));
					dividerHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
					if (dividerHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
					if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0.0f)
						s.paneExtra = std::clamp(paneH - ImGui::GetIO().MouseDelta.y, minPaneH, maxPaneH) - basePaneH;
					if (ImGui::IsItemDeactivated()) VstHostLink::SetBottomPaneExtra(static_cast<int>(s.paneExtra));
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
						s.paneExtra = 0.0f;
						VstHostLink::SetBottomPaneExtra(0);
					}
				}

				list->ChannelsSetCurrent(0);
				const ImVec2 end(at.x + width, bottom);
				list->AddRectFilled(at, end, Ink::App, 12.0f);
				list->AddRectFilled(at, ImVec2(end.x, at.y + barH), Ink::Bar, 12.0f, ImDrawFlags_RoundCornersTop);
				list->AddLine(ImVec2(at.x, at.y + barH), ImVec2(end.x, at.y + barH), Ink::Edge);
				if (libraryW > 0.0f) {
					list->AddRectFilled(ImVec2(at.x, at.y + barH + 1.0f), ImVec2(at.x + libraryW, at.y + barH + mainH), Ink::Library,
						IsOpen(Browser) ? 0.0f : 12.0f, IsOpen(Browser) ? 0 : ImDrawFlags_RoundCornersBottomLeft);
					list->AddLine(ImVec2(at.x + libraryW, at.y + barH), ImVec2(at.x + libraryW, at.y + barH + mainH), Ink::Edge);
				}
				if (IsOpen(Browser)) {
					const float lineY = dividerTop + dividerH * 0.5f;
					list->AddLine(ImVec2(at.x, lineY), ImVec2(end.x, lineY), dividerHot ? Ink::Blue : Ink::Edge, dividerHot ? 2.0f : 1.0f);
					const float mid = at.x + width * 0.5f;   // a grip in the middle says it moves
					list->AddRectFilled(ImVec2(mid - 18.0f, lineY - 2.0f), ImVec2(mid + 18.0f, lineY + 2.0f), dividerHot ? Ink::Blue : Ink::FieldEdge, 2.0f);
				}
				list->AddRect(at, end, Ink::Edge, 12.0f);
				list->ChannelsMerge();
				ImGui::SetCursorScreenPos(ImVec2(at.x, bottom + 6.0f));
				ImGui::Dummy(ImVec2(0, 0));
			}
		}

		void ToneDesignerTab()
		{
			using namespace ToneDesigner;
			// Start reading the song library as soon as the page is shown, so the Songs pane is ready when it is
			// opened. Refresh() runs on its own thread and only starts once a session (later calls return at once).
			SongLibrary::Refresh();
			// A click, scroll or key on the page holds "Always keep loaded" background loads off (VstHostLink), so a
			// New tone or Add plugin is not left waiting behind one. Mouse movement alone does not count.
			const ImGuiIO& io = ImGui::GetIO();
			if (ImGui::IsAnyMouseDown() || io.MouseWheel != 0.0f || io.InputQueueCharacters.Size > 0) VstHostLink::NotePageInUse();
			const ExternalAmp::Snapshot amp = ExternalAmp::GetSnapshot();
			const VstHostLink::Snapshot vst = VstHostLink::GetSnapshot();

			if (!vst.hostInstalled) {
				BeginCard();
				WarnCaption("Tone designer's program (RocksmithAudioBridgeVstHost.exe) is not in the game folder. Run the Rocksmith Audio Bridge installer's Repair.");
				EndCard();
				return;
			}

			// The amp link carries one app at a time; say plainly when another app has it.
			const std::string guest = amp.link.guestName;
			const bool otherApp = amp.link.guestPresent && !guest.empty() && !VstHostLink::IsHostGuest(guest);
			const std::string otherAppText = guest + " (External app tab) is using the amp link, so Tone designer plays nothing. Close "
				+ guest + " to use Tone designer.";

			if (const std::string backup = ToneBackup::Status(); !backup.empty()) {
				BeginCard();
				Caption(backup.c_str());
				EndCard();
			}

			// A plugin crashed the host this session: say which, and that it was restarted (VstHostLink::FollowCrashes).
			if (!vst.crashNote.empty()) {
				BeginCard();
				WarnCaption(vst.crashNote.c_str());
				if (Button("OK")) VstHostLink::DismissCrashNote();
				EndCard();
			}

			if (vst.hostRecovering) {
				BeginCard();
				Caption("Restarting Tone designer with your saved chain...");
				EndCard();
				return;
			}
			if (vst.hostHung) {
				BeginCard();
				WarnCaption("Tone designer has stopped answering.");
				if (Button("Restart Tone designer", true, "Closes the verified stuck host and starts Tone designer again."))
					VstHostLink::RestartHost();
				EndCard();
				return;
			}
			if (vst.hostDisconnected) {
				BeginCard();
				WarnCaption("Tone designer is running but is not connected to the game.");
				if (Button("Restart Tone designer", true, "Closes the verified disconnected host and starts Tone designer again."))
					VstHostLink::RestartHost();
				EndCard();
				return;
			}
			if (vst.hostRunning && (vst.loading || vst.busy)) {
				BeginCard();
				WarnCaption("Tone designer is still loading.");
				if (Button("Restart Tone designer", true, "Stops the current load and starts Tone designer again."))
					VstHostLink::RestartHost();
				EndCard();
			}
			// Off: either closed, or in standby (the power button), where the host keeps its plugins loaded and goes on
			// loading the always-loaded ones, so turning it on is instant.
			if (!vst.hostRunning || vst.standby) {
				BeginCard();
				if (!vst.standby) Caption("Tone designer is off. Starting it brings back the chain you had last.");
				else if (vst.loading || !vst.preloadingName.empty())
					Caption(("Tone designer is off; the game's amp is playing. Getting " + (vst.loading ? std::string("your chain") : vst.preloadingName) + " ready in the background.").c_str());
				else Caption("Tone designer is off; the game's amp is playing. Your plugins are still loaded, so it turns on at once.");
				if (otherApp) WarnCaption(otherAppText.c_str());
				if (!vst.loadError.empty() && vst.standby) WarnCaption(vst.loadError.c_str());
				if (Button(vst.standby ? "Turn on" : "Start Tone designer", true, vst.standby ? "Your guitar plays through the chain again."
					: "Starts the VST host in its own 64-bit program; your guitar plays through it once it connects."))
					VstHostLink::StartHost();
				if (vst.standby) {
					ImGui::SameLine();
					if (Button("Close completely", false, "Closes the VST host and unloads every plugin, freeing their memory. Always-loaded plugins load again next time the game starts."))
						VstHostLink::StopHost();
				}
				bool autoStart = vst.autoStart;
				if (ToggleRow("Start with the game", "Starts Tone designer with your last chain when Rocksmith starts. Off, but with plugins marked Always keep loaded, it starts switched off and loads those in the background.", &autoStart))
					VstHostLink::SetAutoStart(autoStart);
				EndCard();
				return;
			}

			// What stops it being heard or seen, each said plainly.
			if (otherApp || vst.exclusiveFullscreen || !vst.loadError.empty()) {
				BeginCard();
				if (otherApp) WarnCaption(otherAppText.c_str());
				if (vst.exclusiveFullscreen)
					WarnCaption("Rocksmith runs in exclusive fullscreen, where no window can appear over the game, so plugin windows cannot show. Pick non-exclusive (borderless) fullscreen in RSMods' Rocksmith settings.");
				if (!vst.loadError.empty()) WarnCaption(vst.loadError.c_str());
				EndCard();
			}

			Panel(vst, amp);
			Dialogs(vst);
		}

		// ---- Amp sim ------------------------------------------------------------------------------------------
		// One page for the two ways to play through an amp sim over the amp link (AsioProxy/AmpLink.hpp), which
		// carries one at a time: Tone designer (the VST host, VstHostLink.hpp) and an external app on the Rocksmith
		// Audio Bridge ASIO driver (AmpliTube standalone, TONE3000, a DAW). On top, the link's own settings, the
		// same whichever plays (what is connected, latency, late buffers, the safety buffer, compare with the
		// game's amp); under it a tab for each. The page opens on the tab whose app holds the link.
		// Tone designer needs the height, so on its tab the title and tabs share one row and the Amp link card
		// folds into a one-line status bar (click it to open the card); External app keeps the card open.
		enum class AmpTab { ToneDesigner, ExternalApp };
		AmpTab g_ampTab = AmpTab::ToneDesigner;
		bool g_ampLinkOpen = false;   // Tone designer tab: the Amp link card shown in full (session only)

		// The cable's guitar-to-speaker figure in two parts: input (the cable read: packet wait, cable to PC) and output
		// (the rest). Rounded once and subtracted, so the two parts always add up to the total shown.
		struct CableSplit { int total, input, output; };
		CableSplit SplitCableLatency(const ExternalAmp::Snapshot& amp)
		{
			const int total = static_cast<int>(std::lround(amp.cableLatencyMs));
			const int read = (std::min)(total, static_cast<int>(std::lround(amp.cableReadMs)));
			return { total, read, total - read };
		}

		void AmpLinkCard(const ExternalAmp::Snapshot& amp)
		{
			const AmpLink::Status& link = amp.link;
			BeginCard("Amp link");
			if (!amp.proxyLoaded) {
				WarnCaption("Needs Rocksmith Audio Bridge ASIO as the game's audio driver. It is not loaded in this session.");
				EndCard();
				return;
			}
			const bool toneDesigner = link.guestPresent && VstHostLink::IsHostGuest(link.guestName);
			const std::string name = toneDesigner ? "Tone designer" : link.guestName[0] ? link.guestName : "an amp sim";
			if (amp.comparing) InfoRow("Playing", "The game's amp (comparing)", Color::Warn);
			else if (link.connected) InfoRow("Playing", name, Color::Good);
			else if (link.guestPresent) InfoRow("Playing", name + " (audio stopped)", Color::Warn);
			else InfoRow("Playing", "The game's amp (nothing connected)", Color::Muted);
			char text[128];
			// On the cable the link's own blocks are a small part of the delay (the output device and its buffer are the
			// rest), so the measured guitar-to-speaker figure is shown instead (CableAmpLink's measurement), split into
			// input, output and total (the DAW convention).
			if (link.frames > 0 && amp.cableHost && amp.cableLatencyMs > 0.0f) {
				const CableSplit split = SplitCableLatency(amp);
				sprintf_s(text, "48 kHz, %d samples, latency input %d ms, output %d ms, total %d ms", link.frames, split.input, split.output, split.total);
			}
			else if (link.frames > 0)
				sprintf_s(text, "48 kHz, %d samples, +%.1f ms", link.frames, link.frames * (link.latencyBlocks > 1 ? 2 : 1) * 1000.0 / AmpLink::kSampleRate);
			InfoRow("Format", link.frames > 0 ? text : "Waiting for the game's audio");
			sprintf_s(text, "%u this session", link.lateBlocks);
			InfoRow("Late buffers", text, link.lateBlocks > 0 ? Color::Warn : Color::Text);
			if (link.lateBlocks > 0)
				Caption("Late buffers are blocks the amp sim did not finish in time; each one is a click. Try the extra safety buffer, fewer or lighter plugins (or a lighter preset), or a bigger game buffer.");
			if (amp.pedalSemitones != 0) {
				sprintf_s(text, "%+d semitones, sent to the amp sim too", amp.pedalSemitones);
				InfoRow("Drop Pedal", text);
			}
			bool safety = amp.safetyBuffer;
			char safetyCaption[96];
			sprintf_s(safetyCaption, "Adds one more buffer (+%.1f ms) so a busy PC clicks less.", (link.frames > 0 ? link.frames : 128) * 1000.0 / AmpLink::kSampleRate);
			if (ToggleRow("Extra safety buffer", safetyCaption, &safety)) ExternalAmp::SetSafetyBuffer(safety);
			// Real Tone Cable players have no ASIO buffer to set, so the link's block size is picked here
			// (CableAmpLink). With an ASIO interface it is the interface's own buffer size and this row is not shown.
			if (amp.cableHost) {
				static const char* const blocks[] = { "Automatic", "64", "128", "256" };
				static const int frames[] = { 0, 64, 128, 256 };
				int index = 0;
				for (int i = 0; i < 4; ++i) if (frames[i] == amp.cableBlockSetting) index = i;
				if (SegmentedRow("Amp sim block", &index, blocks, 4, "How many samples the amp sim processes in one go; changes at once. Smaller is less delay but more work for the PC; heavy plugins may then show late buffers. Automatic is 64 when the cable is read directly over USB, 128 otherwise. Changing it restarts Tone designer's audio for a moment."))
					ExternalAmp::SetCableBlock(frames[index]);
				if (!amp.cableDirectUsb && amp.cableBlockSetting == 64)
					Caption("64 needs the cable read directly over USB; using 128. Windows hands over the cable's input 480 samples at a time.");
			}
			bool compare = amp.comparing;
			if (ToggleRow("Compare with the game's amp", "A/B: hear Rocksmith's own amp instead for now. Not saved.", &compare))
				ExternalAmp::SetCompare(compare);
			EndCard();
		}

		void ExternalAppTab(const ExternalAmp::Snapshot& amp)
		{
			const AmpLink::Status& link = amp.link;
			// While Tone designer holds the link, nothing here applies; this tab never controls Tone designer.
			if (link.guestPresent && VstHostLink::IsHostGuest(link.guestName)) {
				BeginCard();
				InfoRow("Amp link", "Busy: Tone designer is playing", Color::Warn);
				Caption("An external app can connect once Tone designer is closed (the power button on its tab).");
				if (Button("Go to Tone designer")) g_ampTab = AmpTab::ToneDesigner;
				EndCard();
				return;
			}

			BeginCard();
			bool enabled = amp.enabled;
			const char* state = !amp.enabled ? "Off: the game's amp plays as normal."
				: amp.gameAmpMuted ? "On: the game's amp is muted and you hear the external app."
				: amp.fallback ? "On: the game's amp plays until an app connects."
				: "On: the game's amp stays muted, waiting for an app.";
			if (ToggleRow("Use an external app", state, &enabled, "Saved. Note detection always uses your clean signal, whichever amp you hear."))
				ExternalAmp::SetEnabled(enabled);
			const std::string name = link.guestName[0] ? link.guestName : "an amp sim";
			if (link.connected) InfoRow("Connected app", name, Color::Good);
			else if (link.guestPresent) InfoRow("Connected app", name + " (audio stopped)", Color::Warn);
			else InfoRow("Connected app", "None", Color::Muted);
			if (link.guestPresent && link.guestFrames > 0 && link.frames > 0 && link.guestFrames != link.frames) {
				char frames[16];
				sprintf_s(frames, "%d", link.frames);
				WarnCaption((name + " is set to " + std::to_string(link.guestFrames) + " samples. It should switch to " + frames
					+ " by itself; if not, pick " + frames + " in its audio settings.").c_str());
			}
			if (!amp.guest64Registered)
				WarnCaption("64-bit amp sims (AmpliTube 5) cannot see the driver yet. In the Rocksmith Audio Bridge window, press Repair next to the driver (one admin prompt).");
			bool fallback = amp.fallback;
			if (ToggleRow("Fall back to the game's amp", "If the app disconnects, the game's amp comes back on.", &fallback))
				ExternalAmp::SetFallback(fallback);
			EndCard();

			BeginCard("Levels");
			Meter("Clean signal out", MeterFraction(link.inputPeak), "Your raw guitar as the app receives it, before the Input page's processing.");
			float returnDb = amp.returnTenths / 10.0f;
			bool returnSaved = false;
			if (SliderRow("App return", &returnDb, -30.0f, 6.0f, "%+.1f dB", &returnSaved, "Level of the external app in your mix (Tone designer has its own levels)."))
				ExternalAmp::SetReturnTenths(static_cast<int>(std::lround(returnDb * 10.0f)), false);
			if (returnSaved) ExternalAmp::SetReturnTenths(static_cast<int>(std::lround(returnDb * 10.0f)), true);
			Meter("Return level", MeterFraction(link.returnPeak), "The app's output after the return level.");
			EndCard();

			BeginCard("Setup");
			Caption("1. Open AmpliTube 5, or any ASIO amp sim.");
			Caption("2. In its audio settings, choose Rocksmith Audio Bridge ASIO. Input: Rocksmith guitar. Outputs: Rocksmith out L and R.");
			Caption("3. Keep Use an external app on. The game's amp mutes as soon as the app connects.");
			Caption("For VST3 plugins without an app of their own, use the Tone designer tab instead.");
			EndCard();
		}

		// The Amp link card folded to one line: what plays, the format and late buffers, and a chevron. The whole bar
		// opens and folds the card under it.
		void AmpLinkBar(const ExternalAmp::Snapshot& amp)
		{
			const AmpLink::Status& link = amp.link;
			const float width = ImGui::GetContentRegionAvail().x;
			const float height = ImGui::GetFrameHeight() + 6.0f;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			if (ImGui::InvisibleButton("##ampLinkBar", ImVec2(width, height))) g_ampLinkOpen = !g_ampLinkOpen;
			const bool hovered = ImGui::IsItemHovered();
			Hint(g_ampLinkOpen ? "Fold the amp link settings back into this line." : "The amp link's settings: extra safety buffer, compare with the game's amp, and what the numbers mean.");

			std::string summary;
			ImU32 dot = Color::Muted;
			if (!amp.proxyLoaded) { summary = "Rocksmith Audio Bridge ASIO is not loaded"; dot = Color::Warn; }
			else {
				const bool toneDesigner = link.guestPresent && VstHostLink::IsHostGuest(link.guestName);
				const std::string name = toneDesigner ? "Tone designer" : link.guestName[0] ? link.guestName : "an amp sim";
				if (amp.comparing) { summary = "The game's amp (comparing)"; dot = Color::Warn; }
				else if (link.connected) { summary = name; dot = Color::Good; }
				else if (link.guestPresent) { summary = name + " (audio stopped)"; dot = Color::Warn; }
				else summary = "The game's amp (nothing connected)";
				char text[128];
				if (link.frames > 0) {
					if (amp.cableHost && amp.cableLatencyMs > 0.0f) {
						const CableSplit split = SplitCableLatency(amp);
						sprintf_s(text, "   48 kHz, %d samples, in %d + out %d = %d ms", link.frames, split.input, split.output, split.total);
					}
					else sprintf_s(text, "   48 kHz, %d samples, +%.1f ms", link.frames, link.frames * (link.latencyBlocks > 1 ? 2 : 1) * 1000.0 / AmpLink::kSampleRate);
					summary += text;
				}
				if (amp.safetyBuffer) summary += ", safety buffer on";
			}

			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 end(at.x + width, at.y + height);
			list->AddRectFilled(at, end, hovered ? Color::Hover : Color::Card, 6.0f);
			list->AddRect(at, end, Color::CardEdge, 6.0f);
			ImFont* caption = GetFonts().caption ? GetFonts().caption : ImGui::GetFont();
			const float textY = at.y + (height - caption->FontSize) * 0.5f;
			float x = at.x + 12.0f;
			list->AddText(caption, caption->FontSize, ImVec2(x, textY), Color::Faint, "AMP LINK");
			x += caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, "AMP LINK").x + 14.0f;
			list->AddCircleFilled(ImVec2(x + 4.0f, at.y + height * 0.5f), 4.0f, dot, 12);
			x += 16.0f;

			// Right: late buffers (amber once there are any), then the chevron.
			float right = end.x - 12.0f;
			const float chevron = 4.5f, cy = at.y + height * 0.5f;
			const float cx = right - chevron;
			if (g_ampLinkOpen) list->AddTriangleFilled(ImVec2(cx - chevron, cy + chevron * 0.5f), ImVec2(cx + chevron, cy + chevron * 0.5f), ImVec2(cx, cy - chevron * 0.5f), Color::Muted);
			else list->AddTriangleFilled(ImVec2(cx - chevron, cy - chevron * 0.5f), ImVec2(cx + chevron, cy - chevron * 0.5f), ImVec2(cx, cy + chevron * 0.5f), Color::Muted);
			right -= 2.0f * chevron + 12.0f;
			if (amp.proxyLoaded) {
				char late[48];
				sprintf_s(late, link.lateBlocks == 1 ? "1 late buffer" : "%u late buffers", link.lateBlocks);
				const float lateW = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, late).x;
				right -= lateW;
				list->AddText(caption, caption->FontSize, ImVec2(right, textY), link.lateBlocks > 0 ? Color::Warn : Color::Muted, late);
				right -= 16.0f;
			}
			list->PushClipRect(ImVec2(x, at.y), ImVec2(std::max(x, right), end.y), true);
			list->AddText(caption, caption->FontSize, ImVec2(x, textY), Color::Text, summary.c_str());
			list->PopClipRect();
			ImGui::SetCursorScreenPos(ImVec2(at.x, end.y));
			ImGui::Dummy(ImVec2(0, 4));
		}

		// Two tabs as a segmented strip, `width` wide at the cursor; returns its height.
		float AmpTabs(float width)
		{
			static const char* const labels[] = { "Tone designer", "External app" };
			const float height = ImGui::GetFrameHeight() + 4.0f;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->AddRectFilled(at, ImVec2(at.x + width, at.y + height), Color::Card, 6.0f);
			list->AddRect(at, ImVec2(at.x + width, at.y + height), Color::CardEdge, 6.0f);
			for (int i = 0; i < 2; ++i) {
				const ImVec2 tab(at.x + width * 0.5f * i, at.y);
				ImGui::SetCursorScreenPos(tab);
				ImGui::PushID(i);
				if (ImGui::InvisibleButton("##ampTab", ImVec2(width * 0.5f, height))) g_ampTab = static_cast<AmpTab>(i);
				const bool hovered = ImGui::IsItemHovered();
				ImGui::PopID();
				const bool on = g_ampTab == static_cast<AmpTab>(i);
				if (on) list->AddRectFilled(ImVec2(tab.x + 3.0f, tab.y + 3.0f), ImVec2(tab.x + width * 0.5f - 3.0f, tab.y + height - 3.0f), Color::Accent, 5.0f);
				else if (hovered) list->AddRectFilled(ImVec2(tab.x + 3.0f, tab.y + 3.0f), ImVec2(tab.x + width * 0.5f - 3.0f, tab.y + height - 3.0f), Color::Hover, 5.0f);
				const ImVec2 size = ImGui::CalcTextSize(labels[i]);
				list->AddText(ImVec2(tab.x + (width * 0.5f - size.x) * 0.5f, tab.y + (height - size.y) * 0.5f), on ? Color::Text : Color::Muted, labels[i]);
			}
			return height;
		}

		void AmpSimPage()
		{
			const ExternalAmp::Snapshot amp = ExternalAmp::GetSnapshot();
			// The first time the page opens this session, the tab of whatever holds the link.
			static bool chosen = false;
			if (!chosen && amp.proxyLoaded) {
				chosen = true;
				if (amp.link.guestPresent && !VstHostLink::IsHostGuest(amp.link.guestName)) g_ampTab = AmpTab::ExternalApp;
			}

			// Title and a one-line description on the left, the tabs on the right of the same row.
			const ImVec2 at = ImGui::GetCursorScreenPos();
			const float width = ImGui::GetContentRegionAvail().x;
			const float tabsW = std::min(360.0f, std::max(220.0f, width * 0.32f));
			const float tabsH = ImGui::GetFrameHeight() + 4.0f;   // AmpTabs' height
			ImGui::BeginGroup();
			ImGui::PushFont(GetFonts().title ? GetFonts().title : ImGui::GetFont());
			ImGui::TextUnformatted("Amp sim");
			ImGui::PopFont();
			ImGui::PushFont(GetFonts().caption ? GetFonts().caption : ImGui::GetFont());
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Muted));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(120.0f, width - tabsW - 24.0f));
			ImGui::TextUnformatted("Play through an amp sim instead of the game's amp: VST3 plugins in Tone designer, or an app on Rocksmith Audio Bridge ASIO.");
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::PopFont();
			ImGui::EndGroup();
			const float headH = ImGui::GetItemRectSize().y;
			const float rowH = std::max(headH, tabsH);
			ImGui::SetCursorScreenPos(ImVec2(at.x + width - tabsW, at.y + (rowH - tabsH) * 0.5f));
			AmpTabs(tabsW);
			ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + rowH + 8.0f));
			ImGui::Dummy(ImVec2(0, 0));

			if (g_ampTab == AmpTab::ToneDesigner) AmpLinkBar(amp);
			if (g_ampTab == AmpTab::ExternalApp || g_ampLinkOpen) AmpLinkCard(amp);
			if (g_ampTab == AmpTab::ToneDesigner) ToneDesignerTab();
			else ExternalAppTab(amp);
		}

		// ---- Output device switching ---------------------------------------------------------------------
		// The device list is enumerated on a worker thread (COM + property reads take a few ms), refreshed when
		// the page opens and every few seconds while it stays open. Switching is session only: nothing is saved,
		// so the next launch starts on RS_ASIO.ini's device (ASIO mode) or the Windows default (Cable mode).

		struct DeviceCache
		{
			std::mutex mutex;
			Audio::OutputDevices::Snapshot snapshot;
			bool busy = false;
			ULONGLONG refreshed = 0;
		};

		DeviceCache& Devices()
		{
			static DeviceCache cache;
			return cache;
		}

		void RefreshDevices()
		{
			DeviceCache& cache = Devices();
			{
				std::lock_guard<std::mutex> lock(cache.mutex);
				static ULONGLONG busySince = 0;
				static bool stuckLogged = false;
				if (cache.busy) {
					// A hung scan would freeze the list on an old snapshot; log it once.
					if (!stuckLogged && GetTickCount64() - busySince > 5000) { stuckLogged = true; LOG_WARNING("(OUTPUT DEVICES) device scan has not finished in 5 s; the Output list is stale" << std::endl); }
					return;
				}
				cache.busy = true;
				busySince = GetTickCount64();
			}
			std::thread([] {
				Audio::OutputDevices::Snapshot snapshot = Audio::OutputDevices::Enumerate();
				DeviceCache& cache = Devices();
				std::lock_guard<std::mutex> lock(cache.mutex);
				cache.snapshot = std::move(snapshot);
				cache.busy = false;
				cache.refreshed = GetTickCount64();
			}).detach();
		}

		// A switch that is checked for a few seconds and undone if the new device does not play.
		// ---- Interface switching ----------------------------------------------------------------------------------
		// Which ASIO interface the bridge opens, switched live: RS_ASIO.ini keeps naming the bridge, so the game never
		// notices. The switch parks the game on the bridge's virtual clock, releases the old driver and opens the new
		// one (engine op 25, OutputTap::TryRebindProxyOutput / TryPromoteProxyOutput). A driver that will not open
		// (busy in another app, a buffer size it refuses) costs nothing: the previous interface is opened again and
		// the reason shown. Only a switch that worked is saved (HKCU Target), so the next launch starts there.

		// Every ASIO driver installed in one registry view, except the bridge itself.
		std::vector<std::wstring> AsioDriversIn(REGSAM view)
		{
			std::vector<std::wstring> names;
			HKEY asio{};
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | view, &asio) != ERROR_SUCCESS) return names;
			wchar_t name[256];
			for (DWORD index = 0;; ++index) {
				DWORD length = 256;
				if (RegEnumKeyExW(asio, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
				if (_wcsicmp(name, L"Rocksmith Audio Bridge ASIO") != 0 && _wcsicmp(name, L"Rocksmith Audio Bridge") != 0) names.push_back(name);
			}
			RegCloseKey(asio);
			return names;
		}

		// The ASIO drivers the bridge can open: every 32-bit one (what RS_ASIO itself could open) plus, with `with64`, the
		// ones registered only as 64-bit, which the bridge runs in its 64-bit driver host (AsioProxy/Remote64.hpp).
		// Cable mode's sound-only output (AsioOutputDevice) loads drivers inside the game, so it asks without them.
		// g_host64Drivers (lower case): the drivers the bridge would run in the 64-bit host, by the same rule as its
		// resolver (AsioProxyDriver ResolveAsioDriver): 64-bit only. Every other one is loaded in the game as its 32-bit
		// build.
		std::set<std::wstring> g_host64Drivers;
		std::vector<std::wstring> InstalledAsioDrivers(bool with64 = true)
		{
			static std::vector<std::wstring> cached32, cachedAll;
			static ULONGLONG readAt = 0;
			if (readAt == 0 || GetTickCount64() - readAt >= 3000) {
				readAt = GetTickCount64();
				cached32 = AsioDriversIn(KEY_WOW64_32KEY);
				cachedAll = cached32;
				g_host64Drivers.clear();
				for (const std::wstring& name : AsioDriversIn(KEY_WOW64_64KEY)) {
					const bool has32 = std::any_of(cached32.begin(), cached32.end(), [&](const std::wstring& n) { return _wcsicmp(n.c_str(), name.c_str()) == 0; });
					if (!has32) cachedAll.push_back(name);
					if (has32) continue;
					std::wstring lower = name;
					std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
					g_host64Drivers.insert(lower);
				}
				auto byName = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; };
				std::sort(cached32.begin(), cached32.end(), byName);
				std::sort(cachedAll.begin(), cachedAll.end(), byName);
			}
			return with64 ? cachedAll : cached32;
		}

		// True when the bridge runs this driver's 64-bit build in its driver host; false: its 32-bit build in the game.
		bool RunsIn64BitHost(const std::wstring& name)
		{
			std::wstring lower = name;
			std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
			return g_host64Drivers.count(lower) != 0;
		}

		void SaveInterfaceTarget(const std::wstring& driver)
		{
			HKEY key{};
			if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_32KEY, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
			RegSetValueExW(key, L"Target", 0, REG_SZ, reinterpret_cast<const BYTE*>(driver.c_str()), static_cast<DWORD>((driver.size() + 1) * sizeof(wchar_t)));
			const DWORD one = 1;
			RegSetValueExW(key, L"PreferReal", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
			RegCloseKey(key);
		}

		// 0 idle, 1 switching, 2 switched, 3 refused and back on the previous one, 4 refused and nothing open.
		std::atomic<int> g_interfaceState{ 0 };
		std::mutex g_interfaceMessageMutex;
		std::string g_outputSwitchMessage;   // SwitchInterface, shown on Switch output
		std::string g_inputSwitchMessage;    // SwitchInputInterface, shown on Guitar input

		void SwitchInterface(const std::wstring& driver, const std::wstring& previous)
		{
			g_interfaceState.store(1);
			std::thread([driver, previous] {
				int state = 2;
				std::string message;
				if (SUCCEEDED(BridgeControl(25, driver.c_str()).result)) {
					SaveInterfaceTarget(driver);
					message = "Now on " + Utf8(driver) + ". It is remembered for the next launch.";
				}
				else {
					// The proxy's own reason (busy, refused the buffer size...), read before the fallback clears it.
					const std::wstring why = Audio::OutputTap::ProxyBindError();
					const bool back = !previous.empty() && SUCCEEDED(BridgeControl(25, previous.c_str()).result);
					state = back ? 3 : 4;
					message = (why.empty() ? Utf8(driver) + " would not open." : Utf8(why))
						+ (back ? " Back on " + Utf8(previous) + "." : " Pick an interface to try again.");
				}
				LOG_INFO("(AUDIO BRIDGE) Interface switch to " << Utf8(driver) << ": " << message << std::endl);
				{
					std::lock_guard<std::mutex> lock(g_interfaceMessageMutex);
					g_outputSwitchMessage = message;
				}
				g_interfaceState.store(state);
			}).detach();
		}

		struct PendingSwitch
		{
			bool active = false;
			bool route = false;            // op 21 route (proxy) vs op 4 session switch
			bool promote = false;          // op 25: bind the ASIO interface itself (see PromoteToAsio)
			std::wstring target, previous; // previous: session device to go back to (op 4 only)
			std::string name;
			ULONGLONG started = 0;
		};

		std::string g_outputMessage;
		bool g_outputMessageIsError = false;

		std::wstring g_outputMessageEndpoint;   // the output a "Now playing through" message is about (empty: any other message)
		void OutputSay(const std::string& text, bool error) { g_outputMessage = text; g_outputMessageIsError = error; g_outputMessageEndpoint.clear(); }

		// Picking the ASIO entry while the proxy runs on its virtual clock (the game booted with the interface
		// unplugged, so input is the Real Tone Cable and the mix is routed to a Windows device). Stopping the
		// route (op 22) alone is not enough: the host re-creates its managed route on the next refresh and the
		// proxy never touches the interface, so input stays on the cable. Op 25 binds the interface for input
		// AND output in one step; then op 22 drops any route and forgets the remembered output, and
		// PreferReal=1 makes the next launch start on the interface. Runs off the render thread because loading
		// and initialising a real ASIO driver can take a second. 0 idle, 1 running, 2 bound, 3 failed.
		std::atomic<int> g_promoteState{ 0 };

		void PromoteToAsio(const std::wstring& driver)
		{
			g_promoteState.store(1);
			std::thread([driver] {
				const bool ok = SUCCEEDED(BridgeControl(25, driver.c_str()).result);
				if (ok) {
					BridgeControl(22);
					HKEY key{};
					if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_32KEY, nullptr, &key, nullptr) == ERROR_SUCCESS) {
						const DWORD one = 1;
						RegSetValueExW(key, L"PreferReal", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
						RegCloseKey(key);
					}
				}
				g_promoteState.store(ok ? 2 : 3);
			}).detach();
		}

		void VerifySwitch(PendingSwitch& pending, const LiveState& live, const std::string& asioName)
		{
			if (!pending.active) return;
			const ULONGLONG elapsed = GetTickCount64() - pending.started;
			if (pending.promote) {
				const int state = g_promoteState.load();
				if (state == 1) return;
				if (state == 2) OutputSay("Now playing through " + asioName + ". Guitar input moved to it too.", false);
				else OutputSay("Could not open " + asioName + ". Check it is plugged in and not used by another program. Sound stays on a Windows device and input on the cable.", true);
				g_promoteState.store(0);
				pending.active = false;
				return;
			}
			if (pending.route) {
				const HRESULT health = Audio::SharedOutput::RouteHealth();
				if (health == S_OK) { OutputSay("Now playing through " + pending.name + ". Lasts until you close Rocksmith.", false); pending.active = false; return; }
				if (health == E_PENDING && elapsed < 5000) return;
				// Could not open, lost, or never started: hand the mix straight back to the ASIO device.
				BridgeControl(22);
				OutputSay("Could not play through " + pending.name + ". Back on " + asioName + ".", true);
				pending.active = false;
				return;
			}
			if (std::wstring(live.status.endpoint) == pending.target) { OutputSay("Now playing through " + pending.name + ". Lasts until you close Rocksmith.", false); g_outputMessageEndpoint = pending.target; pending.active = false; return; }
			if (elapsed < 4000) return;
			if (!pending.previous.empty()) BridgeControl(4, pending.previous.c_str());
			if (Audio::SharedOutput::ExclusiveOutputDevice::IsExclusiveEndpoint(pending.target))
				OutputSay("Could not open " + pending.name + ": another program may be using it in exclusive mode, or Windows does not allow exclusive mode on it (Sound settings, the device's Properties, Advanced). Switched back.", true);
			else if (Audio::SharedOutput::AsioOutputDevice::IsAsioEndpoint(pending.target))
				OutputSay("Could not open " + pending.name + "'s ASIO driver: close other programs using it (a DAW, its control app), and check it can run at 48 kHz. Switched back.", true);
			else OutputSay("Could not play through " + pending.name + ". Switched back.", true);
			pending.active = false;
		}

		void OutputDeviceCard(const LiveState& live)
		{
			static PendingSwitch pending;
			static ULONGLONG lastVisible = 0;
			const ULONGLONG now = GetTickCount64();
			DeviceCache& cache = Devices();
			Audio::OutputDevices::Snapshot snapshot;
			ULONGLONG refreshed = 0;
			{
				std::lock_guard<std::mutex> lock(cache.mutex);
				snapshot = cache.snapshot;
				refreshed = cache.refreshed;
			}
			// Refresh on (re)opening the page, then every 4 s while it is shown.
			if (now - lastVisible > 500 || now - refreshed > 4000) RefreshDevices();
			lastVisible = now;

			const std::wstring endpoint = live.status.endpoint;
			const bool proxy = Audio::OutputTap::ProxyAvailable();
			const bool routing = endpoint.rfind(L"(route", 0) == 0;
			const bool passthrough = endpoint == L"(passthrough)" || endpoint == L"(silent)";
			const bool switching = endpoint == L"(downgrading)";
			const bool session = !endpoint.empty() && endpoint[0] != L'(';
			const unsigned outputMode = OutputStatusNumber(live, L"outputMode=");
			std::wstring routedId;
			if (routing) { const size_t close = endpoint.find(L')'); if (close != std::wstring::npos) routedId = endpoint.substr(close + 1); }
			const std::wstring asioDriver = ReadPassthroughOutputName();
			std::string asioName = Utf8(asioDriver);
			if (asioName.empty()) asioName = "your ASIO device";

			VerifySwitch(pending, live, asioName);

			// Where the game is playing now, in words.
			std::string current = "Waiting for the audio engine";
			const char* state = "Unknown";
			ImU32 stateColor = Color::Muted;
			if (routing || session) {
				const std::wstring id = routing ? routedId : endpoint;
				const bool exclusiveId = Audio::SharedOutput::ExclusiveOutputDevice::IsExclusiveEndpoint(id);
				const std::wstring windowsId = Audio::SharedOutput::ExclusiveOutputDevice::WindowsId(id);
				current = "A Windows device";
				for (const auto& device : snapshot.devices) if (device.id == windowsId) current = device.name;
				state = routing ? "Switched" : exclusiveId ? "Exclusive" : "Windows";
				stateColor = Color::AccentHi;
				if (Audio::SharedOutput::AsioOutputDevice::IsAsioEndpoint(id)) {
					current = Utf8(Audio::SharedOutput::AsioOutputDevice::DriverName(id));
					state = "ASIO";
					stateColor = Color::Good;
				}
				if (endpoint.rfind(L"(route-stalled)", 0) == 0) { state = "Stalled"; stateColor = Color::Warn; }
			}
			else if (endpoint == L"(passthrough)") { current = asioName; state = proxy ? "ASIO" : "Direct"; stateColor = Color::Good; }
			else if (endpoint == L"(silent)") { current = "No output"; state = "Silent"; stateColor = Color::Bad; }
			else if (switching) { current = "Switching"; state = "Switching"; stateColor = Color::Warn; }
			else if (endpoint == L"(starting)") { current = "Waiting for audio to start"; state = "Starting"; }
			else if (endpoint == L"(failed)") { current = "Audio could not start"; state = "Failed"; stateColor = Color::Bad; }
			if (session)
			{
				switch (outputMode)
				{
				case 1: state = "Shared"; break;
				case 2: state = "Exclusive (event-driven)"; break;
				case 3: state = "Exclusive (polling)"; stateColor = Color::Warn; break;
				case 4: state = "ASIO"; break;
				default: state = "Opening"; stateColor = Color::Warn; break;
				}
			}

			// The real ASIO device refused to open this launch, so the proxy runs on its virtual clock and the
			// guitar is silent. Without this the card reads "ASIO" in green and the player only notices "no input".
			// The proxy clears the error once the device binds, so it is only shown while it still applies.
			const std::wstring bindError = proxy ? Audio::OutputTap::ProxyBindError() : std::wstring();
			static std::wstring loggedBindError;
			if (!bindError.empty() && bindError != loggedBindError) {
				loggedBindError = bindError;
				LOG_ERROR("(AUDIO BRIDGE) " << Utf8(bindError) << std::endl);
			}
			if (!bindError.empty() && !routing && !session && !switching) { current = asioName; state = "Not open"; stateColor = Color::Bad; }

			BeginCard("Playing to");
			ImGui::PushFont(GetFonts().title);
			ImGui::TextUnformatted(current.c_str());
			ImGui::PopFont();
			StatusPill(state, stateColor);
			if (session && outputMode != 0)
			{
				const unsigned deviceFrames = OutputStatusNumber(live, L"outputBufferFrames=");
				const unsigned periodFrames = OutputStatusNumber(live, L"outputPeriodFrames=");
				char bufferText[64];
				if (deviceFrames)
				{
					sprintf_s(bufferText, "%u samples (%.1f ms)", deviceFrames, deviceFrames / 48.0);
					InfoRow("Device buffer", bufferText);
				}
				if (periodFrames && periodFrames != deviceFrames)
				{
					sprintf_s(bufferText, "%u samples (%.1f ms)", periodFrames, periodFrames / 48.0);
					InfoRow("Output period", bufferText);
				}
			}
			// A setup that crashed the game is not reopened at boot (the driver's last-known-good guard); say what happened.
			static std::string safeBootNotice;
			static uint64_t safeBootPolled = 0;
			if (proxy && GetTickCount64() - safeBootPolled > 1000) { safeBootPolled = GetTickCount64(); safeBootNotice = Audio::OutputTap::ProxySafeBootNotice(); }
			if (!safeBootNotice.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
				ImGui::TextUnformatted(safeBootNotice.c_str());
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}
			if (!bindError.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Bad));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
				// No restart needed: picking it again in Switch output binds it live (PromoteToAsio / SwitchInterface).
				ImGui::TextUnformatted((Utf8(bindError) + " Plug it in, or close other apps using it (its control app, a DAW, OBS), then pick it in Switch output below. No restart needed.").c_str());
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}
			Meter("Left", MeterFraction(live.status.outputPeak[0]), "The game's left-channel output level, after the volume cap.");
			Meter("Right", MeterFraction(live.status.outputPeak[1]), "The game's right-channel output level, after the volume cap.");
			// The device's own Windows volume, here so nobody has to leave the game to find it (a player's speakers sat at 0;
			// in exclusive mode Windows will not even switch to the device). DeviceVolume reads and sets it off this thread.
			if (session && !Audio::SharedOutput::AsioOutputDevice::IsAsioEndpoint(endpoint)) {
				Audio::DeviceVolume::Watch(endpoint, Audio::SharedOutput::ExclusiveOutputDevice::IsExclusiveEndpoint(endpoint));
				const Audio::DeviceVolume::State volume = Audio::DeviceVolume::Get();
				if (volume.known) {
					static float percent = 100.0f;
					static bool dragging = false;
					if (!dragging) percent = volume.level * 100.0f;
					bool committed = false;
					const bool moved = SliderRow("Device volume", &percent, 0.0f, 100.0f, "%.0f%%", &committed,
						"This device's volume in Windows, the same as its slider in Windows' volume settings.");
					dragging = moved && !committed;
					if (moved) Audio::DeviceVolume::Set(percent / 100.0f);
					if (volume.muted || volume.level < 0.01f) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
						ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
						ImGui::TextUnformatted(volume.muted ? "Windows has this device muted, so nothing is heard." : "This device's Windows volume is at 0, so nothing is heard.");
						ImGui::PopTextWrapPos();
						ImGui::PopStyleColor();
						if (volume.muted && ImGui::Button("Unmute")) Audio::DeviceVolume::SetMute(false);
					}
				}
			}
			EndCard();

			BeginCard("Switch output");
			const bool canRoute = proxy && (passthrough || routing);
			if (!canRoute && !session) {
				Caption(endpoint == L"(failed)" ? "Audio initialization failed. Close Rocksmith and include a bug report when reporting this error."
					: endpoint == L"(starting)" ? "Waiting for Rocksmith to open its audio output."
					: switching ? "The audio engine is switching devices. Try again in a moment."
					: "Switching needs the Rocksmith Audio Bridge in the ASIO chain (RS_ASIO.ini Driver set to Rocksmith Audio Bridge), or Cable mode.");
				EndCard();
				return;
			}
			if (canRoute) Caption("Moves the game's sound to another device now, without restarting. An ASIO interface also takes the guitar, unless Guitar input below names another one. It is remembered: next launch starts there if it is plugged in.");
			else Caption("Moves the game's sound to another device now, without restarting. An ASIO interface plays the sound only: the guitar stays on the Real Tone Cable. It is remembered: next launch starts there if it is plugged in.");
			if (!snapshot.ok) Caption("Looking for devices...");

			const bool unidentified = snapshot.asioUnidentified;
			// The list is grouped (ASIO interfaces, then Windows devices) under small subheaders, with rows packed
			// closer than the page's default spacing so a long device list stays short.
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0f));
			ImGui::BeginDisabled(pending.active);
			if (canRoute) {
				ImGui::Dummy(ImVec2(0, 4));
				SectionLabel("ASIO interfaces");
				ImGui::Dummy(ImVec2(0, 2));
				// Every ASIO interface (what RS_ASIO can open). The current one: stop any route so the game plays straight
				// out of it again, or, when the bridge is virtual (booted without it), bind it (PromoteToAsio). Another
				// one: switch the bridge to it live (SwitchInterface), falling back to the current one if it will not open.
				const bool virtualProxy = Audio::OutputTap::ProxyOutputMode() == 1;
				const bool interfaceBusy = g_interfaceState.load() == 1;
				for (const std::wstring& driver : InstalledAsioDrivers()) {
					const bool isCurrent = _wcsicmp(driver.c_str(), asioDriver.c_str()) == 0;
					const std::string name = Utf8(driver);
					const std::string label = name + "##asio-" + name;
					// Which build the bridge uses: 32-bit in the game, or 64-bit in its driver host (one buffer later).
					const bool host64 = RunsIn64BitHost(driver);
					const char* detail = isCurrent ? (host64 ? "ASIO 64-bit, low latency" : "ASIO 32-bit, lowest latency") : (host64 ? "ASIO 64-bit" : "ASIO 32-bit");
					const bool clicked = ChoiceRow(("asio-" + name).c_str(), label.c_str(), detail, isCurrent && !routing && !virtualProxy && !interfaceBusy);
					if (!clicked || interfaceBusy) continue;
					if (isCurrent && virtualProxy) {
						pending = PendingSwitch{};
						pending.active = true;
						pending.promote = true;
						pending.name = name;
						pending.started = GetTickCount64();
						OutputSay("Opening " + name + " for input and output...", false);
						PromoteToAsio(driver);
					}
					else if (isCurrent && routing) {
						BridgeControl(22);
						OutputSay("Back on " + name + ".", false);
					}
					else if (!isCurrent) {
						if (routing) BridgeControl(22);
						OutputSay(std::string(), false);
						SwitchInterface(driver, asioDriver);
					}
				}
			}
			// Cable mode: an ASIO interface's own driver for the sound only (AsioOutputDevice), at the buffer size set in its
			// control panel. Windows' shared audio adds its period and the USB audio driver's buffering on top; through
			// ASIO an interface plays with the delay its owner knows from other programs. The guitar stays on the cable.
			if (!canRoute && session) {
				const std::vector<std::wstring> drivers = InstalledAsioDrivers(false);   // loaded in the game: 32-bit only
				if (!drivers.empty()) {
					ImGui::Dummy(ImVec2(0, 4));
					SectionLabel("ASIO interfaces (sound only)");
					ImGui::Dummy(ImVec2(0, 2));
				}
				for (const std::wstring& driver : drivers) {
					const std::wstring id = Audio::SharedOutput::AsioOutputDevice::EndpointOf(driver);
					const bool selected = endpoint == id;
					const std::string name = Utf8(driver);
					const bool clicked = ChoiceRow(("asio-out-" + name).c_str(), (name + "##asio-out-" + name).c_str(), selected ? "ASIO 32-bit, lowest latency" : "ASIO 32-bit", selected);
					Hint("Plays the game (and Tone designer) through this interface's own ASIO driver, at the buffer size set in its control panel. The guitar stays on the Real Tone Cable. While the game runs, other programs cannot use this ASIO driver.");
					if (!clicked || selected) continue;
					pending = PendingSwitch{};
					pending.active = true;
					pending.target = id;
					pending.name = name;
					pending.started = GetTickCount64();
					pending.previous = endpoint;
					const Audio::ControlResponse result = BridgeControl(4, id.c_str());
					if (FAILED(result.result)) { OutputSay("Could not play through " + name + ".", true); pending.active = false; }
					else OutputSay("Opening " + name + "...", false);
				}
			}
			ImGui::BeginDisabled(unidentified && !snapshot.asioDrivers.empty());
			bool windowsLabelled = false;
			for (const auto& device : snapshot.devices) {
				if (canRoute && device.isAsioTwin) continue;   // see below
				if (!windowsLabelled) {
					ImGui::Dummy(ImVec2(0, 6));
					SectionLabel("Windows devices");
					ImGui::Dummy(ImVec2(0, 2));
					windowsLabelled = true;
				}
				// The ASIO interface is listed once, as the ASIO entry above; its Windows endpoint is the same box and
				// is hidden, bound or not. While bound it would silence the interface; while unbound the ASIO entry
				// is the right pick because it moves the input too.
				if (canRoute && device.isAsioTwin) continue;
				const bool selected = routing ? device.id == routedId
					: (session && device.id == Audio::SharedOutput::ExclusiveOutputDevice::WindowsId(endpoint));
				std::string caption = device.isDefault ? "Windows default" : "";
				// Device-reported periods describe capability, not total latency or the buffer granted after alignment.
				if (device.sharedPeriod || device.exclusivePeriod) {
					char estimate[64] = "";
					const double shared = device.sharedPeriod / 48.0, exclusive = device.exclusivePeriod / 48.0;
					// A device whose driver stalls in event-driven exclusive mode runs it polling, three periods deep
					// (OutputBackend remembers it): say what it really gets, not the minimum it reports.
					DWORD polling = 0, pollingSize = sizeof(polling);
					const bool needsPolling = RegGetValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy\\ExclusivePolling", device.id.c_str(),
						RRF_RT_REG_DWORD, nullptr, &polling, &pollingSize) == ERROR_SUCCESS && polling;
					if (device.sharedPeriod && device.exclusivePeriod && needsPolling)
						sprintf_s(estimate, "Periods: shared %.1f ms, exclusive %.1f ms (polling)", shared, exclusive * 3.0);
					else if (device.sharedPeriod && device.exclusivePeriod) sprintf_s(estimate, "Periods: shared %.1f ms, exclusive min %.1f ms", shared, exclusive);
					else if (device.sharedPeriod) sprintf_s(estimate, "Shared period %.1f ms", shared);
					caption = caption.empty() ? estimate : caption + "  " + estimate;
				}
				ImGui::BeginDisabled(device.isProtected);
				if (device.isProtected) caption = "Used by ASIO";
				const std::string label = device.name + "##" + Utf8(device.id);
				const bool clicked = ChoiceRow(Utf8(device.id).c_str(), label.c_str(), caption.c_str(), selected);
				if (device.isProtected) Hint("This is the same interface RS_ASIO is using. Playing to its Windows device would silence it, so pick the ASIO entry above instead.");
				ImGui::EndDisabled();
				if (!clicked || selected || device.isProtected) continue;
				pending = PendingSwitch{};
				pending.active = true;
				pending.target = device.id;
				pending.name = device.name;
				pending.started = GetTickCount64();
				if (canRoute) {
					pending.route = true;
					const Audio::ControlResponse result = BridgeControl(21, device.id.c_str());
					if (FAILED(result.result)) { OutputSay("Could not play through " + device.name + ".", true); pending.active = false; }
					else OutputSay("Switching to " + device.name + "...", false);
				}
				else {
					pending.previous = endpoint;
					const Audio::ControlResponse result = BridgeControl(4, device.id.c_str());
					if (FAILED(result.result)) { OutputSay("Could not play through " + device.name + ".", true); pending.active = false; }
					else OutputSay("Switching to " + device.name + "...", false);
				}
			}
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			ImGui::PopStyleVar();
			ImGui::Dummy(ImVec2(0, 2));

			// Exclusive and ASIO outputs: other programs cannot play on the device, so the bridge mixes them in
			// (AppAudioMix, Windows' app capture of everything but Rocksmith). Not offered in shared mode, where they play
			// on the device already and mixing them in too would double them.
			if (session && (Audio::SharedOutput::ExclusiveOutputDevice::IsExclusiveEndpoint(endpoint)
				|| Audio::SharedOutput::AsioOutputDevice::IsAsioEndpoint(endpoint))) {
				bool mixApps = Audio::AppAudioMix::Enabled();
				if (ToggleRow("Hear other apps here", "YouTube, Discord, Spotify and Windows sounds play through this device too. Set Windows' default output to a spare device (such as the laptop speakers) and mute it there: the bridge picks the sound up before the mute. The guitar's delay is not affected.", &mixApps))
					Audio::AppAudioMix::SetEnabled(mixApps);
				if (mixApps) Caption(Audio::AppAudioMix::Hearing() ? "Other apps: playing through here now." : "Other apps: nothing playing right now.");
			}

			// Cable mode on a Windows device: exclusive mode (ExclusiveOutputDevice) takes Windows' mixer out of the way,
			// the device's own ~3 ms period instead of its shared 10 ms. Switching it is a device switch ("excl:<id>"), so
			// it is remembered and verified like one.
			const bool onWindowsDevice = session && !Audio::SharedOutput::AsioOutputDevice::IsAsioEndpoint(endpoint);
			if (onWindowsDevice && !pending.active) {
				const bool isExclusive = outputMode == 2 || outputMode == 3;
				bool wantExclusive = isExclusive;
				// Taking Windows' default output away breaks what plays on it (YouTube shows "Audio renderer error"):
				// say so before, while there is still time to move Windows' default.
				bool windowsDefault = false;
				for (const auto& device : snapshot.devices)
					if (device.id == Audio::SharedOutput::ExclusiveOutputDevice::WindowsId(endpoint)) windowsDefault = device.isDefault;
				if (!isExclusive && windowsDefault) {
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
					ImGui::TextUnformatted("This is Windows' default output: programs playing on it (YouTube, Discord) stop when exclusive mode starts. Make another device (such as the laptop speakers) the Windows default first and mute it; \"Hear other apps here\" then plays them through this one.");
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
				}
				// Exclusive mode and direct USB (UsbCableCapture) are two separate latency features: direct USB is the
				// cable's input and is on by default; exclusive mode is this device's output and is opt-in. The text
				// describes exclusive mode on its own and does not mention the input path.
				ImGui::BeginDisabled(outputMode == 0);
				if (ToggleRow("Exclusive mode", "Lowers the delay of the sound you hear by playing straight to this device, past Windows' mixer. Only for devices that support exclusive mode.", &wantExclusive,
					"Other apps cannot play on this device while exclusive mode is on. A device that does not support it stays shared and this switch turns itself off.")) {
					const std::wstring windowsId = Audio::SharedOutput::ExclusiveOutputDevice::WindowsId(endpoint);
					const std::wstring target = wantExclusive ? Audio::SharedOutput::ExclusiveOutputDevice::EndpointOf(windowsId) : windowsId;
					pending = PendingSwitch{};
					pending.active = true;
					pending.target = target;
					pending.name = current + (wantExclusive ? " (exclusive)" : " (shared)");
					pending.started = GetTickCount64();
					pending.previous = endpoint;
					const Audio::ControlResponse result = BridgeControl(4, target.c_str());
					if (FAILED(result.result)) { OutputSay("Could not switch exclusive mode.", true); pending.active = false; }
					else OutputSay(wantExclusive ? "Opening exclusive mode..." : "Back to shared mode...", false);
				}
				ImGui::EndDisabled();
			}

			if (!snapshot.asioWrapper.empty() && canRoute)
				Caption((snapshot.asioWrapper + " wraps your Windows devices rather than being one. A device it is using itself will not open here; switch that device off in "
					+ snapshot.asioWrapper + "'s own panel, or pick another.").c_str());
			if (unidentified && !snapshot.asioDrivers.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
				ImGui::TextUnformatted(("Switching is off: the overlay cannot tell which Windows device is your ASIO interface (" + snapshot.asioDrivers.front()
					+ "), and playing to that device would silence it.").c_str());
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}
			{
				std::string switchMessage;
				{
					std::lock_guard<std::mutex> lock(g_interfaceMessageMutex);
					switchMessage = g_outputSwitchMessage;
				}
				const int state = g_interfaceState.load();
				if (state == 1) switchMessage = "Switching interface...";
				if (!switchMessage.empty()) {
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(state >= 3 ? Color::Bad : Color::Muted));
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
					ImGui::TextUnformatted(switchMessage.c_str());
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
				}
			}
			// The output moved without the user (unplugged, fell back, came back): an old "Now playing through" named the
			// wrong device next to the one shown playing.
			if (!pending.active && !g_outputMessageEndpoint.empty() && std::wstring(live.status.endpoint) != g_outputMessageEndpoint) OutputSay(std::string(), false);
			if (!g_outputMessage.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(g_outputMessageIsError ? Color::Bad : Color::Muted));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
				ImGui::TextUnformatted(g_outputMessage.c_str());
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}
			EndCard();
		}

		// Output buffer size, only when the game plays through a Windows device (Cable mode's managed session).
		// Under ASIO the buffer belongs to RS_ASIO and the interface. Op 5 reports the engine's limits in
		// response.file ("engineMinimum=.. engineFundamental=.. engineMaximum=.. enginePeriod=.."); op 6 sets it
		// ("<frames>" newline "<endpoint>", 0 = automatic). Saved per device in RSMods.ini [Audio Bridge Output Buffer <endpoint>],
		// the same keys the engine reads at launch (ReadSavedPeriod).
		void OutputBufferCard(const LiveState& live)
		{
			const std::wstring endpoint = live.status.endpoint;
			if (endpoint.empty() || endpoint[0] == L'(') return;

			static ULONGLONG polled = 0;
			static unsigned minimum = 0, fundamental = 0, maximum = 0, period = 0;
			static std::wstring polledEndpoint;
			if (GetTickCount64() - polled > 1000 || polledEndpoint != endpoint) {
				const Audio::ControlResponse info = BridgeControl(5);
				polled = GetTickCount64();
				polledEndpoint = endpoint;
				auto field = [&](const wchar_t* key) -> unsigned {
					const std::wstring text = info.file;
					const size_t at = text.find(std::wstring(key) + L"=");
					return at == std::wstring::npos ? 0u : static_cast<unsigned>(wcstoul(text.c_str() + at + wcslen(key) + 1, nullptr, 10));
				};
				minimum = field(L"engineMinimum"); fundamental = field(L"engineFundamental");
				maximum = field(L"engineMaximum"); period = field(L"enginePeriod");
			}
			// Hide the card when there is nothing to pick. A driver without Windows' low-latency engine reports one
			// period (minimum == maximum, usually 480), and the converted-stream fallback reports none; a slider pinned
			// to a single value only made the setting look broken.
			if (!fundamental || maximum <= minimum) return;

			const std::filesystem::path ini = GameFolder() / L"RSMods.ini";
			const std::wstring section = L"Audio Bridge Output Buffer " + endpoint;
			// Read once a second (the page draws every frame), and again right after this page changes it below.
			static std::wstring modeFor;
			static ULONGLONG modeReadAt = 0;
			static bool modeAutomatic = true;
			const ULONGLONG modeNow = GetTickCount64();
			if (modeFor != section || modeReadAt == 0 || modeNow - modeReadAt >= 1000) {
				wchar_t mode[32]{};
				GetPrivateProfileStringW(section.c_str(), L"Mode", L"Automatic", mode, 32, ini.c_str());
				modeAutomatic = _wcsicmp(mode, L"Custom") != 0;
				modeFor = section;
				modeReadAt = modeNow;
			}
			bool automatic = modeAutomatic;

			BeginCard("Device buffer");
			char current[64];
			sprintf_s(current, "Now %u frames (%.2f ms).", period, period / 48.0);
			// At the device's smallest, say so, or players wonder why it cannot go lower.
			const std::string about = period && period <= minimum
				? std::string(current) + " The smallest this device allows in shared mode; for less, turn on Exclusive mode above."
				: std::string(current) + " How much sound Windows hands this device at a time. Smaller is lower latency; raise it if you hear crackles. Applies at once.";
			const bool toggled = ToggleRow("Automatic", about.c_str(), &automatic);
			static float frames = 0.0f;
			if (frames <= 0.0f || toggled) frames = static_cast<float>(period ? period : minimum);
			ImGui::BeginDisabled(automatic);
			bool committed = false;
			char format[32];
			sprintf_s(format, "%%.0f frames");
			SliderRow("Size", &frames, static_cast<float>(minimum), static_cast<float>(maximum), format, &committed,
				"Output buffer in frames at 48 kHz, in steps the device allows.");
			ImGui::EndDisabled();
			const unsigned snapped = std::clamp(minimum + static_cast<unsigned>((frames - minimum) / fundamental + 0.5f) * fundamental, minimum, maximum);
			frames = static_cast<float>(snapped);
			if (toggled || (committed && !automatic)) {
				const unsigned request = automatic ? 0u : snapped;
				const std::wstring value = std::to_wstring(request) + L"\n" + endpoint;
				const Audio::ControlResponse result = BridgeControl(6, value.c_str());
				if (SUCCEEDED(result.result)) {
					WritePrivateProfileStringW(section.c_str(), L"PeriodFrames", automatic ? nullptr : std::to_wstring(snapped).c_str(), ini.c_str());
					WritePrivateProfileStringW(section.c_str(), L"Mode", automatic ? L"Automatic" : L"Custom", ini.c_str());
					modeAutomatic = automatic; modeFor = section; modeReadAt = GetTickCount64();
				}
				polled = 0;   // re-read the applied period next frame
			}
			EndCard();
		}

		// The block size the game's own engine runs at in Cable mode (CableGamePeriod.hpp). RS_ASIO players get it from
		// their interface's buffer; Automatic does the same here from the output the game plays to. Fixed for the
		// session when the game opens its devices, so a change applies with Apply now (the output reopens) or at the next start.
		void GameBlockCard(const LiveState& live)
		{
			const std::wstring endpoint = live.status.endpoint;
			if (endpoint.empty() || endpoint[0] == L'(') return;
			static const char* const labels[] = { "Automatic", "64", "128", "256", "512" };
			static const DWORD values[] = { 0, 64, 128, 256, 512 };
			static ULONGLONG readAt = 0;
			static DWORD setting = 0;
			static UINT32 next = 0;
			if (!readAt || GetTickCount64() - readAt >= 1000) {
				setting = Audio::CableGamePeriod::Setting();
				next = Audio::CableGamePeriod::NextStart();
				readAt = GetTickCount64();
			}
			int index = 0;
			for (int i = 0; i < 5; ++i) if (values[i] == setting) index = i;
			BeginCard("Game engine");
			// What the game's output really runs at; the requested size only takes over when the game reopens its output.
			const UINT32 running = Audio::SharedOutput::GameOutputBlockFrames();
			const UINT32 now = running ? running : Audio::CableGamePeriod::Frames();
			char text[96];
			sprintf_s(text, "%u samples (%.1f ms)", now, now / 48.0);
			InfoRow("Processing block", text);
			if (SegmentedRow("Game block", &index, labels, 5,"How many samples the game's own engine (its amps, pedals and note detection) processes in one go: the ASIO buffer size under RS_ASIO. Smaller is less delay in the game's tones but more work for the PC; raise it if game tones crackle. Automatic follows the output: an ASIO interface's own buffer size, or the device buffer up to 144. A change applies with Apply now, or at the next game start.")) {
				Audio::CableGamePeriod::SetSetting(values[index]);
				readAt = 0;
			}
			// A different size waiting: one action, Apply now. The bridge tells the game its output changed and it reopens
			// it at the new size (the input keeps its start-up size); off the render thread, the game may work inside the
			// call. "Restart the game" only shows once Apply now was tried and the game did not take it within 3 s
			// (a warning plus a button was two answers to one question).
			static ULONGLONG appliedAt = 0;
			static UINT32 appliedSize = 0;
			if (next != now) {
				const bool refused = appliedAt && appliedSize == next && GetTickCount64() - appliedAt > 3000;
				if (refused) {
					sprintf_s(text, "%u samples after you restart the game (the game did not take it now)", next);
					InfoRow("Restart needed", text, Color::Warn);
				}
				else {
					sprintf_s(text, "Apply %u samples now (game sound restarts for a moment)", next);
					const bool pending = appliedAt && appliedSize == next && GetTickCount64() - appliedAt <= 3000;
					ImGui::BeginDisabled(pending);
					if (ImGui::Button(pending ? "Applying..." : text)) {
						appliedAt = GetTickCount64();
						appliedSize = next;
						std::thread([] {
							Audio::CableGamePeriod::ApplyNextStart();
							Audio::PersistentInput::ResetGameOutput();
						}).detach();
					}
					ImGui::EndDisabled();
					if (setting == 0 && !pending) Caption("Automatic follows your output; it also applies at the next game start.");
				}
			}
			EndCard();
		}

		// ---- Channel routing ------------------------------------------------------------------
		// Which channels of the ASIO interface the game's output and each player's input use. RS_ASIO.ini keeps
		// asking for the same channels; the bridge maps them onto the ones picked here and applies a change at
		// once (a short gap while it re-creates the interface's buffers), saved per interface for the next launch.


		// 0 idle, 1 applying, 2 applied, 3 saved for next launch, 4 failed.
		std::atomic<int> g_routeState{ 0 };

		void ApplyRoute(int role, long channel)
		{
			g_routeState.store(1);
			std::thread([role, channel] {
				const int result = Audio::OutputTap::SetProxyRoute(role, channel);
				g_routeState.store(result == 1 ? 2 : result == 2 ? 3 : 4);
			}).detach();
		}

		void SwitchInputInterface(int role, const std::wstring& driver)
		{
			g_interfaceState.store(1);
			std::thread([role, driver] {
				const int result = Audio::OutputTap::SetProxyInputTarget(role, driver);
				const std::string error = Audio::OutputTap::ProxyInputError();
				static const char* const names[4] = { "the output", "Player 1", "Player 2", "the mic" };
				const std::string who = role >= 1 && role <= 3 ? names[role] : "every input";
				std::string message;
				if (result == 0) message = "Could not change the interface for " + who + ".";
				else if (!error.empty()) message.clear();   // the card shows the input error itself
				else if (driver.empty()) message = who + " comes from the output interface again.";
				else message = who + " now comes from " + Utf8(driver) + (result == 2 ? " once the output interface opens." : ".");
				if (!message.empty()) message[0] = static_cast<char>(toupper(static_cast<unsigned char>(message[0])));
				LOG_INFO("(AUDIO BRIDGE) Input interface for " << who << ": " << (driver.empty() ? std::string("same as output") : Utf8(driver)) << ", result " << result << (error.empty() ? "" : ", " + error) << std::endl);
				{
					std::lock_guard<std::mutex> lock(g_interfaceMessageMutex);
					g_inputSwitchMessage = message;
				}
				g_interfaceState.store(result == 0 ? 4 : 2);
			}).detach();
		}

		// "In sync" / "Catching up" for a role's input on another clock (its own input interface, or the Real Tone
		// Cable), with the numbers on hover. Nothing when it shares the output's clock: there is nothing to drift.
		// The pill's state for a role, shared by the clock row and the role's fold-out header. `shown` false: the
		// input shares the output's clock.
		struct ClockView
		{
			bool shown = false;
			bool settled = false;
			char detail[256]{};
		};

		ClockView InputClockState(int role)
		{
			ClockView view;
			const Audio::OutputTap::InputClock clock = Audio::OutputTap::ProxyInputClock(role);
			if (clock.source == 0) return view;
			// "Catching up": not delivering yet, or a dropout in the last 5 s, or the correction near its limit.
			static unsigned lastUnderruns[4]{};
			static ULONGLONG lastDropoutAt[4]{};
			const size_t r = static_cast<size_t>(role & 3);
			if (clock.underruns != lastUnderruns[r]) { lastUnderruns[r] = clock.underruns; lastDropoutAt[r] = GetTickCount64(); }
			const bool recentDropout = lastDropoutAt[r] != 0 && GetTickCount64() - lastDropoutAt[r] < 5000;
			view.shown = true;
			view.settled = clock.primed && !recentDropout && std::abs(clock.ppm) < 2500;
			sprintf_s(view.detail, "%s runs on its own clock. Correcting by %+d ppm (%.3f%%); %d frames buffered (margin %d); %u dropout%s since it opened.",
				clock.source == 1 ? "Its interface" : "The Real Tone Cable", clock.ppm, clock.ppm / 10000.0, clock.fill, clock.margin, clock.underruns, clock.underruns == 1 ? "" : "s");
			return view;
		}

		void InputClockPill(int role, const char* label)
		{
			const ClockView clock = InputClockState(role);
			if (!clock.shown) return;
			PillRow(label, clock.settled ? "In sync" : "Catching up", clock.settled ? Color::Good : Color::Warn, clock.detail);
		}

		// ---- Routing ----------------------------------------------------------------------------
		// Each part of the game's audio picks an interface and a channel, the way RS_ASIO.ini can, but live: the output
		// pair on the output interface (Output page; Switch output picks that interface), and each input (Player 1,
		// Player 2, the mic) on any ASIO interface, the output's by default (Input page, one tab per player). RS_ASIO.ini
		// keeps naming the bridge at fixed channels; the bridge maps them. An input on another interface runs on that
		// interface's clock and is kept in time with the output (SplitInput); its pill shows how well. Changes apply at
		// once with a short gap.
		struct RouteView
		{
			bool viaBridge[4]{};
			long host[4]{};
			long device[4]{};
			std::vector<std::string> names[4];     // per role: its interface's channel names (role 0 = outputs)
			std::wstring interfaceOf[4];           // per input role: its own input interface, empty = the output's
		};
		static const char* const kRoleNames[4] = { "Output", "Player 1", "Player 2", "Mic" };

		// The route reads open RS_ASIO.ini and the registry: once a second, and right after a change lands.
		const RouteView& PolledRouteView()
		{
			static RouteView view;
			static ULONGLONG polled = 0;
			static int lastRouteState = 0, lastInterfaceState = 0;
			const int state = g_routeState.load();
			const int interfaceState = g_interfaceState.load();
			if (state != 1 && interfaceState != 1 && (GetTickCount64() - polled > 1000 || state != lastRouteState || interfaceState != lastInterfaceState)) {
				RouteView fresh;
				for (int role = 0; role < 4; ++role) {
					fresh.viaBridge[role] = Audio::OutputTap::ProxyRoute(role, fresh.host[role], fresh.device[role]);
					fresh.names[role] = Audio::OutputTap::ProxyRoleChannelNames(role);
					if (role > 0) fresh.interfaceOf[role] = Audio::OutputTap::ProxyInputTarget(role);
				}
				view = std::move(fresh);
				polled = GetTickCount64();
			}
			lastRouteState = state;
			lastInterfaceState = interfaceState;
			return view;
		}

		bool RouteBusy() { return g_routeState.load() == 1 || g_interfaceState.load() == 1; }

		// The bridge's reason an input interface would not open, then the result of the last route or interface change.
		void RouteStatusMessage()
		{
			const std::string inputError = Audio::OutputTap::ProxyInputError();
			if (!inputError.empty()) WarnCaption(inputError.c_str());
			const int state = g_routeState.load();
			std::string message;
			ImU32 color = Color::Muted;
			if (RouteBusy()) message = "Applying...";
			else {
				{
					std::lock_guard<std::mutex> lock(g_interfaceMessageMutex);
					message = g_inputSwitchMessage;
				}
				if (state == 2) message = "Applied.";
				else if (state == 3) message = "Saved. It applies when the interface opens.";
				else if (state == 4) { message = "The interface would not take that channel. Check Playing to on the Output page."; color = Color::Bad; }
			}
			if (message.empty()) return;
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
			ImGui::TextUnformatted(message.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
		}

		// Output page: the pair of outputs the game plays on. Most interfaces have one stereo pair, where a dropdown that
		// can't change anything only confuses, so the card only shows when there is a real choice (4+ outputs, or an odd
		// pair saved earlier to move off). Each player's input is picked on the Input page.
		void OutputPairCard()
		{
			if (!Audio::OutputTap::ProxyAvailable()) return;
			const RouteView& view = PolledRouteView();
			if (!view.viaBridge[0] || view.names[0].size() < 2) return;
			std::vector<std::string> labels;
			std::vector<long> channels;
			int index = -1;
			for (long channel = 0; channel + 1 < static_cast<long>(view.names[0].size()); channel += 2) {
				if (channel == view.device[0]) index = static_cast<int>(labels.size());
				labels.push_back(view.names[0][channel] + " / " + view.names[0][channel + 1]);
				channels.push_back(channel);
			}
			if (index < 0 && view.device[0] + 1 < static_cast<long>(view.names[0].size())) {   // an odd pair saved earlier
				index = static_cast<int>(labels.size());
				labels.push_back(view.names[0][view.device[0]] + " / " + view.names[0][view.device[0] + 1]);
				channels.push_back(view.device[0]);
			}
			if (labels.size() <= 1) return;
			const std::wstring outputInterface = ReadPassthroughOutputName();
			BeginCard("Output pair");
			ImGui::BeginDisabled(RouteBusy());
			if (DropdownRow("Output pair", &index, labels, ("The pair of outputs of " + Utf8(outputInterface) + " the game's sound plays on. The interface itself is picked in Switch output above.").c_str()))
				ApplyRoute(0, channels[static_cast<size_t>(index)]);
			ImGui::EndDisabled();
			RouteStatusMessage();
			EndCard();
		}

		// One input role's ASIO source: its interface (the output's or any other) and channel, and the clock pill when it
		// runs on another interface's clock.
		void InputSourceRows(int role, const RouteView& view)
		{
			const std::vector<std::wstring> drivers = InstalledAsioDrivers();
			// RS_ASIO.ini gives this role the same Channel as an earlier one, so there is one input for both and it follows
			// that role. Picking it separately needs its own Channel= in RS_ASIO.ini.
			int sharesWith = 0;
			for (int earlier = 1; earlier < role; ++earlier)
				if (view.viaBridge[earlier] && view.host[earlier] == view.host[role]) { sharesWith = earlier; break; }
			std::vector<std::string> interfaceLabels{ "Same as output" };
			int interfaceIndex = 0;
			for (size_t i = 0; i < drivers.size(); ++i) {
				interfaceLabels.push_back(Utf8(drivers[i]));
				if (!view.interfaceOf[role].empty() && _wcsicmp(drivers[i].c_str(), view.interfaceOf[role].c_str()) == 0) interfaceIndex = static_cast<int>(i) + 1;
			}
			const std::vector<std::string>& inputs = view.names[role];
			int index = view.device[role] >= 0 && view.device[role] < static_cast<long>(inputs.size()) ? static_cast<int>(view.device[role]) : -1;
			ImGui::BeginDisabled(sharesWith != 0 || RouteBusy());
			if (DropdownRow("Interface", &interfaceIndex, interfaceLabels, "Where this input comes from. Same as output: the interface the sound plays on.") && sharesWith == 0)
				SwitchInputInterface(role, interfaceIndex == 0 ? std::wstring() : drivers[static_cast<size_t>(interfaceIndex - 1)]);
			if (DropdownRow("Input channel", &index, inputs, "The input on that interface this player listens to.") && sharesWith == 0)
				ApplyRoute(role, index);
			ImGui::EndDisabled();
			if (sharesWith != 0)
				Caption((std::string(kRoleNames[role]) + " follows " + kRoleNames[sharesWith] + ": it has the same Channel in RS_ASIO.ini. Give it its own Channel there to pick it here.").c_str());
			InputClockPill(role, "Clock");
			RouteStatusMessage();
		}

		// Player 1's input device in Cable mode: automatic (a Real Tone Cable, else the Windows default recording device), or
		// any Windows recording device, for cables that do not report the Real Tone Cable's USB ID. The list is re-read
		// every 3 s while this card is shown, as enumerating is a few COM calls.
		void PlayerOneCableInputRows()
		{
			using namespace Audio::PersistentInput;
			static std::vector<CaptureDeviceInfo> devices;
			static ULONGLONG listedAt = 0;
			const ULONGLONG now = GetTickCount64();
			if (listedAt == 0 || now - listedAt > 3000) { devices = ListCaptureDevices(); listedAt = now; }
			const std::wstring choice = GetPlayerOneInputChoice();
			std::vector<std::string> labels{ "Automatic" };
			int index = 0;
			for (size_t i = 0; i < devices.size(); ++i) {
				labels.push_back(Utf8(devices[i].name.empty() ? devices[i].id : devices[i].name) + (devices[i].realToneCable ? " (Real Tone Cable)" : ""));
				if (!choice.empty() && _wcsicmp(devices[i].id.c_str(), choice.c_str()) == 0) index = static_cast<int>(i) + 1;
			}
			if (!choice.empty() && index == 0) {
				labels.push_back("Chosen device (not plugged in)");
				index = static_cast<int>(labels.size()) - 1;
			}
			if (DropdownRow("Input device", &index, labels, "Automatic plays a Real Tone Cable, or the Windows default recording device when none is plugged in. Pick your cable here if it is not found."))
			{
				if (index == 0) SetPlayerOneInputChoice(L"");
				else if (static_cast<size_t>(index - 1) < devices.size()) SetPlayerOneInputChoice(devices[static_cast<size_t>(index - 1)].id);
			}
		}

		// ---- Input --------------------------------------------------------------------------------
		// One tab per player (and the mic while RS_ASIO.ini sends it through the bridge), each the same component: what the
		// input is (ASIO interface or Real Tone Cable), its level, that source's own settings, then the processing. The
		// processing controls are the same whatever the source: they work alike on ASIO and on the cable. The input chain
		// only runs on Player 1 so far (InputCaptureHook, route 0); Player 2's is planned.
		enum class InputSource { None, Asio, Cable, CableStandIn, GameCable };

		// ---- Noise gate pedal -------------------------------------------------------------------------
		// Player 1's noise gate (NoiseSuppressor.hpp): a row on the Input page, and its dials on a pop-out stomp box
		// that stays over the game while the overlay is open. Threshold goes over op 19 (signed tenths of a dB, 0 = off);
		// decay, reduction and attack over op 31 ("<ms>,<dB>,<tenths of a ms>"). Drags apply live (Preview*) and save
		// once on release. The settings app has the same pedal (GUI/Audio/NoiseGatePedalForm.cs).
		struct NoiseGatePedalState
		{
			bool seeded = false;
			bool on = false;
			bool windowOpen = false;
			int thresholdTenths = -620;   // kept while off (NoiseGateLastThreshold) so the footswitch returns to it
			int decayMs = 200;
			int reductionDb = -60;
			int attackTenths = 5;

			std::string tone;             // the Tone designer tone being edited; empty = all tones (the global gate)
			bool own = false;             // that tone has its own gate
			uint32_t revision = 0;

			// Follows ToneGates every frame: the playing tone's own gate, else the global one. Edits from here or the
			// settings app bump the revision, so both pedals stay on the same values.
			void Seed()
			{
				const ToneGates::View view = ToneGates::Current();
				if (seeded && view.revision == revision) return;
				seeded = true;
				revision = view.revision;
				tone = view.tone;
				own = view.own;
				on = view.gate.on;
				thresholdTenths = view.gate.thresholdTenths;
				decayMs = view.gate.decayMs;
				reductionDb = view.gate.reductionDb;
				attackTenths = view.gate.attackTenths;
			}

			// "Noise gate · Clean Strat" or "Noise gate · All tones", cut to fit the pedal's title bar.
			std::string Title() const
			{
				std::string name = tone.empty() ? std::string("All tones") : tone;
				if (name.size() > 30) name = name.substr(0, 28) + "...";
				return std::string("Noise gate \xC2\xB7 ") + name;
			}

			void SendThreshold(bool save) { if (save) SendInt(19, on ? thresholdTenths : 0); else if (on) PreviewInt(19, thresholdTenths); }

			void SendShape(bool save)
			{
				wchar_t value[48];
				swprintf_s(value, L"%d,%d,%d", decayMs, reductionDb, attackTenths);
				if (save) BridgeControl(31, value); else PreviewControl(31, value);
			}

			void FormatDecay(char* out, size_t size) const
			{
				if (decayMs >= 1000) snprintf(out, size, "%.1f s", decayMs / 1000.0f);
				else snprintf(out, size, "%d ms", decayMs);
			}
		};
		NoiseGatePedalState g_gatePedal;

		namespace PedalInk
		{
			constexpr ImU32 Body = IM_COL32(31, 91, 84, 255);
			constexpr ImU32 Edge = IM_COL32(45, 122, 112, 255);
			constexpr ImU32 Lettering = IM_COL32(234, 247, 243, 255);
			constexpr ImU32 Label = IM_COL32(215, 239, 233, 255);
			constexpr ImU32 Value = IM_COL32(168, 216, 206, 255);
			constexpr ImU32 KnobSkirt = IM_COL32(20, 64, 58, 255);
			constexpr ImU32 KnobSkirtEdge = IM_COL32(13, 42, 38, 255);
			constexpr ImU32 KnobCap = IM_COL32(27, 31, 30, 255);
			constexpr ImU32 KnobCapEdge = IM_COL32(58, 68, 66, 255);
			constexpr ImU32 Pointer = IM_COL32(242, 245, 244, 255);
			constexpr ImU32 MeterTrack = IM_COL32(18, 58, 53, 255);
			constexpr ImU32 MeterFill = IM_COL32(93, 202, 165, 255);
			constexpr ImU32 ThresholdLine = IM_COL32(250, 199, 117, 255);
			constexpr ImU32 LedOn = IM_COL32(151, 196, 89, 255);
			constexpr ImU32 LedOff = IM_COL32(51, 71, 63, 255);
			constexpr ImU32 Switch = IM_COL32(201, 211, 209, 255);
			constexpr ImU32 SwitchOff = IM_COL32(140, 153, 150, 255);
			constexpr ImU32 SwitchText = IM_COL32(61, 74, 71, 255);
		}

		// One dial: drag up or down (150 px = the full sweep), mouse wheel steps, double-click resets. `t` is the
		// 0..1 position. Returns true on any frame it moves; `commit` is set on the frame to save (drag released,
		// a wheel step or a reset).
		bool PedalKnob(const char* id, const char* label, const char* valueText, float& t, float defaultT, float step,
			float cellWidth, bool& commit, const char* hint)
		{
			ImGui::PushID(id);
			ImGuiIO& io = ImGui::GetIO();
			const float radius = 27.0f;
			const ImVec2 start = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("##dial", ImVec2(cellWidth, radius * 2.0f + 40.0f));
			const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
			Hint(hint);
			bool changed = false;
			commit = false;
			if (active && io.MouseDelta.y != 0.0f) { t -= io.MouseDelta.y / 150.0f; changed = true; }
			if (ImGui::IsItemDeactivated()) commit = true;
			if (hovered && io.MouseWheel != 0.0f) { t += io.MouseWheel * step; changed = commit = true; }
			if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { t = defaultT; changed = commit = true; }
			t = std::clamp(t, 0.0f, 1.0f);

			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 center(start.x + cellWidth * 0.5f, start.y + radius + 2.0f);
			list->AddCircleFilled(center, radius, PedalInk::KnobSkirt, 32);
			list->AddCircle(center, radius, PedalInk::KnobSkirtEdge, 32, 2.0f);
			// Sweep marks at the two ends of the 270 degree travel.
			for (const float end : { 0.0f, 1.0f }) {
				const float a = (-135.0f + 270.0f * end) * 3.14159265f / 180.0f;
				list->AddLine(ImVec2(center.x + std::sin(a) * (radius - 4.0f), center.y - std::cos(a) * (radius - 4.0f)),
					ImVec2(center.x + std::sin(a) * (radius - 1.0f), center.y - std::cos(a) * (radius - 1.0f)), PedalInk::Value, 1.5f);
			}
			const float capRadius = radius - 6.0f;
			list->AddCircleFilled(center, capRadius, hovered || active ? Mix(PedalInk::KnobCap, IM_COL32(255, 255, 255, 255), 0.06f) : PedalInk::KnobCap, 32);
			list->AddCircle(center, capRadius, PedalInk::KnobCapEdge, 32, 2.0f);
			const float angle = (-135.0f + 270.0f * t) * 3.14159265f / 180.0f;
			list->AddLine(center, ImVec2(center.x + std::sin(angle) * (capRadius - 3.0f), center.y - std::cos(angle) * (capRadius - 3.0f)),
				PedalInk::Pointer, 3.0f);

			ImFont* caption = GetFonts().caption ? GetFonts().caption : ImGui::GetFont();
			const ImVec2 labelSize = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, label);
			list->AddText(caption, caption->FontSize, ImVec2(center.x - labelSize.x * 0.5f, center.y + radius + 5.0f), PedalInk::Label, label);
			const ImVec2 valueSize = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, valueText);
			list->AddText(caption, caption->FontSize, ImVec2(center.x - valueSize.x * 0.5f, center.y + radius + 5.0f + labelSize.y),
				PedalInk::Value, valueText);
			ImGui::PopID();
			return changed;
		}

		// The pop-out stomp box. Drawn after the main overlay window, so it floats over it and the game.
		void NoiseGatePedalWindow()
		{
			NoiseGatePedalState& gate = g_gatePedal;
			gate.Seed();
			const float width = 340.0f, height = 494.0f;
			ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
			ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - width - 40.0f, 80.0f), ImGuiCond_FirstUseEver);
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(Color::Window));
			ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(Color::CardEdge));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			const bool shown = ImGui::Begin("Noise gate##pedal", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::PopStyleVar(2);
			ImGui::PopStyleColor(2);
			if (!shown) { ImGui::End(); return; }

			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 origin = ImGui::GetWindowPos();
			ImFont* caption = GetFonts().caption ? GetFonts().caption : ImGui::GetFont();
			ImFont* title = GetFonts().title ? GetFonts().title : ImGui::GetFont();

			// Title bar: drag anywhere on the window that is not a control (ImGui moves windows by their background).
			const std::string windowTitle = gate.Title();
			list->AddText(caption, caption->FontSize, ImVec2(origin.x + 14.0f, origin.y + 11.0f), Color::Muted, windowTitle.c_str());
			ImGui::SetCursorScreenPos(ImVec2(origin.x + width - 34.0f, origin.y + 6.0f));
			if (ImGui::InvisibleButton("##close", ImVec2(26.0f, 24.0f))) gate.windowOpen = false;
			const bool closeHovered = ImGui::IsItemHovered();
			Hint("Close the pedal. Its settings stay as they are.");
			DrawIcon(list, ImVec2(origin.x + width - 21.0f, origin.y + 18.0f), Glyph::Close, closeHovered ? Color::Text : Color::Muted);
			list->AddLine(ImVec2(origin.x, origin.y + 36.0f), ImVec2(origin.x + width, origin.y + 36.0f), Color::CardEdge);

			// Whose gate this is: the playing tone's own (with a way back to the global one), or the one all tones share.
			const char* scope = gate.tone.empty() ? "Shared by every tone without its own gate."
				: gate.own ? "Saved with this tone." : "Uses the global gate. Turn a dial to give this tone its own.";
			list->AddText(caption, caption->FontSize, ImVec2(origin.x + 14.0f, origin.y + 46.0f), Color::Muted, scope);
			if (gate.own)
			{
				const char* useGlobal = "Use the global gate";
				const float buttonW = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, useGlobal).x;
				const float scopeW = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, scope).x;
				const ImVec2 at(origin.x + 14.0f + scopeW + 10.0f, origin.y + 46.0f);
				ImGui::SetCursorScreenPos(at);
				if (ImGui::InvisibleButton("##useGlobal", ImVec2(buttonW, caption->FontSize + 2.0f))) BridgeControl(33);
				const bool linkHovered = ImGui::IsItemHovered();
				Hint("This tone stops using its own gate settings and follows the global gate again.");
				list->AddText(caption, caption->FontSize, at, linkHovered ? Color::AccentHi : Color::Accent, useGlobal);
			}

			// Enclosure
			const ImVec2 box(origin.x + 20.0f, origin.y + 74.0f), boxEnd(origin.x + width - 20.0f, origin.y + height - 16.0f);
			list->AddRectFilled(box, boxEnd, PedalInk::Body, 14.0f);
			list->AddRect(box, boxEnd, PedalInk::Edge, 14.0f, 0, 2.0f);
			list->AddText(title, title->FontSize, ImVec2(box.x + 16.0f, box.y + 12.0f), PedalInk::Lettering, "NOISE GATE");

			float meterDb = -120.0f;
			bool gateOpen = false;
			const bool running = Audio::AsioHook::GetNoiseGateMeter(meterDb, gateOpen);
			const ImVec2 led(boxEnd.x - 22.0f, box.y + 22.0f);
			list->AddCircleFilled(led, 7.0f, running && gateOpen && gate.on ? PedalInk::LedOn : PedalInk::LedOff, 20);
			list->AddCircle(led, 7.0f, PedalInk::KnobSkirtEdge, 20, 1.0f);
			ImGui::SetCursorScreenPos(ImVec2(led.x - 9.0f, led.y - 9.0f));
			ImGui::InvisibleButton("##led", ImVec2(18.0f, 18.0f));
			Hint("Lit while the gate is open and your guitar comes through.");

			// Dials, two by two
			const float cell = (boxEnd.x - box.x - 24.0f) / 2.0f;
			char text[32];
			bool commit = false;
			const float rowTop = box.y + 52.0f, rowStep = 102.0f;

			ImGui::SetCursorScreenPos(ImVec2(box.x + 12.0f, rowTop));
			float t = (gate.thresholdTenths / 10.0f + 80.0f) / 60.0f;
			snprintf(text, sizeof(text), "%.1f dB", gate.thresholdTenths / 10.0f);
			if (PedalKnob("threshold", "THRESHOLD", text, t, (-62.0f + 80.0f) / 60.0f, 0.5f / 60.0f, cell, commit,
				"Where it opens. It listens to the guitar's own range, not the hiss or hum. Lower keeps quieter note tails; if noise gets through between notes, raise it a few dB.")) {
				gate.thresholdTenths = std::clamp(static_cast<int>(std::lround((-80.0f + 60.0f * t) * 2.0f)) * 5, -800, -200);
				gate.SendThreshold(false);
			}
			if (commit) gate.SendThreshold(true);

			ImGui::SetCursorScreenPos(ImVec2(box.x + 12.0f + cell, rowTop));
			t = std::log(gate.decayMs / 50.0f) / std::log(40.0f);
			gate.FormatDecay(text, sizeof(text));
			if (PedalKnob("decay", "DECAY", text, t, std::log(200.0f / 50.0f) / std::log(40.0f), 0.02f, cell, commit,
				"How long a note takes to fade once the gate closes.")) {
				gate.decayMs = std::clamp(static_cast<int>(std::lround(50.0f * std::pow(40.0f, t))), 50, 2000);
				gate.SendShape(false);
			}
			if (commit) gate.SendShape(true);

			ImGui::SetCursorScreenPos(ImVec2(box.x + 12.0f, rowTop + rowStep));
			t = (gate.reductionDb + 80.0f) / 70.0f;
			snprintf(text, sizeof(text), "%d dB", gate.reductionDb);
			if (PedalKnob("reduction", "REDUCTION", text, t, 20.0f / 70.0f, 1.0f / 70.0f, cell, commit,
				"How far it turns the noise down. -60 dB is near silence; around -20 dB leaves a faint natural floor.")) {
				gate.reductionDb = std::clamp(static_cast<int>(std::lround(-80.0f + 70.0f * t)), -80, -10);
				gate.SendShape(false);
			}
			if (commit) gate.SendShape(true);

			ImGui::SetCursorScreenPos(ImVec2(box.x + 12.0f + cell, rowTop + rowStep));
			t = std::log(gate.attackTenths / 1.0f) / std::log(200.0f);
			if (gate.attackTenths < 10) snprintf(text, sizeof(text), "%.1f ms", gate.attackTenths / 10.0f);
			else snprintf(text, sizeof(text), "%d ms", static_cast<int>(std::lround(gate.attackTenths / 10.0f)));
			if (PedalKnob("attack", "ATTACK", text, t, std::log(5.0f) / std::log(200.0f), 0.02f, cell, commit,
				"How fast it opens. Keep it short so your pick attack stays sharp.")) {
				gate.attackTenths = std::clamp(static_cast<int>(std::lround(std::pow(200.0f, t))), 1, 200);
				gate.SendShape(false);
			}
			if (commit) gate.SendShape(true);

			// Input meter with the threshold line (-90 dB empty, 0 dB full), on the gate's own detector.
			const float meterTop = rowTop + rowStep * 2.0f + 6.0f;
			list->AddText(caption, caption->FontSize, ImVec2(box.x + 16.0f, meterTop), PedalInk::Label, "Input");
			if (running) snprintf(text, sizeof(text), "%.0f dB", meterDb); else strcpy_s(text, gate.on ? "No signal" : "Off");
			const float textW = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, text).x;
			list->AddText(caption, caption->FontSize, ImVec2(boxEnd.x - 16.0f - textW, meterTop), PedalInk::Label, text);
			const ImVec2 track(box.x + 16.0f, meterTop + caption->FontSize + 5.0f), trackEnd(boxEnd.x - 16.0f, track.y + 10.0f);
			list->AddRectFilled(track, trackEnd, PedalInk::MeterTrack, 5.0f);
			const float fill = running ? std::clamp((meterDb + 90.0f) / 90.0f, 0.0f, 1.0f) : 0.0f;
			if (fill > 0.0f) list->AddRectFilled(track, ImVec2(track.x + (trackEnd.x - track.x) * fill, trackEnd.y), PedalInk::MeterFill, 5.0f);
			const float lineX = track.x + (trackEnd.x - track.x) * std::clamp((gate.thresholdTenths / 10.0f + 90.0f) / 90.0f, 0.0f, 1.0f);
			list->AddRectFilled(ImVec2(lineX - 1.0f, track.y - 2.0f), ImVec2(lineX + 1.0f, trackEnd.y + 2.0f), PedalInk::ThresholdLine);
			ImGui::SetCursorScreenPos(ImVec2(track.x, trackEnd.y + 5.0f));
			ImGui::PushFont(caption);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(PedalInk::Value));
			ImGui::PushTextWrapPos(trackEnd.x);
			ImGui::TextUnformatted("Yellow line = threshold. Set it just above where the meter rests when you're not playing.");
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::PopFont();

			// Footswitch
			const ImVec2 foot(box.x + (boxEnd.x - box.x) * 0.5f, boxEnd.y - 40.0f);
			ImGui::SetCursorScreenPos(ImVec2(foot.x - 28.0f, foot.y - 28.0f));
			if (ImGui::InvisibleButton("##footswitch", ImVec2(56.0f, 56.0f))) { gate.on = !gate.on; gate.SendThreshold(true); }
			const bool footHovered = ImGui::IsItemHovered();
			Hint("Switches the gate on or off.");
			list->AddCircleFilled(foot, 27.0f, gate.on ? PedalInk::Switch : PedalInk::SwitchOff, 32);
			list->AddCircle(foot, 27.0f, footHovered ? PedalInk::Lettering : PedalInk::SwitchOff, 32, 4.0f);
			const char* footText = gate.on ? "ON" : "OFF";
			const ImVec2 footSize = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, footText);
			list->AddText(caption, caption->FontSize, ImVec2(foot.x - footSize.x * 0.5f, foot.y - footSize.y * 0.5f), PedalInk::SwitchText, footText);
			ImGui::End();
		}

		// Player 1's processing: input gain, noise gate, compressor, then the game's own amp gate.
		void PlayerOneProcessing()
		{
			// Seeded once from the keys the DLL applies at launch, then user-driven.
			static bool seeded = false;
			static bool gainOn, compOn, rgOn;
			static int gainTenths, compPct, rgTenths;
			NoiseGatePedalState& gate = g_gatePedal;
			gate.Seed();
			if (!seeded) {
				gainTenths = Settings::GetModSetting("AsioInputGain");        gainOn = gainTenths > 0;
				compPct = Settings::GetModSetting("CompressorStrength");      compOn = compPct > 0;
				rgOn = Settings::GetModSetting("RocksmithGateOverride") != 0; rgTenths = Settings::GetModSetting("RocksmithGateThreshold"); if (rgTenths == 0) rgTenths = -593;
				seeded = true;
			}

			BeginCard("Processing");
			// The capture hook is installed at launch only when one of these is on (ModManager); switching the first one
			// on mid-session has no effect until the game restarts.
			if ((gainOn || gate.on || compOn) && !Audio::AsioHook::InputChainRunning())
				WarnCaption("Not running this launch: restart the game once to start the input processing.");
			// Input make-up gain (op 15, tenths of dB; off = 0).
			if (ToggleRow("Input gain", "Raises a quiet guitar.", &gainOn)) SendInt(15, gainOn ? gainTenths : 0);
			ImGui::BeginDisabled(!gainOn);
			float gainDb = gainTenths / 10.0f;
			bool gainSaved = false;
			if (SliderRow("Gain", &gainDb, 0.0f, 20.0f, "+%.1f dB", &gainSaved, "Input gain from 0 to +20 dB.")) { gainTenths = static_cast<int>(gainDb * 10 + 0.5f); PreviewInt(15, gainTenths); }
			if (gainSaved) SendInt(15, gainTenths);
			ImGui::EndDisabled();

			// Noise gate: one row here, its dials on the pedal pop-out (NoiseGatePedalWindow). Also applied to the amp sim's
			// feed (AmpFeedConditioner.hpp).
			{
				char decay[16], summary[160];
				gate.FormatDecay(decay, sizeof(decay));
				std::string target = gate.tone.empty() ? std::string("All tones") : gate.tone;
				if (target.size() > 24) target = target.substr(0, 22) + "...";
				snprintf(summary, sizeof(summary), "%s: %.1f dB \xC2\xB7 decay %s \xC2\xB7 %d dB", target.c_str(), gate.thresholdTenths / 10.0f, decay, gate.reductionDb);
				if (ToggleRow("Noise gate", gate.on ? summary : "Silent between notes, without cutting your sustain.", &gate.on, "Switches the gate on or off. Open the pedal for its dials."))
					gate.SendThreshold(true);
				if (Button(gate.windowOpen ? "Close pedal" : "Open pedal", false, "The gate's dials: threshold, decay, reduction and attack, with a level meter."))
					gate.windowOpen = !gate.windowOpen;
				float meterDb = -120.0f;
				bool gateOpen = false;
				const bool lit = gate.on && Audio::AsioHook::GetNoiseGateMeter(meterDb, gateOpen) && gateOpen;
				ImGui::SameLine(0.0f, 14.0f);
				const ImVec2 dotAt = ImGui::GetCursorScreenPos();
				const float rowH = ImGui::GetItemRectSize().y;
				ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(dotAt.x + 5.0f, dotAt.y + rowH * 0.5f), 5.0f, lit ? PedalInk::LedOn : PedalInk::LedOff, 16);
				ImGui::Dummy(ImVec2(12.0f, rowH));
				ImGui::SameLine(0.0f, 6.0f);
				ImGui::AlignTextToFramePadding();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Muted));
				ImGui::TextUnformatted(!gate.on ? "Off" : lit ? "Open" : "Closed");
				ImGui::PopStyleColor();
			}

			// Compressor (op 20, 0..100; off = 0).
			if (ToggleRow("Compressor", "Evens out level swings during sustained notes.", &compOn)) SendInt(20, compOn ? compPct : 0);
			ImGui::BeginDisabled(!compOn);
			float comp = static_cast<float>(compPct);
			bool compSaved = false;
			if (SliderRow("Strength", &comp, 0.0f, 100.0f, "%.0f%%", &compSaved)) { compPct = static_cast<int>(comp + 0.5f); PreviewInt(20, compPct); }
			if (compSaved) SendInt(20, compPct);
			ImGui::EndDisabled();
			EndCard();

			// The game's own amp gate (op 23, "<on>,<tenths>"): Player 1's noise floor. While an amp sim plays, the game's
			// amp is held at 0 (ExternalAmp gameAmpMuted), so the gate has nothing to act on.
			const bool ampSimPlays = ExternalAmp::GetSnapshot().gameAmpMuted;
			BeginCard("Rocksmith");
			if (ampSimPlays) Caption("The game's amp is muted while the amp sim plays, so its gate has nothing to do.");
			ImGui::BeginDisabled(ampSimPlays);
			bool rgChanged = ToggleRow("Override Rocksmith's gate", "Stops the game's own amp gate cutting notes as they decay.", &rgOn);
			ImGui::BeginDisabled(!rgOn);
			float rgDb = rgTenths / 10.0f;
			bool rgSaved = false;
			const bool rgSlid = SliderRow("Gate threshold", &rgDb, -100.0f, 10.0f, "%.1f dB", &rgSaved, "Lower values keep the gate open for longer sustain.");
			if (rgSlid) rgTenths = static_cast<int>(rgDb * 10 + (rgDb < 0 ? -0.5f : 0.5f));
			ImGui::EndDisabled();
			if (rgChanged || rgSlid || rgSaved) {
				wchar_t v[24]; swprintf_s(v, L"%d,%d", rgOn ? 1 : 0, rgTenths);
				if (rgChanged || rgSaved) BridgeControl(23, v); else PreviewControl(23, v);
			}
			ImGui::EndDisabled();
			EndCard();
		}

		void InputPage(const LiveState& live)
		{
			PageHeader("Input", "Each player's guitar before Rocksmith hears it. Every change applies live and is saved.");
			const bool asioMode = Audio::CableInput::IsAsioPath();   // RS_ASIO owns the input (else Cable mode)
			const RouteView& view = PolledRouteView();
			static bool p2CableSeeded = false, p2Cable = false;
			if (!p2CableSeeded) { p2Cable = Settings::ReturnSettingValue("CableForPlayerTwo") == "on"; p2CableSeeded = true; }

			// Tabs: Player 1, Player 2, and the mic while RS_ASIO.ini sends it through the bridge. A tab shows a coloured dot
			// only when its input runs on another clock (InputClockState), the Output page's convention.
			static const char* const badges[4] = { "", "P1", "P2", "MIC" };
			static const ImU32 tints[4] = { Color::AccentHi, Color::TintBlue, Color::TintCyan, Color::TintViolet };
			std::vector<int> roles{ 1, 2 };
			if (asioMode && view.viaBridge[3]) roles.push_back(3);
			static int selectedRole = 1;
			std::vector<RoleTab> tabs;
			int tabIndex = 0;
			for (size_t i = 0; i < roles.size(); ++i) {
				const int role = roles[i];
				if (role == selectedRole) tabIndex = static_cast<int>(i);
				const ClockView clock = InputClockState(role);
				RoleTab tab;
				tab.label = kRoleNames[role];
				tab.badge = badges[role];
				tab.tint = tints[role];
				tab.icon = role == 3 ? RoleIcon::Mic : RoleIcon::Guitar;
				tab.hasStatus = clock.shown;
				tab.status = clock.settled ? Color::Good : Color::Warn;
				tabs.push_back(tab);
			}
			if (std::find(roles.begin(), roles.end(), selectedRole) == roles.end()) tabIndex = 0;
			RoleTabs("##input-roles", tabs.data(), static_cast<int>(tabs.size()), &tabIndex);
			selectedRole = roles[static_cast<size_t>(tabIndex)];
			const int role = selectedRole;
			ImGui::PushID(role);

			// What this input is.
			InputSource source = InputSource::None;
			if (role == 1) {
				if (!asioMode) source = InputSource::Cable;
				// The bridge on its virtual clock (no interface open) plays the Real Tone Cable as Player 1. Not ProxyInputMode:
				// that is 2 for a real ASIO stream as well.
				else if (Audio::OutputTap::ProxyOutputMode() == 1) source = InputSource::CableStandIn;
				else source = InputSource::Asio;
			}
			else if (role == 2) {
				if (!asioMode) source = InputSource::GameCable;
				else if (p2Cable) source = InputSource::Cable;
				else if (view.viaBridge[2]) source = InputSource::Asio;
			}
			else source = InputSource::Asio;

			std::string line;
			if (source == InputSource::Asio && !view.viaBridge[role]) line = "ASIO input set in RS_ASIO.ini";
			else if (source == InputSource::Asio) {
				const std::wstring iface = view.interfaceOf[role].empty() ? ReadPassthroughOutputName() : view.interfaceOf[role];
				const std::vector<std::string>& names = view.names[role];
				line = Utf8(iface);
				if (view.device[role] >= 0 && view.device[role] < static_cast<long>(names.size())) line += ", " + names[static_cast<size_t>(view.device[role])];
			}
			// Cable mode Player 1 may be another cable or recording device (CableCaptureSession.hpp, GetPlayerOneInput).
			const Audio::PersistentInput::PlayerOneInput playerOneInput = Audio::PersistentInput::GetPlayerOneInput();
			const bool playerOneOtherDevice = source == InputSource::Cable && role == 1
				&& (playerOneInput.kind == Audio::PersistentInput::PlayerOneInputKind::ChosenDevice
					|| playerOneInput.kind == Audio::PersistentInput::PlayerOneInputKind::WindowsDefault);
			if (playerOneOtherDevice) line = Utf8(playerOneInput.name)
				+ (playerOneInput.kind == Audio::PersistentInput::PlayerOneInputKind::WindowsDefault ? " (Windows default), through Windows" : ", through Windows");
			else if (source == InputSource::Cable) line = role == 1 ? (CableAmpLink::IsDirectUsb() ? "Real Tone Cable, read directly over USB"
				: Audio::AsioHook::IsInputReady(0) ? "Real Tone Cable, through Windows" : "Real Tone Cable, waiting for input") : "Real Tone Cable, added for Player 2";
			else if (source == InputSource::CableStandIn) line = "Real Tone Cable, while no ASIO input is open";
			else if (source == InputSource::GameCable) line = "A second Real Tone Cable, opened by Rocksmith";
			if (source != InputSource::None) {
				BeginCard();
				StatusPill(source == InputSource::Asio ? "ASIO" : playerOneOtherDevice ? "WINDOWS INPUT" : "REAL TONE CABLE", source == InputSource::Asio ? Color::Good : Color::AccentHi);
				ImGui::SameLine(0.0f, 12.0f);
				ImGui::TextUnformatted(line.c_str());
				EndCard();
			}

			// Level.
			if (role != 3) {
				BeginCard("Level");
				Meter(kRoleNames[role], MeterFraction(role == 1 ? live.input.meterPeak : live.input.playerTwoMeterPeak), role == 1 ? "Player 1's level after the input gain." : "Player 2's level.");
				EndCard();
			}

			// Source settings.
			if (role == 2 && asioMode) {
				BeginCard("Source");
				// Op 26: the cable after the ASIO input, as Player 2 (the hot-swap device, always listed; the switch gates it).
				if (ToggleRow("Real Tone Cable for Player 2", "Player 2 plays on a Real Tone Cable instead of an ASIO input. RS_ASIO.ini is not changed.", &p2Cable))
					BridgeControl(26, p2Cable ? L"1" : L"0");
				if (!p2Cable && view.viaBridge[2]) InputSourceRows(2, view);
				else if (!p2Cable) Caption("Player 2 has no ASIO input: RS_ASIO.ini does not send Player 2 to Rocksmith Audio Bridge ASIO. Switch on the cable above, or add Player 2 there.");
				EndCard();
			}
			else if (source == InputSource::Asio) {
				BeginCard("Source");
				if (view.viaBridge[role]) InputSourceRows(role, view);
				else Caption((std::string(kRoleNames[role]) + "'s input is set in RS_ASIO.ini, not through Rocksmith Audio Bridge ASIO. Send it to Rocksmith Audio Bridge ASIO there to pick its interface and channel here.").c_str());
				EndCard();
			}
			else if (source == InputSource::Cable && role == 1) {
				BeginCard("Source");
				PlayerOneCableInputRows();
				InfoRow("Read", CableAmpLink::IsDirectUsb() ? "Directly over USB, about 1 ms packets"
					: Audio::AsioHook::IsInputReady(0) ? "Through Windows, 10 ms packets" : "No active cable capture");
				EndCard();
			}
			else if (source == InputSource::CableStandIn) {
				BeginCard("Source");
				Caption("No ASIO input is open, so the bridge plays the Real Tone Cable as Player 1. Pick an interface in Switch output on the Output page to use it instead.");
				EndCard();
			}
			else if (source == InputSource::GameCable) {
				BeginCard("Source");
				Caption("In Cable mode Rocksmith opens a second Real Tone Cable for Player 2 itself.");
				EndCard();
			}

			// Processing.
			if (role == 1) PlayerOneProcessing();
			else if (role == 2) {
				BeginCard("Processing");
				Caption("Player 2's own input gain, noise suppressor and compressor are coming in the next build.");
				EndCard();
			}
			ImGui::PopID();
		}

		void OutputPage(const LiveState& live)
		{
			PageHeader("Output", "Where the game's sound goes and how loud it can get.");
			OutputDeviceCard(live);
			OutputPairCard();
			OutputBufferCard(live);
			GameBlockCard(live);

			static bool seeded = false;
			static bool limOn = false;
			static int limTenths = -60;
			if (!seeded) {
				limOn = Settings::GetModSetting("AudioBridgeLimiter") != 0;
				limTenths = Settings::GetModSetting("AudioBridgeLimiterLevel"); if (limTenths == 0) limTenths = -60;
				seeded = true;
			}

			BeginCard("Volume cap");
			// Output protection (op 16; ceiling sent linear = pow(10, tenths/200)).
			const bool limChanged = ToggleRow("Cap the maximum volume",
				"A look-ahead limiter eases the level down before anything louder than the ceiling arrives. Adds a few milliseconds of latency.",
				&limOn);
			ImGui::BeginDisabled(!limOn);
			float ceiling = limTenths / 10.0f;
			bool ceilingSaved = false;
			const bool slid = SliderRow("Ceiling", &ceiling, -24.0f, 0.0f, "%.1f dBFS", &ceilingSaved,
				"Nothing in the game's output rises above this. Your interface volume still sets how loud that actually is.");
			if (slid) limTenths = static_cast<int>(ceiling * 10 + (ceiling < 0 ? -0.5f : 0.5f));
			ImGui::EndDisabled();
			if (limChanged || slid || ceilingSaved) {
				wchar_t v[48]; swprintf_s(v, L"%d,%.6f,0,0.1", limOn ? 1 : 0, std::pow(10.0, limTenths / 200.0));
				if (limChanged || ceilingSaved) BridgeControl(16, v); else PreviewControl(16, v);
			}
			EndCard();
		}

		// Round record button: red dot when idle, rounded stop square with a pulsing ring while recording.
		bool RecordButton(bool recording)
		{
			const float size = 72.0f;
			const ImVec2 start = ImGui::GetCursorScreenPos();
			const bool pressed = ImGui::InvisibleButton("##record", ImVec2(size, size));
			const bool hovered = ImGui::IsItemHovered();
			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 c(start.x + size * 0.5f, start.y + size * 0.5f);
			list->AddCircleFilled(c, size * 0.5f, hovered ? Color::Hover : Color::Track, 40);
			if (recording) {
				const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
				list->AddCircle(c, size * 0.5f - 1.0f, Mix(IM_COL32(239, 68, 68, 60), Color::Record, pulse), 40, 3.0f);
				const float h = size * 0.18f;
				list->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), Color::Record, 4.0f);
			}
			else {
				list->AddCircleFilled(c, size * (hovered ? 0.26f : 0.24f), Color::Record, 32);
			}
			return pressed;
		}

		void RecordPage(const LiveState& live)
		{
			PageHeader("Record", "Capture a take without leaving the game.");
			const bool recording = live.status.recording != 0;
			const Audio::Takes::Status take = Audio::Takes::GetStatus();
			// PreferVideo reads the ini: once a second here, not every frame, and again after this page changes it.
			static int cachedFormat = -1;
			static ULONGLONG formatReadAt = 0;
			if (cachedFormat < 0 || GetTickCount64() - formatReadAt >= 1000) { cachedFormat = Audio::Takes::PreferVideo() ? 1 : 0; formatReadAt = GetTickCount64(); }
			int format = cachedFormat;

			BeginCard();
			if (RecordButton(recording)) Keybindings::ToggleAudioBridgeRecording(format == 1);
			Hint("Starts or stops a take. Same as the record hotkey.");
			ImGui::SameLine(0.0f, 20.0f);
			ImGui::BeginGroup();
			ImGui::PushFont(GetFonts().big);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(recording ? Color::Text : Color::Faint));
			ImGui::TextUnformatted(FormatTime(live.status.recordedFrames / 48000.0).c_str());
			ImGui::PopStyleColor();
			ImGui::PopFont();
			using Audio::Takes::VideoPhase;
			if (take.video == VideoPhase::Starting) StatusPill("Starting video", Color::Warn, true);
			else if (take.video == VideoPhase::Capturing && recording) StatusPill("Recording video", Color::Record, true);
			else if (take.video == VideoPhase::Finishing) StatusPill("Saving video", Color::Warn, true);
			else if (recording) StatusPill("Recording audio", Color::Record, true);
			else StatusPill("Ready", Color::Muted);
			ImGui::EndGroup();

			ImGui::Dummy(ImVec2(0, 4));
			static const char* const formats[] = { "Audio", "Video" };
			ImGui::BeginDisabled(recording);
			if (SegmentedRow("Format", &format, formats, 2, "Audio saves WAVs. Video also records the Rocksmith window into an MP4 with the game sound."))
			{
				Audio::Takes::SetPreferVideo(format == 1);
				cachedFormat = format;
			}
			if (format == 1)
			{
				static int cachedQuality = -1;
				static ULONGLONG qualityReadAt = 0;
				if (cachedQuality < 0 || GetTickCount64() - qualityReadAt >= 1000) { cachedQuality = Audio::Takes::VideoQuality(); qualityReadAt = GetTickCount64(); }
				int quality = cachedQuality;
				static const char* const qualities[] = { "High", "Standard", "Small" };
				if (SegmentedRow("Quality", &quality, qualities, 3, "High is the sharpest (about 250 MB a minute at 1440p). Standard is about half that. Small records at 1080p, about 80 MB a minute, for sharing."))
				{
					Audio::Takes::SetVideoQuality(quality);
					cachedQuality = quality;
				}
			}
			ImGui::EndDisabled();
			Caption("The full game mix and Player 1's dry guitar are always saved together as a pair.");
			if (!take.message.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(take.messageIsError ? Color::Bad : Color::Good));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
				ImGui::TextUnformatted(take.message.c_str());
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}
			if (FAILED(live.status.recordingError)) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Bad));
				ImGui::Text("Recording error 0x%08X", static_cast<unsigned>(live.status.recordingError));
				ImGui::PopStyleColor();
			}
			EndCard();

			BeginCard("Saving");
			// Folder() reads the ini and the known-folder API; once a second is plenty for a label.
			static std::wstring cachedFolder;
			static ULONGLONG folderReadAt = 0;
			if (folderReadAt == 0 || GetTickCount64() - folderReadAt >= 1000) { cachedFolder = Audio::Takes::Folder(); folderReadAt = GetTickCount64(); }
			const std::wstring folder = cachedFolder;
			ImGui::TextUnformatted(Utf8(folder).c_str());
			if (Button("Open folder", false, "Opens behind the game, so alt-tab to see it.")) {
				// This launch's session folder once it has a take in it, otherwise the folder of all sessions.
				const std::wstring session = Audio::Takes::SessionFolder();
				std::error_code ec;
				const bool hasSession = std::filesystem::is_directory(session, ec);
				if (!hasSession) std::filesystem::create_directories(folder, ec);
				ShellOpenAsync::Open(hasSession ? session : folder);   // off the render thread (a 103 ms frame here)
			}
			RecordHotkeyPicker();
			Caption("Each game launch saves into its own dated folder. The hotkey uses the format chosen above. Change the folder in RSMods, Rocksmith Audio Bridge page.");
			EndCard();
		}

		void GeneralPage()
		{
			PageHeader("General", "Overlays and library tools.");

			// Here rather than under Amp sim: it silences the game's amp, Tone designer and an external app alike.
			BeginCard("Tuner");
			bool silenceTuning = ExternalAmp::GetSnapshot().silenceWhileTuning;
			if (ToggleRow("Silence guitar while tuning", "No guitar sound in the tuner screens, whichever amp plays it. The tuner still hears you.", &silenceTuning))
				ExternalAmp::SetSilenceWhileTuning(silenceTuning);
			EndCard();

			BeginCard("Display");
			bool diagnostics = Audio::CableInput::IsOverlayEnabled();
			if (ToggleRow("Audio diagnostics HUD", "Signal level, packet rate and route state in the top right corner.", &diagnostics))
				BridgeControl(27, diagnostics ? L"1" : L"0");
			bool latencyReadout = IsLatencyReadoutEnabled();
			if (ToggleRow("Latency readout", "Shows a compact estimate below SIGNAL.", &latencyReadout,
				"Requires Audio diagnostics HUD. Software timing plus reported output delay; cable hardware and USB delivery are excluded. Hidden when no estimate is available."))
				SetLatencyReadoutEnabled(latencyReadout);
			EndCard();

			// ASIO mode's measured latency (LatencyCalibration.hpp). The game measurement runs by itself once per setup;
			// Recalibrate runs it again. The interface's own delay needs a cable looped from its headphone jack into its guitar input, and
			// the output stays muted afterwards until the player says the cable is out (a guitar input patched to the
			// output would feed back through the game's amp).
			BeginCard("Latency");
			{
				static std::string refused;
				static bool cableSteps = false;
				const bool available = Audio::LatencyCalibration::Available();
				const bool running = Audio::LatencyCalibration::Running();
				const bool holding = Audio::LatencyCalibration::HoldingMuted();
				if (!available && !holding) Caption("Measuring works with an ASIO interface. In Cable mode the game tones figure is estimated from your playing.");
				else {
					Caption(Audio::LatencyCalibration::Status().c_str());
					if (holding) {
						Caption("Rocksmith is muted. Take the cable out of the headphone jack, plug your guitar back in, then press Cable out.");
						if (Button("Cable out", true)) { Audio::LatencyCalibration::CableOut(); cableSteps = false; }
					}
					else {
						ImGui::BeginDisabled(running);
						if (Button(running ? "Measuring..." : "Recalibrate", false,
							"Measures again what the game's amp adds (and the Tone designer, if it is playing) by sending three short chirps through it in place of your guitar. About a second of silence; it waits until you stop playing. It also runs by itself once for each interface, buffer size and sample rate, in the menus.")) {
							refused.clear();
							Audio::LatencyCalibration::Recalibrate(refused);
						}
						ImGui::SameLine();
						if (Button("Measure the interface...", false, "Times your interface's own input and output delay by sending a sound out of the headphone jack and back into the guitar input through a cable, instead of trusting what its driver reports. Once per interface and buffer size."))
							cableSteps = !cableSteps;
						ImGui::EndDisabled();
						if (cableSteps && !running) {
							Caption("This needs no guitar: the interface plays a test sound into a cable that loops straight back into it.");
							Caption("1. Unplug the guitar from its cable. Plug that free end into the interface's headphone jack (or line out). The cable now runs from the headphone jack into the guitar input: a closed loop.");
							Caption("2. Headphone volume about halfway. Direct monitor fully to playback (USB), or the loop will feed back.");
							Caption("3. Press Start. Rocksmith stays muted until you press Cable out.");
							if (Button("Start", true)) {
								refused.clear();
								Audio::LatencyCalibration::MeasureHardware(refused);
							}
							if (Audio::LatencyCalibration::Current().roundTripMs >= 0.0f) {
								ImGui::SameLine();
								if (Button("Use the driver's figures", false, "Forget the cable measurement for this interface and show the delays its driver reports."))
									Audio::LatencyCalibration::ForgetHardware();
							}
						}
					}
				}
				if (!refused.empty()) Caption(refused.c_str());
			}
			EndCard();

			BeginCard("Library");
			Caption("Picks up newly added CDLC without restarting. Open the song list once first if nothing changes.");
			if (Button("Update song list")) BridgeControl(28);
			EndCard();
		}

		// Master (RSMODS_PUBLIC_RELEASE) is what users get: no Debug page, no label. Debug and Release builds have the
		// Debug page and name themselves in the header, in amber, so a test build is never mistaken for the release;
		// the label also lists any build extensions compiled in (Extensions.hpp).
#if defined(RSMODS_PUBLIC_RELEASE)
		constexpr bool kDebugPage = false;
		const std::string& BuildLabel() { static const std::string label; return label; }
#else
		constexpr bool kDebugPage = true;
		const std::string& BuildLabel()
		{
#if defined(_DEBUG)
			static const std::string base = "Debug build";
#else
			static const std::string base = "Release build";
#endif
			static const std::string label = Extensions::Any() ? base + " + " + Extensions::Names() : base;
			return label;
		}
#endif

		void DebugPage()
		{
			PageHeader("Debug", "Development tools. Not in public releases.");
			Extensions::DrawOverlayDebugPage();

			// ML service status + restart. The launcher runs in this process, so this reads its state directly.
			// Status is re-read twice a second; opening the result mappings every frame is wasteful.
			{
				static std::string mlMessage;
				static bool mlConnected = false, mlCanRestart = false;
				static uint64_t mlNextPoll = 0;
				const uint64_t now = GetTickCount64();
				if (now >= mlNextPoll) {
					MlServiceLauncher::Describe(mlMessage, mlConnected, mlCanRestart);
					mlNextPoll = now + 500;
				}
				BeginCard("ML note detection");
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(mlConnected ? Color::Good : Color::Muted));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
				ImGui::TextUnformatted(mlMessage.c_str());
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
				ImGui::BeginDisabled(!mlCanRestart);
				if (Button("Restart ML service", false, "Restarts the FretNet service. Note detection keeps running on native and tier-0 meanwhile.")) {
					MlServiceLauncher::RequestRestart();
					mlNextPoll = 0;
				}
				ImGui::EndDisabled();
				EndCard();
			}

		}

		// ---- Chrome -----------------------------------------------------------------------------------------

		constexpr float kHeaderHeight = 58.0f;
		constexpr float kSidebarWidth = 208.0f;

		void LatencyReadout(float right, float top);

		void Header(const LiveState& live, bool* open)
		{
			const ImVec2 origin = ImGui::GetWindowPos();
			const float width = ImGui::GetWindowWidth();
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + kHeaderHeight), Color::Header, ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersTop);
			list->AddLine(ImVec2(origin.x, origin.y + kHeaderHeight - 1.0f), ImVec2(origin.x + width, origin.y + kHeaderHeight - 1.0f), Color::CardEdge);

			// Brand mark: the Rocksmith Audio Bridge logo (same art as the desktop app icon), then the name.
			const float markSize = 34.0f;
			const ImVec2 mark(origin.x + 16.0f, origin.y + (kHeaderHeight - markSize) * 0.5f);
			if (ImTextureID logo = LogoTexture())
				list->AddImage(logo, mark, ImVec2(mark.x + markSize, mark.y + markSize));
			else {
				list->AddRectFilled(mark, ImVec2(mark.x + markSize, mark.y + markSize), Color::Accent, 7.0f);
				DrawIcon(list, ImVec2(mark.x + markSize * 0.5f, mark.y + markSize * 0.5f), Glyph::MusicNote, Color::Text);
			}
			ImGui::SetCursorScreenPos(ImVec2(mark.x + markSize + 12.0f, origin.y + 10.0f));
			ImGui::BeginGroup();
			ImGui::TextUnformatted("Rocksmith Audio Bridge");
			ImGui::PushFont(GetFonts().caption);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Muted));
			ImGui::TextUnformatted("In-game controls");
			ImGui::PopStyleColor();
			if (!BuildLabel().empty()) {
				// Non-public builds say what they are, in amber so a test build is never mistaken for the release.
				ImGui::SameLine(0.0f, 10.0f);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
				ImGui::TextUnformatted(BuildLabel().c_str());
				ImGui::PopStyleColor();
			}
			ImGui::PopFont();
			ImGui::EndGroup();

			// Right side: recording indicator (visible from any page), then close.
			const float closeSize = 32.0f;
			float right = origin.x + width - 14.0f - closeSize;
			ImGui::SetCursorScreenPos(ImVec2(right, origin.y + (kHeaderHeight - closeSize) * 0.5f));
			if (IconButton("##close", Glyph::Close, closeSize)) *open = false;
			Hint("Close. Press \\ to open it again.");
			if (live.status.recording) {
				const std::string text = "REC " + FormatTime(live.status.recordedFrames / 48000.0);
				ImGui::PushFont(GetFonts().caption);
				const float pillW = ImGui::CalcTextSize(text.c_str()).x + 30.0f;
				ImGui::PopFont();
				right -= 12.0f + pillW;
				ImGui::SetCursorScreenPos(ImVec2(right, origin.y + 19.0f));
				StatusPill(text.c_str(), Color::Record, true);
			}
			LatencyReadout(right, origin.y);
		}

		// ASIO mode's figures (LatencyCalibration::SplitAsio, shared with the HUD): the interface's round trip, measured
		// with a patch cable or else the driver's own figures, plus each path's processing, measured or else the amp
		// link's blocks / the game's estimate (picks in the last minute, else GameToneLatency::GameOwnMs). Rounded once
		// and subtracted so the parts add up, like CableSplit.
		struct AsioSplit { int input, ampOutput, ampTotal, gameTotal; const char* ampLabel; const char* gameLabel; };
		AsioSplit SplitAsioLatency(const ExternalAmp::Snapshot& amp)
		{
			const AmpLink::Status& link = amp.link;
			const int rate = link.sampleRate > 0 ? link.sampleRate : AmpLink::kSampleRate;
			const float linkMs = link.frames > 0 ? static_cast<float>(link.frames * (link.latencyBlocks > 1 ? 2 : 1) * 1000.0 / rate) : 0.0f;
			const float blockMs = static_cast<float>((link.frames > 0 ? link.frames : 128) * 1000.0 / rate);
			const bool picks = Audio::GameToneLatency::Valid();
			const float gameOwn = picks ? Audio::GameToneLatency::MedianMs() : Audio::GameToneLatency::GameOwnMs(blockMs);
			const auto split = Audio::LatencyCalibration::SplitAsio(amp.asioInMs, amp.asioOutMs, linkMs, gameOwn, !picks && Audio::GameToneLatency::GameOwnGuessed());
			const int input = static_cast<int>(std::lround(split.inMs));
			const int ampTotal = static_cast<int>(std::lround(split.ampTotalMs));
			return { input, ampTotal - input, ampTotal, static_cast<int>(std::lround(split.gameTotalMs)),
				Audio::LatencyCalibration::Label(split.amp), Audio::LatencyCalibration::Label(split.game) };
		}

		// Guitar to ear, on every page, the one being heard brighter. Cable mode: the Tone designer's delay (measured
		// per sample, CableAmpLink) and the game tones' (measured from picks and attacks, GameToneLatency), both timed
		// from the cable. ASIO mode: nothing is timed; the driver's own reported latency, plus the amp link's blocks for
		// the amp sim, or the game's own processing for the game tones (estimated from the cable, else guessed).
		void LatencyReadout(float right, float top)
		{
			const ExternalAmp::Snapshot amp = ExternalAmp::GetSnapshot();
			const bool asio = !amp.cableHost && amp.asioInMs + amp.asioOutMs > 0.0f;
			if (!amp.cableHost && !asio) return;
			char designer[112], game[48];
			bool toneDesignerPlays = amp.toneDesigner && amp.enabled && !amp.comparing;
			if (asio) {
				const AsioSplit split = SplitAsioLatency(amp);
				const bool ampSim = amp.link.guestPresent && !amp.toneDesigner;
				toneDesignerPlays = amp.link.connected && !amp.comparing;
				sprintf_s(designer, "%s in %d  out %d  total %d ms (%s)", ampSim ? "Amp sim" : "Tone designer", split.input, split.ampOutput, split.ampTotal, split.ampLabel);
				sprintf_s(game, "Game tones ~%d ms (%s)", split.gameTotal, split.gameLabel);
			}
			else if (amp.cableLatencyMs > 0.0f) {
				const CableSplit split = SplitCableLatency(amp);
				sprintf_s(designer, "Tone designer in %d  out %d  total %d ms", split.input, split.output, split.total);
			}
			else sprintf_s(designer, "Tone designer -");
			// Measured now, or the remembered game part plus the live output part (e.g. while the Tone designer plays).
			// Both are worked out from picks, so the figure always reads as an estimate.
			if (asio) {}
			else if (Audio::GameToneLatency::Valid()) sprintf_s(game, "Game tones ~%.0f ms (est.)", Audio::GameToneLatency::MedianMs());
			else if (Audio::GameToneLatency::Available())
				sprintf_s(game, Audio::GameToneLatency::Guessed() ? "Game tones ~%.0f ms (guess)" : "Game tones ~%.0f ms (est.)", Audio::GameToneLatency::CurrentMs());
			else sprintf_s(game, "Game tones -");
			ImGui::PushFont(GetFonts().caption);
			const char* title = "LATENCY ESTIMATE";
			const float gap = 14.0f;
			const float designerW = ImGui::CalcTextSize(designer).x, gameW = ImGui::CalcTextSize(game).x;
			const float x = right - 18.0f - designerW - gap - gameW;
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->AddText(ImVec2(x, top + 12.0f), Color::Muted, title);
			list->AddText(ImVec2(x, top + 30.0f), toneDesignerPlays ? Color::Text : Color::Muted, designer);
			list->AddText(ImVec2(x + designerW + gap, top + 30.0f), toneDesignerPlays ? Color::Muted : Color::Text, game);
			ImGui::PopFont();
			ImGui::SetCursorScreenPos(ImVec2(x, top + 10.0f));
			ImGui::InvisibleButton("##latency", ImVec2(designerW + gap + gameW, 38.0f));
			if (asio) Hint("Guitar to ear in ASIO mode. Input and output are your interface's: measured with a cable looped from the headphone jack into the guitar input (General > Latency), else the delays its driver reports. The total adds what happens inside the PC: the game's amp or the Tone designer, measured on this setup by sending short chirps through it (once, by itself, in the menus; Recalibrate in General > Latency), else estimated. \"measured\" means every part was measured; \"est.\" that some part is the driver's word or an estimate.");
			else Hint("Latency estimates: software timing plus the output driver's reported delay. Input is how long the guitar waits for each packet from the cable (about 1 ms read directly over USB, 10 ms through Windows), plus any measured wait when packets arrive in batches; output is the Tone designer, its buffers and the output device (exclusive mode lowers this part); total is the two together. Tone designer tracks audio blocks; game tones are a rougher estimate, matching your picks to output attacks; until a few picks have been timed with the game amp playing, the game tones figure is a guess from the game block size. The cable's own converter and the USB bus are excluded. This is not a calibrated guitar-to-ear measurement.");
		}

		void Sidebar(Page& page)
		{
			const ImVec2 origin = ImGui::GetWindowPos();
			ImGui::GetWindowDrawList()->AddRectFilled(origin, ImVec2(origin.x + ImGui::GetWindowWidth(), origin.y + ImGui::GetWindowHeight()), Color::Sidebar);
			ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x + ImGui::GetWindowWidth() - 1.0f, origin.y),
				ImVec2(origin.x + ImGui::GetWindowWidth() - 1.0f, origin.y + ImGui::GetWindowHeight()), Color::CardEdge);

			struct Entry { const char* group; unsigned glyph; const char* label; Page page; };
			static const Entry entries[] = {
				{ "Practice", Glyph::MusicNote, "Note by Note", Page::NoteByNote },
				{ nullptr, Glyph::Pedal, "Drop Pedal", Page::DropPedal },
				{ "Audio", Glyph::Mixer, "Mixer", Page::Mixer },
				{ nullptr, Glyph::Input, "Input", Page::Input },
				{ nullptr, Glyph::Library, "Amp sim", Page::AmpSim },
				{ nullptr, Glyph::Speaker, "Output", Page::Output },
				{ nullptr, Glyph::Record, "Record", Page::Record },
				{ "System", Glyph::Settings, "General", Page::General },
				{ nullptr, Glyph::Bug, "Debug", Page::Debug },
			};
			const bool externalAmpAvailable = ExternalAmp::GetSnapshot().proxyLoaded;
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
			for (const Entry& entry : entries) {
				if (entry.page == Page::Debug && !kDebugPage) continue;
				// External amp needs the Audio Bridge ASIO driver in the game (ASIO mode); in Cable mode
				// there is nothing for an amp sim to connect to, so the page is not offered.
				if (entry.page == Page::AmpSim && !externalAmpAvailable) continue;
				if (entry.group) { ImGui::Dummy(ImVec2(0, 8)); ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f); SectionLabel(entry.group); ImGui::Dummy(ImVec2(0, 2)); }
				if (NavItem(entry.glyph, entry.label, page == entry.page)) page = entry.page;
			}
			ImGui::PopStyleVar();

			ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 36.0f);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
			ImGui::PushFont(GetFonts().caption);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Faint));
			ImGui::TextUnformatted("Press \\ to hide");
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}
	}

	namespace { const char* g_pageName = "none"; }
	bool IsLatencyReadoutEnabled()
	{
		return LatencyReadoutSetting().load(std::memory_order_relaxed);
	}

	bool SetLatencyReadoutEnabled(bool enabled)
	{
		HKEY key = nullptr;
		LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\Overlay", 0, nullptr, 0,
			KEY_SET_VALUE, nullptr, &key, nullptr);
		if (result == ERROR_SUCCESS)
		{
			const DWORD value = enabled ? 1 : 0;
			result = RegSetValueExW(key, L"LatencyReadout", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
			RegCloseKey(key);
		}
		if (result != ERROR_SUCCESS)
		{
			LOG_WARNING("(OVERLAY) Could not save latency readout setting, error " << result << std::endl);
			return false;
		}
		LatencyReadoutSetting().store(enabled, std::memory_order_relaxed);
		return true;
	}

	const char* CurrentPageName() { return g_pageName; }

	void DrawShell(bool* open)
	{
		static bool themed = false;
		if (!themed) { ApplyTheme(); themed = true; }
		static Page page = Page::NoteByNote;
#ifdef OVERLAY_PREVIEW
		// Offscreen preview build only (not built into the DLL): renders each page in turn.
		extern int g_overlayPreviewPage;
		if (g_overlayPreviewPage >= 0) page = static_cast<Page>(g_overlayPreviewPage);
#endif

		const Fonts& fonts = GetFonts();
		if (fonts.body) ImGui::PushFont(fonts.body);
		// The height follows the open page so everything shows without scrolling: last frame's measured page
		// height (plus header), never shorter than the sidebar, capped at 92% of the screen, eased so switching
		// pages does not jump. Width stays user-resizable (remembered in imgui.ini). If the window would run off
		// the bottom of the screen it is moved up.
		// A page that fills the window (g_pageFillsHeight, Tone designer) instead lets the height be dragged too, and
		// fills whatever it is. Until it is dragged it opens at the page's natural height (g_pageNaturalHeight: the
		// board, or a long tone list, under the same 92% cap) rather than the cap itself, which left a tall empty
		// library column; once dragged, that height is remembered for the session and eased to when the page opens.
		static float pageContentHeight = 0.0f;
		static float fillContentHeight = 0.0f;
		static float shownHeight = 0.0f;
		static float fillHeight = 0.0f;
		static bool fillDragged = false;   // the fill page's height was set by dragging the edge
		static bool fillPage = false;   // last frame's page filled the window
		static ImVec2 lastPosition(-1.0f, -1.0f);
		const ImGuiIO& io = ImGui::GetIO();
		const float sidebarMinimum = 520.0f;   // nav items plus the "Press \ to hide" footer
		const float naturalFill = std::min(kHeaderHeight + std::max(fillContentHeight, sidebarMinimum), io.DisplaySize.y * 0.92f);
		fillHeight = std::clamp(fillDragged ? fillHeight : naturalFill, kHeaderHeight + sidebarMinimum, io.DisplaySize.y);
		const float wanted = fillPage ? fillHeight : std::min(kHeaderHeight + std::max(pageContentHeight, sidebarMinimum), io.DisplaySize.y * 0.92f);
		shownHeight = shownHeight <= 0.0f ? wanted : shownHeight + (wanted - shownHeight) * std::min(1.0f, io.DeltaTime * 14.0f);
		if (std::fabs(wanted - shownHeight) < 0.5f) shownHeight = wanted;
		const bool userHeight = fillPage && shownHeight == wanted;   // settled on a fill page: the height is the user's
		ImGui::SetNextWindowSize(ImVec2(900, shownHeight), ImGuiCond_FirstUseEver);
		if (userHeight) ImGui::SetNextWindowSizeConstraints(ImVec2(700, kHeaderHeight + sidebarMinimum), ImVec2(4000, io.DisplaySize.y));
		else ImGui::SetNextWindowSizeConstraints(ImVec2(700, shownHeight), ImVec2(4000, shownHeight));
		if (lastPosition.y >= 0.0f && lastPosition.y + shownHeight > io.DisplaySize.y - 8.0f)
			ImGui::SetNextWindowPos(ImVec2(lastPosition.x, std::max(8.0f, io.DisplaySize.y - shownHeight - 8.0f)));
		const bool visible = ImGui::Begin("Rocksmith Audio Bridge##overlay", open,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		if (visible) {
			if (userHeight && std::fabs(ImGui::GetWindowHeight() - shownHeight) >= 0.5f) {   // follows a drag of the edge
				fillDragged = true;
				fillHeight = shownHeight = ImGui::GetWindowHeight();
			}
			const LiveState& live = Poll();
			Header(live, open);

			ImGui::SetCursorPos(ImVec2(0, kHeaderHeight));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
			ImGui::BeginChild("##sidebar", ImVec2(kSidebarWidth, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);
			Sidebar(page);
			ImGui::EndChild();
			ImGui::PopStyleVar();

			ImGui::SameLine(0.0f, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26, 20));
			ImGui::BeginChild("##page", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
			// Left on the Amp sim page when the driver is not loaded (it is hidden from the sidebar then).
			if (page == Page::AmpSim && !ExternalAmp::GetSnapshot().proxyLoaded) page = Page::NoteByNote;
			g_pageFillsHeight = false;
			switch (page) {
			case Page::NoteByNote: NoteByNotePage(); break;
			case Page::Mixer: MixerPage(live); break;
			case Page::Input: InputPage(live); break;
			case Page::Output: OutputPage(live); break;
			case Page::Record: RecordPage(live); break;
			case Page::General: GeneralPage(); break;
			case Page::Debug: DebugPage(); break;
			case Page::DropPedal: DropPedalPage(); break;
			case Page::AmpSim: AmpSimPage(); break;
			}
			static const char* const pageNames[] = { "Note by Note", "Mixer", "Input", "Output", "Record", "General", "Debug", "Drop Pedal", "Amp sim" };
			g_pageName = pageNames[static_cast<int>(page)];
			// Content plus bottom padding, for next frame's height; a page that fills the window gets the most allowed.
			fillPage = g_pageFillsHeight;
			if (!fillPage) pageContentHeight = ImGui::GetCursorPosY() + 20.0f;
			else fillContentHeight = g_pageNaturalHeight;
			ImGui::EndChild();
			ImGui::PopStyleVar();

			// Product version in the bottom-right corner, drawn straight onto the window so it takes no layout space.
			ImGui::PushFont(GetFonts().caption);
			static const std::string versionLabel = std::string("v") + ProductVersion::VERSION;
			const ImVec2 versionSize = ImGui::CalcTextSize(versionLabel.c_str());
			const ImVec2 windowPos = ImGui::GetWindowPos();
			const ImVec2 windowSize = ImGui::GetWindowSize();
			ImGui::GetWindowDrawList()->AddText(ImVec2(windowPos.x + windowSize.x - versionSize.x - 14.0f, windowPos.y + windowSize.y - versionSize.y - 10.0f),
				Color::Muted, versionLabel.c_str());   // Muted, not Faint: Faint is too hard to read here
			ImGui::PopFont();
		}
		lastPosition = ImGui::GetWindowPos();
		ImGui::End();
		// The Noise gate pedal floats over the game while the overlay is open, whatever page is showing.
		if (g_gatePedal.windowOpen) NoiseGatePedalWindow();
		// Layout edit mode is the Note by Note page: its sliders and the on-screen blocks move together.
		if (visible && page == Page::NoteByNote) HudEditor();
		if (fonts.body) ImGui::PopFont();
	}
}
