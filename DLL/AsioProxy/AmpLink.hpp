#pragma once

// External amp link: lets an amp sim (AmpliTube, Neural DSP, a DAW) use the Rocksmith Audio Bridge ASIO
// driver at the same time as Rocksmith, without opening the audio interface a second time.
//
// Most ASIO drivers allow one client. Selecting the proxy in AmpliTube would load a second copy of the
// real driver inside AmpliTube, and two programs fighting over one
// interface is the classic crackle. Running the amp sim on the interface and Rocksmith on something else
// needs a second guitar input (a splitter), which is clumsy.
//
// The design: the proxy copy inside Rocksmith (the HOST) stays the only owner of the interface. Any other
// process that loads the driver (always the 64-bit build, which AmpliTube 5 needs; also a 32-bit DAW) gets
// a GUEST driver (AmpLinkGuest.cpp) that never touches the hardware. The two copies share this block of
// memory:
//   - every host buffer, the host copies the raw guitar input (before the RSModsPlus input chain, so the amp
//     sim gets the clean DI) into an input slot and signals an event;
//   - the guest wakes on that event, runs the amp sim's ASIO callback on that input, and writes the amp's
//     output into the matching output slot;
//   - with the Drop Pedal on, the guest shifts that input by the pedal's semitones (pitchSemitones) with the
//     game's own shifter, so the amp plays what the game hears;
//   - on its NEXT buffer the host mixes that output into what goes to the interface (when the player turns
//     External amp on). The game's own guitar tone is muted by the game DLL (ExternalAmp.cpp).
// One clock (the interface's) drives both programs, so nothing drifts and nothing crackles. The cost is one
// buffer of extra latency (2.7 ms at 128 frames, 48 kHz) plus whatever the amp sim adds.
//
// The guest only processes blocks the host produced; a guest that runs late simply misses that block (the
// host plays it as silence and counts it). The guest and host must agree on the buffer size: the guest
// offers only the host's size, and asks its host for a reset if the game later changes it.
//
// The layout uses fixed-width types only and is identical in the 32-bit (game) and 64-bit (AmpliTube)
// builds. Shared counters are 32-bit and accessed with Interlocked*, which is lock-free in both.
// Sequence numbers wrap after ~130 days at 128 frames; only equality and small differences are compared.

#include <Windows.h>
#include <cstdint>

namespace AmpLink
{
	constexpr wchar_t kMappingName[] = L"Local\\RocksmithAudioBridge.AmpLink.v1";
	constexpr wchar_t kInputEventName[] = L"Local\\RocksmithAudioBridge.AmpLink.v1.Input";
	constexpr LONG kMagic = 0x4B4C4D41;   // "AMLK"
	constexpr LONG kVersion = 1;
	constexpr LONG kSlots = 4;            // blocks of history; the guest may be up to 3 blocks behind and still read
	constexpr LONG kChannels = 2;         // stereo in and out
	constexpr LONG kMaxFrames = 4096;     // largest buffer the link carries (the proxy's virtual maximum)
	constexpr LONG kSampleRate = 48000;   // Rocksmith runs at 48 kHz, always

	// The host's last buffer size, remembered across launches so a guest opened before the game offers the
	// size the game will use. HKCU\Software is shared between the 32-bit and 64-bit registry views.
	constexpr wchar_t kRegistryKey[] = L"Software\\RSMods\\AsioProxy";
	constexpr wchar_t kRegistryFrames[] = L"AmpLinkFrames";

#pragma pack(push, 8)
	struct Shared
	{
		LONG magic;
		LONG version;
		LONG size;                  // sizeof(Shared), a layout check between builds
		// Drop Pedal: semitones the guest shifts its input by, the same shift the game applies to its own copy
		// of the guitar (DropPedal::GetAppliedInputShiftSemitones). The link taps the DI before the game's
		// shifter. Written by the host. Occupies the former reserved0 slot, so an older guest reads 0.
		volatile LONG pitchSemitones;

		// Written by the host.
		volatile LONG hostPid;
		volatile LONG hostFrames;   // frames per block; 0 = no stream yet
		volatile LONG hostRunning;  // the game's stream is started
		volatile LONG inputChannels;
		volatile LONG inputSeq;     // newest published input block
		volatile LONG inputSlotSeq[kSlots];   // which block each input slot holds (seqlock tag)

		// Written by the guest.
		volatile LONG guestPid;     // the guest that owns the output slots; 0 = none
		volatile LONG guestFrames;  // the guest's buffer size
		volatile LONG guestLastSeq; // newest block the guest has written back
		volatile LONG guestNameSeq; // bumped after guestName changes
		char guestName[64];         // the guest process name (UTF-8), for the overlay
		volatile LONG outputSlotSeq[kSlots];  // which block each output slot holds (seqlock tag)

		int32_t input[kSlots][kChannels][kMaxFrames];    // ASIOSTInt32LSB, full scale
		int32_t output[kSlots][kChannels][kMaxFrames];   // ASIOSTInt32LSB, full scale
	};
#pragma pack(pop)

	inline LONG Load(volatile LONG* value) { return InterlockedCompareExchange(value, 0, 0); }
	inline void Store(volatile LONG* value, LONG v) { InterlockedExchange(value, v); }
	inline LONG SlotOf(LONG seq) { return static_cast<LONG>(static_cast<ULONG>(seq) % kSlots); }

	// Status the host reports to the game DLL (RSModsAsio_GetAmpLinkStatus). Plain struct, same process.
	struct Status
	{
		int32_t linkOpen;        // the shared block exists (the proxy is the game's driver)
		int32_t guestPresent;    // a guest has claimed the link
		int32_t connected;       // the guest is writing blocks back right now
		int32_t frames;          // host buffer size
		int32_t sampleRate;
		int32_t guestFrames;     // the guest's buffer size (differs from frames while it resets)
		uint32_t lateBlocks;     // blocks the guest did not return in time while mixing
		float inputPeak;         // clean guitar sent to the guest, linear peak-hold
		float returnPeak;        // guest output after the return gain, linear peak-hold
		int32_t latencyBlocks;   // link latency in blocks: 1, or 2 with the extra safety buffer
		char guestName[64];
	};
}
