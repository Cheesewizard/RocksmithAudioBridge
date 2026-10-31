#pragma once

namespace Overlay
{
	// The Tone designer tab: the built-in engine (Stompbox, ToneDesigner/StompboxLink.hpp) as a pedalboard. Saved tones
	// on the left, one pedal per effect on the board, the effects, amps and cabinets to drag onto it below, and the
	// selected pedal's controls in a movable in-game editor (drawn here, so it works in exclusive fullscreen too).
	// Drawn with the VST rig page's parts (ToneDesignerUi.hpp), so both tabs read as one app. Render thread, inside
	// the overlay window, filling the space left under the page header. Returns the page's natural height (as the
	// shell's Shell::FillWindowHeight), or 0 while it shows only a status card and does not fill the window.
	float DrawStompboxPage();
}
