#pragma once

// The amp sim's input conditioner: the Input page's hum filter and noise suppressor, applied to the guitar the
// Tone designer / an external amp sim gets.
//
// Why: the amp link feeds the amp sim the raw DI from before the game's input chain (AmpLink.hpp), so the hum filter
// and noise suppressor never reached it. On a clean input (the Real Tone Cable) that did not matter; on an interface
// with mains hum or a hissy input (an M-Track on a ground loop) a high-gain amp sim turned that floor into loud static
// while the game's own amp, behind the conditioned chain and the game's gate, stayed silent (2026-10-17).
//
// Used by AmpLink::Host::Publish, which both amp link hosts run through: the bridge driver's (ASIO) and the game
// DLL's (CableAmpLink, Real Tone Cable). The same settings drive both, pushed by ExternalAmp.cpp every tick from the
// game's input chain (AsioHook::GetHumFilterBaseHz / GetNoiseGateThresholdDb), so one switch means the same thing
// everywhere. The input chain's order is kept: hum filter, then the noise suppressor keyed to the de-hummed signal.
//
// The hum filter measures the hum itself from quiet stretches of this feed (its own HumRemover and analysis thread):
// the feed is the same raw signal the game's chain sees, so both lock onto the same lines, and nothing has to cross
// between the game DLL and the bridge driver but two numbers. Header-only, standard library only.
//
// Threading: Configure from any thread; Process on the publishing (audio or capture) thread only.

#include "HumRemover.hpp"
#include "NoiseSuppressor.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace Audio
{
	class AmpFeedConditioner
	{
	public:
		static constexpr size_t kChannels = 2;

		// humBaseHz: 0 = off, else the mains frequency (50 / 60). gateThreshold: linear, 0 = off.
		void Configure(float humBaseHz, float gateThreshold)
		{
			m_humBaseHz.store(humBaseHz >= 20.0f ? humBaseHz : 0.0f, std::memory_order_relaxed);
			m_gateThreshold.store(gateThreshold > 0.0f ? gateThreshold : 0.0f, std::memory_order_relaxed);
			if (humBaseHz >= 20.0f && !m_workerStarted.exchange(true))
				// Lives as long as the process: the conditioner is never freed once created (AmpLink::Host keeps it).
				std::thread([this] { for (;;) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); m_hum.Analyze(); } }).detach();
		}

		bool Enabled() const
		{
			return m_humBaseHz.load(std::memory_order_relaxed) > 0.0f || m_gateThreshold.load(std::memory_order_relaxed) > 0.0f;
		}

		// Int32LSB channel buffers, in place. `count` up to kChannels; a null channel is skipped.
		void ProcessInt32(int32_t* const* channels, size_t count, uint32_t frames, uint32_t sampleRate) noexcept
		{
			const float humHz = m_humBaseHz.load(std::memory_order_relaxed);
			const float gate = m_gateThreshold.load(std::memory_order_relaxed);
			if ((humHz <= 0.0f && gate <= 0.0f) || frames == 0) return;
			count = count < kChannels ? count : kChannels;
			if (humHz > 0.0f) m_hum.BeginBlock(sampleRate, humHz, count);
			for (size_t ch = 0; ch < count; ++ch)
			{
				int32_t* samples = channels[ch];
				if (!samples) continue;
				if (gate > 0.0f) m_gate[ch].Configure(sampleRate, gate);
				for (uint32_t i = 0; i < frames; ++i)
				{
					float x = static_cast<float>(samples[i]) * (1.0f / 2147483648.0f);
					if (humHz > 0.0f) x = m_hum.Process(ch, x);
					if (gate > 0.0f) x *= m_gate[ch].NextGain(x, gate);
					x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
					samples[i] = static_cast<int32_t>(x * 2147483520.0f);
				}
			}
			if (humHz > 0.0f) m_hum.EndBlock();
		}

	private:
		HumRemover m_hum;
		AsioHook::Detail::NoiseSuppressor m_gate[kChannels];
		std::atomic<float> m_humBaseHz{ 0.0f };
		std::atomic<float> m_gateThreshold{ 0.0f };
		std::atomic<bool> m_workerStarted{ false };
	};
}
