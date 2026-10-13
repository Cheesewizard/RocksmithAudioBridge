#pragma once
// The in-game Rocksmith Audio Bridge overlay (toggled with \): a sidebar of feature pages that drive the audio engine
// in-process (SharedOutput::DispatchControl) and the Note by Note settings live.

namespace Overlay
{
	void DrawShell(bool* open);
	const char* CurrentPageName();   // the page last drawn, for the slow-frame log (Menu.cpp)
	bool IsLatencyReadoutEnabled();
	bool SetLatencyReadoutEnabled(bool enabled);
}
