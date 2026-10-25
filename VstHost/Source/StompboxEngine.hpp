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

		HMODULE module = nullptr;
		void* processor = nullptr;
		CreateProcessorFunction createProcessor = nullptr;
		DeleteProcessorFunction deleteProcessor = nullptr;
		InitProcessorFunction initProcessor = nullptr;
		SetMaxAudioBufferSizeFunction setMaxAudioBufferSize = nullptr;
		StartServerFunction startServer = nullptr;
		ProcessFunction process = nullptr;
		HandleCommandFunction handleCommand = nullptr;
		bool isServerStarted = false;
		int frames = 0;
		std::vector<float> input;   // Stompbox takes a writable input buffer
		std::atomic<float> inputPeak{ 0.0f };
		std::atomic<float> outputPeak{ 0.0f };
	};
}
