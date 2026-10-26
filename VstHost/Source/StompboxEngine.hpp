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

		void SendCommand(const std::string& command);

		// Every pedal Stompbox offers with its parameters, in Stompbox's own protocol (PluginConfig, ParameterConfig,
		// ParameterEnumValues, ParameterFileTree, EndConfig lines). Exported by the build script's patch.
		std::string DumpConfig();
		// The current rig as Stompbox preset text (SetPreset, SetChain and SetParam lines): what a saved tone stores.
		std::string DumpProgram();
		std::vector<std::string> ListPresets();
		std::string CurrentPreset();
		bool IsPresetLoading();

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

		HMODULE module = nullptr;
		void* processor = nullptr;
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
		std::vector<float> pumpOutput;
		std::atomic<float> inputPeak{ 0.0f };
		std::atomic<float> outputPeak{ 0.0f };
		static inline std::atomic<float> inputGain{ 1.0f };
	};
}
