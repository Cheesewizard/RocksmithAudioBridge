#pragma once

// The Stompbox engine (github.com/mikeoliphant/stompbox, GPL-3; see VstHost/Stompbox/UPSTREAM.md), loaded from
// stompbox-capi.dll next to the host. One fixed rig (input pedals, NAM amp, tone stack, effects loop, cabinet,
// output) driven by Stompbox's own text commands ("SetParam Amp Model <file>", "LoadPreset <name>"). Its presets are
// plain text files in its data folder, so the Tone designer owns every saved tone.
//
// Threads: Load, Prepare, SendCommand and StartRemoteServer run on the main thread, with the stream stopped for
// Prepare; Process runs on the stream thread. Stompbox swaps its chains itself, so commands may arrive while it plays.

#include "BridgeAudio.hpp"

#include <Windows.h>
#include <atomic>
#include <string>
#include <vector>

namespace VstHost
{
	// Stompbox's parameter kinds (StompBox.h PARAMETER_TYPE_*, same order).
	enum class StompParameterType { Knob, Bool, Int, VerticalSlider, Enum, File, Power };

	struct StompParameterInfo
	{
		std::string name;
		std::string description;
		std::string displayFormat;          // printf-style, e.g. "%.1f dB"; empty when Stompbox gives none
		StompParameterType type = StompParameterType::Knob;
		float minimum = 0.0f;
		float maximum = 1.0f;
		float defaultValue = 0.0f;
		float rangePower = 1.0f;            // knob taper
		std::vector<std::string> enumValues;   // Enum and File: the choices (for File, the files in its data folder)
		bool isAdvanced = false;
		bool isOutput = false;              // a meter the effect writes, not a control
	};

	// One kind of pedal the Tone designer browser offers (an entry of Stompbox's plugin factory).
	struct StompPluginInfo
	{
		std::string name;                   // the factory name ("Delay"); instances are "Delay", "Delay_2", ...
		std::string description;
		std::string backgroundColor;        // "#rrggbb" or empty
		std::string foregroundColor;
		std::vector<StompParameterInfo> parameters;
	};

	class StompboxEngine final : public AudioSink
	{
	public:
		StompboxEngine() = default;
		StompboxEngine(const StompboxEngine&) = delete;
		StompboxEngine& operator=(const StompboxEngine&) = delete;
		~StompboxEngine() override;

		// Loads stompbox-capi.dll from the host's folder and creates the engine on `dataPath` (models, cabinets and
		// presets live under it). False with `error` when the DLL or one of its functions is missing.
		bool Load(const std::wstring& dataPath, std::string& error);
		bool IsLoaded() const { return processor != nullptr; }

		// The stream's block size. Not real-time safe (Stompbox allocates for it): only with the stream stopped.
		void Prepare(int frames);

		void SendCommand(const std::string& command);

		// Main thread. The pedals players can add (Stompbox's user-selectable factory entries) with their parameters,
		// for the browser and the generic pedal editor.
		std::vector<StompPluginInfo> ListPlugins();
		// Replaces chain `chainName` with these plugin instances in order ("NoiseGate", "NAM", "Delay_2", ...).
		void SetChain(const std::string& chainName, const std::vector<std::string>& pluginIds);
		// The current rig as Stompbox preset text (its SetChain / SetParam lines): what a saved tone stores.
		std::string DumpProgram();
		std::vector<std::string> ListPresets();

		// Stompbox's own TCP server, for its desktop remote (StompboxRemote) during development. Bound to
		// 127.0.0.1 by the build script's patch, so nothing outside this PC can reach it.
		void StartRemoteServer();

		// Deletes the engine and releases the DLL (its server and preset threads with it). Main thread, after the stream
		// has stopped; the destructor does it too.
		void Unload();

		float TakeInputPeak() { return inputPeak.exchange(0.0f); }
		float TakeOutputPeak() { return outputPeak.exchange(0.0f); }

		// AudioSink, stream thread. Stompbox is mono: its output goes to both sides.
		void Process(const float* guitar, float* left, float* right, int frames) override;

	private:
		using CreateProcessorFunction = void* (*)(const wchar_t* dataPath);
		using DeleteProcessorFunction = void (*)(void* processor);
		using InitProcessorFunction = void (*)(void* processor, float sampleRate);
		using SetMaxAudioBufferSizeFunction = void (*)(void* processor, size_t numSamples);
		using StartServerFunction = void (*)(void* processor);
		using ProcessFunction = void (*)(void* processor, float* input, float* output, size_t bufferSize);
		using HandleCommandFunction = void (*)(void* processor, const char* command);
		using GetStringVectorSizeFunction = size_t (*)(void* strings);
		using GetStringVectorValueFunction = const char* (*)(void* strings, size_t index);
		using GetListFunction = void* (*)(void* processor);
		using CreatePluginFunction = void* (*)(void* processor, const char* id);
		using DumpProgramFunction = const char* (*)(void* processor);
		using PluginTextFunction = const char* (*)(void* plugin);
		using PluginFlagFunction = bool (*)(void* plugin);
		using PluginCountFunction = size_t (*)(void* plugin);
		using PluginParameterFunction = void* (*)(void* plugin, size_t index);
		using ParameterTextFunction = const char* (*)(void* parameter);
		using ParameterFloatFunction = float (*)(void* parameter);
		using ParameterIntFunction = int (*)(void* parameter);
		using ParameterFlagFunction = bool (*)(void* parameter);
		using ParameterListFunction = void* (*)(void* parameter);

		std::vector<std::string> ReadStrings(void* strings) const;
		StompParameterInfo ReadParameter(void* parameter) const;

		HMODULE module = nullptr;
		void* processor = nullptr;
		CreateProcessorFunction createProcessor = nullptr;
		DeleteProcessorFunction deleteProcessor = nullptr;
		InitProcessorFunction initProcessor = nullptr;
		SetMaxAudioBufferSizeFunction setMaxAudioBufferSize = nullptr;
		StartServerFunction startServer = nullptr;
		ProcessFunction process = nullptr;
		HandleCommandFunction handleCommand = nullptr;
		GetStringVectorSizeFunction getStringVectorSize = nullptr;
		GetStringVectorValueFunction getStringVectorValue = nullptr;
		GetListFunction getAllPlugins = nullptr;
		GetListFunction getPresets = nullptr;
		CreatePluginFunction createPlugin = nullptr;
		DumpProgramFunction dumpProgram = nullptr;
		PluginTextFunction getPluginDescription = nullptr;
		PluginTextFunction getPluginBackgroundColor = nullptr;
		PluginTextFunction getPluginForegroundColor = nullptr;
		PluginFlagFunction getPluginIsUserSelectable = nullptr;
		PluginCountFunction getPluginNumParameters = nullptr;
		PluginParameterFunction getPluginParameter = nullptr;
		ParameterTextFunction getParameterName = nullptr;
		ParameterTextFunction getParameterDescription = nullptr;
		ParameterTextFunction getParameterDisplayFormat = nullptr;
		ParameterFloatFunction getParameterMinValue = nullptr;
		ParameterFloatFunction getParameterMaxValue = nullptr;
		ParameterFloatFunction getParameterDefaultValue = nullptr;
		ParameterFloatFunction getParameterRangePower = nullptr;
		ParameterIntFunction getParameterType = nullptr;
		ParameterListFunction getParameterEnumValues = nullptr;
		ParameterFlagFunction getParameterIsAdvanced = nullptr;
		ParameterFlagFunction getParameterIsOutput = nullptr;
		bool isServerStarted = false;
		int frames = 0;
		std::vector<float> input;   // Stompbox takes a writable input buffer
		std::atomic<float> inputPeak{ 0.0f };
		std::atomic<float> outputPeak{ 0.0f };
	};
}
