#pragma once

// The amp sim's input conditioner: the Input page's noise suppressor, applied to the guitar the Tone designer / an
// external amp sim gets.
//
// Why: the amp link feeds the amp sim the raw DI from before the game's input chain (AmpLink.hpp), so the noise
// suppressor never reached it. On a clean input (the Real Tone Cable) that did not matter; on an interface with a hissy
// input or mains hum (an M-Track on a ground loop) a high-gain amp sim turned that floor into loud static while the
// game's own amp, behind the conditioned chain and the game's gate, stayed silent.
//
// Used by AmpLink::Host::Publish, which both amp link hosts run through: the bridge driver's (ASIO) and the game
// DLL's (CableAmpLink, Real Tone Cable). The same setting drives both, pushed by ExternalAmp.cpp every tick from the
// game's input chain (AsioHook::GetNoiseGateThresholdDb), so one switch means the same thing everywhere.
// Header-only, standard library only.
//
// Threading: Configure from any thread; Process on the publishing (audio or capture) thread only.

#include "NoiseSuppressor.hpp"
#include <atomic>
#include <cstdint>

namespace Audio
{
	class AmpFeedConditioner
	{
	public:
		static constexpr size_t kChannels = 2;

		// gateThreshold: the noise suppressor's threshold, linear, 0 = off.
		void Configure(float gateThreshold)
		{
			m_gateThreshold.store(gateThreshold > 0.0f ? gateThreshold : 0.0f, std::memory_order_relaxed);
		}

		bool Enabled() const { return m_gateThreshold.load(std::memory_order_relaxed) > 0.0f; }

		// Int32LSB channel buffers, in place. `count` up to kChannels; a null channel is skipped.
		void ProcessInt32(int32_t* const* channels, size_t count, uint32_t frames, uint32_t sampleRate) noexcept
		{
			const float gate = m_gateThreshold.load(std::memory_order_relaxed);
			if (gate <= 0.0f || frames == 0) return;
			count = count < kChannels ? count : kChannels;
			for (size_t ch = 0; ch < count; ++ch)
			{
				int32_t* samples = channels[ch];
				if (!samples) continue;
				m_gate[ch].Configure(sampleRate, gate);
				for (uint32_t i = 0; i < frames; ++i)
				{
					float x = static_cast<float>(samples[i]) * (1.0f / 2147483648.0f);
					x *= m_gate[ch].NextGain(x, gate);
					x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
					samples[i] = static_cast<int32_t>(x * 2147483520.0f);
				}
			}
		}

	private:
		AsioHook::Detail::NoiseSuppressor m_gate[kChannels];
		std::atomic<float> m_gateThreshold{ 0.0f };
	};
}
