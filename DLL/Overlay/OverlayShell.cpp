#include "../stdafx.h"
#include "OverlayShell.hpp"
#include "OverlayUi.hpp"
#include "HudLayout.hpp"
#include "../Audio/SharedOutput.hpp"
#include "../Audio/CableInput.hpp"
#include "../Audio/OutputDevices.hpp"
#include "../Audio/OutputTap.hpp"
#include "../Audio/TakeRecorder.hpp"
#include "../Audio/MlServiceLauncher.hpp"
#include "../Audio/ExternalAmp.hpp"
#include "../Research/ResearchBridge.hpp"
#include "../Mods/DropPedal/DropPedal.hpp"
#include "../Mods/NoteByNoteMenu.hpp"
#include "../Mods/NoteByNoteNativeScoring.hpp"
#include "../Mods/DropPedal/DropPedalOverlay.hpp"
#include "../GameState.hpp"
#include "../Keybindings.hpp"
#include "../ProductVersion.hpp"
#include <filesystem>
#include <cmath>
#include <shellapi.h>
#include <mutex>
#include <thread>

namespace Overlay
{
	namespace
	{
		enum class Page { NoteByNote, Mixer, Input, Output, Record, General, Debug, DropPedal, ExternalAmp };

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

			BeginCard("Flow");
			bool flow = NoteByNoteNativeScoring::GetFlowUntilMissEnabled();
			if (ToggleRow("Flow mode", "Keep playing while you keep up: the song only stops on a note you have not played yet. Off stops on every note, the old behaviour. Resets to on at the next launch.", &flow))
				NoteByNoteNativeScoring::SetFlowUntilMissEnabled(flow);
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
		// Play through an amp sim on the bridge's ASIO driver (Audio/ExternalAmp.hpp, AsioProxy/AmpLink.hpp).

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

		void ExternalAmpPage()
		{
			const ExternalAmp::Snapshot amp = ExternalAmp::GetSnapshot();
			const AmpLink::Status& link = amp.link;
			PageHeader("External amp", "Play through AmpliTube or any ASIO amp sim. Rocksmith keeps reading your clean signal.");

			BeginCard();
			bool enabled = amp.enabled;
			const char* state = !amp.enabled ? "Off: the game's amp plays as normal."
				: amp.gameAmpMuted ? "On: the game's amp is muted and you hear the external app."
				: amp.fallback ? "On: the game's amp plays until an amp sim connects."
				: "On: the game's amp stays muted, waiting for an amp sim.";
			if (ToggleRow("Use external amp", state, &enabled, "Saved. Note detection always uses your clean signal, whichever amp you hear."))
				ExternalAmp::SetEnabled(enabled);
			EndCard();

			BeginCard("Connection");
			if (!amp.proxyLoaded) {
				WarnCaption("Needs Rocksmith Audio Bridge ASIO as the game's audio driver. It is not loaded in this session.");
			}
			else {
				const std::string name = link.guestName[0] ? link.guestName : "an amp sim";
				if (link.connected) InfoRow("Connected app", name, Color::Good);
				else if (link.guestPresent) InfoRow("Connected app", name + " (audio stopped)", Color::Warn);
				else InfoRow("Connected app", "None", Color::Muted);
				char text[64];
				sprintf_s(text, "48 kHz, %d samples", link.frames);
				InfoRow("Format", link.frames > 0 ? text : "Waiting for the game's audio");
				sprintf_s(text, "+%.1f ms", link.frames * (link.latencyBlocks > 1 ? 2 : 1) * 1000.0 / AmpLink::kSampleRate);
				InfoRow("Added latency", text);
				sprintf_s(text, "%u this session", link.lateBlocks);
				InfoRow("Late buffers", text, link.lateBlocks > 0 ? Color::Warn : Color::Text);
				InfoRow("Game amp", amp.gameAmpMuted ? "Muted" : "Playing", amp.gameAmpMuted ? Color::Muted : Color::Text);
				if (amp.pedalSemitones != 0) {
					sprintf_s(text, "%+d semitones, sent to the amp sim too", amp.pedalSemitones);
					InfoRow("Drop Pedal", text);
				}
				if (link.guestPresent && link.guestFrames > 0 && link.frames > 0 && link.guestFrames != link.frames) {
					sprintf_s(text, "%d", link.frames);
					WarnCaption((name + " is set to " + std::to_string(link.guestFrames) + " samples. It should switch to " + text
						+ " by itself; if not, pick " + text + " in its audio settings.").c_str());
				}
				if (link.lateBlocks > 0)
					Caption("Late buffers are blocks the amp sim did not finish in time; each one is a click. Try the extra safety buffer below, a lighter preset, or a bigger game buffer.");
			}
			if (!amp.guest64Registered)
				WarnCaption("64-bit amp sims (AmpliTube 5) cannot see the driver yet. In the Rocksmith Audio Bridge window, press Repair next to the driver (one admin prompt).");
			EndCard();

			BeginCard("Levels");
			Meter("Clean signal out", MeterFraction(link.inputPeak), "Your raw guitar as the amp sim receives it, before the Input page's processing.");
			float returnDb = amp.returnTenths / 10.0f;
			bool returnSaved = false;
			if (SliderRow("Amp return", &returnDb, -30.0f, 6.0f, "%+.1f dB", &returnSaved, "Level of the amp sim in your mix."))
				ExternalAmp::SetReturnTenths(static_cast<int>(std::lround(returnDb * 10.0f)), false);
			if (returnSaved) ExternalAmp::SetReturnTenths(static_cast<int>(std::lround(returnDb * 10.0f)), true);
			Meter("Return level", MeterFraction(link.returnPeak), "The amp sim's output after the return level.");
			EndCard();

			BeginCard();
			bool safety = amp.safetyBuffer;
			char safetyCaption[96];
			sprintf_s(safetyCaption, "Adds one more buffer (+%.1f ms) so a busy PC clicks less.", (link.frames > 0 ? link.frames : 128) * 1000.0 / AmpLink::kSampleRate);
			if (ToggleRow("Extra safety buffer", safetyCaption, &safety)) ExternalAmp::SetSafetyBuffer(safety);
			bool fallback = amp.fallback;
			if (ToggleRow("Fall back to the game's amp", "If the app disconnects, the game's amp comes back on.", &fallback))
				ExternalAmp::SetFallback(fallback);
			EndCard();

			BeginCard("Setup");
			Caption("1. Open AmpliTube 5, or any ASIO amp sim.");
			Caption("2. In its audio settings, choose Rocksmith Audio Bridge ASIO. Input: Rocksmith guitar. Outputs: Rocksmith out L and R.");
			Caption("3. Keep Use external amp on. The game's amp mutes as soon as the app connects.");
			EndCard();
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
		// ("<frames>" newline "<endpoint>", 0 = automatic). Saved per device in AudioRouting.ini [Output buffer <endpoint>],
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
			if (!fundamental || maximum < minimum) return;

			const std::filesystem::path ini = GameFolder() / L"AudioRouting.ini";
			const std::wstring section = L"Output buffer " + endpoint;
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
				Audio::Takes::SetPreferVideo(format == 1);
				cachedFormat = format;
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
				std::error_code ec;
				std::filesystem::create_directories(folder, ec);
				ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
			RecordHotkeyPicker();
			Caption("The hotkey uses the format chosen above. Change the folder in RSMods, Rocksmith Audio Bridge page.");
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

			BeginCard("Two player");
			if (ToggleRow("Real Tone Cable for Player 2", "Adds the cable after your ASIO input. RS_ASIO.ini is not changed.", &p2Cable))
				BridgeControl(26, p2Cable ? L"1" : L"0");
			EndCard();

			BeginCard("Library");
			Caption("Picks up newly added CDLC without restarting. Open the song list once first if nothing changes.");
			if (Button("Update song list")) BridgeControl(28);
			EndCard();
		}

		// Three build types: Debug and Release are probe builds (they build and deploy the Note by
		// Note probe) and name themselves in the header, in amber, so a test build is never mistaken for the release.
		// Master ("Master" configuration, RSMODS_PUBLIC_RELEASE) is what users get: no probe, no Debug page, no label.
		// The upstream RSMods "with Wwise Logging" configurations count as Debug / Release.
#if defined(RSMODS_PUBLIC_RELEASE)
		constexpr bool kDebugPage = false;
		constexpr const char* kBuildLabel = "";
#elif defined(_DEBUG)
		constexpr bool kDebugPage = true;
		constexpr const char* kBuildLabel = "Debug + probe";
#else
		constexpr bool kDebugPage = true;
		constexpr const char* kBuildLabel = "Release + probe";
#endif

		void DebugPage()
		{
			PageHeader("Debug", "Development tools. Not in public releases.");

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
#ifndef RSMODS_PUBLIC_RELEASE
			// Only builds that ship a Note by Note probe offer the in-place reload.
			if (ResearchBridge::IsProbeReloadAvailable()) {
				BeginCard("Note by Note probe");
				// Which controller is running and whether it is the build on disk: the loaded file's content
				// hash against the deployed file's, since the compile stamp alone is not reliable. Polled twice
				// a second like the ML card; the deployed file is re-hashed only when it changes.
				{
					static ResearchBridge::ProbeIdentity probe;
					static uint64_t probeNextPoll = 0;
					const uint64_t now = GetTickCount64();
					if (now >= probeNextPoll) {
						ResearchBridge::GetProbeIdentity(probe);
						probeNextPoll = now + 500;
					}
					std::string headline;
					ImU32 headlineColor = Color::Muted;
					if (!probe.loaded) {
						headline = "Not loaded. Note by Note has no controller.";
						headlineColor = Color::Bad;
					}
					else if (probe.inProcess) {
						headline = "Running the built-in controller. The deployed probe is not loaded; Refresh loads it.";
						headlineColor = Color::Muted;
					}
					else if (probe.deployedPresent && probe.loadedHash != 0 && probe.loadedHash == probe.deployedHash) {
						headline = "Loaded and up to date with the deployed probe.";
						headlineColor = Color::Good;
					}
					else if (probe.deployedPresent) {
						headline = "Loaded, but a different probe is deployed now. Refresh to load it.";
						headlineColor = Color::Warn;
					}
					else {
						headline = "Loaded. No deployed probe on disk to compare with.";
						headlineColor = Color::Muted;
					}
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(headlineColor));
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
					ImGui::TextUnformatted(headline.c_str());
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();

					if (probe.loaded) {
						std::string running = std::string(probe.inProcess ? "Running: built-in" : "Running: #" + probe.loadedHashText)
							+ ", built " + probe.buildId
							+ ", " + (probe.isDebugBuild ? "Debug" : "Release")
							+ ", loaded " + probe.loadedAt;
						Caption(running.c_str());
					}
					if (probe.deployedPresent) {
						std::string deployed = "On disk: #" + probe.deployedHashText
							+ ", " + (probe.deployedIsDebugBuild ? "Debug" : "Release")
							+ (probe.deployedModified.empty() ? "" : ", written " + probe.deployedModified);
						Caption(deployed.c_str());
					}
					if (probe.loaded && !probe.inProcess && probe.isDebugBuild != probe.hostIsDebugBuild) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
						ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
						ImGui::TextUnformatted(probe.hostIsDebugBuild
							? "Configuration mismatch: Debug mod with a Release probe."
							: "Configuration mismatch: Release mod with a Debug probe.");
						ImGui::PopTextWrapPos();
						ImGui::PopStyleColor();
					}
				}
				if (Button("Refresh probes", false, "Hot-swaps the deployed probe DLL. Works mid-song: Note by Note is turned off for the swap, waits for the current note to release, and is turned back on.")) {
					std::string error;
					if (!ResearchBridge::ReloadDeployedProbe(error)) Caption(error.c_str());
				}
				const std::string reloadStatus = ResearchBridge::GetDeployedProbeReloadStatus();
				if (!reloadStatus.empty()) Caption(reloadStatus.c_str());
				EndCard();
			}
			else Caption("This build ships no Note by Note probe, so there is nothing to reload.");
#endif
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
			if (kBuildLabel[0] != '\0') {
				// Non-public builds say what they are, in amber so a test build is never mistaken for the release.
				ImGui::SameLine(0.0f, 10.0f);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
				ImGui::TextUnformatted(kBuildLabel);
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
				{ nullptr, Glyph::Link, "External amp", Page::ExternalAmp },
				{ nullptr, Glyph::Speaker, "Output", Page::Output },
				{ nullptr, Glyph::Record, "Record", Page::Record },
				{ "System", Glyph::Settings, "General", Page::General },
				{ nullptr, Glyph::Bug, "Debug", Page::Debug },
			};
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
			for (const Entry& entry : entries) {
				if (entry.page == Page::Debug && !kDebugPage) continue;
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
			switch (page) {
			case Page::NoteByNote: NoteByNotePage(); break;
			case Page::Mixer: MixerPage(live); break;
			case Page::Input: InputPage(live); break;
			case Page::Output: OutputPage(live); break;
			case Page::Record: RecordPage(live); break;
			case Page::General: GeneralPage(); break;
			case Page::Debug: DebugPage(); break;
			case Page::DropPedal: DropPedalPage(); break;
			case Page::ExternalAmp: ExternalAmpPage(); break;
			}
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
