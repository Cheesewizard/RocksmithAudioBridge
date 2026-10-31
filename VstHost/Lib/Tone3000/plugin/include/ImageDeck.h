#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>

/**
 * Shared DSP primitives for the two stereo-image engines: Spread (mono
 * chain mode, Spread.h) and Align (stereo chain mode, StereoOffset.h). The
 * features are independent (separate parameters, separate lifecycles), but
 * their advanced "deck" sections are deliberately the same circuit so the
 * two faces of the stereo-image slot sound and read alike: a wobbling
 * delay, a crossover that keeps lows out of the treatment, and an allpass
 * diffusion cascade. Design notes in plugin/docs/stereo-image.md.
 *
 * Everything here is audio-thread only (the correlation atomic is read by
 * the UI thread) and allocation-free.
 */

/** Crossover knob log map, mirrored by the UI scale (crossoverHzScale):
    32.5-520 Hz with the 130 Hz default exactly at center
    (32.5 * 16^0.5 = 130; 4x per half turn). */
constexpr float kDeckCrossoverMinHz = 32.5f;
constexpr float kDeckCrossoverMaxHz = 520.0f;
inline float deckCrossoverHz(float norm) {
  return kDeckCrossoverMinHz * std::pow(kDeckCrossoverMaxHz / kDeckCrossoverMinHz,
                                        juce::jlimit(0.0f, 1.0f, norm));
}

/** Wobble depth span: 100% = ±1.2 ms of slow drift around the dialed delay
    (≈ ±2-4 cents of continuous pitch wander; pitch shift is the derivative
    of delay time). Absolute, not relative to the delay, so a small offset
    can still carry a full-depth wobble. */
constexpr float kDeckWobbleMaxMs = 1.2f;

/** Section engage/bypass blend time. The deck switches are ~25 ms blends,
    not hard toggles: both endpoints are magnitude-flat but differ in phase,
    so an instant switch would step the waveform. */
constexpr double kDeckFadeSeconds = 0.025;

/** First-order allpass (transposed direct form II, one state). Static
    coefficients: movement comes from the delay wobble; modulating allpass
    coefficients would reintroduce phasiness. */
struct DeckAllpass {
  float a = 0.0f;
  float z = 0.0f;
  float process(float x) noexcept {
    const float v = x - a * z;
    const float y = a * v + z;
    z = v;
    return y;
  }
};

/** Six-stage phase-diffusion cascade, corner frequencies log-spaced over
    300 Hz - 6 kHz. Decorrelates phase without touching magnitude, the same
    principle as the allpass decorrelators evaluated in O. Das, "An
    Open-Source Stereo Widening Plugin", Proc. 27th Int. Conf. on Digital
    Audio Effects (DAFx24), Guildford, UK, 2024
    (https://www.dafx.de/paper-archive/2024/papers/DAFx24_paper_92.pdf). */
struct DeckDiffuser {
  static constexpr int kNumStages = 6;
  static constexpr double kLowHz = 300.0;
  static constexpr double kHighHz = 6000.0;

  void prepare(double sampleRate) {
    for (int i = 0; i < kNumStages; ++i) {
      const double fc =
          kLowHz * std::pow(kHighHz / kLowHz, static_cast<double>(i) / (kNumStages - 1));
      // Corners above Nyquist (hosts below ~12 kHz) put tan() past π/2 and
      // the allpass coefficient outside |a| < 1: unstable. Pin just under
      // Nyquist; the top stages collapse toward a=0 (transparent) there.
      const double fcSafe = juce::jmin(fc, sampleRate * 0.49);
      const double t = std::tan(juce::MathConstants<double>::pi * fcSafe / sampleRate);
      stages[static_cast<size_t>(i)].a = static_cast<float>((t - 1.0) / (t + 1.0));
    }
  }
  void reset() {
    for (auto& stage : stages)
      stage.z = 0.0f;
  }
  float process(float x) noexcept {
    for (auto& stage : stages)
      x = stage.process(x);
    return x;
  }

  std::array<DeckAllpass, kNumStages> stages;
};

/** Random-walk wobble source: white noise through two cascaded 0.3 Hz
    one-poles. One pole is not enough: its 6 dB/oct tail leaves ~1% of the
    noise variance above 20 Hz, and audio-rate delay-time noise FMs the
    delayed channel into broadband fizz (regression covered by
    SpreadTest.WobbleAddsNoBroadbandFizz).

    next() returns the normalized walk in -1..1; callers scale it by
    kDeckWobbleMaxMs and their depth. The normalization is analytic: the
    shaper's impulse response is h[n] = k²(n+1)aⁿ with a = 1-k, so the
    steady-state output variance for unit-variance input is
    Σh² = k⁴(1+a²)/(1-a²)³. Uniform [-1,1] noise has σ² = 1/3; scale so 3σ
    reaches the ±1 clamp. Only then does a depth knob actually span the
    full ±kDeckWobbleMaxMs at any sample rate. */
struct DeckWobble {
  static constexpr double kRateHz = 0.3;

  void prepare(double sampleRate) {
    coeff = 1.0f - std::exp(static_cast<float>(
        -juce::MathConstants<double>::twoPi * kRateHz / sampleRate));
    const double k = coeff, a = 1.0 - k;
    const double gainSq = k * k * k * k * (1.0 + a * a) /
                          ((1.0 - a * a) * (1.0 - a * a) * (1.0 - a * a));
    const double sigma = std::sqrt(gainSq / 3.0);
    norm = static_cast<float>(1.0 / (3.0 * sigma));
  }
  void reset() { state1 = state2 = 0.0f; }
  float next() noexcept {
    state1 += coeff * (random.nextFloat() * 2.0f - 1.0f - state1);
    state2 += coeff * (state1 - state2);
    return juce::jlimit(-1.0f, 1.0f, state2 * norm);
  }

  juce::Random random;
  float state1{0.0f}, state2{0.0f};
  float coeff{0.0f};
  float norm{1.0f};
};

/** ~300 ms running normalized L/R output correlation, published through an
    atomic for the UI mono-safety meter. Normalized correlation is invariant
    to the per-channel balance gains applied downstream, so measuring at the
    engine equals measuring at the bus. Reports 1 when idle or on silence
    (mean-square floor ~-100 dBFS): silence is trivially mono-compatible,
    not "decorrelated". */
struct DeckCorrelation {
  static constexpr double kSeconds = 0.3;
  static constexpr float kFloor = 1.0e-10f;

  void prepare(double newSampleRate) {
    sampleRate = newSampleRate;
    reset();
  }
  void reset() {
    lr = ll = rr = 0.0f;
    out.store(1.0f, std::memory_order_relaxed);
  }
  /** Folds one block's raw sums of L·R, L², R² into the one-pole followers
      and publishes the normalized value. Audio thread. */
  void update(float sumLR, float sumLL, float sumRR, int numSamples) {
    const float norm = 1.0f / static_cast<float>(numSamples);
    const float coeff =
        1.0f - std::exp(static_cast<float>(-numSamples / (kSeconds * sampleRate)));
    lr += coeff * (sumLR * norm - lr);
    ll += coeff * (sumLL * norm - ll);
    rr += coeff * (sumRR * norm - rr);
    const float energy = ll * rr;
    out.store(energy > kFloor * kFloor ? juce::jlimit(-1.0f, 1.0f, lr / std::sqrt(energy))
                                       : 1.0f,
              std::memory_order_relaxed);
  }
  /** Latest value (-1..1). Readable from any thread. */
  float value() const { return out.load(std::memory_order_relaxed); }

  float lr{0.0f}, ll{0.0f}, rr{0.0f};
  std::atomic<float> out{1.0f};
  double sampleRate{48000.0};
};
