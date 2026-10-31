#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "ImageDeck.h"

/**
 * Align (stereo chain mode): a short delay applied to one chain, in place,
 * primarily for time-aligning the two chains (e.g. captures of the same
 * performance that land a few ms apart). Stereo chain mode only; mono mode
 * has Spread instead (see Spread.h), a separate feature with its own
 * parameters.
 *
 * Controls (see StereoOffsetParams for the normalized encoding): one bipolar
 * knob (alignOffset). Center = 0 ms; left of center delays the left chain,
 * right of center the right chain, up to ±24 ms.
 *
 * On top of the corrective delay sits the same advanced deck as Spread
 * (shared primitives in ImageDeck.h), all default OFF so align stays a
 * pure corrective tool until asked otherwise:
 *  - Wobble: random-walk drift of the delay time (up to ±1.2 ms), which
 *    turns the delayed chain into an ADT-style second take.
 *  - Crossover: both channels are split (LR4, 32.5-520 Hz); lows pass the
 *    deck untouched on BOTH sides. Splitting the undelayed side too is
 *    deliberate: the channels get the identical LR4 phase rotation, so the
 *    inter-chain alignment (the whole point of this engine) is preserved.
 *    Note the flip side: with the crossover on, lows are no longer
 *    corrected by the delay; it is a creative width control, not part of
 *    the corrective path.
 *  - Diffuse: the phase-diffusion cascade on the delayed side's high band.
 *    No precedence trim here, unlike Spread's lag deck: these are two real
 *    chains and a corrective tool must not color levels.
 *
 * Lifecycle: while Align is on the engine always runs (with the deck off a
 * 0 ms delay is identity, and the per-sample cost is small), so knob moves
 * never hard-enable/disable DSP. Every transition passes through identity
 * on the processed side, which is why none of them click: turning Align off
 * or crossing the knob through center glides the delay to zero AND blends
 * wobble/diffusion out first, then swaps sides or goes idle. The crossover
 * is side-agnostic (it always splits both channels), so side swaps keep it
 * engaged; only a disengage blends it out (the idle buffer must be
 * genuinely untouched). The section switches themselves are ~25 ms blends
 * (see kDeckFadeSeconds), and the bypassed filters keep running so
 * re-engaging blends into warm state.
 *
 * The delay time ramps through a SmoothedValue into a Lagrange-interpolated
 * DelayLine (4-point cubic: a static fractional delay through linear
 * interpolation would carry a fixed HF droop, wrong for a corrective tool).
 * The ramp (kRampSeconds) is chosen so the delay never grows faster than
 * real time; after a line clear, the read point can never land on unwritten
 * (stale or zero) samples. The interpolator's outer taps reach 2 samples
 * further back than the read point, so for the first few samples after a
 * clear the effective delay is additionally clamped to the freshly written
 * region (samplesSinceClear); without that clamp the taps graze unwritten
 * zeros and the engage / side-swap handover reads as a tick. The wobble
 * adds at most ±1.2 ms of ~0.3 Hz drift on top, far below real time, so
 * the invariant holds with it engaged.
 *
 * Correlation: process() keeps the same ~300 ms running normalized L/R
 * output correlation as Spread (DeckCorrelation), published for the UI
 * mono-safety meter in the align advanced panel.
 *
 * Audio thread only (the correlation atomic is read by the UI thread);
 * zero allocation after prepare().
 */

/** Decoded offset parameters. Normalized knob values map here in exactly one
    place so the DSP and any UI readouts agree. */
struct StereoOffsetParams {
  static constexpr float kMaxOffsetMs = 24.0f;

  int targetChannel = 1;  // channel that gets delayed (0 = left, 1 = right)
  float offsetMs = 0.0f;
  float wobbleDepth = 0.0f;  // 0..1 of the ±1.2 ms wobble range; 0 when off
  float crossoverHz = 130.0f;
  bool crossoverOn = false;
  bool diffuseOn = false;

  /** offsetNorm: bipolar 0..1, 0.5 = center = 0 ms. Values within a hair of
      center decode to exactly zero so the knob detent genuinely means zero.
      The hair is deliberately tight (~1 sample at 48 kHz): auto-align
      measures to sub-sample precision, and corrections of a few samples
      must not vanish into the detent.

      The deck arguments default to "everything off": a bare
      fromNormalized(offset) is the plain corrective tool. */
  static StereoOffsetParams fromNormalized(float offsetNorm, float wobbleNorm = 0.0f,
                                           float crossoverNorm = 0.5f, bool wobbleOn = false,
                                           bool crossoverOn = false, bool diffuseOn = false) {
    constexpr float kEps = 0.001f;
    StereoOffsetParams p;
    const float bipolar = juce::jlimit(0.0f, 1.0f, offsetNorm) * 2.0f - 1.0f;
    p.targetChannel = bipolar < 0.0f ? 0 : 1;
    const float amount = std::abs(bipolar);
    p.offsetMs = amount < kEps ? 0.0f : amount * kMaxOffsetMs;
    p.wobbleDepth = wobbleOn ? juce::jlimit(0.0f, 1.0f, wobbleNorm) : 0.0f;
    p.crossoverHz = deckCrossoverHz(crossoverNorm);
    p.crossoverOn = crossoverOn;
    p.diffuseOn = diffuseOn;
    return p;
  }
};

class StereoOffset {
public:
  void prepare(double sampleRate, int maxBlockSize);

  /** Per-block parameter update. `engaged` is Align's power switch: turning
      it off starts a glide-out (isRunning() stays true until the whole deck
      lands on identity); turning it on from idle starts clean at 0 ms with
      the deck blending in. */
  void setTarget(const StereoOffsetParams& params, bool engaged);

  /** True while the engine needs process() this block. False = fully idle,
      safe to skip. */
  bool isRunning() const { return running; }

  /** Immediate hard stop, no glide. Only safe while the bus is already
      silent; the chain-edit fade covers mono/stereo mode switches, which is
      the one caller. */
  void forceIdle() {
    running = false;
    engaged = false;
    corrMeter.reset();
  }

  /** Requires >= 2 channels: applies the deck to the current offset channel
      in place. Completes pending glide-out transitions (side swap / idle). */
  void process(juce::AudioBuffer<float>& buffer);

  /** Latest output correlation (-1..1) for the UI meter; 1 when idle or on
      silence. Readable from any thread. */
  float correlation() const { return corrMeter.value(); }

private:
  void retargetDeck();

  juce::dsp::LinkwitzRileyFilter<float> crossoverFilter;  // both channels
  juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> delayLine;
  juce::SmoothedValue<float> delaySamples;
  DeckDiffuser diffuser;
  DeckWobble wobble;
  DeckCorrelation corrMeter;

  // Deck blends (1 = section engaged) + wobble depth, all kDeckFadeSeconds.
  // The cutoff is only pushed into the filter when the knob actually moved.
  juce::LinearSmoothedValue<float> wobbleDepth;
  juce::LinearSmoothedValue<float> crossoverMix;
  juce::LinearSmoothedValue<float> diffuseMix;
  float appliedCrossoverHz{130.0f};

  bool running{false};
  bool engaged{false};
  int currentChannel{1};       // side being delayed right now
  StereoOffsetParams params;   // knob targets (side adopted via a zero glide)
  int samplesSinceClear{0};    // pushes since the last line clear (saturating)

  double sampleRate{48000.0};
  float msToSamples{48.0f};
  float maxDelaySamples{0.0f};

  // Delay ramp: longer than the max delay swing (24 ms) so delay time never
  // grows faster than real time; see class comment.
  static constexpr double kRampSeconds = 0.040;
};
