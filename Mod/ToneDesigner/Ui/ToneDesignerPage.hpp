#pragma once

namespace Overlay
{
	// The VST rig tab of the Guitar Tone page (GuitarTonePage.cpp): VST3 plugins in the VST host (VstHostLink.hpp) as
	// a chain loader, with the tone library, the pedalboard and the plugin / song browser in one framed panel. Render
	// thread, inside the overlay window; fills the window's height (Shell::FillWindowHeight).
	void ToneDesignerTab();
}
