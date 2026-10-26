#pragma once

namespace Overlay
{
	// The Tone designer tab: the built-in engine (Stompbox, ToneDesigner/StompboxLink.hpp) as a pedalboard. Saved tones
	// on the left, one pedal per effect on the board, the effects, amps and cabinets to drag onto it below, and the
	// selected pedal's controls in a panel beside them (drawn here, so it works in exclusive fullscreen too).
	// Render thread, inside the overlay window, filling the space left under the page header.
	void DrawStompboxPage();
}
