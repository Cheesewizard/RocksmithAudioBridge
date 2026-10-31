#pragma once

#include <string>

// The Windows volume of the device the game plays to, for the overlay's Output page.
//
// Why: the bridge plays to whichever device is picked on the Output page, but that device's volume and mute stay in
// Windows. A player whose speakers sit at 0 hears nothing and has to leave the game, make the speakers the
// Windows default and raise them there; with exclusive mode on, Windows will not even switch to the device.
//
// A background thread (COM never runs on the game's render thread) watches the device named by Watch, reads its
// level and mute twice a second, and applies Set / SetMute. Exclusive mode bypasses Windows' software volume, so on a
// device without hardware volume the bridge applies the level itself: SoftwareGain, read by the exclusive output.
namespace Audio::DeviceVolume
{
	struct State
	{
		bool known = false;      // read at least once for the watched device
		float level = 1.0f;      // 0..1, Windows' scalar
		bool muted = false;
		bool hardware = false;   // the device changes its own volume (so exclusive mode follows it)
	};

	// Any thread: the Windows endpoint id to watch ("excl:" ids are accepted), empty for none.
	void Watch(const std::wstring& endpointId, bool exclusive);
	State Get();
	void Set(float level);
	void SetMute(bool muted);
	// Exclusive output on a device without hardware volume: the level to apply (0 when muted); 1 otherwise.
	float SoftwareGain();
}
