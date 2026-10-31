#pragma once

#include <cstdint>
#include <string>

namespace GameTones
{
	void Initialize();
	void OnFrame();
	uintptr_t PlayingSongObject();
	// The key of the tone the game plays now (the manifest's Tone_Base / Tone_A..D key, "MySong_Lead"), as read by the
	// last OnFrame; empty outside a song, or when this game build's tone component does not match (no guessing).
	// The game itself switches it at the chart's tone changes, seeks, Riff Repeater loops and the 1-4 keys.
	const std::string& CurrentToneKey();
}
