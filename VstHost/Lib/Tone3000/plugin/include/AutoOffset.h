#pragma once
#include "StereoOffset.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

/**
 * Auto Align: probe-based time-alignment measurement between the two chains
 * in stereo chain mode. Different NAM models / IRs carry different baked-in
 * latency, so two chains fed the same instrument can land a few ms apart;
 * this measures that misalignment and produces the corrective delay for the
 * Align feature (StereoOffset engine). On arm() the plugin output fades to
 * silence, both chains are driven with an identical exponential sine sweep
 * (Farina, 108th AES Convention, 2000), the raw chain outputs are captured,
 * and the lag is estimated on the message thread; the caller applies it to
 * the host parameters and resume()s, ramping the output back. The whole
 * measurement mutes the output for under half a second.
 *
 * Why a probe instead of listening to the player (the previous design): a
 * deterministic stimulus, identical into both chains, makes the model
 * nonlinearities common-mode and the result repeatable to a fraction of a
 * sample. It needs no signal gate, no timeout, no splice handling, and is
 * immune to the pitch-period ambiguity of correlating periodic guitar
 * playing. The exponential sweep specifically drives the chains across
 * their whole band at realistic level while keeping harmonic distortion
 * products away from the linear response the lag lives in.
 *
 * Analysis: generalized cross-correlation with soft PHAT weighting (Knapp &
 * Carter, IEEE TASSP 1976): each cross-spectrum bin is normalized by
 * |X|^0.8, which whitens away the chains' voicing differences so the
 * correlation peak approaches a delta at the true lag. The peak is searched
 * on magnitude over ±24 ms, the range the Align Offset knob can express
 * (captures don't share a polarity convention, so one chain can arrive 180°
 * out and the true peak is then negative; the sign comes back as
 * `inverted`). The integer peak is refined to sub-sample precision by
 * evaluating the cross-spectrum's inverse DFT on a fine grid around it:
 * parabolic interpolation straight on integer lags carries a known
 * position-dependent bias (Cespedes et al., Ultrasonic Imaging 1995);
 * band-limited interpolation does not (Qin et al., ICSP 2008). StereoOffset
 * applies fractional offsets exactly (Lagrange delay line).
 *
 * Quality comes back two ways. `peakSharpness` (winning peak against the
 * best peak more than 1 ms away) is the gate: it is voicing-immune like the
 * PHAT estimate itself and sits comfortably high on every healthy run, so a
 * low value is a diagnostic (a chain edit spliced the capture, a chain is
 * silently broken, the true lag is beyond ±24 ms), not a bad take, and the
 * caller rejects the measurement instead of setting a junk offset.
 * `confidence` (normalized raw-waveform correlation at the winning lag) is
 * reporting only: two differently voiced rigs legitimately read low there
 * even when perfectly aligned.
 *
 * State machine, one atomic:
 *   Idle -> FadeOut -> Probing -> Tail -> Captured -> Analyzing -> RampBack -> Idle
 *
 * Processor integration contract:
 *  1. renderProbeInput() before the chains render; while it returns true,
 *     feed the filled mono block to BOTH chain inputs and discard the
 *     instrument (it keeps flowing upstream: meters and gate stay live).
 *  2. captureChainOutputs() with the chain outputs pre-StereoOffset and
 *     pre-image-matrix, so the measurement is the absolute misalignment and
 *     unflipped polarity (a second run measures the total, not the residual).
 *  3. applyOutputGain() on the output after the last audible stage: it fades
 *     out, holds silence through capture and analysis, and ramps back.
 *  4. Never arm() offline (isNonRealtime()): the probe prints silence into
 *     a bounce.
 *  5. Message thread: poll state() for Captured, analyze(), apply the
 *     result to the host parameters, then resume(). The unmute never runs
 *     ahead of the parameter write.
 *
 * Threading: the state atomic is the hand-off, same as before. The audio
 * thread only copies into pre-allocated buffers and flips the atomic;
 * analyze() runs one-shot on the message thread and may allocate.
 */
class AutoOffset {
public:
  enum class State : int { Idle = 0, FadeOut, Probing, Tail, Captured, Analyzing, RampBack };

  struct Result {
    /** Corrective offset in ms, the StereoOffset convention: positive delays
        the right chain (i.e. the left chain lags), negative the left. */
    float offsetMs = 0.0f;
    /** The chains are polarity-inverted relative to each other (the
        correlation peak at the winning lag is negative). */
    bool inverted = false;
    /** Normalized cross-correlation magnitude at the winning lag, 0..1. */
    float confidence = 0.0f;
    /** Winning |peak| over the best |peak| more than 1 ms away, >= 1. */
    float peakSharpness = 0.0f;
  };

  /** Lag search half-window: what the Offset knob can express. */
  static constexpr float kMaxLagMs = StereoOffsetParams::kMaxOffsetMs;

  /** Builds the sweep and sizes the capture for this rate. Any measurement
      in flight is dropped (a rate change invalidates the capture anyway). */
  void prepare(double sampleRate);

  void arm();     // message thread: start the fade-out -> probe sequence
  void cancel();  // any state; the output ramps back if it was down

  /** Message thread, only valid in Captured: run the estimation. Leaves the
      state at Analyzing (output still muted) so the caller can apply the
      result to the parameters before resume() unmutes. */
  Result analyze();
  void resume();  // message thread: after the parameters are applied

  /** Audio thread, before the chains render: true while the probe owns the
      chain input, with `probeOut` filled with sweep (then tail-silence)
      samples. False in every other state, including the muted re-settle
      window after capture, where the live signal runs the chains again. */
  bool renderProbeInput(float* probeOut, int numSamples);

  /** Audio thread: append the two raw chain outputs while probing. The
      schedule is deterministic, so there is no gate and no splicing. */
  void captureChainOutputs(const float* chainL, const float* chainR, int numSamples);

  /** Audio thread, after the last audible stage: applies the mute fade, the
      silent hold, and the ramp back to the whole buffer. */
  void applyOutputGain(juce::AudioBuffer<float>& output);

  State state() const {
    return static_cast<State>(stateFlag.load(std::memory_order_acquire));
  }

  /** Capture progress 0..1 through the fixed schedule, for the UI poll. */
  float progress() const;

private:
  static constexpr float kSweepSeconds = 0.28f;
  static constexpr float kSweepLowHz = 30.0f;
  /** Sweep top as a fraction of the band actually reaching the chains: the
      chain domain is 48 kHz-based (see ChainDomain.h), so on higher host
      rates anything above its Nyquist would just die in the resampler. */
  static constexpr float kSweepHighFraction = 0.45f;
  static constexpr float kProbeEdgeFadeSeconds = 0.005f;
  static constexpr float kProbeAmplitude = 0.25f;  // ~ -12 dBFS into the chains
  /** Post-sweep capture: enough for the chain tails that matter (the sweep
      body dominates the correlation; a long reverb IR's late tail adds
      nothing) and always more than the lag window, so a chain lagging by
      the full knob range still lands its sweep end inside the capture. */
  static constexpr float kTailSeconds = 0.10f;
  static constexpr float kMuteFadeSeconds = 0.005f;
  static constexpr float kRampBackSeconds = 0.010f;
  static constexpr float kPhatRho = 0.8f;  // soft PHAT exponent, see class comment
  static constexpr int kFineStepsPerSample = 8;

  void buildSweep();
  void setState(State s) { stateFlag.store(static_cast<int>(s), std::memory_order_release); }

  double sampleRate = 48000.0;
  int sweepSamples = 0;
  int capacity = 0;  // sweepSamples + tail

  juce::AudioBuffer<float> probeBuffer;    // mono sweep, built in prepare()
  juce::AudioBuffer<float> captureBuffer;  // 2 ch raw chain outputs

  // Audio-thread-only cursors and gain (progress() reads `written` relaxed).
  int probePos = 0;
  float outputGain = 1.0f;
  float muteStep = 0.0f;
  float rampStep = 0.0f;

  std::atomic<int> written{0};
  std::atomic<int> stateFlag{static_cast<int>(State::Idle)};
};
