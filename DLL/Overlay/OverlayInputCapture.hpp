#pragma once

namespace OverlayInputCapture
{
	bool Install();
	void SetMouseCapture(bool shouldCapture);
	// While true, the game's DirectInput keyboard reads come back empty (all keys up, no buffered events).
	void SetKeyboardCapture(bool shouldCapture);
	void Shutdown();
}
