#pragma once

// What other features (Note by Note) need to know about the pedal's shift of Player 1's guitar input, from any thread
// (Note by Note asks from the game's scoring thread). 4.x's DLL/DropPedal/DropPedal.cpp GetAppliedInputShiftSemitones
// and Note by Note's HostGetInputOnsetShiftSemitones, on the framework mod's state. Everything here is lock-free.
namespace AudioBridge::DropPedal::Shift
{
	// The semitones the input shifter is applying to Player 1's samples right now: the gap between the player's
	// physical pitch and the audio every later consumer of the input hears (Note by Note's raw pitch verifier and ML
	// export observe after the shifter, so they measure in the shifted frame). 0 while the pedal is off, in Speaker
	// Mode, or Player 1's input isn't running through the chain.
	int AppliedInputShiftSemitones();

	// The shift Note by Note grades the expected pitch by: in Drop Pedal mode, the shift that puts the player's
	// physical guitar in the chart's tuning when the chart is readable (equal to the dialed shift when the pedal is set
	// right), else the dialed shift; 0 in any other mode.
	int InputOnsetShiftSemitones();

	// PedalControls, MainThread: the chart-match shift, or unknown outside a song and its tuner.
	void PublishChartMatch(bool known, int shiftSemitones);
}
