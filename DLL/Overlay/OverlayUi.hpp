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
		// Muted identity tints (role badges). Red, amber and green stay reserved for status.
		constexpr ImU32 TintBlue = IM_COL32(104, 144, 222, 255);
		constexpr ImU32 TintCyan = IM_COL32(84, 172, 196, 255);
		constexpr ImU32 TintViolet = IM_COL32(150, 128, 220, 255);
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
		constexpr unsigned Library = 0xE8F1;    // Tone designer
		constexpr unsigned Star = 0xE734;
		constexpr unsigned StarFilled = 0xE735;
		constexpr unsigned Add = 0xE710;
		constexpr unsigned ChevronLeft = 0xE76B;
		constexpr unsigned ChevronRight = 0xE76C;
		constexpr unsigned Power = 0xE7E8;
		constexpr unsigned NewWindow = 0xE8A7;   // the amp window
		// Every glyph here must also be in kIconRanges (OverlayUi.cpp), or it draws as a blank.
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
	// SegmentedRow's control without the label column, `width` wide at the cursor. `enabled` (optional, `count`
	// entries) greys out options that cannot be picked right now; `hint` shows over any segment.
	bool Segmented(const char* id, int* index, const char* const* options, int count, float width,
		const bool* enabled = nullptr, const char* hint = nullptr);
	// One fixed-width control: a minus cell, `text` (plus an optional `badge` pill in `badgeColor`) centred in the
	// middle, a plus cell. The width never follows the text, so the cells never move. Frame height. Returns -1 or
	// +1 on the frame a cell is clicked, else 0. `dim` mutes the text (the value is not in effect).
	int JoinedStepper(const char* id, float width, const char* text, const char* badge, ImU32 badgeColor,
		bool canDown, bool canUp, bool dim, const char* downHint = nullptr, const char* upHint = nullptr);
	// Button of a set width and frame height, so it lines up with the dropdowns and segmented controls beside it.
	bool WideButton(const char* label, float width, bool primary = false, const char* hint = nullptr);
	// Labelled drop-down list; `index` -1 shows an empty box. Returns true on the frame a different item is picked.
	bool DropdownRow(const char* label, int* index, const std::vector<std::string>& items, const char* hint = nullptr);
	bool ColorSwatch(const char* label, float rgb[3], bool* committed, const char* hint = nullptr);
	bool Button(const char* label, bool primary = false, const char* hint = nullptr);
	// One selectable line in a list (output devices): radio mark, label, optional caption on the right.
	// Disabled rows (BeginDisabled) show dimmed and never return true.
	bool ChoiceRow(const char* id, const char* label, const char* caption, bool selected);
	void Meter(const char* label, float fraction, const char* hint = nullptr);
	void StatusPill(const char* text, ImU32 color, bool pulse = false);
	// Labelled row whose control column holds a status pill, lined up with DropdownRow/SliderRow controls.
	void PillRow(const char* label, const char* text, ImU32 color, const char* hint = nullptr);

	// Segmented tab strip for picking one role (Player 1 | Player 2 | Mic) whose rows show underneath. Each tab:
	// a small rounded monogram badge in a muted `tint`, a drawn guitar or the microphone glyph, the label, and a
	// tiny status dot (Good/Warn/Bad colour) only when `hasStatus`, so a problem on another tab still shows. The
	// selected tab is lighter with a soft blue underline. Returns true on the frame `selected` changes; wrap the
	// selected role's rows in BeginTabBody/EndTabBody, which frame them under the strip (no nesting).
	enum class RoleIcon { None, Guitar, Mic };
	struct RoleTab
	{
		const char* label = "";
		const char* badge = nullptr;     // "P1", "P2", "MIC"
		ImU32 tint = 0;
		RoleIcon icon = RoleIcon::None;
		bool hasStatus = false;
		ImU32 status = 0;
		const char* hint = nullptr;
	};
	bool RoleTabs(const char* id, const RoleTab* tabs, int count, int* selected);
	void BeginTabBody();
	void EndTabBody();
	// A darker inset strip with a coloured edge and an icon in its gutter, for rows that stand apart from the
	// ones around them. `contentHeight` is the height of the rows inside (one row: ImGui::GetFrameHeight()).
	void BeginInset(float contentHeight, unsigned glyph, ImU32 accent);
	void EndInset();

	// Chrome
	bool NavItem(unsigned glyph, const char* label, bool selected);
	bool IconButton(const char* id, unsigned glyph, float size);
	bool FaderStrip(const char* id, const char* label, float* value, ImVec2 size);
}
