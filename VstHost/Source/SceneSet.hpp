#pragma once

// A song's tones, prepared before it plays: scene 0 is the Tone designer's editing engine (what plays for a game tone
// the song set does not assign), scenes 1.. are Stompbox engines of their own, each with one saved tone loaded.
// Only the active scene is processed. A switch fades the active scene out, then the new one in (about 10 ms each, so
// two amps never run on the guitar at once), and lets the outgoing scene ring out on silence for a while: its delay
// and reverb tails. The main thread bypasses that scene's NAM amps for the tail (they only cost time on silence) and
// turns them back on when the tail ends.
//
// Threads: the constructor, SetTarget, the queries and the destructor run on the main thread; Process on the stream
// thread. Every engine is prepared at the stream's block size before the set is built.

#include "BridgeAudio.hpp"
#include "StompboxEngine.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace VstHost
{
	class SceneSet final : public AudioSink
	{
	public:
		static constexpr int FADE_FRAMES = 480;          // 10 ms out, 10 ms in
		static constexpr int TAIL_FRAMES = 48000 * 2;    // how long an outgoing scene rings out

		// `editing` is not owned (the Tone designer's engine); `scenes` are. `frames`: the stream's block size.
		SceneSet(StompboxEngine& editing, std::vector<std::unique_ptr<StompboxEngine>> scenes, int frames);
		SceneSet(const SceneSet&) = delete;
		SceneSet& operator=(const SceneSet&) = delete;

		int Count() const { return static_cast<int>(scenes.size()) + 1; }
		StompboxEngine& Engine(int index);

		void SetTarget(int index);
		int Active() const { return active.load(std::memory_order_acquire); }
		int Tail() const { return tail.load(std::memory_order_acquire); }   // -1 when nothing rings out

		float TakeInputPeak() { return inputPeak.exchange(0.0f); }
		float TakeOutputPeak() { return outputPeak.exchange(0.0f); }

		// AudioSink, stream thread.
		void Process(const float* guitar, float* left, float* right, int frames) override;

	private:
		enum class Phase { Steady, FadingOut, FadingIn };

		StompboxEngine& editing;
		std::vector<std::unique_ptr<StompboxEngine>> scenes;
		std::atomic<int> target{ 0 };
		std::atomic<int> active{ 0 };
		std::atomic<int> tail{ -1 };
		Phase phase = Phase::Steady;
		float gain = 1.0f;
		int tailLeft = 0;
		int frames = 0;
		std::vector<float> silence, tailLeftChannel, tailRightChannel;
		std::atomic<float> inputPeak{ 0.0f };
		std::atomic<float> outputPeak{ 0.0f };
	};
}
