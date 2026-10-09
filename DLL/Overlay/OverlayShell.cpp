#include "stdafx.h"
#include "OverlayShell.hpp"
#include "OverlayUi.hpp"
#include "Lib/ImGUI/imgui_internal.h"   // SetItemUsingMouseWheel (the pedalboard scrolls sideways)
#include "NoteByNote/Ui/NoteByNoteHudLayout.hpp"
#include "AudioBridge/Engine/AudioBridgeEngine.hpp"
#include "AudioBridge/Cable/CableInput.hpp"
#include "AudioBridge/Engine/OutputDevices.hpp"
#include "AudioBridge/Engine/OutputTap.hpp"
#include "Audio/TakeRecorder.hpp"
#include "NoteByNote/Ml/MlServiceLauncher.hpp"
#include "ExternalAmp/ExternalAmp.hpp"
#include "Audio/VstHostLink.hpp"
#include "Shared/VstHostControl.hpp"
#include "Audio/SongLibrary.hpp"
#include "Audio/ToneBackup.hpp"
#include "Extensions.hpp"
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

namespace Overlay
{
	namespace
	{
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
			Audio::CableInput::Diagnostics input{};
		};

		const LiveState& Poll()
		{
			static LiveState state;
			static ULONGLONG last = 0;
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
			return state;
		}

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

			static const char* const styles[] = { "Detailed", "Simple", "Tab" };
			int style = static_cast<int>(Settings::GetNoteByNoteTargetStyle());
			if (SegmentedRow("Style", &style, styles, 3, "Detailed names the string, fret and note. Simple shows just the fret next to the string colour, plus the bend (half bend, full bend). Tab draws a six-line tab with the fret on its string, chords included."))
				Settings::SetNoteByNoteTargetStyle(static_cast<Settings::NoteByNoteTargetStyle>(style));

			// Custom is the only draggable position. Its spot is remembered while Under readout or Center is chosen,
			// so cycling back to Custom puts the target where it was left.
			using Position = Settings::NoteByNoteTargetPosition;
			static const char* const positions[] = { "Under readout", "Center", "Custom" };
			const Position current = Settings::GetNoteByNoteTargetPosition();
			int position = current == Position::Custom ? 2 : current == Position::Center ? 1 : 0;
			if (SegmentedRow("Position", &position, positions, 3, "Under readout stacks it below the detection lines. Center puts it near the gameplay area. Custom is wherever you drag it, and is remembered.")) {
				if (position == 2 && !Settings::GetNoteByNoteHudPlacement(Block::Target).IsSet()) {
					// First time in Custom: start from where the target is drawn now.
					const HudLayout::Block& drawn = HudLayout::blocks[static_cast<int>(Block::Target)];
					if (HudLayout::IsLive(Block::Target) && HudLayout::frameWidth > 0 && HudLayout::frameHeight > 0)
						Settings::SetNoteByNoteHudPlacement(Block::Target, { drawn.x0 / HudLayout::frameWidth, drawn.y0 / HudLayout::frameHeight }, true);
				}
				Settings::SetNoteByNoteTargetPosition(position == 2 ? Position::Custom : position == 1 ? Position::Center : Position::Left);
			}
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

		void InputPage(const LiveState& live)
		{
			PageHeader("Input", "Guitar signal before Rocksmith hears it. Every change applies live and is saved.");

			BeginCard("Levels");
			const bool twoPlayer = GameState::IsMultiplayer();
			Meter(twoPlayer ? "Player 1" : "Guitar", MeterFraction(live.input.meterPeak), "Player 1's guitar level after the input gain.");
			if (twoPlayer) Meter("Player 2", MeterFraction(live.input.playerTwoMeterPeak), "Player 2's guitar level.");
			EndCard();

			// Seeded once from the keys the DLL applies at launch, then user-driven.
			static bool seeded = false;
			static bool gainOn, gateOn, compOn, humOn, rgOn;
			static int gainTenths, gateTenths, compPct, humHz, rgTenths;
			if (!seeded) {
				gainTenths = Settings::GetModSetting("AsioInputGain");        gainOn = gainTenths > 0;
				gateTenths = Settings::GetModSetting("NoiseGateThreshold");   gateOn = gateTenths < 0; if (!gateOn) gateTenths = -500;
				compPct = Settings::GetModSetting("CompressorStrength");      compOn = compPct > 0;
				humHz = Settings::GetModSetting("HumFilter");                 humOn = humHz >= 20; if (!humOn) humHz = 50;
				rgOn = Settings::GetModSetting("RocksmithGateOverride") != 0; rgTenths = Settings::GetModSetting("RocksmithGateThreshold"); if (rgTenths == 0) rgTenths = -593;
				seeded = true;
			}

			BeginCard("Processing");
			// Input make-up gain (op 15, tenths of dB; off = 0).
			if (ToggleRow("Input gain", "Boosts a quiet guitar before the game's detection.", &gainOn)) SendInt(15, gainOn ? gainTenths : 0);
			ImGui::BeginDisabled(!gainOn);
			float gainDb = gainTenths / 10.0f;
			bool gainSaved = false;
			if (SliderRow("Gain", &gainDb, 0.0f, 20.0f, "+%.1f dB", &gainSaved, "Input gain from 0 to +20 dB.")) { gainTenths = static_cast<int>(gainDb * 10 + 0.5f); PreviewInt(15, gainTenths); }
			if (gainSaved) SendInt(15, gainTenths);
			ImGui::EndDisabled();

			// Noise suppressor (op 19, signed tenths of dB; off = 0).
			if (ToggleRow("Noise suppressor", "Needs a clear attack, then holds the note open down to the sustain threshold.", &gateOn)) SendInt(19, gateOn ? gateTenths : 0);
			ImGui::BeginDisabled(!gateOn);
			float gateDb = gateTenths / 10.0f;
			bool gateSaved = false;
			if (SliderRow("Sustain threshold", &gateDb, -80.0f, -20.0f, "%.1f dB", &gateSaved, "Lower values keep quieter note tails after a valid attack.")) { gateTenths = static_cast<int>(gateDb * 10 - 0.5f); PreviewInt(19, gateTenths); }
			if (gateSaved) SendInt(19, gateTenths);
			ImGui::EndDisabled();

			// Compressor (op 20, 0..100; off = 0).
			if (ToggleRow("Compressor", "Evens out level swings during sustained notes.", &compOn)) SendInt(20, compOn ? compPct : 0);
			ImGui::BeginDisabled(!compOn);
			float comp = static_cast<float>(compPct);
			bool compSaved = false;
			if (SliderRow("Strength", &comp, 0.0f, 100.0f, "%.0f%%", &compSaved)) { compPct = static_cast<int>(comp + 0.5f); PreviewInt(20, compPct); }
			if (compSaved) SendInt(20, compPct);
			ImGui::EndDisabled();

			// Adaptive mains hum / buzz remover (op 24, nominal Hz; off = 0). HumRemover.hpp.
			if (ToggleRow("Hum filter", "Measures your mains hum between notes and removes only its lines, up to 8 kHz.", &humOn)) SendInt(24, humOn ? humHz : 0);
			ImGui::BeginDisabled(!humOn);
			static const char* const mains[] = { "50 Hz", "60 Hz" };
			int mainsIndex = humHz >= 55 ? 1 : 0;
			if (SegmentedRow("Mains frequency", &mainsIndex, mains, 2, "50 Hz in the UK, EU and AU. 60 Hz in the US.")) { humHz = mainsIndex ? 60 : 50; SendInt(24, humHz); }
			ImGui::EndDisabled();
			EndCard();

			BeginCard("Rocksmith");
			// Rocksmith gate override (op 23, "<on>,<tenths>").
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
			EndCard();
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
		// The VST host (VstHost/, Audio/VstHostLink.hpp) plays the guitar through a chain of VST3 plugins and shows the
		// selected pedal's own window over the game. This page is laid out like a chain loader, in one framed panel:
		//   - a bar: File / Edit / View, the tone's name between previous / next arrows (amber dot = unsaved),
		//     VST | Game amp (A/B), the amp window and power buttons;
		//   - the tone library on the left (favourites, all tones, song tones);
		//   - the pedalboard: In jack, cables, one pedal per plugin coloured by kind (LED, knobs, footswitch), Add,
		//     Out jack. Click selects, double-click opens the plugin's window, the footswitch bypasses, drag
		//     reorders, right-click a pedal, a tone, the board or the song strip for its actions;
		//   - a strip with the in / out meters, latency, and the current song's tone;
		//   - the plugin browser under it.
		// View shows or hides the library, the browser and the meters (remembered). Commands are queued to the host;
		// the page shows its state a few times a second. It shares the amp link with the External app tab, which
		// carries one app at a time: each page says when the other one has it.
		namespace ToneDesigner
		{
			enum Section : unsigned { Library = 2, Browser = 4, Meters = 8 };
			enum class Dialog { None, SaveAs, Rename, Delete, NewTone, Discard, Levels, NewFolder, DeleteTones, Restore };
			enum Filter { All, Favourites, Amps, Effects, Dynamics, Instruments, Recent, FilterCount };

			// The panel's own colours (the mockup's): neutral greys, so it reads as a separate app inside the overlay.
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
				// The main area: the pedalboard, or the songs (give songs a tone without playing them).
				bool songsMode = false;
				char songSearch[64] = {};
				int songFilter = 0;
				std::vector<std::string> selectedSongs;   // song keys, chosen the same way as tones
				std::string songAnchor;
				std::vector<ToneBackup::Entry> backups;   // File, Backups: listed when the menu opens
				size_t dialogSongs = 0;         // Delete: songs that use it (read once, when the dialog opens)
				char name[64] = {};
			};

			State& S()
			{
				static State state;
				if (!state.loaded) { state.open = VstHostLink::SectionsOpen(Library | Browser | Meters); state.loaded = true; }
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
			void OpenWindow(int slot)
			{
				VstHostLink::ShowSlot(slot);
				VstHostLink::SetPopout(true);
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
			template <typename Pick> void PluginMenuItems(Pick pick)
			{
				const std::vector<VstHostLink::Plugin>& plugins = VstHostLink::Plugins();
				if (plugins.empty()) ImGui::TextDisabled("No 64-bit VST3 plugins found");
				for (const VstHostLink::Plugin& plugin : plugins) {
					if (plugin.kind == VstHostLink::Kind::Instrument) continue;
					if (ImGui::MenuItem(plugin.name.c_str(), plugin.vendor.c_str())) pick(plugin);
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
				const bool idle = !vst.busy;
				if (ImGui::MenuItem("New tone", nullptr, false, idle)) RequestNew(vst);
				if (ImGui::BeginMenu("Open tone", idle)) {
					const std::vector<std::string>& tones = VstHostLink::Tones();
					if (tones.empty()) ImGui::TextDisabled("No saved tones yet");
					for (const std::string& tone : tones)
						if (ImGui::MenuItem(tone.c_str(), nullptr, _stricmp(tone.c_str(), vst.toneName.c_str()) == 0)) RequestLoad(vst, tone);
					ImGui::EndMenu();
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Save", nullptr, false, idle && (vst.toneName.empty() || vst.toneModified))) Save(vst);
				if (ImGui::MenuItem("Save as...", nullptr, false, idle)) Ask(Dialog::SaveAs, vst.toneName);
				if (ImGui::MenuItem("Revert to saved", nullptr, false, idle && !vst.toneName.empty() && vst.toneModified)) VstHostLink::LoadTone(vst.toneName);
				if (ImGui::MenuItem("Rename...", nullptr, false, idle && !vst.toneName.empty())) Ask(Dialog::Rename, vst.toneName);
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
				if (ImGui::MenuItem("Mute while tuning", nullptr, vst.muteWhileTuning)) VstHostLink::SetMuteWhileTuning(!vst.muteWhileTuning);
				if (ImGui::MenuItem("Start with the game", nullptr, vst.autoStart)) VstHostLink::SetAutoStart(!vst.autoStart);
				ImGui::Separator();
				// Fast tone changes: plugins kept loaded outside the chain (the host's spares). A tone that uses them
				// switches in a moment; a heavy amp sim loaded from scratch takes seconds. They cost memory, not CPU.
				if (ImGui::BeginMenu("Fast tone changes")) {
					ImGui::TextDisabled("Plugins that leave the chain stay loaded:");
					static const int counts[] = { 0, 2, 4, 8 };
					for (int count : counts) {
						const std::string label = count == 0 ? "No, unload them" : "The last " + std::to_string(count);
						if (ImGui::MenuItem(label.c_str(), nullptr, vst.keepRecent == count)) VstHostLink::SetKeepRecent(count);
					}
					Hint("Switching back to a tone that uses them is then instant. Each one takes memory while loaded.");
					ImGui::Separator();
					ImGui::TextDisabled("Right-click a plugin in the browser to load it in the background,");
					ImGui::TextDisabled("or to always keep it loaded.");
					for (const std::wstring& failed : vst.preloadFailed) {
						const size_t slash = failed.find_last_of(L"\\/");
						const std::string name = Utf8(slash == std::wstring::npos ? failed : failed.substr(slash + 1));
						ImGui::TextDisabled("Not loaded in the background: %s (it failed doing that)", name.c_str());
					}
					ImGui::Separator();
					ImGui::TextDisabled("The song list's highlighted song gets its tone ready too.");
					ImGui::EndMenu();
				}
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

					// Pedals | Songs: what the main area shows (the songs give tones to songs without playing them).
					{
						ImFont* modeFont = CaptionFont();
						static const char* const modes[2] = { "Pedals", "Songs" };
						State& st = S();
						float mx = menusEnd + 8.0f;
						const ImVec2 start(origin.x + mx, origin.y + y);
						for (int i = 0; i < 2; ++i) {
							const float w = TextWidth(modeFont, modes[i]) + 18.0f;
							const ImVec2 at(origin.x + mx, origin.y + y);
							const bool on = st.songsMode == (i == 1);
							ImGui::SetCursorScreenPos(at);
							ImGui::PushID(i);
							if (ImGui::InvisibleButton("##mode", ImVec2(w, button))) st.songsMode = i == 1;
							ImGui::PopID();
							const bool hovered = ImGui::IsItemHovered();
							Hint(i == 1 ? "Your songs in the main area: give them tones of their own without playing them, and see which wait for plugins to load."
								: "The pedalboard: the chain you hear.");
							if (on) list->AddRectFilled(at, ImVec2(at.x + w, at.y + button), Ink::Blue, 6.0f, i ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersLeft);
							Text(list, modeFont, ImVec2(at.x + 9.0f, at.y + (button - modeFont->FontSize) * 0.5f), on ? IM_COL32_WHITE : hovered ? Ink::Text : Ink::Muted, modes[i]);
							mx += w;
						}
						list->AddRect(start, ImVec2(origin.x + mx, origin.y + y + button), Ink::FieldEdge, 6.0f);
						menusEnd = mx + 4.0f;
					}

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
					if (IconSquare("##power", Glyph::Power, false, "Close Tone designer (saves the chain). The game's amp comes back.", button))
						VstHostLink::StopHost();

					// Middle: < tone name >, centred in the space between the menus and A/B.
					const float left = menusEnd + 12.0f, right = ab.x - origin.x - 12.0f;
					const float nameW = std::clamp(right - left - 2.0f * (button + 6.0f), 80.0f, 240.0f);
					const float groupW = nameW + 2.0f * (button + 6.0f);
					float gx = left + std::max(0.0f, (right - left - groupW) * 0.5f);
					ImGui::SetCursorScreenPos(ImVec2(origin.x + gx, origin.y + y));
					if (IconSquare("##prevTone", Glyph::ChevronLeft, false, "Previous saved tone.", button) && !vst.busy) StepTone(vst, -1);
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
					if (IconSquare("##nextTone", Glyph::ChevronRight, false, "Next saved tone.", button) && !vst.busy) StepTone(vst, +1);
					if (ImGui::BeginPopup("##toneList")) {
						const std::vector<std::string>& tones = VstHostLink::Tones();
						if (tones.empty()) ImGui::TextDisabled("No saved tones yet");
						for (const std::string& tone : tones)
							if (ImGui::MenuItem(tone.c_str(), nullptr, _stricmp(tone.c_str(), vst.toneName.c_str()) == 0) && !vst.busy) RequestLoad(vst, tone);
						ImGui::Separator();
						if (ImGui::MenuItem("Save as...", nullptr, false, !vst.busy)) Ask(Dialog::SaveAs, vst.toneName);
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
				if (ImGui::BeginMenu("Move to folder", !vst.busy)) {
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
				if (ImGui::MenuItem(("Delete " + std::to_string(batch.size()) + " tones...").c_str(), nullptr, false, !vst.busy)) {
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
				if (hovered && !onStar && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !current && !vst.busy) RequestLoad(vst, tone);
				// Drag onto songs (Songs mode) to give them this tone.
				if (s.songsMode && ImGui::BeginDragDropSource()) {
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
					if (ImGui::MenuItem("Load", "or double-click it", false, !current && !vst.busy)) RequestLoad(vst, tone);
					if (s.songsMode && !s.selectedSongs.empty()) {
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
					if (ImGui::MenuItem("Rename...", nullptr, false, !vst.busy)) Ask(Dialog::Rename, tone);
					if (ImGui::BeginMenu("Move to folder", !vst.busy)) {
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
					if (ImGui::MenuItem("Delete...", nullptr, false, !vst.busy)) Ask(Dialog::Delete, tone);
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
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
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
				if (ImGui::IsItemClicked()) { s.songsMode = true; s.songFilter = songs == 0 ? 0 : 1; }
				Hint("Songs that load their own tone when they start. Click to see them (Songs, at the top), or give more songs a tone there.");
				ImGui::PopStyleColor();

				// Right-click on the empty part of the column.
				PushPopupPadding();
				if (ImGui::BeginPopupContextWindow("##libraryMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
					ImGui::TextDisabled("Tones");
					ImGui::Separator();
					if (ImGui::MenuItem("New tone", nullptr, false, !vst.busy)) RequestNew(vst);
					if (ImGui::MenuItem("Save current as...", nullptr, false, !vst.busy)) Ask(Dialog::SaveAs, vst.toneName);
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
				const float hintH = CaptionFont()->FontSize * 2.0f + 8.0f;
				const float rackW = ImGui::GetContentRegionAvail().x;
				const float rackH = std::max(150.0f, ImGui::GetContentRegionAvail().y - hintH);
				const ImVec2 rack = ImGui::GetCursorScreenPos();
				ImDrawList* outer = ImGui::GetWindowDrawList();
				outer->AddRectFilled(rack, ImVec2(rack.x + rackW, rack.y + rackH), Ink::Rack, 10.0f);
				outer->AddRect(rack, ImVec2(rack.x + rackW, rack.y + rackH), Ink::RackEdge, 10.0f);

				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 0));
				ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
				ImGui::BeginChild("##rack", ImVec2(rackW, rackH), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysUseWindowPadding);
				ImDrawList* list = ImGui::GetWindowDrawList();
				ImFont* caption = CaptionFont();
				const float pedalH = 124.0f;
				const float top = ImGui::GetCursorScreenPos().y + std::max(0.0f, (rackH - 6.0f - pedalH) * 0.5f);
				const float midY = top + pedalH * 0.5f;
				float x = ImGui::GetCursorScreenPos().x;
				const ImVec2 rackStart = ImGui::GetCursorScreenPos();
				auto cable = [&] { list->AddLine(ImVec2(x, midY), ImVec2(x + 14.0f, midY), Ink::Cable, 3.0f); x += 22.0f; };
				bool overTile = false;   // the mouse is on a pedal (its menu, not the board's)

				Jack(list, ImVec2(x + 21.0f, midY - 6.0f), "In");
				x += 42.0f + 8.0f;
				cable();
				for (int i = 0; i < count; ++i) {
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
					ImGui::PopID();
					x = end.x + 8.0f;
					cable();
				}

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
					Hint("Add a plugin at the end of the chain. The plugin browser below has search.");
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
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rackW);
				ImGui::TextUnformatted(count == 0 ? "The chain is empty, so you hear your clean guitar. Click Add for an amp sim, then pedals before it (gate, drive) or after it (delay, reverb)."
					: vst.chainBypassed ? "The whole chain is bypassed (Edit menu): you hear your clean guitar."
					: "Right-click a pedal, a tone, or the board. Double-click a pedal to open its window. Click a footswitch to bypass. Drag pedals to reorder; scroll or middle-drag to move along a long chain.");
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
				ImGui::PopFont();
				ImGui::EndChild();
				ImGui::PopStyleVar();
			}

			void MeterBar(ImDrawList* list, ImVec2 at, float width, float peak)
			{
				const float height = 7.0f;
				list->AddRectFilled(at, ImVec2(at.x + width, at.y + height), Ink::Field, 4.0f);
				const float fill = MeterFraction(peak);
				if (fill > 0.0f) list->AddRectFilled(at, ImVec2(at.x + width * fill, at.y + height), peak > 1.0f ? Ink::Bad : Ink::Meter, 4.0f);
			}

			// The strip: IN and OUT meters, the link's latency and late buffers, and the current song's tone.
			void Strip(const VstHostLink::Snapshot& vst, const ExternalAmp::Snapshot& amp, float width)
			{
				ImDrawList* list = ImGui::GetWindowDrawList();
				ImFont* caption = CaptionFont();
				const ImVec2 at = ImGui::GetCursorScreenPos();
				const float rowH = caption->FontSize + 6.0f;
				const float pad = 14.0f;
				// IN and OUT; the link's latency and late buffers are in the Amp link card above.
				const float labelsW = TextWidth(caption, "IN") + TextWidth(caption, "OUT") + 4.0f * 10.0f;
				const float meterW = std::max(40.0f, (width - 2.0f * pad - labelsW) * 0.5f);
				float x = at.x + pad;
				const float y = at.y + 9.0f;
				Text(list, caption, ImVec2(x, y), Ink::Muted, "IN");
				x += TextWidth(caption, "IN") + 10.0f;
				MeterBar(list, ImVec2(x, y + (caption->FontSize - 7.0f) * 0.5f), meterW, vst.inputPeak);
				x += meterW + 10.0f;
				Text(list, caption, ImVec2(x, y), Ink::Muted, "OUT");
				x += TextWidth(caption, "OUT") + 10.0f;
				MeterBar(list, ImVec2(x, y + (caption->FontSize - 7.0f) * 0.5f), meterW, vst.outputPeak);
				ImGui::SetCursorScreenPos(at);
				ImGui::InvisibleButton("##levels", ImVec2(width, rowH + 6.0f));
				Hint(vst.outputPeak > 1.0f ? "The chain's output is clipping. Click to lower the output trim, or lower the last plugin's volume."
					: "Your guitar into the chain and the chain's output. Click for the levels.");
				if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) Ask(Dialog::Levels);

				// Song row: the song, its tone, and a button to give it the tone playing now (or forget its own).
				const ImVec2 song(at.x + pad, at.y + rowH + 12.0f);
				std::string songAction;
				bool songActionEnabled = true;
				if (!vst.songKey.empty()) {
					if (!vst.songTone.empty() && _stricmp(vst.songTone.c_str(), vst.toneName.c_str()) == 0) songAction = "Forget this song's tone";
					else if (vst.toneName.empty() || vst.toneModified) { songAction = "Save the tone to use it here"; songActionEnabled = false; }
					else songAction = "Use \"" + BaseName(vst.toneName) + "\" for this song";
				}
				const float actionW = songAction.empty() ? 0.0f : ImGui::CalcTextSize(songAction.c_str()).x + 28.0f + pad;
				ImGui::SetCursorScreenPos(ImVec2(at.x, song.y - 4.0f));
				ImGui::InvisibleButton("##song", ImVec2(std::max(width - actionW, 10.0f), ImGui::GetFontSize() + 10.0f));
				DrawIcon(list, ImVec2(song.x + 8.0f, song.y + ImGui::GetFontSize() * 0.5f), Glyph::MusicNote, Ink::Soft);
				float sx = song.x + 22.0f;
				if (vst.songKey.empty()) {
					const std::string note = vst.preloadingName.empty() ? std::string("Start a song to give it its own tone.")
						: "Getting " + vst.preloadingName + " ready in the background, for fast tone changes...";
					Text(list, caption, ImVec2(sx, song.y + 2.0f), Ink::Dim, note.c_str());
				}
				else {
					list->AddText(ImVec2(sx, song.y), Ink::Soft, vst.songKey.c_str());
					sx += ImGui::CalcTextSize(vst.songKey.c_str()).x + 10.0f;
					const std::string tag = !vst.songTone.empty() ? BaseName(vst.songTone)
						: !vst.defaultTone.empty() ? "default: " + BaseName(vst.defaultTone) : "your usual tone";
					const float tagW = TextWidth(caption, tag.c_str()) + 16.0f;
					list->AddRectFilled(ImVec2(sx, song.y - 1.0f), ImVec2(sx + tagW, song.y + ImGui::GetFontSize() + 1.0f), Ink::Tag, 5.0f);
					Text(list, caption, ImVec2(sx + 8.0f, song.y + (ImGui::GetFontSize() - caption->FontSize) * 0.5f), Ink::TagText, tag.c_str());
					sx += tagW + 10.0f;
					Text(list, caption, ImVec2(sx, song.y + 2.0f), Ink::Dim, "right-click to change");
				}
				PushPopupPadding();
				if (ImGui::BeginPopupContextItem("##songMenu")) {
					ImGui::TextDisabled("%s", vst.songKey.empty() ? "No song playing" : vst.songKey.c_str());
					ImGui::Separator();
					const bool canAssign = !vst.songKey.empty() && !vst.toneName.empty() && !vst.toneModified
						&& _stricmp(vst.songTone.c_str(), vst.toneName.c_str()) != 0;
					if (ImGui::MenuItem(vst.toneName.empty() || vst.toneModified ? "Use current tone (save it first)" : ("Use \"" + vst.toneName + "\"").c_str(),
						nullptr, false, canAssign))
						VstHostLink::SetSongTone(vst.toneName);
					if (ImGui::BeginMenu("Use a saved tone", !vst.songKey.empty() && !vst.busy)) {
						for (const std::string& tone : VstHostLink::Tones())
							if (ImGui::MenuItem(tone.c_str(), nullptr, _stricmp(tone.c_str(), vst.songTone.c_str()) == 0)) {
								VstHostLink::SetSongTone(tone);
								if (_stricmp(tone.c_str(), vst.toneName.c_str()) != 0) RequestLoad(vst, tone);
							}
						ImGui::EndMenu();
					}
					if (ImGui::MenuItem("Forget (use your usual tone)", nullptr, false, !vst.songTone.empty())) VstHostLink::ClearSongTone();
					ImGui::EndPopup();
				}
				ImGui::PopStyleVar();
				if (!songAction.empty()) {
					ImGui::SetCursorScreenPos(ImVec2(at.x + width - actionW, song.y - 5.0f));
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 3));
					ImGui::BeginDisabled(!songActionEnabled || vst.busy);
					if (ImGui::Button(songAction.c_str())) {
						if (_stricmp(vst.songTone.c_str(), vst.toneName.c_str()) == 0) VstHostLink::ClearSongTone();
						else VstHostLink::SetSongTone(vst.toneName);
					}
					ImGui::EndDisabled();
					ImGui::PopStyleVar();
					Hint("A song with its own tone loads it whenever it starts; other songs get the default tone (right-click a tone), or keep the one playing.");
				}
				ImGui::SetCursorScreenPos(ImVec2(at.x, song.y + ImGui::GetFontSize() + 10.0f));
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

			// Status: what happens if it is added.
			void PluginStatus(const VstHostLink::Snapshot& vst, const VstHostLink::Plugin& plugin, const char*& text, ImU32& color)
			{
				if (VstHostLink::FailedToLoad(plugin.path)) { text = "Failed to load"; color = Ink::Bad; }
				else if (InChain(vst, plugin.path)) { text = "In chain"; color = Ink::Good; }
				else if (plugin.kind == VstHostLink::Kind::Instrument) { text = "Not for guitar"; color = Ink::Amber; }
				else if (Loading(vst, plugin.path)) { text = "Loading..."; color = Ink::Amber; }
				else if (KeptLoaded(vst, plugin.path)) { text = Listed(vst.alwaysLoaded, plugin.path) ? "Always loaded" : "Loaded"; color = Ink::Blue; }
				else if (Listed(vst.alwaysLoaded, plugin.path)) { text = Listed(vst.preloadFailed, plugin.path) ? "Failed to load" : "Loads soon"; color = Ink::Amber; }
				else { text = "Ready"; color = Ink::Good; }
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

			// Tone choices for a menu: tones outside folders, then a submenu per folder.
			template <typename Pick> void ToneChoices(Pick pick)
			{
				const std::vector<std::string>& tones = VstHostLink::Tones();
				if (tones.empty()) ImGui::TextDisabled("No saved tones yet");
				for (const std::string& tone : tones) if (FolderOf(tone).empty() && ImGui::MenuItem(tone.c_str())) pick(tone);
				for (const std::string& folder : ToneFolders(tones))
					if (ImGui::BeginMenu(folder.c_str())) {
						for (const std::string& tone : tones) if (FolderOf(tone) == folder && ImGui::MenuItem(BaseName(tone).c_str())) pick(tone);
						ImGui::EndMenu();
					}
			}

			// Songs mode: the pedalboard's space shows every song in the library (SongLibrary) with its tone and how long
			// its tone took to load when it last started (VstHostLink::SongLoads, the audit). Select songs (Ctrl or
			// Shift for several), then drag a tone from the library onto them, right-click, or use the buttons below.
			void SongsView(const VstHostLink::Snapshot& vst, float width, float height)
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
				auto assignTo = [](const std::vector<std::string>& keys, const std::string& tone) { for (const std::string& key : keys) VstHostLink::SetToneForSong(key, tone); };

				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
				ImGui::BeginChild("##songsView", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);
				ImGui::PopStyleVar();
				ImFont* caption = CaptionFont();
				const float inner = ImGui::GetContentRegionAvail().x;
				const float left = ImGui::GetCursorPosX();

				// Top line: search, filters, count.
				ImGui::SetNextItemWidth(std::min(200.0f, inner * 0.3f));
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
				ImGui::InputTextWithHint("##songSearch", "Search songs or artists", s.songSearch, sizeof(s.songSearch));
				ImGui::PopStyleVar();
				ImGui::PopStyleColor();
				enum { AllSongs, WithTone, SlowToLoad, SongFilterCount };
				static const char* const chips[SongFilterCount] = { "All", "With a tone", "Slow to load" };
				for (int c = 0; c < SongFilterCount; ++c) {
					ImGui::SameLine(0.0f, c == 0 ? 10.0f : 6.0f);
					if (Chip(chips[c], s.songFilter == c)) s.songFilter = c;
					if (c == SlowToLoad) Hint("Songs whose tone waited 3 s or more for plugins to load when they last started. Always keep loaded (right-click the plugin in the plugin browser) makes them instant; songs you rarely play can stay as they are.");
				}
				const SongLibrary::State state = SongLibrary::GetState();
				const std::string count = state == SongLibrary::State::Reading && library->songs.empty() ? std::string("reading your songs...")
					: std::to_string(library->songs.size()) + " songs, " + std::to_string(songTones.size()) + " with a tone";
				const float countW = TextWidth(caption, count.c_str());
				ImGui::SameLine(0.0f, 10.0f);
				if (ImGui::GetCursorPosX() + countW < left + inner) {
					ImGui::SetCursorPosX(left + inner - countW);
					ImGui::PushFont(caption);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Dim));
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(count.c_str());
					ImGui::PopStyleColor();
					ImGui::PopFont();
				}
				else ImGui::NewLine();

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
				std::vector<std::string> order;
				order.reserve(rows.size());
				for (const Row& row : rows) order.push_back(row.key);

				// Column heads.
				const float titleX = 8.0f, artistX = inner * 0.40f, toneX = inner * 0.66f;
				ImDrawList* list = ImGui::GetWindowDrawList();
				{
					const ImVec2 at = ImGui::GetCursorScreenPos();
					Text(list, caption, ImVec2(at.x + titleX, at.y), Ink::Dim, "Song");
					Text(list, caption, ImVec2(at.x + artistX, at.y), Ink::Dim, "Artist");
					Text(list, caption, ImVec2(at.x + toneX, at.y), Ink::Dim, "Tone");
					ImGui::Dummy(ImVec2(inner, caption->FontSize + 2.0f));
					list->AddLine(ImVec2(at.x, at.y + caption->FontSize + 4.0f), ImVec2(at.x + inner, at.y + caption->FontSize + 4.0f), Ink::Edge);
				}

				const bool canUseCurrent = !vst.toneName.empty() && !vst.toneModified;
				const std::string useCurrent = "Use \"" + (vst.toneName.empty() ? std::string("current tone") : BaseName(vst.toneName)) + "\"";
				const float footH = ImGui::GetFontSize() + 12.0f;
				const float listH = std::max(ImGui::GetContentRegionAvail().y - footH, 40.0f);
				const float rowH = ImGui::GetFontSize() + 8.0f;
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
								about += "\n\nDrag a tone from the left onto it, or right-click.";
								Hint(about.c_str());
							}
							PushPopupPadding();
							if (ImGui::BeginPopupContextItem("##songMenu")) {
								if (!selected && ImGui::IsWindowAppearing()) { s.selectedSongs = { row.key }; s.songAnchor = row.key; }
								const std::vector<std::string> keys = s.selectedSongs;
								ImGui::TextDisabled("%s", keys.size() == 1 ? row.title.c_str() : (std::to_string(keys.size()) + " songs").c_str());
								ImGui::Separator();
								if (ImGui::MenuItem(useCurrent.c_str(), nullptr, false, canUseCurrent)) assignTo(keys, vst.toneName);
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
							rowsList->PushClipRect(ImVec2(at.x + titleX, at.y), ImVec2(at.x + artistX - 8.0f, end.y), true);
							rowsList->AddText(ImVec2(at.x + titleX, textY), IM_COL32_WHITE, row.title.c_str());
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
					ImGui::BeginDisabled(!canUseCurrent);
					if (Button(useCurrent.c_str(), true)) assignTo(keys, vst.toneName);
					ImGui::EndDisabled();
					ImGui::SameLine();
					if (Button("Give it a tone...")) ImGui::OpenPopup("##songTonePick");
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
				ImGui::EndChild();
			}

			// Under the board: every plugin found, with search, filters, type and status.
			void PluginBrowser(const VstHostLink::Snapshot& vst, float width)
			{
				State& s = S();
				const std::vector<VstHostLink::Plugin>& plugins = VstHostLink::Plugins();
				const bool canAdd = !vst.busy && vst.slots.size() < static_cast<size_t>(VstHostControl::kMaxSlots);
				const bool canReplace = !vst.busy && s.selected >= 0 && s.selected < static_cast<int>(vst.slots.size());
				ImFont* caption = CaptionFont();
				const float pad = 14.0f;
				const float left = ImGui::GetCursorPosX() + pad;
				const float inner = width - 2.0f * pad;
				ImGui::Dummy(ImVec2(0, 4));

				// Title, count and search on one line.
				ImGui::SetCursorPosX(left);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(IM_COL32_WHITE));
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted("Plugin browser");
				ImGui::PopStyleColor();
				ImGui::SameLine(0.0f, 10.0f);
				const size_t folders = VstHostLink::ScanFolders().size() + 1;
				const std::string count = std::to_string(plugins.size()) + " found, " + std::to_string(folders) + (folders == 1 ? " folder" : " folders");
				ImGui::PushFont(caption);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Dim));
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(count.c_str());
				ImGui::PopStyleColor();
				// How many plugins are loaded outside the chain (the host's spares): visible, so the speed is not a mystery.
				int loadedOutside = 0;
				for (const std::wstring& spare : vst.spares) if (!InChain(vst, spare)) ++loadedOutside;
				if (loadedOutside > 0 || !vst.preloadingName.empty()) {
					ImGui::SameLine(0.0f, 8.0f);
					const std::string kept = !vst.preloadingName.empty() ? "loading " + vst.preloadingName + " in the background..."
						: std::to_string(loadedOutside) + (loadedOutside == 1 ? " plugin" : " plugins") + " loaded for fast tone changes";
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Blue));
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(kept.c_str());
					ImGui::PopStyleColor();
					Hint("Plugins kept loaded outside the chain: a tone that uses them switches at once instead of waiting seconds for each one to start. Right-click a plugin to load it in the background, always keep it loaded, or unload it. View > Fast tone changes sets how many recent ones stay.");
				}
				ImGui::PopFont();
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
				const float nameX = 28.0f, vendorX = inner * 0.40f, kindX = inner * 0.66f;
				ImDrawList* list = ImGui::GetWindowDrawList();
				ImGui::SetCursorPosX(left);
				{
					const ImVec2 at = ImGui::GetCursorScreenPos();
					Text(list, caption, ImVec2(at.x + nameX, at.y), Ink::Dim, "Plugin");
					Text(list, caption, ImVec2(at.x + vendorX, at.y), Ink::Dim, "Vendor");
					Text(list, caption, ImVec2(at.x + kindX, at.y), Ink::Dim, "Type");
					Text(list, caption, ImVec2(at.x + inner - TextWidth(caption, "Status") - 10.0f, at.y), Ink::Dim, "Status");
					ImGui::Dummy(ImVec2(inner, caption->FontSize + 2.0f));
					list->AddLine(ImVec2(at.x, at.y + caption->FontSize + 4.0f), ImVec2(at.x + inner, at.y + caption->FontSize + 4.0f), Ink::Edge);
				}
				const float rowH = ImGui::GetFontSize() + 10.0f;
				if (rows.empty()) {
					ImGui::SetCursorPosX(left);
					ImGui::PushFont(caption);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
					ImGui::TextUnformatted(plugins.empty() ? "No 64-bit VST3 plugins found. Install some, or View, Add plugin folder if they are somewhere else."
						: s.filter == Favourites ? "No favourites yet: click the star next to a plugin."
						: s.filter == Recent ? "Plugins you add show here."
						: "Nothing matches.");
					ImGui::PopStyleColor();
					ImGui::PopFont();
				}
				else {
					const int shownRows = std::min(static_cast<int>(rows.size()), 7);
					ImGui::SetCursorPosX(left);
					ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
					ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
					ImGui::BeginChild("##plugins", ImVec2(inner, shownRows * rowH), false);
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
						Hint("Double-click to add it to the end of the chain. Right-click to keep it loaded for fast tone changes, and more. The star marks a favourite.");
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
						const char* status = nullptr;
						ImU32 statusColor = Ink::Muted;
						PluginStatus(vst, plugin, status, statusColor);
						Text(rowsList, caption, ImVec2(end.x - TextWidth(caption, status) - 10.0f, captionY), statusColor, status);
						ImGui::PopID();
					}
					ImGui::EndChild();
					ImGui::PopStyleVar(2);
				}

				// The selected plugin, and what can be done with it.
				const VstHostLink::Plugin* selected = FindPlugin(s.selectedPlugin);
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
					std::string line = selected->name + (selected->vendor.empty() ? "" : ", " + selected->vendor) + (selected->version.empty() ? "" : " " + selected->version)
						+ ":  " + Utf8(selected->path);
					ImGui::PushFont(caption);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Dim));
					ImGui::PushTextWrapPos(left + inner);
					ImGui::TextUnformatted(line.c_str());
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
					ImGui::PopFont();
					if (selected->kind == VstHostLink::Kind::Instrument) {
						ImGui::SetCursorPosX(left);
						WarnCaption("This is an instrument: it makes its own sound from MIDI notes and does nothing with a guitar.");
					}
				}
				ImGui::Dummy(ImVec2(0, 6));
			}

			void LevelsDialogBody(const VstHostLink::Snapshot& vst)
			{
				// Trims around the chain. Dragging sends the new level as it moves (the overlay coalesces them).
				static float inputDb = 0.0f, outputDb = 0.0f;
				static bool dragging = false;
				if (!dragging) { inputDb = vst.inputTenths / 10.0f; outputDb = vst.outputTenths / 10.0f; }
				bool inputDone = false, outputDone = false;
				const bool inputMoved = SliderRow("Into the chain", &inputDb, -24.0f, 24.0f, "%+.1f dB", &inputDone,
					"Level of your guitar going into the first plugin. Amp sims expect a real guitar's level; raise it if they sound thin.");
				const bool outputMoved = SliderRow("Out of the chain", &outputDb, -24.0f, 24.0f, "%+.1f dB", &outputDone,
					"Level of the whole chain going to the game.");
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
				ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 18));
				if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
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

			// The framed panel: bar, library + board, strip, browser. Backgrounds are drawn on the page's draw list
			// first; the parts are child windows on top.
			void Panel(const VstHostLink::Snapshot& vst, const ExternalAmp::Snapshot& amp)
			{
				const float width = ImGui::GetContentRegionAvail().x;
				const float barH = ImGui::GetFontSize() + 20.0f;
				const float mainH = 250.0f;
				const float stripH = IsOpen(Meters) ? CaptionFont()->FontSize + ImGui::GetFontSize() + 34.0f : 0.0f;
				const float libraryW = IsOpen(Library) ? std::min(240.0f, width * 0.32f) : 0.0f;
				const ImVec2 at = ImGui::GetCursorScreenPos();
				ImDrawList* list = ImGui::GetWindowDrawList();
				// The browser's height is only known after it is drawn: split the draw list so the panel's background
				// (channel 0) goes under everything drawn after it (channel 1).
				list->ChannelsSplit(2);
				list->ChannelsSetCurrent(1);

				Bar(vst, amp, width, barH);
				ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + barH));
				if (libraryW > 0.0f) {
					LibraryColumn(vst, libraryW, mainH);
					ImGui::SameLine(0.0f, 0.0f);
				}
				if (S().songsMode) SongsView(vst, width - libraryW, mainH);
				else Board(vst, width - libraryW, mainH);
				ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + barH + mainH));
				if (stripH > 0.0f) Strip(vst, amp, width);
				const float browserTop = ImGui::GetCursorScreenPos().y;
				if (IsOpen(Browser)) PluginBrowser(vst, width);
				const float bottom = std::max(ImGui::GetCursorScreenPos().y, browserTop);

				list->ChannelsSetCurrent(0);
				const ImVec2 end(at.x + width, bottom);
				list->AddRectFilled(at, end, Ink::App, 12.0f);
				list->AddRectFilled(at, ImVec2(end.x, at.y + barH), Ink::Bar, 12.0f, ImDrawFlags_RoundCornersTop);
				list->AddLine(ImVec2(at.x, at.y + barH), ImVec2(end.x, at.y + barH), Ink::Edge);
				if (libraryW > 0.0f) {
					list->AddRectFilled(ImVec2(at.x, at.y + barH + 1.0f), ImVec2(at.x + libraryW, at.y + barH + mainH), Ink::Library);
					list->AddLine(ImVec2(at.x + libraryW, at.y + barH), ImVec2(at.x + libraryW, at.y + barH + mainH), Ink::Edge);
				}
				if (stripH > 0.0f) {
					list->AddRectFilled(ImVec2(at.x, at.y + barH + mainH), ImVec2(end.x, at.y + barH + mainH + stripH), Ink::Bar,
						IsOpen(Browser) ? 0.0f : 12.0f, IsOpen(Browser) ? 0 : ImDrawFlags_RoundCornersBottom);
					list->AddLine(ImVec2(at.x, at.y + barH + mainH), ImVec2(end.x, at.y + barH + mainH), Ink::Edge);
				}
				if (IsOpen(Browser)) list->AddLine(ImVec2(at.x, browserTop), ImVec2(end.x, browserTop), Ink::Edge);
				list->AddRect(at, end, Ink::Edge, 12.0f);
				list->ChannelsMerge();
				ImGui::SetCursorScreenPos(ImVec2(at.x, bottom + 6.0f));
				ImGui::Dummy(ImVec2(0, 0));
			}
		}

		void ToneDesignerTab()
		{
			using namespace ToneDesigner;
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

			if (vst.hostHung) {
				BeginCard();
				WarnCaption("Tone designer has stopped answering.");
				if (Button("Force close", false, "Ends it. The game's amp comes back if fallback is on; the chain is as it was at its last autosave."))
					VstHostLink::ForceCloseHost();
				EndCard();
				return;
			}
			if (!vst.hostRunning) {
				BeginCard();
				Caption("Tone designer is off. Starting it brings back the chain you had last.");
				if (otherApp) WarnCaption(otherAppText.c_str());
				if (Button("Start Tone designer", true, "Starts the VST host in its own 64-bit program; your guitar plays through it once it connects."))
					VstHostLink::StartHost();
				bool autoStart = vst.autoStart;
				if (ToggleRow("Start with the game", "Starts Tone designer with your last chain when Rocksmith starts.", &autoStart))
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
		enum class AmpTab { ToneDesigner, ExternalApp };
		AmpTab g_ampTab = AmpTab::ToneDesigner;

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
			char text[96];
			if (link.frames > 0)
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

		// Two tabs as a segmented strip.
		void AmpTabs()
		{
			static const char* const labels[] = { "Tone designer", "External app" };
			const float height = ImGui::GetFrameHeight() + 4.0f;
			const float width = ImGui::GetContentRegionAvail().x;
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
			ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + height));
			ImGui::Dummy(ImVec2(0, 6));
		}

		void AmpSimPage()
		{
			const ExternalAmp::Snapshot amp = ExternalAmp::GetSnapshot();
			PageHeader("Amp sim", "Play through an amp sim instead of the game's amp: VST3 plugins in Tone designer, or an app on Rocksmith Audio Bridge ASIO. Rocksmith keeps reading your clean signal.");
			// The first time the page opens this session, the tab of whatever holds the link.
			static bool chosen = false;
			if (!chosen && amp.proxyLoaded) {
				chosen = true;
				if (amp.link.guestPresent && !VstHostLink::IsHostGuest(amp.link.guestName)) g_ampTab = AmpTab::ExternalApp;
			}
			AmpLinkCard(amp);
			AmpTabs();
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

		void OutputSay(const std::string& text, bool error) { g_outputMessage = text; g_outputMessageIsError = error; }

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
			if (std::wstring(live.status.endpoint) == pending.target) { OutputSay("Now playing through " + pending.name + ". Lasts until you close Rocksmith.", false); pending.active = false; return; }
			if (elapsed < 4000) return;
			if (!pending.previous.empty()) BridgeControl(4, pending.previous.c_str());
			OutputSay("Could not play through " + pending.name + ". Switched back.", true);
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
			const bool switching = endpoint == L"(starting)" || endpoint == L"(downgrading)";
			const bool session = !endpoint.empty() && endpoint[0] != L'(';
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
				current = "A Windows device";
				for (const auto& device : snapshot.devices) if (device.id == id) current = device.name;
				state = routing ? "Switched" : "Windows";
				stateColor = Color::AccentHi;
				if (endpoint.rfind(L"(route-stalled)", 0) == 0) { state = "Stalled"; stateColor = Color::Warn; }
			}
			else if (endpoint == L"(passthrough)") { current = asioName; state = proxy ? "ASIO" : "Direct"; stateColor = Color::Good; }
			else if (endpoint == L"(silent)") { current = "No output"; state = "Silent"; stateColor = Color::Bad; }
			else if (switching) { current = "Switching"; state = "Switching"; stateColor = Color::Warn; }

			BeginCard("Playing to");
			ImGui::PushFont(GetFonts().title);
			ImGui::TextUnformatted(current.c_str());
			ImGui::PopFont();
			StatusPill(state, stateColor);
			Meter("Left", MeterFraction(live.status.outputPeak[0]), "The game's left-channel output level, after the volume cap.");
			Meter("Right", MeterFraction(live.status.outputPeak[1]), "The game's right-channel output level, after the volume cap.");
			EndCard();

			BeginCard("Switch output");
			const bool canRoute = proxy && (passthrough || routing);
			if (!canRoute && !session) {
				Caption(switching ? "The audio engine is switching devices. Try again in a moment."
					: "Switching needs the Rocksmith Audio Bridge in the ASIO chain (RS_ASIO.ini Driver set to Rocksmith Audio Bridge), or Cable mode.");
				EndCard();
				return;
			}
			Caption("Moves the game's sound to another device now, without restarting. It is remembered: next launch starts there if the device is plugged in. Pick the ASIO entry to go back.");
			if (!snapshot.ok) Caption("Looking for devices...");

			const bool unidentified = snapshot.asioUnidentified;
			ImGui::BeginDisabled(pending.active);
			if (canRoute) {
				// The ASIO interface itself. Bound already: stop any route so the game plays straight out of it
				// again. Virtual (booted without it): bind it for input and output, see PromoteToAsio.
				const bool virtualProxy = Audio::OutputTap::ProxyOutputMode() == 1;
				const std::string asioLabel = asioName + "##asio";
				const bool asioClicked = ChoiceRow("asio", asioLabel.c_str(), "ASIO, lowest latency", !routing && !virtualProxy);
				if (asioClicked && virtualProxy && !asioDriver.empty()) {
					pending = PendingSwitch{};
					pending.active = true;
					pending.promote = true;
					pending.name = asioName;
					pending.started = GetTickCount64();
					OutputSay("Opening " + asioName + " for input and output...", false);
					PromoteToAsio(asioDriver);
				}
				else if (asioClicked && routing) {
					BridgeControl(22);
					OutputSay("Back on " + asioName + ".", false);
				}
			}
			ImGui::BeginDisabled(unidentified && !snapshot.asioDrivers.empty());
			for (const auto& device : snapshot.devices) {
				// The ASIO interface is listed once, as the ASIO entry above; its Windows endpoint is the same box and
				// is hidden, bound or not. While bound it would silence the interface; while unbound the ASIO entry
				// is the right pick because it moves the input too.
				if (canRoute && device.isAsioTwin) continue;
				const bool selected = routing ? device.id == routedId : (session && device.id == endpoint);
				std::string caption = device.isDefault ? "Windows default" : "";
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

			if (unidentified && !snapshot.asioDrivers.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
				ImGui::TextUnformatted(("Switching is off: the overlay cannot tell which Windows device is your ASIO interface (" + snapshot.asioDrivers.front()
					+ "), and playing to that device would silence it.").c_str());
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}
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

			BeginCard("Buffer");
			char current[64];
			sprintf_s(current, "Now %u frames (%.2f ms).", period, period / 48.0);
			const bool toggled = ToggleRow("Automatic buffer", (std::string(current) + " Smaller is lower latency; raise it if you hear crackles.").c_str(), &automatic);
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

		void OutputPage(const LiveState& live)
		{
			PageHeader("Output", "Where the game's sound goes and how loud it can get.");
			OutputDeviceCard(live);
			OutputBufferCard(live);

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
				ShellExecuteW(nullptr, L"open", (hasSession ? session : folder).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
			RecordHotkeyPicker();
			Caption("Each game launch saves into its own dated folder. The hotkey uses the format chosen above. Change the folder in RSMods, Rocksmith Audio Bridge page.");
			EndCard();
		}

		void GeneralPage()
		{
			PageHeader("General", "Overlays, second player and library tools.");
			static bool seeded = false;
			static bool p2Cable = false;
			if (!seeded) { p2Cable = Settings::ReturnSettingValue("CableForPlayerTwo") == "on"; seeded = true; }

			BeginCard("Display");
			bool diagnostics = Audio::CableInput::IsOverlayEnabled();
			if (ToggleRow("Audio diagnostics HUD", "Signal level, packet rate and route state in the top right corner.", &diagnostics))
				BridgeControl(27, diagnostics ? L"1" : L"0");
			EndCard();

			// The cable is added as Player 2 next to an ASIO Player 1; in Cable mode it already is Player 1.
			if (Audio::CableInput::IsAsioPath()) {
				BeginCard("Two player");
				if (ToggleRow("Real Tone Cable for Player 2", "Adds the cable after your ASIO input. RS_ASIO.ini is not changed.", &p2Cable))
					BridgeControl(26, p2Cable ? L"1" : L"0");
				EndCard();
			}

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
				ImGui::SetCursorScreenPos(ImVec2(right - 12.0f - pillW, origin.y + 19.0f));
				StatusPill(text.c_str(), Color::Record, true);
			}
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
		static float pageContentHeight = 0.0f;
		static float shownHeight = 0.0f;
		static ImVec2 lastPosition(-1.0f, -1.0f);
		const ImGuiIO& io = ImGui::GetIO();
		const float sidebarMinimum = 520.0f;   // nav items plus the "Press \ to hide" footer
		const float wanted = std::min(kHeaderHeight + std::max(pageContentHeight, sidebarMinimum), io.DisplaySize.y * 0.92f);
		shownHeight = shownHeight <= 0.0f ? wanted : shownHeight + (wanted - shownHeight) * std::min(1.0f, io.DeltaTime * 14.0f);
		ImGui::SetNextWindowSize(ImVec2(900, shownHeight), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(700, shownHeight), ImVec2(4000, shownHeight));
		if (lastPosition.y >= 0.0f && lastPosition.y + shownHeight > io.DisplaySize.y - 8.0f)
			ImGui::SetNextWindowPos(ImVec2(lastPosition.x, std::max(8.0f, io.DisplaySize.y - shownHeight - 8.0f)));
		const bool visible = ImGui::Begin("Rocksmith Audio Bridge##overlay", open,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		if (visible) {
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
			pageContentHeight = ImGui::GetCursorPosY() + 20.0f;   // content plus bottom padding, for next frame's height
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
		// Layout edit mode is the Note by Note page: its sliders and the on-screen blocks move together.
		if (visible && page == Page::NoteByNote) HudEditor();
		if (fonts.body) ImGui::PopFont();
	}
}
