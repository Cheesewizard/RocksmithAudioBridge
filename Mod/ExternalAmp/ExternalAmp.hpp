#pragma once

#include "../Shared/AmpLink.hpp"

// External amp: play through an amp sim (AmpliTube 5 or any ASIO host) instead of Rocksmith's
// own amp, while Rocksmith keeps the clean guitar signal for note detection.
//
// The audio side lives in the proxy driver (Driver/AsioProxy/AmpLink.hpp): the game's copy of the driver sends
// the raw DI to the amp sim's copy and mixes the amp's output back in. This file is the game side:
//   - pushes the player's switch and return level to the proxy (RSModsAsio_ConfigureAmpLink);
//   - mutes the game's own guitar bus (Mixer_Player1) while an amp sim is connected, and restores the
//     player's level when it disconnects ("fall back to the game's amp") or the switch goes off. Detection
//     reads the input, not this bus, so it is unaffected. The level is re-asserted every tick, because the
//     game, the Mixer page or the volume keybinds can raise Player 1 again; the raised level becomes the one
//     restored later;
//   - sends the Drop Pedal's input shift to the proxy (RSModsAsio_SetAmpLinkPitch), so the amp sim's copy of the
//     guitar is shifted the same way as the game's (the link taps the DI before the game's shifter);
//   - silences the guitar in the tuner screens when asked (SilenceGuitarWhileTuning, General page): holds
//     Mixer_Player1 at 0 the same way and stops mixing in the amp sim's return, so it works in every mode;
//   - reports link status to the overlay's External amp page.
// Settings live in HKCU\Software\RSMods\AsioProxy (not RSMods.ini, which the desktop GUI rewrites with only
// the keys it knows): ExternalAmp (default on), ExternalAmpFallback (default on), ExternalAmpReturn
// (tenths of a dB, default 0), ExternalAmpSafety (default off), CableBlockFrames (0 = Automatic, the cable's link block). On + fallback means the switch is effectively
// automatic: the game's amp plays until an amp sim connects, then the amp sim takes over.
// The Guitar Tone page's choice (GuitarTone below) is saved in HKCU\Software\RSMods\VstHost GuitarTone. Game amp is
// enforced here: the link's return is not mixed in and Mixer_Player1 is not held at 0, whatever app is connected.
namespace ExternalAmp
{
	// Where the guitar's sound comes from: the Guitar Tone page's tabs, in their order. Unset: the player has not picked
	// a tab since the tabs came (an upgrade), so nothing is enforced and the page shows the tab of whatever plays.
	enum class GuitarTone { Unset = -1, GameAmp = 0, ToneDesigner = 1, VstRig = 2, ExternalApp = 3 };

	struct Snapshot
	{
		bool enabled = true;           // the External amp switch: mix in an external app
		bool toneDesigner = false;     // the link's guest is Tone designer (the VST host): mixed in whatever the switch,
		                               // at 0 dB (it has its own trims), always falling back to the game's amp
		bool gameAmp = false;          // Game amp is chosen (GuitarTone::GameAmp): the game's amp plays, no amp sim is mixed in
		bool fallback = true;
		int returnTenths = 0;          // amp return level, tenths of a dB
		bool safetyBuffer = false;     // one more buffer of link latency, fewer clicks on a busy PC
		bool proxyLoaded = false;      // an amp link host exists: the Audio Bridge ASIO driver, or (Real Tone Cable) CableAmpLink
		bool guest64Registered = false; // 64-bit amp sims can list the driver
		bool gameAmpMuted = false;     // Mixer_Player1 is held at 0 by this controller
		bool silenceWhileTuning = false; // General: no guitar in the tuner screens, whatever plays it
		int pedalSemitones = 0;        // Drop Pedal shift sent to the amp sim's input
		bool cableHost = false;        // the Real Tone Cable hosts the link (no ASIO driver): the block size is ours to pick
		bool cableDirectUsb = false;   // ...and the cable is read directly over USB
		int cableBlockSetting = 0;     // cable block size: 0 = Automatic, else 64 / 128 / 256 frames
		float cableLatencyMs = 0.0f;   // measured guitar-to-speaker delay through the amp sim on the cable (0 = not yet)
		float cableReadMs = 0.0f;      // the read part of it (packet wait, cable to PC); the rest is tone to speaker
		float asioInMs = 0.0f;         // ASIO mode: the driver's reported input latency (0 = no proxy stream)
		float asioOutMs = 0.0f;        // ...and output latency; both include the driver's buffer
		AmpLink::Status link{};
	};

	void Start();                      // the controller thread; call once at startup
	void Stop();                       // ends it, waiting at most 1 s (then it is left detached); MainThread, at shutdown
	Snapshot GetSnapshot();
	void SetEnabled(bool enabled);
	GuitarTone GetGuitarTone();
	// Saves the choice. Game amp takes effect at once (next tick); the others only stop enforcing it: what plays
	// for them (the VST host on or in standby, the Tone designer engine) is the page's to set.
	void SetGuitarTone(GuitarTone tone);
	void SetFallback(bool fallback);
	void SetReturnTenths(int tenths, bool persist);
	void SetSafetyBuffer(bool on);
	void SetSilenceWhileTuning(bool on);   // also the game's amp, so it lives here, not in Tone designer
	void SetCableBlock(int frames);        // 0 = Automatic, 64 / 128 / 256 (CableBlockFrames); cable players only
}
