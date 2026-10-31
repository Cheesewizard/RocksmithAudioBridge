#pragma once

// The input noise gate. Header-only and standard library only, so the game DLL's input chain
// (InputCaptureHook.cpp) and the amp sim's feed (AmpFeedConditioner.hpp, also built into the bridge driver) run the
// same code.
//
// 2026-10-19 rewrite. The old version was a 6:1 downward expander keyed to the full-band RMS of the raw input. Full-
// band noise (interface hiss, ground-loop hum, buzz) sits at the same level as a note's tail, so any threshold high
// enough not to leak cut notes while they were still clearly audible. An offline harness (atlas
// tools/noise-gate-harness, write-up docs/investigations/noise-gate-harness-2026-10-19.md) measured that on four
// real noise recordings: the old version cut notes 9-15 dB above the noise; this one lets them fade 0-17 dB further,
// with no more leaking between notes.
//
// What changed, both standard practice on guitar gates:
//  1. Filtered detector. The detector (never the audio) is band-passed to the guitar's fundamental range, 70 Hz to
//     1.5 kHz, so hiss above and the mains fundamental below count far less. Peak follower, 0.5 ms attack, 20 ms
//     release.
//  2. Gate with hysteresis, hold and a soft close. It opens at the threshold, stays open (plus a 60 ms hold) until the
//     detector falls 6 dB under it, then fades linearly in dB to the reduction floor over the decay time, so the
//     decay shown is the real fade time (the old exponential on linear gain took ~1.7 s to reach its floor).
// 2026-10-22: decay, reduction and attack became the Noise gate pedal's dials (overlay pop-out and settings app,
// control op 31); defaults are the harness's 200 ms, -60 dB, 0.5 ms.
// Nothing is delayed; the gain is computed per sample from the current input.
//
// Considered and left out: a pitch hold (stay open while the string is still periodic) kept 4-10 dB more sustain on
// synthetic notes but stayed open on real ground-loop hum and buzz; it lives in the harness. Machine-learning
// denoisers are speech-trained and add 10-40 ms of latency.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Audio::AsioHook::Detail
{
	// The pedal's other three dials (the threshold travels separately, linear, 0 = off). Defaults = the harness's
	// tested values. Decay = the fade from fully open down to the reduction floor once the gate closes; attack = the
	// fade back up when it opens. Both linear in dB, so the time shown is the time it takes.
	struct NoiseGateShape
	{
		float decayMs = 200.0f;      // 50..2000
		float reductionDb = -60.0f;  // -80..-10: how far down it turns the signal while closed
		float attackMs = 0.5f;       // 0.1..20
		bool operator==(const NoiseGateShape& other) const
		{
			return decayMs == other.decayMs && reductionDb == other.reductionDb && attackMs == other.attackMs;
		}
		NoiseGateShape Clamped() const
		{
			NoiseGateShape s;
			s.decayMs = std::isfinite(decayMs) ? (std::clamp)(decayMs, 50.0f, 2000.0f) : 200.0f;
			s.reductionDb = std::isfinite(reductionDb) ? (std::clamp)(reductionDb, -80.0f, -10.0f) : -60.0f;
			s.attackMs = std::isfinite(attackMs) ? (std::clamp)(attackMs, 0.1f, 20.0f) : 0.5f;
			return s;
		}
	};

	class NoiseSuppressor final
	{
	public:
		// threshold: linear open level of the filtered detector (the overlay / Settings value in dB, converted).
		void Configure(uint32_t sampleRate, float threshold, const NoiseGateShape& requestedShape = {})
		{
			if (sampleRate == 0) return;
			const NoiseGateShape shape = requestedShape.Clamped();
			if (sampleRate == configuredRate && threshold == configuredThreshold && shape == configuredShape) return;

			const bool rateChanged = sampleRate != configuredRate;
			configuredRate = sampleRate;
			configuredThreshold = threshold;
			configuredShape = shape;
			const double fs = static_cast<double>(sampleRate);
			openLevel = threshold;
			closeLevel = threshold * std::pow(10.0f, -HYSTERESIS_DB / 20.0f);
			rangeGain = std::pow(10.0f, shape.reductionDb / 20.0f);
			holdSamples = static_cast<uint32_t>(HOLD_SECONDS * fs);
			// Linear-in-dB ramps as a per-sample multiply: no log/pow in the sample loop.
			attackFactor = static_cast<float>(std::exp(-std::log(static_cast<double>(rangeGain)) / (std::max)(1.0, shape.attackMs * 0.001 * fs)));
			releaseFactor = static_cast<float>(std::exp(std::log(static_cast<double>(rangeGain)) / (std::max)(1.0, shape.decayMs * 0.001 * fs)));
			if (appliedGain < rangeGain) appliedGain = rangeGain;   // reduction raised while closed: rise to the new floor
			envAttack = static_cast<float>(1.0 - std::exp(-1.0 / (DETECTOR_ATTACK_SECONDS * fs)));
			envRelease = static_cast<float>(1.0 - std::exp(-1.0 / (DETECTOR_RELEASE_SECONDS * fs)));
			if (rateChanged)
			{
				SetHighpass(highpass, fs, DETECTOR_LOW_HZ, 0.70710678);
				SetLowpass(lowpass1, fs, DETECTOR_HIGH_HZ, 0.54119610);   // 4th-order Butterworth = two sections
				SetLowpass(lowpass2, fs, DETECTOR_HIGH_HZ, 1.30656296);
				Reset();
			}
		}

		float NextGain(float sidechain, float threshold)
		{
			if (!std::isfinite(sidechain) || threshold <= 0.0f) return 1.0f;
			float detector = highpass.Run(sidechain);
			detector = lowpass1.Run(detector);
			detector = lowpass2.Run(detector);
			const float magnitude = std::fabs(detector);
			envelope += (magnitude - envelope) * (magnitude > envelope ? envAttack : envRelease);

			if (!isOpen)
			{
				if (envelope >= openLevel) { isOpen = true; holdLeft = holdSamples; }
			}
			else if (envelope >= closeLevel)
				holdLeft = holdSamples;
			else if (holdLeft > 0)
				--holdLeft;
			else
				isOpen = false;

			if (isOpen) appliedGain = (std::min)(1.0f, appliedGain * attackFactor);
			else appliedGain = (std::max)(rangeGain, appliedGain * releaseFactor);
			return appliedGain;
		}

		void Process(float* samples, size_t sampleCount, uint32_t sampleRate, float threshold)
		{
			if (samples == nullptr || sampleCount == 0 || threshold <= 0.0f) return;
			Configure(sampleRate, threshold);
			for (size_t index = 0; index < sampleCount; ++index)
				samples[index] *= NextGain(samples[index], threshold);
		}

		// For the pedal's meter and light: the filtered detector's level (dBFS) and whether the gate is open.
		float DetectorDb() const { return 20.0f * std::log10((std::max)(envelope, 1e-6f)); }
		bool IsOpen() const { return isOpen; }

		void Reset()
		{
			configuredThreshold = -1.0f;
			highpass.Clear(); lowpass1.Clear(); lowpass2.Clear();
			envelope = 0.0f;
			appliedGain = rangeGain;
			holdLeft = 0;
			isOpen = false;
		}

	private:
		struct Biquad
		{
			float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
			float Run(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
			void Clear() { z1 = z2 = 0.0f; }
		};

		static void SetLowpass(Biquad& b, double fs, double hz, double q)
		{
			const double w = 2.0 * 3.14159265358979 * hz / fs, c = std::cos(w), alpha = std::sin(w) / (2.0 * q), a0 = 1.0 + alpha;
			b.b0 = static_cast<float>((1.0 - c) / 2.0 / a0); b.b1 = static_cast<float>((1.0 - c) / a0); b.b2 = b.b0;
			b.a1 = static_cast<float>(-2.0 * c / a0); b.a2 = static_cast<float>((1.0 - alpha) / a0);
		}

		static void SetHighpass(Biquad& b, double fs, double hz, double q)
		{
			const double w = 2.0 * 3.14159265358979 * hz / fs, c = std::cos(w), alpha = std::sin(w) / (2.0 * q), a0 = 1.0 + alpha;
			b.b0 = static_cast<float>((1.0 + c) / 2.0 / a0); b.b1 = static_cast<float>(-(1.0 + c) / a0); b.b2 = b.b0;
			b.a1 = static_cast<float>(-2.0 * c / a0); b.a2 = static_cast<float>((1.0 - alpha) / a0);
		}

		// Fixed, tested in the harness ("level-only" variant). Decay, reduction and attack are the pedal's dials
		// (NoiseGateShape).
		static constexpr double DETECTOR_LOW_HZ = 70.0;
		static constexpr double DETECTOR_HIGH_HZ = 1500.0;
		static constexpr double DETECTOR_ATTACK_SECONDS = 0.0005;
		static constexpr double DETECTOR_RELEASE_SECONDS = 0.020;
		static constexpr float HYSTERESIS_DB = 6.0f;
		static constexpr double HOLD_SECONDS = 0.060;

		Biquad highpass, lowpass1, lowpass2;
		float envelope = 0.0f;
		float envAttack = 0.0f;
		float envRelease = 0.0f;
		float openLevel = 0.001f;
		float closeLevel = 0.0005f;
		float rangeGain = 0.001f;
		float appliedGain = 0.001f;
		float attackFactor = 1.0f;
		float releaseFactor = 1.0f;
		float configuredThreshold = -1.0f;
		NoiseGateShape configuredShape;
		uint32_t holdSamples = 0;
		uint32_t holdLeft = 0;
		uint32_t configuredRate = 0;
		bool isOpen = false;
	};
}
