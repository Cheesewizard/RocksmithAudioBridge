#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cmath>

#include "ImageDeck.h"

/**
 * Spread: a mono-to-stereo ADT-style doubler (mono chain mode only; see
 * plugin/docs/stereo-image.md for the design overview).
 *
 * The mono chain output is split by a Linkwitz-Riley 4th-order crossover
 * (default 130 Hz, adjustable 32.5-520 Hz, bypassable). The low band feeds
 * both channels untouched, mono-safe by construction. The high band goes
 * dry to one channel and through the "lag deck", a wobbling fractional
 * delay (4-point Lagrange, mandatory: linear interpolation under a
 * time-varying fractional offset low-passes in rhythm with the wobble) plus
 * a phase-diffusion cascade (bypassable) and a +1.5 dB precedence trim, to
 * the other. The wobble (random-walk modulation of the delay time) is what
 * makes the lag read as a second human performance, and it keeps the
 * mono-sum comb moving so fold-down reads as gentle chorus, never a
 * stationary notch.
 *
 * The deck sections (wobble source, diffuser, crossover map, correlation
 * meter) are the shared stereo-image primitives in ImageDeck.h; Align
 * (StereoOffset.h) mounts the same circuit in stereo chain mode. The two
 * features stay independent (separate parameters and lifecycles); they
 * just sound and read alike on purpose.
 *
 * The crossover and diffuse switches are ~25 ms blends, not hard toggles:
 * both endpoints are magnitude-flat but differ in phase, so an instant
 * switch would step the waveform. The bypassed filters keep running on the
 * signal, so re-engaging blends into warm state. With the crossover off the
 * whole band is doubled (mono-safe lows traded for maximum width); with the
 * diffuser off the lag side is a pure delay (more coherent, more comb-like
 * on a mono sum).
 *
 * The single musical control is a signed Offset in ms (±24): the sign picks
 * the lagged channel ("knob points at the fake one"; precedence pulls the
 * image toward the dry side), the magnitude the base delay. The signed value
 * runs through one ~100 ms one-pole so sweeps across center pass cleanly
 * through identity instead of leaving decaying lag on the old side, and
 * knob moves read as a tape-style varispeed glide.
 *
 * Wobble depth is absolute, not relative to the offset: 100% = ±1.2 ms of
 * slow drift around the dialed offset (see kDeckWobbleMaxMs).
 *
 * Center identity: at T = 0 the lag deck's output still differs from dry
 * (the diffuser and lag gain stay engaged), but the center detent must be
 * exactly dual-mono. So I blend the lag path back to the dry high band as
 * |t| falls below kCenterBlendMs, which keeps sweeps continuous (the
 * tape-flange zone effectively starts around 1 ms).
 *
 * Engage/bypass is a ~25 ms equal-gain crossfade between the untouched input
 * and the doubled image: unlike the stereo offset there is no glide-through-
 * zero trick available, because even at 0 ms the deck's output differs from
 * the input (LR4 recombination is allpass-flat, not identity). While the
 * switch is on the deck always runs at full strength: there is no wet/dry;
 * Offset is the only musical dimension.
 *
 * Audio thread only (the correlation atomic is read by the UI thread); zero
 * allocation after prepare().
 */

/** Decoded spread parameters. Normalized knob values map here in exactly
    one place so the DSP and any UI readouts agree. */
struct SpreadParams {
  // Same span as the stereo-mode Align delay (StereoOffsetParams) so
  // the two faces of the stereo-image slot read identically.
  static constexpr float kMaxOffsetMs = 24.0f;

  float offsetMs = 0.0f;      // signed; > 0 lags the right channel
  float wobbleDepth = 0.25f;  // 0..1 of the ±1.2 ms wobble range; 0 when off
  float crossoverHz = 130.0f;
  bool crossoverOn = true;    // off: full-band doubling, low band bypassed
  bool diffuseOn = true;      // off: lag side is a pure delay

  /** offsetNorm: bipolar 0..1, 0.5 = center = 0 ms (values within a hair of
      center decode to exactly zero so the knob detent genuinely means zero).
      wobbleNorm: 0..1, folded to zero depth while the wobble switch is off
      (the depth smoother then makes the switch click-free). crossoverNorm:
      0..1 on the shared log Hz map (deckCrossoverHz). */
  static SpreadParams fromNormalized(float offsetNorm, float wobbleNorm, float crossoverNorm,
                                     bool wobbleOn, bool crossoverOn, bool diffuseOn) {
    constexpr float kEps = 0.005f;
    SpreadParams p;
    const float bipolar = juce::jlimit(0.0f, 1.0f, offsetNorm) * 2.0f - 1.0f;
    p.offsetMs = std::abs(bipolar) < kEps ? 0.0f : bipolar * kMaxOffsetMs;
    p.wobbleDepth = wobbleOn ? juce::jlimit(0.0f, 1.0f, wobbleNorm) : 0.0f;
    p.crossoverHz = deckCrossoverHz(crossoverNorm);
    p.crossoverOn = crossoverOn;
    p.diffuseOn = diffuseOn;
    return p;
  }
};

class Spread {
public:
  void prepare(double sampleRate, int maxBlockSize);

  /** Per-block parameter update. `engaged` is the power switch: turning it
      off starts the fade-out (isRunning() stays true until it lands);
      turning it on from idle resets the deck and fades in. */
  void setTarget(const SpreadParams& params, bool engaged);

  /** True while the engine needs process() this block. False = fully idle,
      safe to skip. */
  bool isRunning() const { return running; }

  /** Immediate hard stop, no fade. Only safe while the bus is already
      silent; the chain-edit fade covers mono/stereo mode switches, which is
      the one caller. */
  void forceIdle();

  /** Requires >= 2 channels. The deck is seeded from channel 0 (the mono
      chain output); the engage/bypass crossfade endpoints are each channel's
      own untouched signal, so with a true stereo source in mono chain mode
      (where the channels differ) the fade lands exactly on the input.
      Writes the stereo image in place. */
  void process(juce::AudioBuffer<float>& buffer);

  /** Latest output correlation (-1..1) for the UI meter; 1 when idle or on
      silence. Readable from any thread. */
  float correlation() const { return corrMeter.value(); }

private:
  void resetDeck();

  // Fixed design values (rationale in plugin/docs/stereo-image.md).
  static constexpr float kLagGainDb = 1.5f;       // precedence patch, not cure
  static constexpr double kOffsetSmoothSeconds = 0.1;  // signed-value one-pole
  static constexpr float kCenterBlendMs = 1.0f;   // lag→ref blend below this

  juce::dsp::LinkwitzRileyFilter<float> crossover;
  juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> delayLine;
  DeckDiffuser diffuser;
  DeckWobble wobble;
  DeckCorrelation corrMeter;

  // Crossover / diffuse bypasses as ~25 ms blends (see the class comment);
  // 1 = section engaged. The cutoff is only pushed into the filter when the
  // knob actually moved (coefficient recompute per change, not per block).
  juce::LinearSmoothedValue<float> crossoverMix;
  juce::LinearSmoothedValue<float> diffuseMix;
  float appliedCrossoverHz{130.0f};

  bool running{false};
  bool engaged{false};

  float targetOffsetMs{0.0f};
  float offsetStateMs{0.0f};  // smoothed SIGNED offset (sign = lagged side)
  float offsetCoeff{0.0f};

  juce::LinearSmoothedValue<float> wobbleDepth;

  // Engage/bypass crossfade: 0 = untouched input, 1 = doubled image.
  juce::LinearSmoothedValue<float> wetGain;

  double sampleRate{48000.0};
  float msToSamples{48.0f};
  float lagGain{1.0f};
};
