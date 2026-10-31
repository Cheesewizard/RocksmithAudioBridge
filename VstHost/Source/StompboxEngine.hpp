#pragma once

// The Stompbox engine (github.com/mikeoliphant/stompbox, GPL-3; see VstHost/Stompbox/UPSTREAM.md), loaded from
// stompbox-capi.dll next to the host. One chain of Stompbox effects (NAM amps, tone stacks, cabinets, drives,
// modulation, delays, reverbs) driven by Stompbox's own text commands ("SetChain Main NoiseGate NAM Cabinet",
// "SetParam NAM Model \"Crunch.nam\"", "LoadPreset Classic"). Its presets are plain text in its data folder, so the
// Tone designer owns every saved tone.
//
// Threads: Load, Prepare and the command/query calls run on the main thread, with the stream stopped for Prepare;
// Process runs on the stream thread. Stompbox swaps its chains itself (double-buffered), so commands may arrive while
// it plays, and it loads presets and models on a thread of its own (IsPresetLoading).
//
// Safe failure: every call into Stompbox is guarded (C++ exceptions and crashes alike), and nothing is thrown out of
// Process. A call that fails marks the engine faulted: from then on Process passes the guitar through dry (after the
// input trim) and the queries answer empty, until a fresh Stompbox processor is adopted (Adopt, the page's Rescan).

#include "BridgeAudio.hpp"

#include <Windows.h>
#include <atomic>
#include <string>
#include <vector>

namespace VstHost
{
	class StompboxEngine final : public AudioSink
	{
	public:
		StompboxEngine() = default;
		StompboxEngine(const StompboxEngine&) = delete;
		StompboxEngine& operator=(const StompboxEngine&) = delete;
		~StompboxEngine() override;

		// Loads stompbox-capi.dll from the host's folder and creates the engine on `dataPath` (NAM\ for models,
		// Cabinets\ for cabinet IRs, Reverb\ for reverb IRs, Presets\ for saved tones). False with `error` when the
		// DLL or one of its functions is missing.
		bool Load(const std::wstring& dataPath, std::string& error);
		bool IsLoaded() const { return processor != nullptr; }

		// Deletes the engine and releases the DLL (its server and preset threads with it). Main thread, after the
		// stream has stopped; the destructor does it too.
		void Unload();

		// The stream's block size. Not real-time safe (Stompbox allocates for it): only while the stream does not run
		// this engine.
		void Prepare(int frames);
		int PreparedFrames() const { return frames; }

		// Main thread, only while the stream does not run this engine: processes `blocks` blocks of silence. Stompbox
		// applies chain changes and loads presets only inside its processing (a fade, then a loading thread), and a
		// NAM model settles on its first blocks; a tone prepared ahead of playback is run like this until it is ready.
		void Pump(int blocks);

		// False when Stompbox threw or crashed on it (LastError says how; a crash also faults the engine).
		bool SendCommand(const std::string& command);
		const std::string& LastError() const { return lastError; }
		// A call into Stompbox crashed: Process plays the guitar dry. Any thread.
		bool HasFaulted() const { return faulted.load(std::memory_order_acquire); }

		// Every pedal Stompbox offers with its parameters, in Stompbox's own protocol (PluginConfig, ParameterConfig,
		// ParameterEnumValues, ParameterFileTree, EndConfig lines). Exported by the build script's patch.
		std::string DumpConfig();
		// The current rig as Stompbox preset text (SetPreset, SetChain and SetParam lines): what a saved tone stores.
		std::string DumpProgram();
		std::vector<std::string> ListPresets();
		std::string CurrentPreset();
		bool IsPresetLoading();

		// Main thread, only while the stream does not run this engine (the file checks' probe): runs `frames`
		// (PreparedFrames) samples of `input` through the chain. False when Stompbox failed on it.
		bool Render(float* input, float* output);

		// Rescan: takes over `next`'s Stompbox processor (loaded and prepared at this engine's block size, its tone
		// set up and run until ready), leaving `next` empty. The stream switches to it on its next block with a
		// one-block crossfade; FinishAdopt, called each main-thread tick, frees the old processor once the stream has
		// let go of it and returns true when the switch is complete. With `force` (the stream has not run this engine
		// for a while: the VST rig plays, or a song tone) the main thread switches itself. Main thread.
		bool Adopt(StompboxEngine& next);
		bool IsAdopting() const { return incoming.load(std::memory_order_acquire) != nullptr || retiring != nullptr; }
		bool FinishAdopt(bool force);

		// Stompbox's own TCP server, for its desktop remote (StompboxRemote) during development. Bound to
		// 127.0.0.1 by the build script's patch, so nothing outside this PC can reach it.
		void StartRemoteServer();

		// The player's input trim, linear, shared by every engine (the board's and each prepared song tone's): set
		// from the Tone designer's input calibration so the guitar drives the captures at the level they were
		// recorded at. Any thread.
		static void SetInputGain(float gain) { inputGain.store(gain, std::memory_order_relaxed); }
		static float InputGain() { return inputGain.load(std::memory_order_relaxed); }

		// What the chain receives: after the input trim.
		float TakeInputPeak() { return inputPeak.exchange(0.0f); }
		float TakeOutputPeak() { return outputPeak.exchange(0.0f); }

		// AudioSink, stream thread. Stompbox is mono: its output goes to both sides.
		void Process(const float* guitar, float* left, float* right, int frames) override;

	private:
		using CreateProcessorFunction = void* (*)(const wchar_t* dataPath);
		using DeleteProcessorFunction = void (*)(void* processor);
		using InitProcessorFunction = void (*)(void* processor, float sampleRate);
		using SetMaxAudioBufferSizeFunction = void (*)(void* processor, size_t numSamples);
		using ProcessorFunction = void (*)(void* processor);
		using ProcessFunction = void (*)(void* processor, float* input, float* output, size_t bufferSize);
		using HandleCommandFunction = void (*)(void* processor, const char* command);
		using ProcessorTextFunction = const char* (*)(void* processor);
		using ProcessorFlagFunction = bool (*)(void* processor);
		using ProcessorListFunction = void* (*)(void* processor);
		using StringVectorSizeFunction = size_t (*)(void* strings);
		using StringVectorValueFunction = const char* (*)(void* strings, size_t index);

		void RequireLoaded(const char* operation) const;
		std::string TakeAllocatedText(const char* text) const;   // a CoTaskMemAlloc'd string from Stompbox
		// Runs `call` with C++ exceptions and crashes caught. False on either; a crash faults the engine.
		template <typename Call> bool Guard(const char* operation, Call&& call);
		// Deletes a Stompbox processor, or keeps it (and the DLL) for good when it faulted: its state is unknown.
		void Release(void* old, bool hasFaulted);

		HMODULE module = nullptr;
		void* processor = nullptr;                  // main thread's view; the stream reads `active`
		std::atomic<void*> active{ nullptr };
		std::atomic<void*> incoming{ nullptr };     // Adopt: the stream switches to it on its next block
		void* retiring = nullptr;                   // the processor `incoming` replaced, freed by FinishAdopt
		std::atomic<bool> retiringFaulted{ false };
		std::atomic<bool> streamLock{ false };      // held by the stream for a block; by the main thread to switch
		std::atomic<bool> faulted{ false };
		std::string lastError;
		CreateProcessorFunction createProcessor = nullptr;
		DeleteProcessorFunction deleteProcessor = nullptr;
		InitProcessorFunction initProcessor = nullptr;
		SetMaxAudioBufferSizeFunction setMaxAudioBufferSize = nullptr;
		ProcessorFunction startServer = nullptr;
		ProcessFunction process = nullptr;
		HandleCommandFunction handleCommand = nullptr;
		ProcessorTextFunction dumpConfig = nullptr;
		ProcessorTextFunction dumpProgram = nullptr;
		ProcessorTextFunction getCurrentPreset = nullptr;
		ProcessorFlagFunction isPresetLoading = nullptr;
		ProcessorListFunction getPresets = nullptr;
		StringVectorSizeFunction getStringVectorSize = nullptr;
		StringVectorValueFunction getStringVectorValue = nullptr;
		bool isServerStarted = false;
		int frames = 0;
		std::vector<float> input;   // Stompbox takes a writable input buffer
		std::vector<float> incomingInput, incomingOutput;   // the stream's crossfade into an adopted processor
		std::vector<float> pumpInput, pumpOutput;           // main thread
		std::atomic<float> inputPeak{ 0.0f };
		std::atomic<float> outputPeak{ 0.0f };
		static inline std::atomic<float> inputGain{ 1.0f };
	};
}
