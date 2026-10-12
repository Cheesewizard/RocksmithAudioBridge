#pragma once

#include <string>

// The game's own "TONE SWITCH" readout (the in-song notification shown at song start and at each tone change) names
// the Tone designer's tone while the player hears it, instead of the game's song name and tone descriptors.
//
// Every producer of that readout ends in one generic notification function with type 7 (tone change); a detour there
// swaps the body text when a name is set and leaves every other notification untouched. Reverse engineering and
// addresses: atlas docs/investigations/tone-change-notification.md (2026-10-11).
namespace ToneSwitchPopup
{
	void Initialize();                         // installs the detour once; refuses on an unknown game build
	void SetToneName(const std::string& name); // any thread; empty: the game's own text
	// Brings the game's own tone readout up (a frame or two later, on the game thread) in the current song. Overlay
	// thread. False when there is no song object to show it on.
	bool ShowNow();
	// The song is playing (its objects exist and its clock has passed 1 s): the moment the readout can show.
	bool SongPlaying();
}
