#pragma once

#include <string>

// Starts the installed RSMods.exe background host. Its model and feature code
// live in RocksmithAudioBridge.dll; the child is bound to the game's kill-on-close job.
namespace MlServiceLauncher
{
	void EnsureStarted();
	// Called from the mod loop. The service (a separate process with the FretNet model) only feeds Note by
	// Note, so it is started the first time `wanted` is true (Note by Note on, or the Riff Repeater menus
	// open) or when Restart is pressed, and then kept for the session. Before that it costs nothing.
	void Poll(bool wanted);
	void Shutdown();

	// Plain-language service state for the overlay Debug page. connected = fresh results are flowing;
	// canRestart = RequestRestart() is meaningful right now.
	void Describe(std::string& message, bool& connected, bool& canRestart);
	// Same as the GUI's Restart button: signals the restart event EnsureStarted() polls.
	bool RequestRestart();
}
