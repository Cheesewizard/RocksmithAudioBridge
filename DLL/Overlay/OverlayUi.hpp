#pragma once
// In-game overlay look and widgets. ImGui 1.85 draws everything as flat rectangles, so the overlay builds its
// own vocabulary on top of the draw list: a navy palette matching the RSModsPlus desktop tab, a sidebar, cards,
// animated toggle switches, segmented pickers, filled sliders, LED meters and colour swatches.
//
// Rules for callers:
//  - Wrap page content in BeginCard/EndCard. Cards cannot nest (they split the window draw list into channels).
//  - Row widgets return true on the frame the value changes. Where a value is saved to disk, the row also reports
//    `committed` (mouse released / picker closed) so a drag writes RSMods.ini once, not every frame.

namespace Overlay
{
	// Palette (IM_COL32 is ABGR-packed; these are the only colours the overlay uses).
	namespace Color
	{
		constexpr ImU32 Window = IM_COL32(11, 17, 30, 246);
		constexpr ImU32 Header = IM_COL32(14, 21, 38, 255);
		constexpr ImU32 Sidebar = IM_COL32(14, 21, 38, 255);
		constexpr ImU32 Card = IM_COL32(21, 30, 50, 255);
		constexpr ImU32 CardEdge = IM_COL32(34, 46, 72, 255);
		constexpr ImU32 Hover = IM_COL32(30, 42, 66, 255);
		constexpr ImU32 Track = IM_COL32(36, 48, 74, 255);
		constexpr ImU32 Text = IM_COL32(230, 234, 242, 255);
		constexpr ImU32 Muted = IM_COL32(138, 151, 173, 255);
		constexpr ImU32 Faint = IM_COL32(91, 104, 128, 255);
		constexpr ImU32 Accent = IM_COL32(59, 139, 235, 255);
		constexpr ImU32 AccentHi = IM_COL32(96, 165, 250, 255);
		constexpr ImU32 AccentSoft = IM_COL32(59, 139, 235, 60);
		constexpr ImU32 Good = IM_COL32(52, 211, 153, 255);
		constexpr ImU32 Warn = IM_COL32(251, 191, 36, 255);
		constexpr ImU32 Bad = IM_COL32(248, 113, 113, 255);
		constexpr ImU32 Record = IM_COL32(239, 68, 68, 255);
	}

	// Segoe Fluent Icons / Segoe MDL2 Assets code points (same values in both fonts).
	namespace Glyph
	{
		constexpr unsigned MusicNote = 0xEC4F;
		constexpr unsigned Mixer = 0xE9E9;
		constexpr unsigned Input = 0xE720;
		constexpr unsigned Speaker = 0xE7F5;
		constexpr unsigned Record = 0xE7C8;
		constexpr unsigned Settings = 0xE713;
		constexpr unsigned Bug = 0xEBE8;
		constexpr unsigned Close = 0xE8BB;
		constexpr unsigned Folder = 0xE838;
		constexpr unsigned Refresh = 0xE72C;
		constexpr unsigned Pedal = 0xE8D6;      // "Audio" waveform glyph
		constexpr unsigned Link = 0xE71B;       // chain link (External amp)
	}

	struct Fonts
	{
		ImFont* body = nullptr;    // Roboto, controls and text
		ImFont* caption = nullptr; // Roboto, captions and section labels
		ImFont* title = nullptr;   // Segoe UI Semibold (Roboto fallback), page titles
		ImFont* big = nullptr;     // Segoe UI Semibold (Roboto fallback), record timer
		ImFont* icons = nullptr;   // Windows icon font; null when neither icon font is installed
	};

	// Adds the overlay fonts to the atlas. Call once, after CreateContext and before the first NewFrame.
	void LoadFonts(ImGuiIO& io);
	const Fonts& GetFonts();
	void ApplyTheme();

	// The Rocksmith Audio Bridge logo as a managed-pool texture (survives device resets). Call once with the
	// game device; LogoTexture() is null until then or if creation failed, and the header falls back to a tile.
	void CreateTextures(IDirect3DDevice9* device);
	ImTextureID LogoTexture();

	// Small helpers
	ImU32 Mix(ImU32 a, ImU32 b, float t);
	float Animate(const char* key, float target, float speed = 14.0f);
	void DrawIcon(ImDrawList* list, ImVec2 center, unsigned glyph, ImU32 color);
	void Hint(const char* text);          // delayed tooltip for the item just submitted
	void Caption(const char* text);       // wrapped muted text
	void SectionLabel(const char* text);  // small uppercase label

	// Layout
	void PageHeader(const char* title, const char* subtitle);
	void BeginCard(const char* title = nullptr);
	void EndCard();
	float RowWidth();                     // usable width inside the current card

	// Rows
	bool ToggleRow(const char* label, const char* caption, bool* value, const char* hint = nullptr);
	bool SliderRow(const char* label, float* value, float min, float max, const char* format,
		bool* committed = nullptr, const char* hint = nullptr);
	bool SegmentedRow(const char* label, int* index, const char* const* options, int count, const char* hint = nullptr);
	bool ColorSwatch(const char* label, float rgb[3], bool* committed, const char* hint = nullptr);
	bool Button(const char* label, bool primary = false, const char* hint = nullptr);
	// One selectable line in a list (output devices): radio mark, label, optional caption on the right.
	// Disabled rows (BeginDisabled) show dimmed and never return true.
	bool ChoiceRow(const char* id, const char* label, const char* caption, bool selected);
	void Meter(const char* label, float fraction, const char* hint = nullptr);
	void StatusPill(const char* text, ImU32 color, bool pulse = false);

	// Chrome
	bool NavItem(unsigned glyph, const char* label, bool selected);
	bool IconButton(const char* id, unsigned glyph, float size);
	bool FaderStrip(const char* id, const char* label, float* value, ImVec2 size);
}
