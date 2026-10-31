#pragma once

#include "Framework/Framework.hpp"

#include "Detection/DetectionFeedback.hpp"

#include <cstdint>

// Note by Note's settings: the keys NoteByNoteMod declares (RSMods.ini [Note by Note], 4.x's names, so existing
// settings and the desktop GUI carry over), and a snapshot of their values any thread can read. 4.x kept these in its
// Settings.cpp atomics; the engine itself reads none of them, they are the detection readout's and the target's
// (Ui/NoteByNoteHud), the look-ahead tab's (Ui/TablaturePreviewWindow) and the overlay page's (Ui/NoteByNotePage).
// Every value is 4.x's default until the first Load.
//
// The overlay page and the on-screen layout editor change them the way 4.x's Settings::SetNoteByNote* did: the
// snapshot changes at once (a slider drag shows live) and, when `persist` is set, the value is saved through upstream's
// setting edits (RSMods.ini, or the RSMods GUI while it owns the file), which brings it back through Load.
namespace NoteByNote::ModSettings
{
	namespace Keys
	{
		inline constexpr char DETECTION_OVERLAY[] = "AudioBridgeNoteByNoteDetectionOverlay";
		inline constexpr char TARGET_POSITION[] = "AudioBridgeNoteByNoteTargetPosition";
		inline constexpr char TARGET_STYLE[] = "AudioBridgeNoteByNoteTargetStyle";
		inline constexpr char UI_SIZE[] = "AudioBridgeNoteByNoteUiSize";
		inline constexpr char TARGET_SIZE[] = "AudioBridgeNoteByNoteTargetSize";
		inline constexpr char LINE_SPACING[] = "AudioBridgeNoteByNoteLineSpacing";
		inline constexpr char READOUT_PLACEMENT[] = "AudioBridgeNoteByNoteReadoutPlacement";
		inline constexpr char TARGET_PLACEMENT[] = "AudioBridgeNoteByNoteTargetPlacement";
		inline constexpr char CUSTOM_COLOURS[] = "AudioBridgeNoteByNoteCustomColours";
		inline constexpr char NEUTRAL_COLOR[] = "AudioBridgeNoteByNoteNeutralColor";
		inline constexpr char CONFIRMED_COLOR[] = "AudioBridgeNoteByNoteConfirmedColor";
		inline constexpr char PARTIAL_COLOR[] = "AudioBridgeNoteByNotePartialColor";
		inline constexpr char REJECTED_COLOR[] = "AudioBridgeNoteByNoteRejectedColor";
		inline constexpr char TAB_DOCKED[] = "AudioBridgeNoteByNoteTabDocked";
		inline constexpr char TAB_HIGHLIGHT[] = "AudioBridgeNoteByNoteTabHighlight";
	}

	inline constexpr char INI_SECTION[] = "Note by Note";
	inline constexpr char DETECTION_OVERLAY_INI[] = "NoteByNoteDetectionOverlay";

	enum class TargetPosition { Left, Center, Custom };   // Custom = the dragged TargetPlacement
	enum class TargetStyle { Detailed, Simple, Tab, TabTimeline };
	// The two HUD blocks the layout editor moves (4.x's Settings::NoteByNoteHudBlock).
	enum class HudBlock { Readout, Target };

	// Fractions of the game window; x < 0 = the built-in layout ("Default").
	struct Placement
	{
		float x = -1.0f;
		float y = -1.0f;
		bool IsSet() const { return x >= 0.0f && y >= 0.0f; }
	};

	struct Snapshot
	{
		bool detectionVisible = true;
		TargetPosition targetPosition = TargetPosition::Left;
		TargetStyle targetStyle = TargetStyle::Detailed;
		int uiSize = 100;          // percent, 50..300
		int targetSize = 150;      // percent, 50..300
		int lineSpacing = 100;     // percent, 50..300
		Placement readoutPlacement;
		Placement targetPlacement;
		bool customColours = false;
		// ARGB (opaque): neutral, confirmed, partial, rejected. The defaults whether or not customColours is on.
		std::uint32_t colors[4] = { 0xFFFFFFFFu, 0xFF55DD77u, 0xFFFFAA44u, 0xFFFF5555u };
		bool tabDocked = false;
		std::uint32_t tabHighlight = 0;   // 0xRRGGBB; 0 = the overlay accent ("Default")
	};

	Framework::SettingDefs Declare();

	// MainThread (OnInitialize / OnSettingsChanged).
	void Load(const Framework::ModContext& c);

	// Any thread.
	Snapshot Get();
	// The colours the readout draws with: the custom ones while Custom colours is on, else the defaults (4.x's
	// Settings::GetNoteByNoteDetectionPalette).
	DetectionPalette Palette(const Snapshot& snapshot);
	Placement GetPlacement(const Snapshot& snapshot, HudBlock block);

	// Any thread (the overlay page and the layout editor call them on the render thread). See the header comment.
	void SetDetectionVisible(bool visible);
	void SetUiSize(int percent, bool persist);         // clamped to 50..300
	void SetTargetSize(int percent, bool persist);
	void SetLineSpacing(int percent, bool persist);
	void SetPlacement(HudBlock block, Placement placement, bool persist);   // unset = the built-in layout
	void SetTargetPosition(TargetPosition position);
	void SetTargetStyle(TargetStyle style);
	void SetCustomColours(bool enabled);
	void SetColor(int index, std::uint32_t rgb, bool persist);   // 0 neutral, 1 confirmed, 2 partial, 3 rejected; 0xRRGGBB
	void ResetColors();
	void SetTabDocked(bool docked);
	void SetTabHighlight(std::uint32_t rgb, bool persist);       // 0 = the overlay accent ("Default")
}
