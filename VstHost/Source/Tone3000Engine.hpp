#pragma once

// The TONE3000 engine (github.com/tone-3000/tone3000-plugin, MIT; VstHost/Tone3000/UPSTREAM.md), compiled into the host
// from VstHost/Lib/Tone3000: its whole audio processor (NAM through NeuralAmpModelerCore, full-length cabinet IRs,
// per-block EQ and normalisation, gate, Spread stereo), without its interface or any network use.
//
// Phase 1 of docs/ToneDesigner/EngineSwap-TONE3000-Plan.md: the Tone designer page still edits a Stompbox tone, and
// the host plays that tone's amp capture and cabinet through this engine instead (StompboxService::ApplyToTone3000).
// HKCU\Software\RSMods\VstHost Tone3000Engine = 0 switches back to Stompbox.
//
// Threads: construction, Prepare and Apply on the main thread (the JUCE message thread), Prepare only while the
// stream does not run this engine; Process on the stream thread. Loading a capture or an IR is the processor's own
// background work, with its own fades, so Apply may run while it plays.
#include "BridgeAudio.hpp"
#include <atomic>
#include <memory>
#include <string>

class TONE3000Processor;

namespace VstHost
{
	class Tone3000Engine final : public AudioSink
	{
	public:
		Tone3000Engine();
		~Tone3000Engine() override;
		Tone3000Engine(const Tone3000Engine&) = delete;
		Tone3000Engine& operator=(const Tone3000Engine&) = delete;

		// The stream's block size, at 48 kHz.
		void Prepare(int frames);
		// The rig: a capture (.nam) and a cabinet IR (.wav); either may be empty. Loads only what changed.
		void Apply(const std::wstring& capture, const std::wstring& cabinet);
		// Spread: the TONE3000 stereo image from one mono chain (an ADT-style double). On by default.
		void SetSpread(bool on);

		float TakeInputPeak() { return inputPeak.exchange(0.0f); }
		float TakeOutputPeak() { return outputPeak.exchange(0.0f); }
		// The processor threw on the stream: the guitar plays dry until the host restarts.
		bool HasFaulted() const { return faulted.load(std::memory_order_acquire); }

		// AudioSink, stream thread.
		void Process(const float* guitar, float* left, float* right, int frames) override;

	private:
		void RemoveBlocks();

		std::unique_ptr<TONE3000Processor> processor;
		int preparedFrames = 0;
		std::wstring capturePath, cabinetPath;
		std::atomic<bool> faulted{ false };
		std::atomic<float> inputPeak{ 0.0f };
		std::atomic<float> outputPeak{ 0.0f };
	};
}
