#pragma once

// 64-bit ASIO drivers: lets the bridge use an interface whose maker ships only a 64-bit ASIO driver (Line 6 Helix
// Stadium, for one), which the 32-bit game can never load into itself.
//
// The bridge (inside Rocksmith, 32-bit) gets a stand-in driver (Remote64Driver.cpp) in place of the real one. The
// stand-in starts RocksmithAudioBridgeDriverHost.exe (64-bit, DriverHost64/), which loads the real driver and owns
// the interface. The two share this block of memory:
//   - setup calls (init, channels, buffer sizes, sample rate, channel info, create buffers, start, stop...) go as one
//     request at a time: the stand-in fills `control`, signals the request event and waits for the reply event;
//   - every buffer of the real driver, the host copies the interface's input into the shared buffer of that half,
//     plays the output the game finished for the previous block, and signals the block event;
//   - the stand-in's thread wakes on it, runs the bridge's bufferSwitch on the shared buffers of that half (the
//     bridge reads input and writes output in place) and marks that half done.
// One clock (the interface's) drives both programs. The cost is one buffer of extra output latency (2.7 ms at 128
// frames, 48 kHz): the host cannot wait inside the driver's callback for the game without risking a click on every
// late wake-up, so it plays each block one buffer later. A block the game did not finish in time plays as silence
// and is counted.
//
// Driver messages (reset request, latencies changed, sample rate changed) go through a small queue to the stand-in,
// which passes them to the bridge like a real driver would. If the host dies, the stand-in asks the bridge for a reset,
// so the bridge's own reopen path (ScheduleResetRebind) starts a fresh host.
//
// The layout uses fixed-width types only and is identical in the 32-bit (game) and 64-bit (host) builds. ASIO's own
// structs (ASIOChannelInfo, ASIOClockSource) are made of long and char only, so they are the same size in both and are
// copied through `payload` as they are.

#include <Windows.h>
#include <cstdint>

namespace Remote64
{
	constexpr wchar_t kHostExe[] = L"RocksmithAudioBridgeDriverHost.exe";
	constexpr LONG kMagic = 0x42343652;   // "R64B"
	constexpr LONG kVersion = 1;
	constexpr LONG kMaxChannels = 64;     // buffers the bridge may create (inputs + outputs)
	constexpr LONG kMaxFrames = 4096;     // largest ASIO buffer carried (the bridge's own maximum)
	constexpr LONG kSlotBytes = kMaxFrames * 8;   // one half of one channel, room for the widest sample type
	constexpr LONG kMessages = 16;

	// One mapping per stand-in: "Local\RocksmithAudioBridge.Remote64.<game pid>.<instance>", plus these suffixes.
	constexpr wchar_t kNamePrefix[] = L"Local\\RocksmithAudioBridge.Remote64.";
	constexpr wchar_t kRequestSuffix[] = L".Request";
	constexpr wchar_t kReplySuffix[] = L".Reply";
	constexpr wchar_t kBlockSuffix[] = L".Block";

	enum Op : LONG
	{
		OpNone = 0,
		OpInit,              // -> result ASIOBool; text = error message when refused; name, version
		OpGetChannels,       // -> out[0] inputs, out[1] outputs
		OpGetLatencies,      // -> out[0] input, out[1] output (frames, as the driver reports them)
		OpGetBufferSize,     // -> out[0..3] min, max, preferred, granularity
		OpCanSampleRate,     // value
		OpGetSampleRate,     // -> outValue
		OpSetSampleRate,     // value
		OpGetClockSources,   // arg[0] room -> out[0] count; payload ASIOClockSource[count]
		OpSetClockSource,    // arg[0]
		OpGetChannelInfo,    // payload ASIOChannelInfo in and out
		OpCreateBuffers,     // audio.channels / numChannels / frames set by the stand-in -> out[0] outputReady supported
		OpDisposeBuffers,
		OpStart,
		OpStop,
		OpControlPanel,      // answered at once; the panel opens after the reply
		OpFuture,            // arg[0] selector (only selectors that take no data)
		OpQuit,
	};

	struct Control
	{
		LONG op;
		LONG arg[4];
		double value;
		LONG result;          // the ASIOError (or ASIOBool for OpInit)
		LONG out[4];
		double outValue;
		char text[128];       // OpInit: error message, or the driver's name when it starts
		LONG version;         // OpInit: the driver's version
		BYTE payload[48 * 32];
	};

	struct Channel
	{
		LONG isInput;
		LONG channelNum;
		LONG type;            // ASIOSampleType, as the real driver reports it; the bridge converts formats itself
		LONG bytes;           // bytes per sample of `type`
	};

	struct Message
	{
		LONG kind;            // 1 asioMessage, 2 sampleRateDidChange
		LONG selector;
		LONG value;
		double rate;
	};

#pragma pack(push, 8)
	struct Shared
	{
		LONG magic;
		LONG version;
		LONG gamePid;
		LONG hostPid;
		volatile LONG hostReady;      // 1 once the host has loaded the driver's DLL (or 2: it would not load; text says why)
		Control control;

		// Audio: set by the stand-in before OpCreateBuffers.
		LONG numChannels;
		LONG frames;
		Channel channels[kMaxChannels];

		// Written by the host every block.
		volatile LONG blockSeq;       // 1, 2, 3... per block since start
		volatile LONG blockIndex;     // the half of that block (0 or 1)
		volatile LONGLONG samplePosition;
		volatile LONGLONG systemTime; // nanoseconds, as ASIOTimeStamp
		volatile LONG missed;         // blocks played as silence: the game had not finished them

		// Written by the stand-in: the block whose output it finished in each half.
		volatile LONG outSeq[2];

		// Host -> stand-in driver messages.
		volatile LONG messageWrite;
		volatile LONG messageRead;
		Message messages[kMessages];
	};
#pragma pack(pop)

	constexpr DWORD kHeaderBytes = (sizeof(Shared) + 4095) & ~4095u;
	constexpr DWORD kMappingBytes = kHeaderBytes + static_cast<DWORD>(kMaxChannels) * 2 * static_cast<DWORD>(kSlotBytes);

	// The shared buffer of half `half` of created channel `channel`.
	inline BYTE* Slot(void* base, LONG channel, LONG half)
	{
		return static_cast<BYTE*>(base) + kHeaderBytes + (static_cast<size_t>(channel) * 2 + static_cast<size_t>(half)) * static_cast<size_t>(kSlotBytes);
	}

	inline LONG SampleBytes(LONG type)
	{
		switch (type)
		{
		case 0: case 16: return 2;                      // Int16
		case 1: case 17: return 3;                      // Int24
		case 4: case 20: return 8;                      // Float64
		default: return (type >= 0 && type <= 27) ? 4 : 0;   // Int32 variants, Float32
		}
	}
}
