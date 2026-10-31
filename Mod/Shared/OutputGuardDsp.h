#pragma once

// Output loudness guard: a slow loudness AGC (equalises perceived loudness between songs) followed by
// a fast look-ahead brickwall limiter (a real ceiling that pins transients, for hearing safety). The
// limiter drives its gain from an estimated true peak (the inter-sample reconstructed peak, not just the
// sample values) so the ceiling still holds after the DAC. Shared by the proxy (which runs it on the game
// output before the real driver plays it) and an offline test. Pure and header-only so it stays
// unit-testable with synthetic signals, no game or hardware.
//
// Layout matches the proxy: audio is planar (one buffer per channel). Per output buffer the caller:
//   1) computes each channel's mean-square and calls BeginBlock(...) once  -> advances the shared AGC gain
//   2) calls ProcessChannel(ch, x, frames) for each channel               -> applies AGC gain then limits
// Both stages are individually toggleable. With both off the signal is bit-for-bit unchanged and no
// latency is added. With the limiter on, output is delayed by the look-ahead length (a few ms); the
// AGC alone adds no latency.
#include <vector>
#include <cmath>

namespace OutputGuardDsp
{
	struct Config
	{
		float sampleRate = 48000.0f;

		// Limiter (safety ceiling). Off by default = unity passthrough, no latency.
		bool  limiterOn = false;
		float ceilingLin = 1.0f;    // linear peak ceiling in [0,1]; nothing leaves above this
		float lookaheadMs = 3.0f;   // look-ahead so the gain is down before the peak arrives
		float releaseMs = 120.0f;   // how fast gain recovers after a transient
		// True-peak detection: drive the gain from the estimated inter-sample (reconstructed) peak, not just
		// the sample values, so the ceiling still holds after the DAC rebuilds the analog waveform (a signal
		// whose samples all sit under the ceiling can still overshoot it between samples: see ITU-R BS.1770
		// dBTP). Adds a small per-sample oversampling cost while the limiter is on. On by default; when off the
		// limiter falls back to sample-peak only.
		bool  truePeak = true;

		// Loudness AGC (song-to-song equalisation). Off by default = no gain change, no latency.
		bool  agcOn = false;
		float targetRms = 0.10f;    // aim perceived loudness (RMS) here; keep a few dB below the ceiling
		float agcMaxBoostDb = 6.0f; // never boost more than this (keeps quiet passages from getting loud)
		float agcMaxCutDb = 18.0f;  // may cut this much to tame a hot song
		float agcTimeMs = 1500.0f;  // slow: equalise between songs, not within one
		float agcFloorRms = 0.005f; // below this the signal is treated as silence/noise: hold gain, no boost
	};

	class OutputGuard
	{
	public:
		void Configure(const Config& c)
		{
			m_cfg = c;
			const float sr = c.sampleRate > 0.0f ? c.sampleRate : 48000.0f;

			m_look = static_cast<int>(c.lookaheadMs * 0.001f * sr + 0.5f);
			if (m_look < 1) m_look = 1;
			// Attack reaches the target within the look-ahead window (~e^-4 residual over m_look samples).
			m_attack = 1.0f - std::exp(-4.0f / static_cast<float>(m_look));
			const float relSamples = c.releaseMs * 0.001f * sr;
			m_release = relSamples > 0.0f ? (1.0f - std::exp(-1.0f / relSamples)) : 1.0f;

			const float agcSamples = c.agcTimeMs * 0.001f * sr;
			m_agcBlockDecayPerSample = agcSamples > 0.0f ? (1.0f / agcSamples) : 1.0f;
			m_agcMinGain = std::pow(10.0f, -c.agcMaxCutDb / 20.0f);
			m_agcMaxGain = std::pow(10.0f, c.agcMaxBoostDb / 20.0f);

			BuildTruePeakFilter();
			ResizeChannels(static_cast<int>(m_chans.size()));
			ResetState();
		}

		// Drop all history: ring buffers to silence, gains to unity. Call on (re)start.
		void Reset(int channels)
		{
			ResizeChannels(channels);
			ResetState();
		}

		// Advance the shared AGC gain from this block's loudness. channelMeanSquare[i] is the mean of the
		// squared samples for channel i over `frames`; the block loudness is their average. Cheap: once per
		// buffer, not per sample.
		void BeginBlock(const double* channelMeanSquare, int channels, int frames)
		{
			if (channels > static_cast<int>(m_chans.size())) ResizeChannels(channels);
			if (!m_cfg.agcOn || channels <= 0 || frames <= 0) return;

			double agg = 0.0;
			for (int i = 0; i < channels; ++i) agg += channelMeanSquare[i];
			agg /= channels;

			// Smooth the mean-square toward this block over the AGC time constant.
			const double a = 1.0 - std::pow(1.0 - static_cast<double>(m_agcBlockDecayPerSample), frames);
			m_envMs += (agg - m_envMs) * a;

			const double rms = std::sqrt(m_envMs > 0.0 ? m_envMs : 0.0);
			double desired = m_agcGain;
			if (rms > m_cfg.agcFloorRms)
			{
				desired = m_cfg.targetRms / rms;
				if (desired < m_agcMinGain) desired = m_agcMinGain;
				if (desired > m_agcMaxGain) desired = m_agcMaxGain;
			}
			// Move the applied gain slowly toward the target (second smoothing = no zipper on the gain).
			m_agcGain += (desired - m_agcGain) * a;
		}

		// Live scalar updates safe to call from the audio thread: they only flip a flag or move a level,
		// never resize a buffer, so a slider drag or stage toggle needs no allocation and cannot race.
		// Timing params (look-ahead, release, AGC time constant) and channel count are fixed by Configure.
		void SetLimiterLive(bool on, float ceilingLin) { m_cfg.limiterOn = on; m_cfg.ceilingLin = ceilingLin; }
		void SetAgcLive(bool on, float targetRms) { m_cfg.agcOn = on; m_cfg.targetRms = targetRms; }

		float AgcGain() const { return static_cast<float>(m_agcGain); }
		int   LookaheadSamples() const { return m_look; }

		// Process one channel's planar buffer in place: apply the shared AGC gain, then the look-ahead
		// brickwall limiter. When the limiter is off there is no delay (zero added latency).
		void ProcessChannel(int ch, float* x, int frames)
		{
			if (ch < 0 || !x || frames <= 0) return;
			if (ch >= static_cast<int>(m_chans.size())) ResizeChannels(ch + 1);
			Chan& c = m_chans[ch];
			const bool agc = m_cfg.agcOn;
			const bool lim = m_cfg.limiterOn && m_cfg.ceilingLin > 0.0f;
			const bool tp = lim && m_cfg.truePeak;
			const float g = static_cast<float>(m_agcGain);
			const float ceil = m_cfg.ceilingLin;

			for (int i = 0; i < frames; ++i)
			{
				float s = x[i];
				if (agc) s *= g;

				if (!lim) { x[i] = s; continue; }

				// Delay line: emit the sample from m_look ago, store the incoming one.
				const float delayed = c.delay[c.widx];
				c.delay[c.widx] = s;
				c.widx = (c.widx + 1 == m_look) ? 0 : c.widx + 1;

				// Gain reacts to the incoming (future) sample so it is already down when that sample emerges.
				// With true-peak on, use the estimated inter-sample peak so the reconstructed analog peak (not
				// just this sample) is what the ceiling holds; otherwise fall back to the raw sample magnitude.
				const float mag = tp ? TruePeakMag(c, s) : (s < 0.0f ? -s : s);
				const float target = (mag > ceil) ? (ceil / mag) : 1.0f;
				c.gain += (target < c.gain ? m_attack : m_release) * (target - c.gain);

				float out = delayed * c.gain;
				if (out > ceil) out = ceil; else if (out < -ceil) out = -ceil; // hard backstop: guarantees the ceiling
				x[i] = out;
			}
		}

	private:
		struct Chan
		{
			std::vector<float> delay;   // look-ahead ring, m_look samples
			int widx = 0;
			float gain = 1.0f;          // current limiter gain, <= 1
			std::vector<float> tpHist;  // true-peak FIR history, kTpTaps samples
			int tpIdx = 0;              // next write slot in tpHist
		};

		// 4x oversampling estimate of the inter-sample (true) peak. A symmetric windowed-sinc polyphase filter
		// reconstructs the waveform at 4 points across one sample interval in the middle of the history, so the
		// interpolation has taps on both sides of the node (a one-sided/causal sinc badly undershoots peaks
		// near fs/4, the +3 dB worst case). That centering costs kTpTaps/2 samples of detection group delay,
		// far inside the look-ahead window, so the gain is still down before the matching output sample emerges.
		// Phase 0 is unity (the centre sample passes through), so a clean signal with no inter-sample overshoot
		// reads back at its own level and the limiter does not falsely engage. Cheap: kOversample * kTpTaps
		// taps per sample, only while limiting.
		static constexpr int kOversample = 4;
		static constexpr int kTpTaps = 16;   // even; the node sits at tap kTpTaps/2

		static float Sinc(float x)
		{
			if (x > -1e-6f && x < 1e-6f) return 1.0f;
			const float px = 3.14159265358979f * x;
			return std::sin(px) / px;
		}

		void BuildTruePeakFilter()
		{
			const float pi = 3.14159265358979f;
			const int mid = kTpTaps / 2;
			const float half = static_cast<float>(kTpTaps) / 2.0f;
			for (int p = 0; p < kOversample; ++p)
			{
				const float d = static_cast<float>(p) / static_cast<float>(kOversample); // node at mid + d
				float sum = 0.0f;
				for (int k = 0; k < kTpTaps; ++k)
				{
					const float dist = (static_cast<float>(mid) + d) - static_cast<float>(k); // node minus tap
					const float u = -dist / half; // symmetric window coordinate in [-1,1]
					float w = 0.0f;
					if (u > -1.0f && u < 1.0f)
						w = 0.42f + 0.5f * std::cos(pi * u) + 0.08f * std::cos(2.0f * pi * u); // Blackman
					const float coef = Sinc(dist) * w;
					m_tpCoef[p][k] = coef;
					sum += coef;
				}
				if (sum > 1e-6f) for (int k = 0; k < kTpTaps; ++k) m_tpCoef[p][k] /= sum; // unity DC gain
			}
		}

		// Push the incoming sample and return the estimated true peak: the largest of the newest sample and the
		// oversampled reconstruction across one interval mid-history. The mid-history interval means every
		// sample interval is evaluated kTpTaps/2 samples after it arrives (the detection group delay).
		float TruePeakMag(Chan& c, float s)
		{
			c.tpHist[c.tpIdx] = s;
			const int start = (c.tpIdx + 1 == kTpTaps) ? 0 : c.tpIdx + 1; // oldest of the kTpTaps history
			float tp = s < 0.0f ? -s : s;
			for (int p = 0; p < kOversample; ++p)
			{
				float acc = 0.0f;
				int idx = start;
				for (int k = 0; k < kTpTaps; ++k)
				{
					acc += m_tpCoef[p][k] * c.tpHist[idx];
					idx = (idx + 1 == kTpTaps) ? 0 : idx + 1;
				}
				const float a = acc < 0.0f ? -acc : acc;
				if (a > tp) tp = a;
			}
			c.tpIdx = (c.tpIdx + 1 == kTpTaps) ? 0 : c.tpIdx + 1;
			return tp;
		}

		void ResizeChannels(int channels)
		{
			if (channels < 0) channels = 0;
			m_chans.resize(channels);
			for (auto& c : m_chans)
			{
				c.delay.assign(m_look > 0 ? m_look : 1, 0.0f);
				c.tpHist.assign(kTpTaps, 0.0f);
				c.tpIdx = 0;
			}
		}

		void ResetState()
		{
			m_agcGain = 1.0;
			m_envMs = 0.0;
			for (auto& c : m_chans)
			{
				std::fill(c.delay.begin(), c.delay.end(), 0.0f);
				c.widx = 0;
				c.gain = 1.0f;
				std::fill(c.tpHist.begin(), c.tpHist.end(), 0.0f);
				c.tpIdx = 0;
			}
		}

		Config m_cfg;
		int   m_look = 144;
		float m_attack = 0.03f;
		float m_release = 0.0002f;
		float m_agcBlockDecayPerSample = 1.0f / 72000.0f;
		float m_agcMinGain = 0.125f;
		float m_agcMaxGain = 2.0f;

		double m_agcGain = 1.0;
		double m_envMs = 0.0;
		float m_tpCoef[kOversample][kTpTaps] = {}; // polyphase true-peak interpolation filter, built in Configure
		std::vector<Chan> m_chans;
	};
}
