#pragma once

#include "AsioProxy/AmpLink.hpp"
#include <cstdint>

// The amp link (AsioProxy/AmpLink.hpp) for Real Tone Cable players, who have no ASIO driver to host it.
//
// With an ASIO interface the bridge's ASIO driver owns the link: it hands the guitar to the Tone designer's VST
// host (or another amp app) and mixes the processed sound back into the interface's output. In cable mode there is
// no such driver, so the game DLL hosts the same link itself (the same AmpLink::Host, unchanged protocol), and the
// VST host attaches exactly as it does with ASIO:
//   - input: the cable's raw guitar (before the bridge's input conditioner, as with ASIO) from the cable capture
//     (PersistentInput::CaptureSession::Publish, Player 1 only), cut into 128-frame blocks and published on that
//     thread;
//   - output: the guest's processed stereo is pulled by the physical output device's own pump
//     (SharedOutput::OutputBackend::Pump) and added to what it plays, so the return follows the device clock and
//     never waits for the game's 480-frame packets.
// The cable and the output device run on different clocks, so the return FIFO between them is kept near a small
// target by dropping or repeating single frames (a few hundred ppm at most, inaudible on a guitar signal).
//
// A future direct-USB capture (atlas design cable-direct-usb-input.md) feeds FeedInput the same way, in 1 ms packets.
namespace CableAmpLink
{
	// Cable mode's bridge output exists (OutputSession initialised): create the link. Idempotent.
	void Open();
	void Close();
	bool IsOpen();

	// Capture thread: mono float guitar samples (48 kHz).
	void FeedInput(const float* mono, uint32_t frames) noexcept;
	// Output device thread: add the guest's return to `stereo` (interleaved float, `frames` frames).
	void Render(float* stereo, uint32_t frames) noexcept;

	// Block size between the cable and the guest (Amp sim page, cable players only): 0 = Automatic (64 on direct USB,
	// 128 on the Windows cable input), else 64 / 128 / 256; 64 is raised to 128 on the Windows input. A change while
	// open reopens the link at the new size and the VST host resets its stream to it.
	void SetBlockSetting(int frames);
	int GetBlockSetting();
	// The cable's physical source (CaptureSession, Player 1): Automatic follows it.
	void SetDirectUsb(bool directUsb);
	bool IsDirectUsb();

	// The ASIO driver's RSModsAsio_* link exports, for ExternalAmp when the cable hosts the link.
	AmpLink::Status GetStatus();
	void Configure(bool mixOn, float returnGain);
	void SetLatencyBlocks(int blocks);
	void SetPitchSemitones(int semitones);
}
