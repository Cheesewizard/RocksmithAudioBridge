#pragma once

#include <string>

// Measured latency in ASIO mode, for the readouts: when to measure, and what was measured on this setup.
//
// The measuring itself happens in the bridge driver, on its buffers (Driver/AsioProxy/LatencyMeasure.hpp). This side:
//   - runs the game measurement (no cable, about a second of silence) by itself, in the menus with nobody playing,
//     once per setup: a new interface, buffer size, sample rate or driver host is a new setup;
//   - stores every result per setup (HKCU\Software\RSMods\AsioProxy\MeasuredLatency\<driver>@<frames>@<rate>[@64]),
//     so a later launch on the same setup shows it at once;
//   - gives the overlay Recalibrate (the game measurement again, now) and the patch-cable hardware measurement.
//
// The readouts then show guitar to ear as the interface's round trip (measured with the cable, else the driver's
// own figures) plus the path's processing (measured, else the old estimate). The amp path is stored split: the link's
// blocks and the delay the Tone designer's plugins report are added live (the safety buffer, a new chain or a plugin
// changing its own delay shows at once), and only the rest is measured. A different guest (another amp sim app) is
// measured again by itself. After a song the game path is checked once more in the menus (at most every 5 minutes),
// logged as "after-song recheck", to catch a shift after a dropout.
namespace Audio::LatencyCalibration
{
	// ExternalAmp's worker, every 100 ms.
	void Tick();

	// What is stored for the setup now; -1 = not measured. All -1 outside ASIO mode.
	struct Figures
	{
		float gameMs = -1.0f;        // the game's amp, input buffer to output buffer
		float ampMs = -1.0f;         // the amp link's return (Tone designer / amp sim), input buffer to output buffer
		float roundTripMs = -1.0f;   // the interface: output back to input through a loop cable
		// The amp figure split (stored with it): what is neither the link's blocks nor the plugins' reported delay,
		// and the guest it was measured with. The readout adds the live blocks and plugin delay back.
		float ampBaseMs = 0.0f;
		bool hasAmpBase = false;
		std::string ampGuest;
	};
	Figures Current();

	// Guitar to ear in ASIO mode, for both readouts (the overlay header and the HUD), so they agree. Input and output
	// are the interface's: measured round trip split in the driver's own proportion, else the driver's figures. Each
	// path's total adds its processing: measured, else `linkMs` (the amp link's blocks) or `gameEstimateMs`.
	enum class Quality { Measured, Estimate, Guess };
	struct AsioSplit
	{
		float inMs = 0.0f, outMs = 0.0f;        // the interface
		float ampTotalMs = 0.0f, gameTotalMs = 0.0f;
		Quality amp = Quality::Estimate, game = Quality::Guess;   // the worse of the interface's and the path's
	};
	AsioSplit SplitAsio(float driverInMs, float driverOutMs, float linkMs, float gameEstimateMs, bool gameEstimateGuessed);
	const char* Label(Quality quality);      // "measured", "est.", "guess"

	// Overlay.
	bool Available();                       // ASIO mode with a bound interface: measuring can run
	bool Running();                         // a measurement is under way
	bool HoldingMuted();                    // the hardware measurement ended; the output waits for the cable to come out
	bool Recalibrate(std::string& why);     // the game measurement now (forgets the stored game and amp figures)
	bool MeasureHardware(std::string& why); // the patch-cable measurement
	void CableOut();                        // the player has unplugged the cable: unmute
	void ForgetHardware();                  // back to the driver's own figures
	std::string Status();                   // one line: what it is doing, or what is stored
}
