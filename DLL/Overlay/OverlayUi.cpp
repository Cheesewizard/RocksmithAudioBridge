#include "../stdafx.h"
#include "OverlayUi.hpp"
#include "AudioBridgeLogo.h"
#include <filesystem>
#include <cmath>

namespace Overlay
{
	namespace
	{
		Fonts g_fonts;

		// Only the glyphs the overlay draws, so the icon font adds a few KB to the atlas instead of the whole
		// private-use block. Must outlive the atlas build (ImGui keeps the pointer).
		const ImWchar kIconRanges[] = {
			0xE713, 0xE713, 0xE720, 0xE720, 0xE72C, 0xE72C, 0xE7C8, 0xE7C8, 0xE7F5, 0xE7F5,
			0xE838, 0xE838, 0xE8BB, 0xE8BB, 0xE8D6, 0xE8D6, 0xE9E9, 0xE9E9, 0xEBE8, 0xEBE8, 0xEC4F, 0xEC4F, 0
		};

		// Card state. Cards split the window draw list into a background and a foreground channel so the card
		// rectangle can be drawn behind content whose height is only known once the content is laid out.
		constexpr float kCardPad = 16.0f;
		bool g_inCard = false;
		ImVec2 g_cardStart;
		float g_cardRight = 0.0f;

		std::string WindowsFont(const char* file)
		{
			char windows[MAX_PATH]{};
			GetWindowsDirectoryA(windows, MAX_PATH);
			const std::filesystem::path path = std::filesystem::path(windows) / "Fonts" / file;
			std::error_code ec;
			return std::filesystem::exists(path, ec) ? path.string() : std::string();
		}

		const char* LabelEnd(const char* label)
		{
			const char* hidden = strstr(label, "##");
			return hidden ? hidden : label + strlen(label);
		}

		// Custom-drawn colours ignore the style alpha, so BeginDisabled would not dim them without this.
		ImU32 Fade(ImU32 color)
		{
			const float alpha = ((color >> IM_COL32_A_SHIFT) & 0xFF) * ImGui::GetStyle().Alpha;
			return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
		}

		ImU32 WithAlpha(ImU32 color, float alpha)
		{
			return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f) << IM_COL32_A_SHIFT);
		}

		struct Utf8Glyph { char text[4]{}; };
		Utf8Glyph Encode(unsigned cp)
		{
			Utf8Glyph g;
			g.text[0] = static_cast<char>(0xE0 | (cp >> 12));
			g.text[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			g.text[2] = static_cast<char>(0x80 | (cp & 0x3F));
			return g;
		}

		// Label column shared by slider and segmented rows so their controls line up down a card.
		float LabelColumn(float width) { return std::clamp(width * 0.36f, 110.0f, 220.0f); }
	}

	void LoadFonts(ImGuiIO& io)
	{
		g_fonts.body = io.Fonts->AddFontFromMemoryCompressedTTF(RobotoFont_data, RobotoFont_size, 18.0f);
		g_fonts.caption = io.Fonts->AddFontFromMemoryCompressedTTF(RobotoFont_data, RobotoFont_size, 15.0f);

		// Headings use Segoe UI Semibold when Windows has it; Roboto otherwise. Nothing is shipped or bundled.
		const std::string semibold = WindowsFont("seguisb.ttf");
		g_fonts.title = semibold.empty()
			? io.Fonts->AddFontFromMemoryCompressedTTF(RobotoFont_data, RobotoFont_size, 26.0f)
			: io.Fonts->AddFontFromFileTTF(semibold.c_str(), 26.0f);
		g_fonts.big = semibold.empty()
			? io.Fonts->AddFontFromMemoryCompressedTTF(RobotoFont_data, RobotoFont_size, 40.0f)
			: io.Fonts->AddFontFromFileTTF(semibold.c_str(), 40.0f);

		// Windows 11 ships Segoe Fluent Icons, Windows 10 ships Segoe MDL2 Assets (same code points). With
		// neither, icons are simply left out and every control still has its text label.
		std::string icons = WindowsFont("SegoeIcons.ttf");
		if (icons.empty()) icons = WindowsFont("segmdl2.ttf");
		if (!icons.empty()) {
			ImFontConfig config;
			config.PixelSnapH = true;
			g_fonts.icons = io.Fonts->AddFontFromFileTTF(icons.c_str(), 18.0f, &config, kIconRanges);
		}
	}

	const Fonts& GetFonts() { return g_fonts; }

	namespace { IDirect3DTexture9* g_logo = nullptr; }

	// Uploads the embedded logo pixels directly (no D3DX, no file on disk). Managed pool, so D3D keeps a system
	// copy and restores it itself after a device reset.
	void CreateTextures(IDirect3DDevice9* device)
	{
		if (g_logo || !device) return;
		const int size = LogoData::Size;
		if (FAILED(device->CreateTexture(size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_logo, nullptr))) { g_logo = nullptr; return; }
		D3DLOCKED_RECT locked{};
		if (FAILED(g_logo->LockRect(0, &locked, nullptr, 0))) { g_logo->Release(); g_logo = nullptr; return; }
		for (int y = 0; y < size; ++y)
			memcpy(static_cast<BYTE*>(locked.pBits) + y * locked.Pitch, LogoData::Pixels + y * size, size * sizeof(uint32_t));
		g_logo->UnlockRect(0);
	}

	ImTextureID LogoTexture() { return g_logo; }

	void ApplyTheme()
	{
		ImGuiStyle& s = ImGui::GetStyle();
		s.WindowRounding = 6.0f; s.ChildRounding = 0.0f; s.FrameRounding = 4.0f; s.PopupRounding = 5.0f;
		s.GrabRounding = 4.0f; s.ScrollbarRounding = 4.0f; s.TabRounding = 4.0f;
		s.WindowBorderSize = 1.0f; s.ChildBorderSize = 0.0f; s.PopupBorderSize = 1.0f; s.FrameBorderSize = 0.0f;
		s.WindowPadding = ImVec2(0, 0); s.FramePadding = ImVec2(10, 6); s.ItemSpacing = ImVec2(10, 10);
		s.ItemInnerSpacing = ImVec2(8, 6); s.GrabMinSize = 14.0f; s.ScrollbarSize = 10.0f;
		s.DisabledAlpha = 0.4f;

		ImVec4* c = s.Colors;
		auto v = [](ImU32 col) { return ImGui::ColorConvertU32ToFloat4(col); };
		c[ImGuiCol_Text] = v(Color::Text);
		c[ImGuiCol_TextDisabled] = v(Color::Muted);
		c[ImGuiCol_WindowBg] = v(Color::Window);
		c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_PopupBg] = v(IM_COL32(18, 26, 44, 250));
		c[ImGuiCol_Border] = v(Color::CardEdge);
		c[ImGuiCol_FrameBg] = v(Color::Track);
		c[ImGuiCol_FrameBgHovered] = v(IM_COL32(44, 58, 88, 255));
		c[ImGuiCol_FrameBgActive] = v(IM_COL32(50, 66, 100, 255));
		c[ImGuiCol_TitleBg] = v(Color::Header);
		c[ImGuiCol_TitleBgActive] = v(Color::Header);
		c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ScrollbarGrab] = v(Color::Track);
		c[ImGuiCol_ScrollbarGrabHovered] = v(Color::Faint);
		c[ImGuiCol_ScrollbarGrabActive] = v(Color::Muted);
		c[ImGuiCol_CheckMark] = v(Color::AccentHi);
		c[ImGuiCol_SliderGrab] = v(Color::AccentHi);
		c[ImGuiCol_SliderGrabActive] = v(IM_COL32(147, 197, 253, 255));
		c[ImGuiCol_Button] = v(Color::Track);
		c[ImGuiCol_ButtonHovered] = v(IM_COL32(48, 64, 96, 255));
		c[ImGuiCol_ButtonActive] = v(Color::Accent);
		c[ImGuiCol_Header] = v(Color::Hover);
		c[ImGuiCol_HeaderHovered] = v(Color::Track);
		c[ImGuiCol_HeaderActive] = v(Color::Accent);
		c[ImGuiCol_Separator] = v(Color::CardEdge);
		c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ResizeGripHovered] = v(Color::AccentSoft);
		c[ImGuiCol_ResizeGripActive] = v(Color::Accent);
		c[ImGuiCol_PlotHistogram] = v(Color::Accent);
		c[ImGuiCol_TextSelectedBg] = v(Color::AccentSoft);
		c[ImGuiCol_NavHighlight] = v(Color::AccentHi);
		c[ImGuiCol_ModalWindowDimBg] = v(IM_COL32(0, 0, 0, 120));
	}

	ImU32 Mix(ImU32 a, ImU32 b, float t)
	{
		t = std::clamp(t, 0.0f, 1.0f);
		auto channel = [&](int shift) {
			const float ca = static_cast<float>((a >> shift) & 0xFF), cb = static_cast<float>((b >> shift) & 0xFF);
			return static_cast<ImU32>(ca + (cb - ca) * t + 0.5f) << shift;
		};
		return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) | channel(IM_COL32_A_SHIFT);
	}

	// Eases a per-widget value toward `target`, stored in the current window's ImGui state so nothing leaks
	// between frames or windows. Frame-rate independent via DeltaTime.
	float Animate(const char* key, float target, float speed)
	{
		ImGuiStorage* storage = ImGui::GetStateStorage();
		float* value = storage->GetFloatRef(ImGui::GetID(key), target);
		*value += (target - *value) * std::min(1.0f, ImGui::GetIO().DeltaTime * speed);
		return *value;
	}

	void DrawIcon(ImDrawList* list, ImVec2 center, unsigned glyph, ImU32 color)
	{
		if (!g_fonts.icons) return;
		const Utf8Glyph g = Encode(glyph);
		const float size = g_fonts.icons->FontSize;
		const ImVec2 extent = g_fonts.icons->CalcTextSizeA(size, FLT_MAX, 0.0f, g.text);
		list->AddText(g_fonts.icons, size, ImVec2(std::floor(center.x - extent.x * 0.5f), std::floor(center.y - extent.y * 0.5f)), Fade(color), g.text);
	}

	// Delayed tooltip: ImGui 1.85 has no hover delay, so track how long the
	// same item (identified by its top-left corner) has been hovered and only show after half a second.
	void Hint(const char* text)
	{
		if (!text || !text[0] || !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
		static ImVec2 hoveredItem(-1.0f, -1.0f);
		static double hoverStart = 0.0;
		static int lastFrame = -2;
		const ImVec2 itemMin = ImGui::GetItemRectMin();
		const int frame = ImGui::GetFrameCount();
		const double t = ImGui::GetTime();
		if (itemMin.x != hoveredItem.x || itemMin.y != hoveredItem.y || frame - lastFrame > 1) { hoveredItem = itemMin; hoverStart = t; }
		lastFrame = frame;
		if (t - hoverStart < 0.5) return;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
		ImGui::BeginTooltip();
		ImGui::PushFont(g_fonts.caption);
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::PopFont();
		ImGui::EndTooltip();
		ImGui::PopStyleVar();
	}

	void Caption(const char* text)
	{
		ImGui::PushFont(g_fonts.caption);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Muted));
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}

	void SectionLabel(const char* text)
	{
		ImGui::PushFont(g_fonts.caption);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Faint));
		std::string upper(text);
		for (char& ch : upper) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
		ImGui::TextUnformatted(upper.c_str());
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}

	void PageHeader(const char* title, const char* subtitle)
	{
		ImGui::PushFont(g_fonts.title);
		ImGui::TextUnformatted(title);
		ImGui::PopFont();
		if (subtitle && subtitle[0]) Caption(subtitle);
		ImGui::Dummy(ImVec2(0, 4));
	}

	void BeginCard(const char* title)
	{
		IM_ASSERT(!g_inCard && "Overlay cards cannot nest");
		ImDrawList* list = ImGui::GetWindowDrawList();
		list->ChannelsSplit(2);
		list->ChannelsSetCurrent(1);
		g_cardStart = ImGui::GetCursorScreenPos();
		g_cardRight = g_cardStart.x + ImGui::GetContentRegionAvail().x;
		g_inCard = true;
		ImGui::SetCursorScreenPos(ImVec2(g_cardStart.x + kCardPad, g_cardStart.y + kCardPad));
		ImGui::BeginGroup();
		if (title) SectionLabel(title);
	}

	void EndCard()
	{
		ImGui::EndGroup();
		const float bottom = ImGui::GetItemRectMax().y + kCardPad;
		ImDrawList* list = ImGui::GetWindowDrawList();
		list->ChannelsSetCurrent(0);
		list->AddRectFilled(g_cardStart, ImVec2(g_cardRight, bottom), Color::Card, 6.0f);
		list->AddRect(g_cardStart, ImVec2(g_cardRight, bottom), Color::CardEdge, 6.0f);
		list->ChannelsMerge();
		g_inCard = false;
		ImGui::SetCursorScreenPos(ImVec2(g_cardStart.x, bottom));
		ImGui::Dummy(ImVec2(0, 4));
	}

	float RowWidth()
	{
		if (g_inCard) return std::max(40.0f, g_cardRight - kCardPad - ImGui::GetCursorScreenPos().x);
		return ImGui::GetContentRegionAvail().x;
	}

	bool ToggleRow(const char* label, const char* caption, bool* value, const char* hint)
	{
		ImGui::PushID(label);
		const float width = RowWidth();
		const ImVec2 start = ImGui::GetCursorScreenPos();
		const float switchW = 42.0f, switchH = 22.0f;

		// Text column: label, then an optional muted caption that wraps before the switch.
		ImGui::BeginGroup();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - switchW - 16.0f);
		ImGui::TextUnformatted(label, LabelEnd(label));
		if (caption && caption[0]) {
			ImGui::PushFont(g_fonts.caption);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Muted));
			ImGui::TextUnformatted(caption);
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		const float rowH = std::max(ImGui::GetItemRectSize().y, switchH);

		// The whole row is the hit target, so clicking the label flips the switch too.
		ImGui::SetCursorScreenPos(start);
		const bool clicked = ImGui::InvisibleButton("##row", ImVec2(width, rowH));
		const bool hovered = ImGui::IsItemHovered();
		if (clicked) *value = !*value;
		Hint(hint);

		const float t = Animate("knob", *value ? 1.0f : 0.0f);
		ImDrawList* list = ImGui::GetWindowDrawList();
		const ImVec2 track(start.x + width - switchW, start.y + (rowH - switchH) * 0.5f);
		ImU32 trackColor = Mix(Color::Track, Color::Accent, t);
		if (hovered) trackColor = Mix(trackColor, IM_COL32(255, 255, 255, 255), 0.08f);
		list->AddRectFilled(track, ImVec2(track.x + switchW, track.y + switchH), Fade(trackColor), switchH * 0.5f);
		const float radius = switchH * 0.5f - 3.0f;
		const float knobX = track.x + switchH * 0.5f + t * (switchW - switchH);
		list->AddCircleFilled(ImVec2(knobX, track.y + switchH * 0.5f), radius, Fade(IM_COL32(245, 248, 252, 255)), 20);

		ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + rowH));
		ImGui::Dummy(ImVec2(0, 0));
		ImGui::PopID();
		return clicked;
	}

	// Starts a labelled row: draws the label in the left column and returns the screen position where the
	// control column begins. Positions by screen coordinates because SameLine(offset) adds the group indent
	// a second time inside a card and pushed controls past the card edge.
	static ImVec2 BeginLabelledRow(const char* label, float width, float height)
	{
		const ImVec2 start = ImGui::GetCursorScreenPos();
		const ImVec2 text = ImGui::CalcTextSize(label, LabelEnd(label));
		ImGui::GetWindowDrawList()->AddText(ImVec2(start.x, start.y + (height - text.y) * 0.5f), Fade(Color::Text), label, LabelEnd(label));
		return ImVec2(start.x + LabelColumn(width), start.y);
	}

	static void EndLabelledRow(ImVec2 start, float height)
	{
		ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height));
		ImGui::Dummy(ImVec2(0, 0));
	}

	// Slim slider: a thin track, an accent fill up to the value, a round thumb, and the value printed in its own
	// column on the right so the thumb never covers it. Click or drag anywhere on the track.
	bool SliderRow(const char* label, float* value, float min, float max, const char* format, bool* committed, const char* hint)
	{
		ImGui::PushID(label);
		const float width = RowWidth();
		const float height = ImGui::GetFrameHeight();
		const ImVec2 start = ImGui::GetCursorScreenPos();
		const ImVec2 control = BeginLabelledRow(label, width, height);
		const float valueW = 86.0f;
		const float trackW = std::max(40.0f, start.x + width - control.x - valueW);

		ImGui::SetCursorScreenPos(control);
		ImGui::InvisibleButton("##slider", ImVec2(trackW, height));
		const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
		if (committed) *committed = ImGui::IsItemDeactivated();
		Hint(hint);

		const float thumb = 8.0f;
		const float x0 = control.x + thumb, x1 = control.x + trackW - thumb;
		bool changed = false;
		if (active && max > min) {
			const float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
			const float next = min + t * (max - min);
			if (next != *value) { *value = next; changed = true; }
		}

		const float fraction = max > min ? std::clamp((*value - min) / (max - min), 0.0f, 1.0f) : 0.0f;
		const float cy = control.y + height * 0.5f;
		const float cx = x0 + fraction * (x1 - x0);
		ImDrawList* list = ImGui::GetWindowDrawList();
		list->AddRectFilled(ImVec2(x0, cy - 2.5f), ImVec2(x1, cy + 2.5f), Fade(Color::Track), 2.5f);
		list->AddRectFilled(ImVec2(x0, cy - 2.5f), ImVec2(cx, cy + 2.5f), Fade(active || hovered ? Color::AccentHi : Color::Accent), 2.5f);
		if (active || hovered) list->AddCircleFilled(ImVec2(cx, cy), thumb + 4.0f, Fade(Color::AccentSoft), 24);
		list->AddCircleFilled(ImVec2(cx, cy), thumb, Fade(IM_COL32(240, 244, 250, 255)), 24);

		char text[48]; sprintf_s(text, format, *value);
		const ImVec2 size = ImGui::CalcTextSize(text);
		list->AddText(ImVec2(start.x + width - size.x, control.y + (height - size.y) * 0.5f), Fade(active ? Color::AccentHi : Color::Text), text);

		EndLabelledRow(start, height);
		ImGui::PopID();
		return changed;
	}

	bool SegmentedRow(const char* label, int* index, const char* const* options, int count, const char* hint)
	{
		ImGui::PushID(label);
		const float width = RowWidth();
		const float height = ImGui::GetFrameHeight();
		const ImVec2 rowStart = ImGui::GetCursorScreenPos();
		const ImVec2 start = BeginLabelledRow(label, width, height);
		const float segment = (rowStart.x + width - start.x) / static_cast<float>(count);
		ImDrawList* list = ImGui::GetWindowDrawList();
		list->AddRectFilled(start, ImVec2(start.x + segment * count, start.y + height), Fade(Color::Track), 5.0f);

		// Highlight slides between segments.
		const float slide = Animate("slide", static_cast<float>(*index), 18.0f);
		const ImVec2 hiMin(start.x + slide * segment + 2.0f, start.y + 2.0f);
		list->AddRectFilled(hiMin, ImVec2(hiMin.x + segment - 4.0f, start.y + height - 2.0f), Fade(Color::Accent), 4.0f);

		bool changed = false;
		for (int i = 0; i < count; ++i) {
			ImGui::PushID(i);
			ImGui::SetCursorScreenPos(ImVec2(start.x + segment * i, start.y));
			if (ImGui::InvisibleButton("##seg", ImVec2(segment, height)) && *index != i) { *index = i; changed = true; }
			const bool hovered = ImGui::IsItemHovered();
			Hint(hint);
			const ImVec2 text = ImGui::CalcTextSize(options[i]);
			const ImU32 color = i == *index || hovered ? Color::Text : Color::Muted;
			list->AddText(ImVec2(start.x + segment * i + (segment - text.x) * 0.5f, start.y + (height - text.y) * 0.5f), Fade(color), options[i]);
			ImGui::PopID();
		}
		EndLabelledRow(rowStart, height);
		ImGui::PopID();
		return changed;
	}

	bool ColorSwatch(const char* label, float rgb[3], bool* committed, const char* hint)
	{
		ImGui::PushID(label);
		*committed = false;
		ImGuiStorage* storage = ImGui::GetStateStorage();
		const ImGuiID dirty = ImGui::GetID("dirty");

		ImGui::BeginGroup();
		const ImVec4 color(rgb[0], rgb[1], rgb[2], 1.0f);
		if (ImGui::ColorButton("##swatch", color, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(64, 32)))
			ImGui::OpenPopup("picker");
		Hint(hint);
		ImGui::PushFont(g_fonts.caption);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Muted));
		ImGui::TextUnformatted(label, LabelEnd(label));
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::EndGroup();

		// Edits apply live while the picker is open; the save happens once when it closes.
		bool changed = false;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
		if (ImGui::BeginPopup("picker")) {
			ImGui::SetNextItemWidth(220.0f);
			changed = ImGui::ColorPicker3("##picker", rgb, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview
				| ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_DisplayHex);
			if (changed) storage->SetBool(dirty, true);
			ImGui::EndPopup();
		}
		else if (storage->GetBool(dirty)) {
			storage->SetBool(dirty, false);
			*committed = true;
		}
		ImGui::PopStyleVar();
		ImGui::PopID();
		return changed;
	}

	bool Button(const char* label, bool primary, const char* hint)
	{
		if (primary) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(Color::Accent));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(Color::AccentHi));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(IM_COL32(37, 99, 235, 255)));
		}
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 7));
		const bool pressed = ImGui::Button(label);
		ImGui::PopStyleVar();
		if (primary) ImGui::PopStyleColor(3);
		Hint(hint);
		return pressed;
	}

	bool ChoiceRow(const char* id, const char* label, const char* caption, bool selected)
	{
		ImGui::PushID(id);
		const float width = RowWidth();
		const float height = ImGui::GetFrameHeight() + 6.0f;
		const ImVec2 start = ImGui::GetCursorScreenPos();
		const bool clicked = ImGui::InvisibleButton("##choice", ImVec2(width, height));
		const float hover = Animate("hover", ImGui::IsItemHovered() || selected ? 1.0f : 0.0f, 16.0f);
		ImDrawList* list = ImGui::GetWindowDrawList();
		const ImVec2 end(start.x + width, start.y + height);
		if (selected) list->AddRectFilled(start, end, Fade(WithAlpha(Color::Accent, 0.16f)), 5.0f);
		else if (hover > 0.01f) list->AddRectFilled(start, end, Fade(WithAlpha(Color::Hover, hover)), 5.0f);

		// Radio mark
		const ImVec2 mark(start.x + 16.0f, start.y + height * 0.5f);
		list->AddCircle(mark, 7.0f, Fade(selected ? Color::AccentHi : Color::Faint), 20, 1.6f);
		if (selected) list->AddCircleFilled(mark, 3.8f, Fade(Color::AccentHi), 16);

		const float textY = start.y + (height - ImGui::GetFontSize()) * 0.5f;
		float captionW = 0.0f;
		if (caption && caption[0]) {
			ImGui::PushFont(g_fonts.caption);
			const ImVec2 size = ImGui::CalcTextSize(caption);
			captionW = size.x + 12.0f;
			list->AddText(ImVec2(end.x - size.x - 12.0f, start.y + (height - size.y) * 0.5f), Fade(Color::Muted), caption);
			ImGui::PopFont();
		}
		list->PushClipRect(ImVec2(start.x + 32.0f, start.y), ImVec2(end.x - captionW - 8.0f, end.y), true);
		list->AddText(ImVec2(start.x + 32.0f, textY), Fade(selected ? Color::Text : Mix(Color::Muted, Color::Text, hover)), label, LabelEnd(label));
		list->PopClipRect();
		ImGui::SetCursorScreenPos(ImVec2(start.x, end.y + 2.0f));
		ImGui::Dummy(ImVec2(0, 0));
		ImGui::PopID();
		return clicked;
	}

	// LED-style level meter on a dB-shaped fraction (0..1). Green to amber to red with a short peak hold.
	void Meter(const char* label, float fraction, const char* hint)
	{
		ImGui::PushID(label);
		fraction = std::clamp(fraction, 0.0f, 1.0f);
		const float width = RowWidth();
		const float labelW = 72.0f;
		const ImVec2 start = ImGui::GetCursorScreenPos();
		const float height = 12.0f;
		const float rowH = ImGui::GetFontSize();

		ImGui::InvisibleButton("##meter", ImVec2(width, rowH));
		Hint(hint);

		ImDrawList* list = ImGui::GetWindowDrawList();
		ImGui::PushFont(g_fonts.caption);
		list->AddText(ImVec2(start.x, start.y + (rowH - ImGui::GetFontSize()) * 0.5f), Fade(Color::Muted), label, LabelEnd(label));
		ImGui::PopFont();

		ImGuiStorage* storage = ImGui::GetStateStorage();
		float* peak = storage->GetFloatRef(ImGui::GetID("peak"), 0.0f);
		*peak = std::max(fraction, *peak - ImGui::GetIO().DeltaTime * 0.35f);

		const float barX = start.x + labelW, barW = width - labelW;
		const float y = start.y + (rowH - height) * 0.5f;
		const int segments = std::max(8, static_cast<int>(barW / 7.0f));
		const float step = barW / segments;
		const int peakSegment = static_cast<int>(*peak * segments + 0.5f) - 1;
		for (int i = 0; i < segments; ++i) {
			const float position = (i + 0.5f) / segments;
			const ImU32 hue = position < 0.72f ? Color::Good : (position < 0.9f ? Color::Warn : Color::Bad);
			const bool lit = position <= fraction || i == peakSegment;
			const ImU32 color = lit ? hue : WithAlpha(hue, 0.12f);
			const float x0 = barX + i * step;
			list->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + step - 2.0f, y + height), Fade(color), 1.5f);
		}
		ImGui::PopID();
	}

	void StatusPill(const char* text, ImU32 color, bool pulse)
	{
		ImGui::PushFont(g_fonts.caption);
		const ImVec2 size = ImGui::CalcTextSize(text);
		const ImVec2 start = ImGui::GetCursorScreenPos();
		const float height = size.y + 8.0f, dot = 4.0f;
		const float width = size.x + 30.0f;
		ImDrawList* list = ImGui::GetWindowDrawList();
		list->AddRectFilled(start, ImVec2(start.x + width, start.y + height), Fade(WithAlpha(color, 0.16f)), height * 0.5f);
		float alpha = 1.0f;
		if (pulse) alpha = 0.45f + 0.55f * (0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f));
		list->AddCircleFilled(ImVec2(start.x + 12.0f, start.y + height * 0.5f), dot, Fade(WithAlpha(color, alpha)), 12);
		list->AddText(ImVec2(start.x + 22.0f, start.y + 4.0f), Fade(color), text);
		ImGui::Dummy(ImVec2(width, height));
		ImGui::PopFont();
	}

	bool NavItem(unsigned glyph, const char* label, bool selected)
	{
		ImGui::PushID(label);
		const ImVec2 start = ImGui::GetCursorScreenPos();
		const float width = ImGui::GetContentRegionAvail().x, height = 40.0f;
		const bool clicked = ImGui::InvisibleButton("##nav", ImVec2(width, height));
		const float hover = Animate("hover", ImGui::IsItemHovered() || selected ? 1.0f : 0.0f, 16.0f);

		ImDrawList* list = ImGui::GetWindowDrawList();
		const ImVec2 end(start.x + width, start.y + height);
		if (hover > 0.01f)
			list->AddRectFilled(start, end, WithAlpha(Color::Hover, selected ? 1.0f : hover * 0.7f), 5.0f);
		if (selected)
			list->AddRectFilled(ImVec2(start.x, start.y + 9.0f), ImVec2(start.x + 3.0f, end.y - 9.0f), Color::Accent, 2.0f);
		const ImU32 ink = selected ? Color::Text : Mix(Color::Muted, Color::Text, hover);
		DrawIcon(list, ImVec2(start.x + 24.0f, start.y + height * 0.5f), glyph, selected ? Color::AccentHi : ink);
		const float textX = start.x + (g_fonts.icons ? 46.0f : 16.0f);
		list->AddText(ImVec2(textX, start.y + (height - ImGui::GetFontSize()) * 0.5f), ink, label, LabelEnd(label));
		ImGui::PopID();
		return clicked;
	}

	bool IconButton(const char* id, unsigned glyph, float size)
	{
		const ImVec2 start = ImGui::GetCursorScreenPos();
		const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
		const bool hovered = ImGui::IsItemHovered();
		ImDrawList* list = ImGui::GetWindowDrawList();
		const ImVec2 center(start.x + size * 0.5f, start.y + size * 0.5f);
		if (hovered) list->AddRectFilled(start, ImVec2(start.x + size, start.y + size), Color::Hover, 5.0f);
		if (g_fonts.icons) DrawIcon(list, center, glyph, hovered ? Color::Text : Color::Muted);
		else {
			// No icon font: a drawn cross is the only icon button the overlay uses without text.
			const float r = size * 0.18f;
			list->AddLine(ImVec2(center.x - r, center.y - r), ImVec2(center.x + r, center.y + r), hovered ? Color::Text : Color::Muted, 1.6f);
			list->AddLine(ImVec2(center.x - r, center.y + r), ImVec2(center.x + r, center.y - r), hovered ? Color::Text : Color::Muted, 1.6f);
		}
		return clicked;
	}

	// Vertical mixer fader: value 0..100 on top, a thumb on a filled track, channel name underneath.
	// Drag anywhere in the strip, or use the mouse wheel for 1% steps.
	bool FaderStrip(const char* id, const char* label, float* value, ImVec2 size)
	{
		ImGui::PushID(id);
		const ImVec2 start = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##fader", size);
		const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();

		ImDrawList* list = ImGui::GetWindowDrawList();
		ImGui::PushFont(g_fonts.caption);
		const float lineH = ImGui::GetFontSize();
		const float trackTop = start.y + lineH + 10.0f, trackBottom = start.y + size.y - lineH - 10.0f;
		const float centerX = start.x + size.x * 0.5f;

		bool changed = false;
		if (active && trackBottom > trackTop) {
			const float mouseY = ImGui::GetIO().MousePos.y;
			const float next = std::clamp((trackBottom - mouseY) / (trackBottom - trackTop), 0.0f, 1.0f) * 100.0f;
			if (std::fabs(next - *value) >= 0.5f) { *value = std::round(next); changed = true; }
		}
		else if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
			*value = std::clamp(std::round(*value + ImGui::GetIO().MouseWheel), 0.0f, 100.0f);
			changed = true;
		}

		const float fraction = std::clamp(*value / 100.0f, 0.0f, 1.0f);
		const float thumbY = trackBottom - fraction * (trackBottom - trackTop);
		list->AddRectFilled(ImVec2(centerX - 3.0f, trackTop), ImVec2(centerX + 3.0f, trackBottom), Color::Track, 3.0f);
		list->AddRectFilled(ImVec2(centerX - 3.0f, thumbY), ImVec2(centerX + 3.0f, trackBottom), active || hovered ? Color::AccentHi : Color::Accent, 3.0f);
		const float thumbW = std::min(size.x - 12.0f, 34.0f);
		list->AddRectFilled(ImVec2(centerX - thumbW * 0.5f, thumbY - 5.0f), ImVec2(centerX + thumbW * 0.5f, thumbY + 5.0f), IM_COL32(236, 241, 248, 255), 3.0f);

		char text[16]; sprintf_s(text, "%d", static_cast<int>(*value + 0.5f));
		ImVec2 extent = ImGui::CalcTextSize(text);
		list->AddText(ImVec2(centerX - extent.x * 0.5f, start.y), active ? Color::AccentHi : Color::Text, text);
		extent = ImGui::CalcTextSize(label);
		const float labelX = std::max(start.x, centerX - extent.x * 0.5f);
		list->PushClipRect(start, ImVec2(start.x + size.x, start.y + size.y), true);
		list->AddText(ImVec2(labelX, start.y + size.y - lineH), Color::Muted, label);
		list->PopClipRect();
		ImGui::PopFont();
		ImGui::PopID();
		return changed;
	}
}
