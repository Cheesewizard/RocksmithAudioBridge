#include "NoteByNotePage.hpp"

#include "NoteByNoteHudLayout.hpp"
#include "NoteByNoteMenu.hpp"
#include "../NoteByNoteSettings.hpp"
#include "../Host/NoteByNoteMode.hpp"
#include "../../Shell/OverlayUi.hpp"
#include "../../Shell/ShellPages.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdint>

using namespace Overlay;
namespace NbnSettings = NoteByNote::ModSettings;

namespace NoteByNote::Ui
{
	namespace
	{
		// The frame the page last drew in, so the layout editor (the page's floating callback, which the shell runs
		// whatever page shows) runs only while this page is the one open. Render thread.
		int pageDrawnFrame = -1;

		uint32_t PackRgb(const float rgb[3])
		{
			auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
			return (channel(rgb[0]) << 16) | (channel(rgb[1]) << 8) | channel(rgb[2]);
		}

		ImU32 ToImColor(uint32_t rgb)
		{
			return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
		}

		void NoteByNotePage()
		{
			pageDrawnFrame = ImGui::GetFrameCount();
			const NbnSettings::Snapshot settings = NbnSettings::Get();

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

			using Block = NbnSettings::HudBlock;

			BeginCard("Detection readout");
			bool visible = settings.detectionVisible;
			if (ToggleRow("Show detection readout", "Native, Enhanced and ML lines while you practise. Detection keeps running when hidden.", &visible))
				NbnSettings::SetDetectionVisible(visible);

			bool committed = false;
			float uiSize = static_cast<float>(settings.uiSize);
			if (SliderRow("Text size", &uiSize, 50.0f, 300.0f, "%.0f%%", &committed, "Size of the Native, Enhanced and ML lines. 100% is the original size. The target has its own size below."))
				NbnSettings::SetUiSize(static_cast<int>(uiSize + 0.5f), false);
			if (committed) NbnSettings::SetUiSize(static_cast<int>(uiSize + 0.5f), true);

			committed = false;
			float spacing = static_cast<float>(settings.lineSpacing);
			if (SliderRow("Line spacing", &spacing, 50.0f, 300.0f, "%.0f%%", &committed, "Vertical gap between the Native, Enhanced and ML lines. 100% is the original spacing."))
				NbnSettings::SetLineSpacing(static_cast<int>(spacing + 0.5f), false);
			if (committed) NbnSettings::SetLineSpacing(static_cast<int>(spacing + 0.5f), true);
			EndCard();

			BeginCard("Target");
			committed = false;
			float targetSize = static_cast<float>(settings.targetSize);
			if (SliderRow("Size", &targetSize, 50.0f, 300.0f, "%.0f%%", &committed, "Size of the target note or chord and its string colour."))
				NbnSettings::SetTargetSize(static_cast<int>(targetSize + 0.5f), false);
			if (committed) NbnSettings::SetTargetSize(static_cast<int>(targetSize + 0.5f), true);

			static const char* const styles[] = { "Detailed", "Simple", "Tab", "Look ahead" };
			int style = static_cast<int>(settings.targetStyle);
			if (SegmentedRow("Style", &style, styles, 4, "Detailed names the string, fret and note. Simple shows just the fret next to the string colour, plus the bend (half bend, full bend). Tab shows the current note or chord. Look ahead opens a resizable song tab; scrolling never changes the current target."))
				NbnSettings::SetTargetStyle(static_cast<NbnSettings::TargetStyle>(style));

			// Custom is the only draggable position. Its spot is remembered while Under readout or Center is chosen,
			// so cycling back to Custom puts the target where it was left.
			using Position = NbnSettings::TargetPosition;
			static const char* const positions[] = { "Under readout", "Center", "Custom" };
			const Position current = settings.targetPosition;
			int position = current == Position::Custom ? 2 : current == Position::Center ? 1 : 0;
			ImGui::BeginDisabled(style == static_cast<int>(NbnSettings::TargetStyle::TabTimeline));
			if (SegmentedRow("Position", &position, positions, 3, "Under readout stacks it below the detection lines. Center puts it near the gameplay area. Custom is wherever you drag it, and is remembered.")) {
				if (position == 2 && !settings.targetPlacement.IsSet()) {
					// First time in Custom: start from where the target is drawn now.
					const HudLayout::Block& drawn = HudLayout::blocks[static_cast<int>(Block::Target)];
					if (HudLayout::IsLive(Block::Target) && HudLayout::frameWidth > 0 && HudLayout::frameHeight > 0)
						NbnSettings::SetPlacement(Block::Target, { drawn.x0 / HudLayout::frameWidth, drawn.y0 / HudLayout::frameHeight }, true);
				}
				NbnSettings::SetTargetPosition(position == 2 ? Position::Custom : position == 1 ? Position::Center : Position::Left);
			}
			ImGui::EndDisabled();
			if (style == static_cast<int>(NbnSettings::TargetStyle::TabTimeline))
				Caption("Drag the tab window by its title and resize it using its edges. Scroll to peek ahead; Back to current resumes following.");
			EndCard();

			BeginCard("Layout");
			Caption("While this page is open, drag the readout or the target on screen to move it (dragging the target sets its Position to Custom). Scroll over either to resize it, double-click to put it back.");
			if (!HudLayout::IsLive(Block::Readout) && !HudLayout::IsLive(Block::Target))
				Caption("Start a song with Note by Note on to see them.");
			if (Button("Reset layout", false, "Puts both back in their built-in places (target Under readout), forgets the Custom spot and restores text size, target size and line spacing to 100%. Colours are kept.")) {
				NbnSettings::SetPlacement(Block::Readout, {}, true);
				NbnSettings::SetPlacement(Block::Target, {}, true);
				NbnSettings::SetTargetPosition(Position::Left);
				NbnSettings::SetUiSize(100, true);
				NbnSettings::SetTargetSize(100, true);
				NbnSettings::SetLineSpacing(100, true);
			}
			EndCard();

			BeginCard("Colours");
			bool custom = settings.customColours;
			if (ToggleRow("Custom colours", custom ? "Your colours are active." : "Using the default colours.", &custom))
				NbnSettings::SetCustomColours(custom);

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
				const uint32_t rgb = settings.colors[i] & 0xFFFFFFu;
				float color[3] = { ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f };
				bool saved = false;
				if (ColorSwatch(names[i], color, &saved, hints[i])) NbnSettings::SetColor(i, PackRgb(color), false);
				if (saved) NbnSettings::SetColor(i, PackRgb(color), true);
				if (i < 2) ImGui::SameLine(0.0f, 18.0f);
			}

			// Preview on a game-black strip, so colours are judged against what they will sit on.
			const ImVec2 start = ImGui::GetCursorScreenPos();
			const float width = RowWidth(), height = 38.0f;
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->AddRectFilled(start, ImVec2(start.x + width, start.y + height), IM_COL32(0, 0, 0, 255), 4.0f);
			NbnSettings::Snapshot previewed = settings;
			previewed.customColours = custom;
			const NoteByNote::DetectionPalette palette = NbnSettings::Palette(previewed);
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
				NbnSettings::ResetColors();
			ImGui::EndDisabled();
			EndCard();

			Caption("Turn on Note by Note from the Riff Repeater menu in Rocksmith.");
		}

		// ---- Note by Note HUD layout editor ---------------------------------------------------------------
		// Runs while the Note by Note page is open. The HUD is drawn on ImGui's background draw list, not as ImGui
		// items: the HUD publishes the rectangles it drew (HudLayout) and this hit-tests the mouse against them,
		// outlining each block on the background draw list (under the HUD text, over the game). The shell window wins:
		// nothing here starts while the mouse is over any ImGui window. Dragging writes the placement live and saves
		// once on release; scroll steps and double-click resets save immediately.

		void HudEditor()
		{
			using Block = NbnSettings::HudBlock;
			ImGuiIO& io = ImGui::GetIO();
			static int dragging = -1;
			static ImVec2 grabOffset;
			const float frameW = HudLayout::frameWidth, frameH = HudLayout::frameHeight;
			if (frameW <= 0.0f || frameH <= 0.0f) { dragging = -1; return; }
			const NbnSettings::Snapshot settings = NbnSettings::Get();
			// The target's dragged spot is its Custom position: dragging it while Under readout or Center is chosen
			// switches the position to Custom (from where it is drawn now). Double-click reset only applies in Custom.
			const bool targetIsCustom = settings.targetPosition == NbnSettings::TargetPosition::Custom;
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
				const float x = std::clamp(mouse.x - grabOffset.x, 0.0f, (std::max)(0.0f, frameW - w));
				const float y = std::clamp(mouse.y - grabOffset.y, 0.0f, (std::max)(0.0f, frameH - h));
				const bool released = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
				// A few pixels of travel before anything moves, so a plain click (or the first half of a
				// double-click) never turns a built-in position into a custom one.
				static bool moved = false;
				if (!moved && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) moved = true;
				if (moved) {
					NbnSettings::SetPlacement(block, { x / frameW, y / frameH }, released);
					if (block == Block::Target && !targetIsCustom)
						NbnSettings::SetTargetPosition(NbnSettings::TargetPosition::Custom);
				}
				if (released) { dragging = -1; moved = false; }
			}
			else if (hovered >= 0) {
				const Block block = static_cast<Block>(hovered);
				const HudLayout::Block& r = HudLayout::blocks[hovered];
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					if (resettable(hovered)) NbnSettings::SetPlacement(block, {}, true);
				}
				else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
					dragging = hovered;
					grabOffset = ImVec2(mouse.x - r.x0, mouse.y - r.y0);
				}
				else if (io.MouseWheel != 0.0f) {
					const int step = io.MouseWheel > 0.0f ? 10 : -10;
					if (block == Block::Readout) NbnSettings::SetUiSize(settings.uiSize + step, true);
					else NbnSettings::SetTargetSize(settings.targetSize + step, true);
				}
			}

			// Keep the click, drag and wheel away from the game while the editor owns them. The overlay already claims
			// the input while it is open (AudioBridgeShellMod); this also makes ImGui report that it wants the mouse.
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

		// Layout edit mode is the Note by Note page: its sliders and the on-screen blocks move together. 4.x ran the
		// editor after the overlay's window whenever that page was the one shown.
		void HudEditorWhilePageShows()
		{
			if (pageDrawnFrame == ImGui::GetFrameCount()) HudEditor();
		}

		// Note by Note's settings only matter while it is switched on (Riff Repeater, Advanced Settings). The page stays
		// while its switch is missing from that menu, because it carries the repair message.
		bool IsPageAvailable()
		{
			return NoteByNoteProbe::IsAutomaticEnabledFast() || NoteByNoteMenu::IsMenuRowMissing();
		}
	}

	void RegisterNoteByNotePage()
	{
		AudioBridge::Shell::RegisterPage({ "note-by-note", "Practice", Glyph::MusicNote, "Note by Note", 10, NoteByNotePage,
			IsPageAvailable, HudEditorWhilePageShows });
	}
}
