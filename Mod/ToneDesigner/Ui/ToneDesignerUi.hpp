#pragma once

#include "../../Shell/OverlayUi.hpp"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <vector>

// The Tone designer's drawing parts, shared by its two tabs: the VST rig page (ToneDesignerPage.cpp) and the built-in
// Tone designer (StompboxPage.cpp) draw the same framed panel, bar buttons, tone rows, rack, jacks, cables, pedals,
// meters and browser pills, so the two read as one app. Only what each tab shows differs.
namespace Overlay::ToneDesignerUi
{
	// The panel's own colours: the overlay shell's navy family (OverlayUi.hpp Color), a shade darker for the rack and
	// fields, so the page reads as part of the overlay. The pedals keep their own colours (Looks).
	namespace Ink
	{
		constexpr ImU32 App = IM_COL32(13, 20, 36, 255);
		constexpr ImU32 Edge = IM_COL32(34, 46, 72, 255);
		constexpr ImU32 Bar = IM_COL32(17, 26, 45, 255);
		constexpr ImU32 Field = IM_COL32(9, 14, 26, 255);
		constexpr ImU32 FieldEdge = IM_COL32(44, 58, 88, 255);
		constexpr ImU32 Text = IM_COL32(230, 234, 242, 255);
		constexpr ImU32 Soft = IM_COL32(186, 196, 214, 255);
		constexpr ImU32 Muted = IM_COL32(138, 151, 173, 255);
		constexpr ImU32 Dim = IM_COL32(112, 126, 152, 255);
		constexpr ImU32 Blue = IM_COL32(59, 139, 235, 255);
		constexpr ImU32 Amber = IM_COL32(240, 180, 41, 255);
		constexpr ImU32 Library = IM_COL32(15, 23, 40, 255);
		constexpr ImU32 RowHover = IM_COL32(30, 42, 66, 255);
		constexpr ImU32 RowSelected = IM_COL32(36, 62, 110, 255);
		constexpr ImU32 Rack = IM_COL32(8, 13, 24, 255);
		constexpr ImU32 RackEdge = IM_COL32(30, 42, 66, 255);
		constexpr ImU32 Jack = IM_COL32(70, 86, 116, 255);
		constexpr ImU32 Cable = IM_COL32(54, 70, 98, 255);
		constexpr ImU32 Outline = IM_COL32(96, 165, 250, 255);
		constexpr ImU32 Led = IM_COL32(229, 72, 77, 255);
		constexpr ImU32 LedOff = IM_COL32(61, 37, 38, 255);
		constexpr ImU32 Sub = IM_COL32(150, 163, 186, 255);
		constexpr ImU32 Knob = IM_COL32(16, 23, 38, 255);
		constexpr ImU32 KnobEdge = IM_COL32(84, 100, 130, 255);
		constexpr ImU32 Switch = IM_COL32(160, 172, 194, 255);
		constexpr ImU32 SwitchEdge = IM_COL32(96, 110, 138, 255);
		constexpr ImU32 AddText = IM_COL32(120, 170, 250, 255);
		constexpr ImU32 Meter = IM_COL32(48, 164, 108, 255);
		constexpr ImU32 Tag = IM_COL32(34, 60, 108, 255);
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

	// The bar's text button (Save), `height` tall: blue while there is something to do, dim otherwise.
	inline float BarButtonWidth(const char* label) { return ImGui::CalcTextSize(label).x + 28.0f; }

	inline bool BarButton(const char* id, const char* label, float height, bool enabled, const char* hint)
	{
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const float width = BarButtonWidth(label);
		const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height)) && enabled;
		const bool hovered = ImGui::IsItemHovered();
		ImDrawList* list = ImGui::GetWindowDrawList();
		const ImVec2 end(at.x + width, at.y + height);
		if (enabled) list->AddRectFilled(at, end, hovered ? Mix(Ink::Blue, IM_COL32_WHITE, 0.12f) : Ink::Blue, 6.0f);
		else list->AddRect(at, end, Ink::FieldEdge, 6.0f);
		const ImVec2 size = ImGui::CalcTextSize(label);
		list->AddText(ImVec2(at.x + (width - size.x) * 0.5f, at.y + (height - size.y) * 0.5f), enabled ? IM_COL32_WHITE : Ink::Dim, label);
		Hint(hint);
		return pressed;
	}

	// The bar's labelled switch (Bypass all): the label, then a small track with its knob, lit while on.
	inline float BarToggleWidth(const char* label) { return ImGui::CalcTextSize(label).x + 8.0f + 30.0f; }

	inline bool BarToggle(const char* id, const char* label, bool on, float height, bool enabled, const char* hint)
	{
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const float width = BarToggleWidth(label);
		const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height)) && enabled;
		const bool hovered = ImGui::IsItemHovered();
		ImDrawList* list = ImGui::GetWindowDrawList();
		const ImVec2 size = ImGui::CalcTextSize(label);
		list->AddText(ImVec2(at.x, at.y + (height - size.y) * 0.5f), !enabled ? Ink::Dim : hovered ? IM_COL32_WHITE : Ink::Soft, label);
		const ImVec2 track(at.x + size.x + 8.0f, at.y + height * 0.5f - 8.0f);
		list->AddRectFilled(track, ImVec2(track.x + 30.0f, track.y + 16.0f), on ? Ink::Amber : Ink::Field, 8.0f);
		list->AddRect(track, ImVec2(track.x + 30.0f, track.y + 16.0f), on ? Ink::Amber : Ink::FieldEdge, 8.0f);
		list->AddCircleFilled(ImVec2(on ? track.x + 22.0f : track.x + 8.0f, track.y + 8.0f), 5.5f, enabled ? IM_COL32_WHITE : Ink::Dim, 16);
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

	// ---- Board size --------------------------------------------------------------------------------------
	// The board draws its pedals at one of three sizes (View, Board size; Ctrl+wheel on the board), remembered in
	// HKCU\Software\RSMods\Overlay\BoardSize (0 small, 1 medium, 2 large). Every board measure below is multiplied by
	// the scale, so the pedals, jacks, cables and the Add tile grow together and a long chain wraps into rows.

	constexpr float BOARD_SCALES[3] = { 1.0f, 1.3f, 1.6f };
	constexpr const char* BOARD_SIZE_NAMES[3] = { "Small", "Medium", "Large" };

	inline int& BoardSizeSlot()
	{
		static int size = -1;
		if (size < 0)
		{
			DWORD value = 1, bytes = sizeof(value);
			if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\Overlay", L"BoardSize", RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS) value = 1;
			size = static_cast<int>(std::min<DWORD>(value, 2));
		}
		return size;
	}

	inline int BoardSize() { return BoardSizeSlot(); }
	inline float BoardScale() { return BOARD_SCALES[BoardSize()]; }

	inline void SetBoardSize(int size)
	{
		size = std::clamp(size, 0, 2);
		if (size == BoardSizeSlot()) return;
		BoardSizeSlot() = size;
		const DWORD value = static_cast<DWORD>(size);
		RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\Overlay", L"BoardSize", REG_DWORD, &value, sizeof(value));
	}

	// View's Board size items (both tabs).
	inline void BoardSizeMenu()
	{
		if (!ImGui::BeginMenu("Board size")) return;
		for (int i = 0; i < 3; ++i)
		{
			if (ImGui::MenuItem(BOARD_SIZE_NAMES[i], i == 0 ? "Ctrl+wheel on the board" : nullptr, BoardSize() == i)) SetBoardSize(i);
		}
		ImGui::EndMenu();
	}

	// Ctrl+wheel over the board steps the size; call it inside the board's window. Returns true when it took the wheel.
	inline bool BoardSizeWheel()
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (!io.KeyCtrl || io.MouseWheel == 0.0f || !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) return false;
		SetBoardSize(BoardSize() + (io.MouseWheel > 0.0f ? 1 : -1));
		return true;
	}

	// ---- Pedal tiles -------------------------------------------------------------------------------------

	// The symbol in a pedal's middle, by what it does: an amp's grille, a cabinet's speaker, an EQ's sliders, a
	// compressor's meter, else a waveform. It replaced five decorative knobs that looked like controls and were not.
	enum class Symbol { Wave, Amp, Cabinet, Eq, Dynamics };

	inline void DrawSymbol(ImDrawList* list, Symbol symbol, ImVec2 c, ImU32 color, float s)
	{
		const float w = 1.5f * s;
		switch (symbol)
		{
		case Symbol::Amp:
			list->AddRect(ImVec2(c.x - 26 * s, c.y - 9 * s), ImVec2(c.x + 26 * s, c.y + 9 * s), color, 3.0f * s, 0, w);
			for (int i = -2; i <= 2; ++i) list->AddCircle(ImVec2(c.x + i * 10.0f * s, c.y), 2.0f * s, color, 10, w);
			return;
		case Symbol::Cabinet:
			list->AddCircle(c, 13.0f * s, color, 20, w);
			list->AddCircle(c, 7.0f * s, color, 16, w);
			return;
		case Symbol::Eq:
			for (int i = -3; i <= 3; ++i)
			{
				const float x = c.x + i * 7.0f * s;
				list->AddLine(ImVec2(x, c.y - 12 * s), ImVec2(x, c.y + 12 * s), color, w);
				list->AddCircleFilled(ImVec2(x, c.y + (i % 2 == 0 ? -4 : 4) * s), 2.0f * s, color);
			}
			return;
		case Symbol::Dynamics:
			for (int i = 0; i < 5; ++i)
			{
				const float x = c.x + (i - 2) * 8.0f * s, h = (4.0f + i * 4.0f) * s;
				list->AddRectFilled(ImVec2(x - 2.5f * s, c.y + 11 * s - h), ImVec2(x + 2.5f * s, c.y + 11 * s), color, 1.0f);
			}
			return;
		default:
		{
			const ImVec2 wave[] = { ImVec2(c.x - 25 * s, c.y), ImVec2(c.x - 15 * s, c.y), ImVec2(c.x - 7 * s, c.y - 10 * s), ImVec2(c.x, c.y + 12 * s),
				ImVec2(c.x + 7 * s, c.y - 13 * s), ImVec2(c.x + 15 * s, c.y), ImVec2(c.x + 25 * s, c.y) };
			list->AddPolyline(wave, IM_ARRAYSIZE(wave), color, false, w);
			return;
		}
		}
	}

	// What a pedal tile shows. `sub` is the line under the name (vendor, file); `badge` an icon in its top-right corner
	// (0: none).
	struct PedalTile
	{
		const char* name = "";
		const char* sub = "";
		PedalLook look = Looks::Other;
		Symbol symbol = Symbol::Wave;
		bool isPlaying = true;       // not bypassed: lit LED, full colour
		bool isSelected = false;
		bool isHovered = false;
		bool isOnSwitch = false;     // the mouse is on the footswitch
		unsigned badge = 0;
		ImU32 badgeColor = 0;
	};

	inline float PedalWidth(const PedalLook& look, float s) { return std::floor(look.width * s); }
	inline float PedalHeight(float s) { return std::floor(PEDAL_HEIGHT * s); }

	// The footswitch's centre for a tile at `at`.
	inline ImVec2 FootswitchCenter(ImVec2 at, const PedalLook& look, float s)
	{
		return ImVec2(at.x + PedalWidth(look, s) * 0.5f, at.y + PedalHeight(s) - 20.0f * s);
	}

	inline bool IsOnFootswitch(ImVec2 at, const PedalLook& look, ImVec2 mouse, float s)
	{
		const ImVec2 footswitch = FootswitchCenter(at, look, s);
		const float r = 15.0f * s;
		return (mouse.x - footswitch.x) * (mouse.x - footswitch.x) + (mouse.y - footswitch.y) * (mouse.y - footswitch.y) < r * r;
	}

	// A pedal at scale `s`: body, LED, name (the body font, scaled, so it reads from across the room) and the line under
	// it, its symbol, footswitch. A bypassed pedal is drawn faded into the rack.
	inline void DrawPedalTile(ImDrawList* list, ImVec2 at, const PedalTile& tile, float s)
	{
		const PedalLook& look = tile.look;
		const float width = PedalWidth(look, s);
		const ImVec2 end(at.x + width, at.y + PedalHeight(s));
		const ImVec2 footswitch = FootswitchCenter(at, look, s);
		ImFont* font = ImGui::GetFont();
		ImFont* caption = CaptionFont();
		const float nameSize = ImGui::GetFontSize() * s, subSize = caption->FontSize * s;
		auto fade = [&](ImU32 color) { return tile.isPlaying ? color : Mix(Ink::Rack, color, 0.55f); };
		if (tile.isSelected) list->AddRect(ImVec2(at.x - 3.0f, at.y - 3.0f), ImVec2(end.x + 3.0f, end.y + 3.0f), Ink::Outline, 11.0f * s, 0, 2.0f);
		list->AddRectFilled(at, end, fade(tile.isHovered ? Mix(look.body, IM_COL32_WHITE, 0.06f) : look.body), 9.0f * s);
		list->AddRect(at, end, fade(look.edge), 9.0f * s);
		list->AddCircleFilled(ImVec2(at.x + width * 0.5f, at.y + 12.0f * s), 4.0f * s, tile.isPlaying ? Ink::Led : Ink::LedOff, 12);
		if (tile.badge) DrawIcon(list, ImVec2(end.x - 13.0f * s, at.y + 13.0f * s), tile.badge, fade(tile.badgeColor));
		list->PushClipRect(ImVec2(at.x + 4.0f, at.y), ImVec2(end.x - 4.0f, end.y), true);
		const float nameW = font->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, tile.name).x;
		const float nameY = at.y + 21.0f * s;
		list->AddText(font, nameSize, ImVec2(at.x + std::max(5.0f, (width - nameW) * 0.5f), nameY), fade(IM_COL32_WHITE), tile.name);
		const float subW = caption->CalcTextSizeA(subSize, FLT_MAX, 0.0f, tile.sub).x;
		list->AddText(caption, subSize, ImVec2(at.x + std::max(5.0f, (width - subW) * 0.5f), nameY + nameSize + 2.0f * s), fade(Ink::Sub), tile.sub);
		list->PopClipRect();
		DrawSymbol(list, tile.symbol, ImVec2(at.x + width * 0.5f, at.y + 72.0f * s), fade(Ink::Soft), s);
		list->AddCircleFilled(footswitch, 12.0f * s, fade(tile.isHovered && tile.isOnSwitch ? IM_COL32(190, 198, 212, 255) : Ink::Switch), 20);
		list->AddCircle(footswitch, 12.0f * s, fade(Ink::SwitchEdge), 20, 2.0f);
	}

	// Dimmed tile with a turning arc and a word under it: a pedal that is opening or loading.
	inline void DrawTileBusy(ImDrawList* list, ImVec2 at, float width, const char* word, float s)
	{
		const ImVec2 end(at.x + width, at.y + PedalHeight(s));
		list->AddRectFilled(at, end, IM_COL32(0, 0, 0, 120), 9.0f * s);
		const ImVec2 c(at.x + width * 0.5f, at.y + PedalHeight(s) * 0.5f - 6.0f * s);
		const float a0 = static_cast<float>(ImGui::GetTime()) * 6.0f;
		list->PathArcTo(c, 14.0f * s, a0, a0 + 4.2f, 24);
		list->PathStroke(Ink::AddText, 0, 3.0f);
		ImFont* caption = CaptionFont();
		Text(list, caption, ImVec2(c.x - TextWidth(caption, word) * 0.5f, c.y + 20.0f * s), IM_COL32_WHITE, word);
	}

	// The dashed Add tile at `at`, the pedals' height.
	inline void DrawAddTile(ImDrawList* list, ImVec2 at, bool hovered, float s)
	{
		const float width = std::floor(ADD_TILE_WIDTH * s);
		const ImVec2 end(at.x + width, at.y + PedalHeight(s));
		const float midY = at.y + PedalHeight(s) * 0.5f;
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
		DrawIcon(list, ImVec2(at.x + width * 0.5f, midY - 8.0f * s), Glyph::Add, Ink::AddText);
		const float size = caption->FontSize * s;
		list->AddText(caption, size, ImVec2(at.x + (width - caption->CalcTextSizeA(size, FLT_MAX, 0.0f, "Add").x) * 0.5f, midY + 6.0f * s), Ink::AddText, "Add");
	}

	// A jack (In, Out) whose box starts at `at`, the pedals' height: the socket on the cable line, its label under it.
	inline void DrawJack(ImDrawList* list, ImVec2 at, const char* label, float s)
	{
		const ImVec2 center(at.x + JACK_WIDTH * s * 0.5f, at.y + PedalHeight(s) * 0.5f - 6.0f * s);
		list->AddCircle(center, 7.0f * s, Ink::Jack, 16, 3.0f);
		ImFont* caption = CaptionFont();
		const float size = caption->FontSize * s;
		list->AddText(caption, size, ImVec2(center.x - caption->CalcTextSizeA(size, FLT_MAX, 0.0f, label).x * 0.5f, center.y + 12.0f * s), Ink::Dim, label);
	}

	// The rack's backdrop at `at`, `width` x `height`.
	inline void DrawRack(ImDrawList* list, ImVec2 at, float width, float height)
	{
		list->AddRectFilled(at, ImVec2(at.x + width, at.y + height), Ink::Rack, 10.0f);
		list->AddRect(at, ImVec2(at.x + width, at.y + height), Ink::RackEdge, 10.0f);
	}

	// ---- The rack's layout -------------------------------------------------------------------------------
	// In, the pedals, Add and Out flow left to right with a cable between each two; one that would cross the rack's
	// right edge starts a new row, so a long chain wraps instead of scrolling sideways and every pedal stays in view.
	// Panel (for the strip's height) and Board (to draw) use the same RackLayout, so the two always agree.

	constexpr float RACK_INSET = 10.0f;     // the rack's inner margin, left and right
	constexpr float RACK_ROW_GAP = 18.0f;   // between rows, before scale

	// The gap between two items, cable included.
	inline float CableGap(float s) { return std::floor(30.0f * s); }
	inline float RowHeight(float s) { return PedalHeight(s) + std::floor(RACK_ROW_GAP * s); }

	struct RackPlacement
	{
		std::vector<ImVec2> at;     // each item's top-left, relative to the rack's inner top-left
		std::vector<int> row;       // each item's row
		int rows = 1;
		float height = 0.0f;        // every row, without the rack's padding
	};

	// `widths`: each item's width at scale `s` (jacks, pedals, the Add tile, in order); `inner`: the rack's width
	// inside RACK_INSET on both sides.
	inline RackPlacement RackLayout(const std::vector<float>& widths, float inner, float s)
	{
		RackPlacement placement;
		float x = 0.0f, y = 0.0f;
		int row = 0;
		for (size_t i = 0; i < widths.size(); ++i)
		{
			if (i > 0 && x + widths[i] > inner)
			{
				x = 0.0f;
				y += RowHeight(s);
				++row;
			}
			placement.at.emplace_back(x, y);
			placement.row.push_back(row);
			x += widths[i] + CableGap(s);
		}
		placement.rows = row + 1;
		placement.height = placement.rows * PedalHeight(s) + row * std::floor(RACK_ROW_GAP * s);
		return placement;
	}

	// The cable between item `i - 1` and item `i` when both are on the same row (a new row starts without one).
	inline void DrawCable(ImDrawList* list, ImVec2 origin, const RackPlacement& placement, const std::vector<float>& widths, size_t i, float s)
	{
		if (i == 0 || placement.row[i] != placement.row[i - 1]) return;
		const float y = origin.y + placement.at[i].y + PedalHeight(s) * 0.5f;
		const float from = origin.x + placement.at[i - 1].x + widths[i - 1] + 8.0f * s;
		list->AddLine(ImVec2(from, y), ImVec2(origin.x + placement.at[i].x - 8.0f * s, y), Ink::Cable, 3.0f * s);
	}

	// ---- The panel's layout ----------------------------------------------------------------------------
	// Both tabs lay their panel out the same way:
	//   - the bar across the top: File / Edit / View, the tone's name between arrows, Save, Bypass all;
	//   - one browser column on the left, the full height, with its own tabs (Tones | Plugins or Pedals | Songs);
	//   - on the right, the board as a strip only as tall as its rows of pedals, and under it the Inspector: the
	//     selected pedal's controls.
	// The board used to take every pixel the tone list asked for, which left most of it empty; the Inspector takes
	// that height now, and new per-pedal features go there as rows, so the layout itself does not change again.

	constexpr float BROWSER_MAX_WIDTH = 270.0f;
	constexpr float BOARD_PADDING = 14.0f;
	constexpr float METER_COLUMN_WIDTH = 44.0f;

	// The browser column's width for a panel `width` wide.
	inline float BrowserWidth(float width) { return std::floor(std::min(BROWSER_MAX_WIDTH, width * 0.30f)); }

	// The rack's inner width on a board `boardWidth` wide (the board's padding, the meters, the rack's inset).
	inline float RackInnerWidth(float boardWidth, bool meters)
	{
		return std::max(1.0f, boardWidth - 2.0f * BOARD_PADDING - (meters ? 2.0f * METER_COLUMN_WIDTH : 0.0f) - 2.0f * RACK_INSET);
	}

	// The board strip's height for a rack `rackHeight` tall (RackLayout's height): the rack's top and bottom margin,
	// the board's padding, and `extraLine` a caption line under the rack (the chain is quiet and says why).
	inline float BoardStripHeight(float rackHeight, bool extraLine = false)
	{
		const float line = extraLine ? CaptionFont()->FontSize + ImGui::GetStyle().ItemSpacing.y + 4.0f : 0.0f;
		return std::floor(rackHeight + 24.0f + line + 2.0f * BOARD_PADDING);
	}

	// The framed panel behind a tab: the app background, the bar, the browser column, and the line between the board
	// strip and the Inspector.
	struct PanelFrame
	{
		ImVec2 at;
		float width = 0.0f;
		float barHeight = 0.0f;
		float mainHeight = 0.0f;     // browser column = board + Inspector
		float libraryWidth = 0.0f;   // the browser column; 0: hidden
		float bottom = 0.0f;         // the panel's bottom edge (screen y)
		float inspectorTop = 0.0f;   // screen y of the line over the Inspector; 0: no Inspector
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
			list->AddRectFilled(ImVec2(at.x, at.y + frame.barHeight + 1.0f), ImVec2(at.x + frame.libraryWidth, frame.bottom), Ink::Library,
				12.0f, ImDrawFlags_RoundCornersBottomLeft);
			list->AddLine(ImVec2(at.x + frame.libraryWidth, at.y + frame.barHeight), ImVec2(at.x + frame.libraryWidth, frame.bottom), Ink::Edge);
		}
		if (frame.inspectorTop > 0.0f)
			list->AddLine(ImVec2(at.x + frame.libraryWidth + 12.0f, frame.inspectorTop), ImVec2(end.x - 12.0f, frame.inspectorTop), Ink::Edge);
		list->AddRect(at, end, Ink::Edge, 12.0f);
	}

	// ---- Inspector parts -------------------------------------------------------------------------------

	// What a knob drag did this frame. A drag is one gesture: `began` on the click, `changed` while it moves, `ended`
	// on release. The host turns one gesture into one undo step (juce::UndoManager, coalesced by parameter).
	struct KnobResult
	{
		bool began = false;
		bool changed = false;
		bool ended = false;
		bool reset = false;   // double-click: back to the default
	};

	constexpr float KNOB_SIZE = 46.0f;
	constexpr float KNOB_CELL_WIDTH = 84.0f;

	// A knob at the cursor in a cell KNOB_CELL_WIDTH wide: the label over it, the value text under it. `position` is
	// 0..1 along the knob's travel. Drag up or down to turn it (Shift for fine steps); double-click resets.
	inline KnobResult Knob(const char* id, const char* label, const char* valueText, float* position, const char* hint = nullptr)
	{
		KnobResult result;
		ImFont* caption = CaptionFont();
		ImDrawList* list = ImGui::GetWindowDrawList();
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const float cellW = KNOB_CELL_WIDTH;
		const float labelH = caption->FontSize + 4.0f;
		const float cellH = labelH + KNOB_SIZE + 6.0f + caption->FontSize + 2.0f;
		ImGui::InvisibleButton(id, ImVec2(cellW, cellH));
		const bool hovered = ImGui::IsItemHovered();
		const bool active = ImGui::IsItemActive();
		if (ImGui::IsItemActivated()) result.began = true;
		if (active && ImGui::GetIO().MouseDelta.y != 0.0f)
		{
			const float speed = ImGui::GetIO().KeyShift ? 0.0015f : 0.006f;
			const float next = std::clamp(*position - ImGui::GetIO().MouseDelta.y * speed, 0.0f, 1.0f);
			if (next != *position) { *position = next; result.changed = true; }
		}
		if (ImGui::IsItemDeactivated()) result.ended = true;
		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) result.reset = true;
		if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
		if (hint && hovered && !active) Hint(hint);

		// Label, knob (a 270 degree arc: the travel dim, the value lit), pointer, value.
		const float labelW = TextWidth(caption, label);
		list->PushClipRect(at, ImVec2(at.x + cellW, at.y + cellH), true);
		Text(list, caption, ImVec2(at.x + std::max(0.0f, (cellW - labelW) * 0.5f), at.y), Ink::Soft, label);
		list->PopClipRect();
		const ImVec2 center(at.x + cellW * 0.5f, at.y + labelH + KNOB_SIZE * 0.5f);
		const float radius = KNOB_SIZE * 0.5f;
		constexpr float START = 2.356f, TRAVEL = 4.712f;   // 135 degrees, 270 degrees
		const float valueAngle = START + TRAVEL * std::clamp(*position, 0.0f, 1.0f);
		list->PathArcTo(center, radius - 2.0f, START, START + TRAVEL, 36);
		list->PathStroke(Ink::Edge, 0, 4.0f);
		list->PathArcTo(center, radius - 2.0f, START, valueAngle, 36);
		list->PathStroke(active ? Ink::AddText : Ink::Blue, 0, 4.0f);
		list->AddCircleFilled(center, radius - 8.0f, hovered || active ? Mix(Ink::Knob, IM_COL32_WHITE, 0.06f) : Ink::Knob, 32);
		list->AddCircle(center, radius - 8.0f, Ink::KnobEdge, 32, 1.5f);
		const ImVec2 tip(center.x + std::cos(valueAngle) * (radius - 12.0f), center.y + std::sin(valueAngle) * (radius - 12.0f));
		list->AddLine(ImVec2(center.x + std::cos(valueAngle) * 4.0f, center.y + std::sin(valueAngle) * 4.0f), tip, IM_COL32_WHITE, 2.0f);
		const float valueW = TextWidth(caption, valueText);
		list->PushClipRect(at, ImVec2(at.x + cellW, at.y + cellH), true);
		Text(list, caption, ImVec2(at.x + std::max(0.0f, (cellW - valueW) * 0.5f), center.y + radius + 4.0f), active ? IM_COL32_WHITE : Ink::Muted, valueText);
		list->PopClipRect();
		return result;
	}

	// The height of one row of knobs (Knob's cell).
	inline float KnobRowHeight() { return CaptionFont()->FontSize + 4.0f + KNOB_SIZE + 6.0f + CaptionFont()->FontSize + 2.0f; }

	// The Inspector's empty state, centred in the space left: what to do to see something here.
	inline void InspectorEmpty(const char* line, float width, float height)
	{
		ImFont* caption = CaptionFont();
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const float textW = std::min(TextWidth(caption, line), width - 20.0f);
		ImGui::SetCursorScreenPos(ImVec2(at.x + std::max(0.0f, (width - textW) * 0.5f), at.y + std::max(0.0f, height * 0.5f - caption->FontSize)));
		MutedCaption(line, textW + 1.0f);
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

	// A segmented toggle ("Tones | Plugins | Songs"): `count` labels, `selected` the one lit. `fillWidth` > 0 shares
	// that width out evenly (the browser column's tabs); otherwise each label is as wide as its text.
	// Returns the label clicked, or -1.
	inline int PaneToggle(const char* const* labels, int count, int selected, const char* const* hints = nullptr, float fillWidth = 0.0f)
	{
		ImFont* font = CaptionFont();
		ImDrawList* list = ImGui::GetWindowDrawList();
		const float height = ImGui::GetFrameHeight();
		const ImVec2 start = ImGui::GetCursorScreenPos();
		float x = start.x;
		int clicked = -1;
		for (int i = 0; i < count; ++i)
		{
			const float w = fillWidth > 0.0f ? std::floor(fillWidth / count) : TextWidth(font, labels[i]) + 22.0f;
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
			const float textX = fillWidth > 0.0f ? at.x + std::max(4.0f, (w - TextWidth(font, labels[i])) * 0.5f) : at.x + 11.0f;
			Text(list, font, ImVec2(textX, at.y + (height - font->FontSize) * 0.5f), on ? IM_COL32_WHITE : hovered ? Ink::Text : Ink::Muted, labels[i]);
			x += w;
		}
		list->AddRect(start, ImVec2(x, start.y + height), Ink::FieldEdge, 6.0f);
		ImGui::SetCursorScreenPos(start);
		ImGui::Dummy(ImVec2(x - start.x, height));
		return clicked;
	}
}
