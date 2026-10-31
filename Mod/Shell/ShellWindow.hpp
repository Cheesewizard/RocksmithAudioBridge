#pragma once

#include <string>

// Draws the Audio Bridge overlay: header, the sidebar of registered pages (ShellPages.hpp) and the chosen page.
// Render thread, inside an ImGui frame.
namespace AudioBridge::Shell
{
	// Its own window (the overlay key). Clicking close sets *open to false. keyName is the overlay key, for the hint.
	void DrawWindow(bool* open, const std::string& keyName);
}
