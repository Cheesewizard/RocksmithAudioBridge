#pragma once

#include "OverlayUi.hpp"

#include <algorithm>
#include <cmath>

// The Tone designer's drawing parts, shared by its two tabs: the VST rig page (OverlayShell.cpp) and the built-in
// Tone designer (StompboxPage.cpp) draw the same framed panel, bar buttons, tone rows, rack, jacks, cables, pedals,
// meters and browser pills, so the two read as one app. Only what each tab shows differs.
namespace Overlay::ToneDesignerUi
{
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

	// A pedal's body and edge, its width and how many knobs it shows.
	struct PedalLook { ImU32 body, edge; float width; int knobs; };

	// The looks by role: dynamics teal, amps brown and wider, effects purple, others slate.
	namespace Looks
	{
		constexpr PedalLook Amp{ IM_COL32(58, 42, 34, 255), IM_COL32(90, 64, 51, 255), 150.0f, 5 };
		constexpr PedalLook Dynamics{ IM_COL32(38, 53, 58, 255), IM_COL32(53, 80, 88, 255), 96.0f, 2 };
		constexpr PedalLook Effect{ IM_COL32(44, 43, 68, 255), IM_COL32(69, 67, 106, 255), 96.0f, 2 };
		constexpr PedalLook Other{ IM_COL32(43, 50, 64, 255), IM_COL32(60, 69, 87, 255), 96.0f, 3 };
	}

	constexpr float PEDAL_HEIGHT = 124.0f;
	constexpr float ADD_TILE_WIDTH = 70.0f;
	constexpr float JACK_WIDTH = 42.0f;

	// Popups and menus get room around their items (the overlay's windows have no padding of their own).
	inline void PushPopupPadding() { ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8)); }

	inline ImFont* CaptionFont() { return GetFonts().caption ? GetFonts().caption : ImGui::GetFont(); }

	inline void Text(ImDrawList* list, ImFont* font, ImVec2 at, ImU32 color, const char* text)
	{
		list->AddText(font, font->FontSize, at, color, text);
	}

	inline float TextWidth(ImFont* font, const char* text) { return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text).x; }

	// Linear peak to meter fraction on a -60..0 dBFS scale, as the D3D diagnostics HUD does.
	inline float MeterFraction(float peak)
	{
		if (peak <= 0.0f) return 0.0f;
		return std::clamp((20.0f * std::log10(peak) + 60.0f) / 60.0f, 0.0f, 1.0f);
	}

	inline bool IconSquare(const char* id, unsigned glyph, bool on, const char* hint, float size)
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

	inline void FoldLabel(const char* text)
	{
		ImGui::Dummy(ImVec2(0, 2));
		ImGui::PushFont(CaptionFont());
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Dim));
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
		ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}

	// Muted caption text wrapped to `width` (the empty states of the library and the browser).
	inline void MutedCaption(const char* text, float width)
	{
		ImGui::PushFont(CaptionFont());
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}

	inline void Jack(ImDrawList* list, ImVec2 center, const char* label)
	{
		list->AddCircle(center, 7.0f, Ink::Jack, 16, 3.0f);
		ImFont* caption = CaptionFont();
		Text(list, caption, ImVec2(center.x - TextWidth(caption, label) * 0.5f, center.y + 12.0f), Ink::Dim, label);
	}

	// A list row's background: selected, hovered or nothing.
	inline void RowBackground(ImDrawList* list, ImVec2 at, ImVec2 end, bool selected, bool hovered)
	{
		if (selected) list->AddRectFilled(at, end, Ink::RowSelected, 5.0f);
		else if (hovered) list->AddRectFilled(at, end, Ink::RowHover, 5.0f);
	}

	// A vertical segmented meter centred in a column `width` wide (left edge `x`), between `labelTop` and the bottom of
	// its bar: green, then amber, then red at the top; all lit while it clips. Returns the height it takes.
	inline float MeterSegments(ImDrawList* list, float x, float labelTop, float width, const char* label, float peak)
	{
		constexpr int SEGMENTS = 6;
		const float segH = 8.0f, gap = 3.0f, barW = 10.0f;
		const float barH = SEGMENTS * (segH + gap) - gap;
		ImFont* caption = CaptionFont();
		const float barTop = labelTop + caption->FontSize + 6.0f;
		const float cx = x + width * 0.5f;
		Text(list, caption, ImVec2(cx - TextWidth(caption, label) * 0.5f, labelTop), Ink::Dim, label);
		const bool clipping = peak > 1.0f;
		const int lit = clipping ? SEGMENTS : static_cast<int>(std::lround(MeterFraction(peak) * SEGMENTS));
		for (int i = 0; i < SEGMENTS; ++i)
		{
			const float y = barTop + barH - (i + 1) * (segH + gap) + gap;
			const ImVec2 a(cx - barW * 0.5f, y), b(cx + barW * 0.5f, y + segH);
			const ImU32 on = i == SEGMENTS - 1 ? Ink::Bad : i == SEGMENTS - 2 ? Ink::Amber : Ink::Meter;
			list->AddRectFilled(a, b, i < lit ? on : Ink::Edge, 2.0f);   // unlit segments stay visible on the rack
		}
		return barTop + barH - labelTop;
	}

	// The meter's total height for a centred column (MeterSegments' layout).
	inline float MeterHeight() { return 6.0f * (8.0f + 3.0f) - 3.0f + CaptionFont()->FontSize + 6.0f; }

	// What a pedal tile shows. `sub` is the line under the name (vendor, file); `badge` an icon in its top-right corner
	// (0: none).
	struct PedalTile
	{
		const char* name = "";
		const char* sub = "";
		PedalLook look = Looks::Other;
		bool isPlaying = true;       // not bypassed: lit LED, full colour
		bool isSelected = false;
		bool isHovered = false;
		bool isOnSwitch = false;     // the mouse is on the footswitch
		unsigned badge = 0;
		ImU32 badgeColor = 0;
	};

	// The footswitch's centre for a tile at `at`.
	inline ImVec2 FootswitchCenter(ImVec2 at, const PedalLook& look) { return ImVec2(at.x + look.width * 0.5f, at.y + PEDAL_HEIGHT - 20.0f); }

	inline bool IsOnFootswitch(ImVec2 at, const PedalLook& look, ImVec2 mouse)
	{
		const ImVec2 footswitch = FootswitchCenter(at, look);
		return (mouse.x - footswitch.x) * (mouse.x - footswitch.x) + (mouse.y - footswitch.y) * (mouse.y - footswitch.y) < 15.0f * 15.0f;
	}

	// A pedal: body, LED, name and the line under it, knobs, footswitch. A bypassed pedal is drawn faded into the rack.
	inline void DrawPedalTile(ImDrawList* list, ImVec2 at, const PedalTile& tile)
	{
		const PedalLook& look = tile.look;
		const ImVec2 end(at.x + look.width, at.y + PEDAL_HEIGHT);
		const ImVec2 footswitch = FootswitchCenter(at, look);
		ImFont* caption = CaptionFont();
		auto fade = [&](ImU32 color) { return tile.isPlaying ? color : Mix(Ink::Rack, color, 0.55f); };
		if (tile.isSelected) list->AddRect(ImVec2(at.x - 3.0f, at.y - 3.0f), ImVec2(end.x + 3.0f, end.y + 3.0f), Ink::Outline, 11.0f, 0, 2.0f);
		list->AddRectFilled(at, end, fade(tile.isHovered ? Mix(look.body, IM_COL32_WHITE, 0.06f) : look.body), 9.0f);
		list->AddRect(at, end, fade(look.edge), 9.0f);
		list->AddCircleFilled(ImVec2(at.x + look.width * 0.5f, at.y + 12.0f), 4.0f, tile.isPlaying ? Ink::Led : Ink::LedOff, 12);
		if (tile.badge) DrawIcon(list, ImVec2(end.x - 13.0f, at.y + 13.0f), tile.badge, fade(tile.badgeColor));
		list->PushClipRect(ImVec2(at.x + 4.0f, at.y), ImVec2(end.x - 4.0f, end.y), true);
		const float nameW = ImGui::CalcTextSize(tile.name).x;
		list->AddText(ImVec2(at.x + std::max(5.0f, (look.width - nameW) * 0.5f), at.y + 22.0f), fade(IM_COL32_WHITE), tile.name);
		const float subW = TextWidth(caption, tile.sub);
		Text(list, caption, ImVec2(at.x + std::max(5.0f, (look.width - subW) * 0.5f), at.y + 24.0f + ImGui::GetFontSize()), fade(Ink::Sub), tile.sub);
		list->PopClipRect();
		const float knobsW = look.knobs * 16.0f + (look.knobs - 1) * 7.0f;
		for (int k = 0; k < look.knobs; ++k)
		{
			const ImVec2 knob(at.x + (look.width - knobsW) * 0.5f + 8.0f + k * 23.0f, at.y + 68.0f);
			list->AddCircleFilled(knob, 8.0f, fade(Ink::Knob), 16);
			list->AddCircle(knob, 8.0f, fade(Ink::KnobEdge), 16, 2.0f);
			list->AddLine(knob, ImVec2(knob.x - 3.0f, knob.y - 5.0f), fade(Ink::KnobEdge), 1.5f);
		}
		list->AddCircleFilled(footswitch, 12.0f, fade(tile.isHovered && tile.isOnSwitch ? IM_COL32(190, 198, 212, 255) : Ink::Switch), 20);
		list->AddCircle(footswitch, 12.0f, fade(Ink::SwitchEdge), 20, 2.0f);
	}

	// Dimmed tile with a turning arc and a word under it: a pedal that is opening or loading.
	inline void DrawTileBusy(ImDrawList* list, ImVec2 at, float width, const char* word)
	{
		const ImVec2 end(at.x + width, at.y + PEDAL_HEIGHT);
		list->AddRectFilled(at, end, IM_COL32(0, 0, 0, 120), 9.0f);
		const ImVec2 c(at.x + width * 0.5f, at.y + PEDAL_HEIGHT * 0.5f - 6.0f);
		const float a0 = static_cast<float>(ImGui::GetTime()) * 6.0f;
		list->PathArcTo(c, 14.0f, a0, a0 + 4.2f, 24);
		list->PathStroke(Ink::AddText, 0, 3.0f);
		ImFont* caption = CaptionFont();
		Text(list, caption, ImVec2(c.x - TextWidth(caption, word) * 0.5f, c.y + 20.0f), IM_COL32_WHITE, word);
	}

	// The dashed Add tile, at `at`; `midY` is the rack's cable line.
	inline void DrawAddTile(ImDrawList* list, ImVec2 at, float midY, bool hovered)
	{
		const ImVec2 end(at.x + ADD_TILE_WIDTH, at.y + PEDAL_HEIGHT);
		const ImU32 dash = hovered ? Ink::AddText : Ink::Jack;
		for (float d = at.x + 9.0f; d < end.x - 9.0f; d += 8.0f)
		{
			list->AddLine(ImVec2(d, at.y), ImVec2(std::min(d + 4.0f, end.x - 9.0f), at.y), dash);
			list->AddLine(ImVec2(d, end.y), ImVec2(std::min(d + 4.0f, end.x - 9.0f), end.y), dash);
		}
		for (float d = at.y + 9.0f; d < end.y - 9.0f; d += 8.0f)
		{
			list->AddLine(ImVec2(at.x, d), ImVec2(at.x, std::min(d + 4.0f, end.y - 9.0f)), dash);
			list->AddLine(ImVec2(end.x, d), ImVec2(end.x, std::min(d + 4.0f, end.y - 9.0f)), dash);
		}
		ImFont* caption = CaptionFont();
		DrawIcon(list, ImVec2(at.x + ADD_TILE_WIDTH * 0.5f, midY - 8.0f), Glyph::Add, Ink::AddText);
		Text(list, caption, ImVec2(at.x + (ADD_TILE_WIDTH - TextWidth(caption, "Add")) * 0.5f, midY + 6.0f), Ink::AddText, "Add");
	}

	// The rack's backdrop at `at`, `width` x `height`.
	inline void DrawRack(ImDrawList* list, ImVec2 at, float width, float height)
	{
		list->AddRectFilled(at, ImVec2(at.x + width, at.y + height), Ink::Rack, 10.0f);
		list->AddRect(at, ImVec2(at.x + width, at.y + height), Ink::RackEdge, 10.0f);
	}

	// The framed panel behind a tab: the app background, the bar, the library column and the divider over the pane.
	struct PanelFrame
	{
		ImVec2 at;
		float width = 0.0f;
		float barHeight = 0.0f;
		float mainHeight = 0.0f;     // library + board
		float libraryWidth = 0.0f;   // 0: hidden
		float bottom = 0.0f;         // the panel's bottom edge (screen y)
		bool hasPane = false;        // the pane under the board is shown
		float dividerTop = 0.0f;
		float dividerHeight = 0.0f;
		bool isDividerHot = false;
	};

	inline void DrawPanelFrame(ImDrawList* list, const PanelFrame& frame)
	{
		const ImVec2 at = frame.at;
		const ImVec2 end(at.x + frame.width, frame.bottom);
		list->AddRectFilled(at, end, Ink::App, 12.0f);
		list->AddRectFilled(at, ImVec2(end.x, at.y + frame.barHeight), Ink::Bar, 12.0f, ImDrawFlags_RoundCornersTop);
		list->AddLine(ImVec2(at.x, at.y + frame.barHeight), ImVec2(end.x, at.y + frame.barHeight), Ink::Edge);
		if (frame.libraryWidth > 0.0f)
		{
			list->AddRectFilled(ImVec2(at.x, at.y + frame.barHeight + 1.0f), ImVec2(at.x + frame.libraryWidth, at.y + frame.barHeight + frame.mainHeight), Ink::Library,
				frame.hasPane ? 0.0f : 12.0f, frame.hasPane ? 0 : ImDrawFlags_RoundCornersBottomLeft);
			list->AddLine(ImVec2(at.x + frame.libraryWidth, at.y + frame.barHeight), ImVec2(at.x + frame.libraryWidth, at.y + frame.barHeight + frame.mainHeight), Ink::Edge);
		}
		if (frame.hasPane)
		{
			const float lineY = frame.dividerTop + frame.dividerHeight * 0.5f;
			list->AddLine(ImVec2(at.x, lineY), ImVec2(end.x, lineY), frame.isDividerHot ? Ink::Blue : Ink::Edge, frame.isDividerHot ? 2.0f : 1.0f);
			const float mid = at.x + frame.width * 0.5f;   // a grip in the middle says it moves
			list->AddRectFilled(ImVec2(mid - 18.0f, lineY - 2.0f), ImVec2(mid + 18.0f, lineY + 2.0f), frame.isDividerHot ? Ink::Blue : Ink::FieldEdge, 2.0f);
		}
		list->AddRect(at, end, Ink::Edge, 12.0f);
	}

	// A status pill's text and colours.
	struct PillLook { const char* text; ImU32 fill, edge, ink; };

	inline PillLook TintPill(const char* text, ImU32 hue) { return { text, Mix(Ink::App, hue, 0.18f), Mix(Ink::App, hue, 0.55f), hue }; }

	inline PillLook NeutralPill(const char* text) { return { text, IM_COL32(40, 45, 55, 255), Ink::FieldEdge, Ink::Soft }; }

	inline float PillWidth(ImFont* font, const char* text, bool dot = false) { return TextWidth(font, text) + 22.0f + (dot ? 14.0f : 0.0f); }

	// Draws a pill at `at`, `height` tall, with an optional status dot before the text; returns its width.
	// A `fixedWidth` makes it that wide with the text centred (a status column).
	inline float DrawPill(ImDrawList* list, ImFont* font, ImVec2 at, float height, const PillLook& look, ImU32 dot = 0, float fixedWidth = 0.0f)
	{
		const float width = fixedWidth > 0.0f ? fixedWidth : PillWidth(font, look.text, dot != 0);
		const ImVec2 end(at.x + width, at.y + height);
		list->AddRectFilled(at, end, look.fill, height * 0.5f);
		list->AddRect(at, end, look.edge, height * 0.5f);
		float x = fixedWidth > 0.0f ? at.x + (width - TextWidth(font, look.text)) * 0.5f : at.x + 11.0f;
		if (dot)
		{
			list->AddCircleFilled(ImVec2(x + 4.0f, at.y + height * 0.5f), 4.0f, dot, 12);
			x += 14.0f;
		}
		Text(list, font, ImVec2(x, at.y + (height - font->FontSize) * 0.5f), look.ink, look.text);
		return width;
	}

	inline bool Chip(const char* label, bool on)
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

	// A segmented toggle drawn like the pane's title ("Plugins | Songs"): `count` labels, `selected` the one lit.
	// Returns the label clicked, or -1.
	inline int PaneToggle(const char* const* labels, int count, int selected, const char* const* hints = nullptr)
	{
		ImFont* font = CaptionFont();
		ImDrawList* list = ImGui::GetWindowDrawList();
		const float height = ImGui::GetFrameHeight();
		const ImVec2 start = ImGui::GetCursorScreenPos();
		float x = start.x;
		int clicked = -1;
		for (int i = 0; i < count; ++i)
		{
			const float w = TextWidth(font, labels[i]) + 22.0f;
			const ImVec2 at(x, start.y);
			const bool on = selected == i;
			ImGui::SetCursorScreenPos(at);
			ImGui::PushID(i);
			if (ImGui::InvisibleButton("##pane", ImVec2(w, height))) clicked = i;
			ImGui::PopID();
			const bool hovered = ImGui::IsItemHovered();
			if (hints && hints[i]) Hint(hints[i]);
			const ImDrawFlags corners = i == 0 ? ImDrawFlags_RoundCornersLeft : i == count - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;
			if (on) list->AddRectFilled(at, ImVec2(at.x + w, at.y + height), Ink::Blue, 6.0f, corners);
			Text(list, font, ImVec2(at.x + 11.0f, at.y + (height - font->FontSize) * 0.5f), on ? IM_COL32_WHITE : hovered ? Ink::Text : Ink::Muted, labels[i]);
			x += w;
		}
		list->AddRect(start, ImVec2(x, start.y + height), Ink::FieldEdge, 6.0f);
		ImGui::SetCursorScreenPos(start);
		ImGui::Dummy(ImVec2(x - start.x, height));
		return clicked;
	}
}
