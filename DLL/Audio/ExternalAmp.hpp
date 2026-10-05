#pragma once

#include "../AsioProxy/AmpLink.hpp"

// External amp: play through an amp sim (AmpliTube 5 or any ASIO host) instead of Rocksmith's
// own amp, while Rocksmith keeps the clean guitar signal for note detection.
//
// The audio side lives in the proxy driver (DLL/AsioProxy/AmpLink.hpp): the game's copy of the driver sends
// the raw DI to the amp sim's copy and mixes the amp's output back in. This file is the game side:
//   - pushes the player's switch and return level to the proxy (RSModsAsio_ConfigureAmpLink);
//   - mutes the game's own guitar bus (Mixer_Player1) while an amp sim is connected, and restores the
//     player's level when it disconnects ("fall back to the game's amp") or the switch goes off. Detection
//     reads the input, not this bus, so it is unaffected. The level is re-asserted every tick, because the
//     game, the Mixer page or the volume keybinds can raise Player 1 again; the raised level becomes the one
//     restored later;
//   - sends the Drop Pedal's input shift to the proxy (RSModsAsio_SetAmpLinkPitch), so the amp sim's copy of the
//     guitar is shifted the same way as the game's (the link taps the DI before the game's shifter);
//   - reports link status to the overlay's External amp page.
// Settings live in HKCU\Software\RSMods\AsioProxy (not RSMods.ini, which the desktop GUI rewrites with only
// the keys it knows): ExternalAmp (default on), ExternalAmpFallback (default on), ExternalAmpReturn
// (tenths of a dB, default 0), ExternalAmpSafety (default off). On + fallback means the switch is effectively
// automatic: the game's amp plays until an amp sim connects, then the amp sim takes over.
namespace ExternalAmp
{
	struct Snapshot
	{
		bool enabled = true;
		bool fallback = true;
		int returnTenths = 0;          // amp return level, tenths of a dB
		bool safetyBuffer = false;     // one more buffer of link latency, fewer clicks on a busy PC
		bool proxyLoaded = false;      // the game's audio driver is the Audio Bridge proxy
		bool guest64Registered = false; // 64-bit amp sims can list the driver
		bool gameAmpMuted = false;     // Mixer_Player1 is held at 0 by this controller
		int pedalSemitones = 0;        // Drop Pedal shift sent to the amp sim's input
		AmpLink::Status link{};
	};

	void Start();                      // the controller thread; call once at startup
	Snapshot GetSnapshot();
	void SetEnabled(bool enabled);
	void SetFallback(bool fallback);
	void SetReturnTenths(int tenths, bool persist);
	void SetSafetyBuffer(bool on);
}
