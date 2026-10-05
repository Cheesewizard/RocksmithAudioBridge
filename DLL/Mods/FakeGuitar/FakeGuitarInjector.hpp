#pragma once

#include <cstdint>

// A synthetic guitar input for hands-free Note by Note testing. It installs an
// IInputProcessor on the Player 1 ASIO route (the same tap Drop Pedal uses) and, when
// armed, overwrites the captured cable samples with a tone at a requested MIDI pitch.
// Rocksmith's own detection then runs on the synthesized signal exactly as it would on a
// real pickup, so a driver can walk a chart by reading the expected note over the research
// bridge, injecting it, and waiting for the freeze to advance (no guitar, no playing in
// time). Debug-only.
namespace FakeGuitar
{
	// Install the synthetic input as a SOURCE stage on the Player 1 route, before RS_ASIO
	// unmarshals its capture stream. It runs ahead of the route's processor, so when armed it
	// feeds the real Drop Pedal shifter exactly as a physical cable would - the two now
	// coexist instead of competing for the one processor slot.
	void Install();

	// True once Install() claimed the route. The host loop calls Poll() while this holds so
	// the ASIO hook auto-enables processing when the song's capture attaches.
	bool IsInstalled();
	void Poll();

	// Synthetic playback on/off. Off passes the real cable through untouched, so an
	// installed-but-idle harness is transparent to a live guitar. Turning it off also
	// clears any queued notes.
	void SetSynthEnabled(bool enabled);
	bool IsSynthEnabled();

	// True when the route is attached, prepared and processing - i.e. injected notes will
	// actually reach the game. False until the player is in a song with the cable capture
	// live.
	bool IsCaptureReady();

	// Queue one note. amplitude is a 0..1 linear peak; durationMs is how long it sounds;
	// leadSilenceMs is silence played before it so a repeated pitch still presents a fresh
	// attack the onset detector can see. Returns false if the queue is full. Safe to call
	// from the bridge thread against the live audio thread.
	bool QueueNote(int midi, float amplitude, uint32_t durationMs, uint32_t leadSilenceMs);

	// Queue up to six simultaneous pitches as one strum, for chord tests. Same parameters
	// as QueueNote; extra pitches beyond six are ignored.
	bool QueueChord(const int* midis, int count, float amplitude, uint32_t durationMs, uint32_t leadSilenceMs);

	// Queue a single-note bend: sound baseMidi, hold for bendHoldMs, then glide to endMidi
	// over bendRampMs and hold there for the rest of durationMs - the pitch rise a real
	// string bend makes, so the game's bend detector accepts it.
	bool QueueBend(int baseMidi, int endMidi, float amplitude, uint32_t durationMs, uint32_t bendHoldMs, uint32_t bendRampMs);

	// Drop the current note and everything queued behind it; the processor returns to
	// silence.
	void ClearQueue();

	// The MIDI pitch currently sounding, or -1 when silent. For bridge status.
	int CurrentMidi();
	// How many notes are queued behind the one now playing.
	uint32_t PendingCount();

	// Load a bank of per-note WAV recordings (note_<midi>.wav) from a folder, so injected
	// notes play real guitar samples instead of the synth tone (bends resample the recording).
	// Returns how many notes loaded. Empty/absent folder falls back to the synth.
	int LoadSamples(const char* folder);
	// How many guitar samples are currently loaded (0 = using the synth).
	int SampleCount();
}
