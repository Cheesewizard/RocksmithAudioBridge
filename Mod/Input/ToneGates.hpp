#pragma once
#include <cstdint>
#include <string>

// Per-tone Noise gate settings. Each saved Tone designer tone can keep its own gate (on/off, threshold, decay,
// reduction, attack); a tone without its own uses the global gate (RSMods.ini). While a saved tone plays in the
// Tone designer, the pedal (overlay or settings app, control ops 19 and 31) edits that tone and it is remembered at
// once; otherwise (the game's amp, an unsaved chain) it edits the global gate. Stored in ToneGates.ini next to
// SongTones.ini, so tone files and the VST host are unchanged; a shared tone file does not carry its gate.
// Design: atlas docs/designs/per-tone-noise-gate-2026-10-22.md.
//
// Threads: the overlay (OnFrame, the pedal) and the control pipe call in; one mutex inside. The audio thread only
// sees the result, through the shared InputConditioner's atomics.
namespace ToneGates
{
	struct Gate
	{
		bool on = false;
		int thresholdTenths = -620;   // kept while off, so switching on returns to it
		int decayMs = 200;
		int reductionDb = -60;
		int attackTenths = 5;
	};

	struct View
	{
		std::string tone;             // the tone being edited; empty = all tones (the global gate)
		bool own = false;             // that tone has its own settings
		Gate gate;                    // what is playing now
		uint32_t revision = 0;        // changes whenever the target or a value changes
	};

	// Launch: the global gate from RSMods.ini. Applies it.
	void InitGlobal(const Gate& global);
	// The Tone designer's playing tone (empty when it is off, in standby or playing an unsaved chain). Applies that
	// tone's gate, or the global one, without resetting the gate, so a ringing note is not cut. Cheap when unchanged.
	void SetActiveTone(const std::string& tone);
	View Current();
	// A pedal edit (ops 19 and 31): goes to the playing tone (remembered in ToneGates.ini when `save`) or to the global
	// gate. Applies it live. Returns true when it went to a tone; false = the caller saves the global one in RSMods.ini.
	bool Apply(const Gate& gate, bool save);
	// "Use the global gate for this tone": the playing tone forgets its own settings. False when there is nothing to
	// remove.
	bool UseGlobal();
	// Tone files renamed, deleted or saved under a new name (VstHostLink).
	void RenameTone(const std::string& from, const std::string& to);
	void DeleteTone(const std::string& name);
	// Saving the chain as `name`: a tone saved from one with its own gate keeps that gate (Save as). A tone that
	// already has its own settings keeps them.
	void ToneSaved(const std::string& name);
}
