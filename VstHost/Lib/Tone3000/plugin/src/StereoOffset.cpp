#include "StereoOffset.h"
#include <cmath>

void StereoOffset::prepare(double newSampleRate, int maxBlockSize) {
  sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
  msToSamples = static_cast<float>(sampleRate * 0.001);

  const int maxDelay = static_cast<int>(std::ceil(
      (StereoOffsetParams::kMaxOffsetMs + kDeckWobbleMaxMs) * 0.001 * sampleRate)) + 8;
  maxDelaySamples = static_cast<float>(maxDelay);
  delayLine.setMaximumDelayInSamples(maxDelay);
  delayLine.prepare({sampleRate, static_cast<juce::uint32>(juce::jmax(1, maxBlockSize)), 1});

  // The crossover splits BOTH channels (see the class comment), so it
  // prepares with two channels even though the delay line has one.
  crossoverFilter.setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
  crossoverFilter.setCutoffFrequency(appliedCrossoverHz);
  crossoverFilter.prepare({sampleRate, static_cast<juce::uint32>(juce::jmax(1, maxBlockSize)), 2});

  diffuser.prepare(sampleRate);
  wobble.prepare(sampleRate);
  corrMeter.prepare(sampleRate);

  delaySamples.reset(sampleRate, kRampSeconds);
  delaySamples.setCurrentAndTargetValue(0.0f);
  wobbleDepth.reset(sampleRate, kDeckFadeSeconds);
  crossoverMix.reset(sampleRate, kDeckFadeSeconds);
  diffuseMix.reset(sampleRate, kDeckFadeSeconds);

  running = false;
  engaged = false;
  delayLine.reset();
  crossoverFilter.reset();
  samplesSinceClear = 0;
}

void StereoOffset::setTarget(const StereoOffsetParams& newParams, bool nowEngaged) {
  if (!running) {
    if (!nowEngaged)
      return;  // idle and staying idle
    // Engage from idle: clean state, 0 ms delay, deck sections primed at
    // bypass so they blend in (an instant split/diffuse at the engage
    // boundary would step the waveform). The delay ramp never outruns real
    // time (kRampSeconds > max delay swing), so after this clear a read can
    // never reach back past the engage point: no stale audio, no gap.
    delayLine.reset();
    samplesSinceClear = 0;
    crossoverFilter.reset();
    diffuser.reset();
    wobble.reset();
    corrMeter.reset();
    currentChannel = newParams.targetChannel;
    delaySamples.setCurrentAndTargetValue(0.0f);
    wobbleDepth.setCurrentAndTargetValue(0.0f);
    crossoverMix.setCurrentAndTargetValue(0.0f);
    diffuseMix.setCurrentAndTargetValue(0.0f);
    running = true;
  }

  engaged = nowEngaged;
  params = newParams;
  if (params.crossoverHz != appliedCrossoverHz) {
    appliedCrossoverHz = params.crossoverHz;
    crossoverFilter.setCutoffFrequency(appliedCrossoverHz);
  }

  // Disengaging or switching sides glides the processed side to identity
  // first: delay to zero, wobble depth and diffusion to bypass (at the
  // handover instant the processed side must equal its split input);
  // process() completes the transition once everything lands. The crossover
  // is side-agnostic (it always splits both channels), so a side swap keeps
  // it at the knob and only a disengage blends it out. Otherwise track knob
  // edits immediately.
  if (!engaged || params.targetChannel != currentChannel) {
    delaySamples.setTargetValue(0.0f);
    wobbleDepth.setTargetValue(0.0f);
    diffuseMix.setTargetValue(0.0f);
    crossoverMix.setTargetValue(engaged && params.crossoverOn ? 1.0f : 0.0f);
  } else {
    retargetDeck();
  }
}

void StereoOffset::retargetDeck() {
  const float totalMs = juce::jlimit(0.0f, StereoOffsetParams::kMaxOffsetMs, params.offsetMs);
  delaySamples.setTargetValue(totalMs * msToSamples);
  wobbleDepth.setTargetValue(params.wobbleDepth);
  crossoverMix.setTargetValue(params.crossoverOn ? 1.0f : 0.0f);
  diffuseMix.setTargetValue(params.diffuseOn ? 1.0f : 0.0f);
}

void StereoOffset::process(juce::AudioBuffer<float>& buffer) {
  if (!running || buffer.getNumChannels() < 2)
    return;

  const int numSamples = buffer.getNumSamples();
  float* data[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
  const int d = currentChannel;  // deck side
  const int o = 1 - d;           // pass-through side

  // Correlation block sums (means folded into the meter after the loop).
  float sumLR = 0.0f, sumLL = 0.0f, sumRR = 0.0f;

  for (int i = 0; i < numSamples; ++i) {
    // Crossover both channels, blended toward its bypass (low = 0, high =
    // the full band). With the deck fully off this reduces to the input
    // bit-exactly, preserving the "untouched side never moves" guarantee.
    const float xo = crossoverMix.getNextValue();
    float low[2], high[2];
    for (int ch = 0; ch < 2; ++ch) {
      const float x = data[ch][i];
      float lo = 0.0f, hi = 0.0f;
      crossoverFilter.processSample(ch, x, lo, hi);
      low[ch] = lo * xo;
      high[ch] = x + (hi - x) * xo;
    }

    // The deck (delay + wobble + diffusion) rides the current side's high
    // band; the other side passes its split straight through. The delay is
    // clamped so every interpolation tap stays inside audio written since
    // the last line clear (see the class comment): right after an engage or
    // side swap that pins it at 0 (identity) until the fresh region can
    // carry the glide.
    const float freshLimit = static_cast<float>(juce::jmax(0, samplesSinceClear - 2));
    samplesSinceClear = juce::jmin(samplesSinceClear + 1, 1 << 30);
    const float wobbleMs = kDeckWobbleMaxMs * wobbleDepth.getNextValue() * wobble.next();
    const float delay = juce::jlimit(0.0f, juce::jmin(maxDelaySamples, freshLimit),
                                     delaySamples.getNextValue() + wobbleMs * msToSamples);
    delayLine.pushSample(0, high[d]);
    float wet = delayLine.popSample(0, delay);
    wet += (diffuser.process(wet) - wet) * diffuseMix.getNextValue();

    data[d][i] = low[d] + wet;
    data[o][i] = low[o] + high[o];

    sumLR += data[0][i] * data[1][i];
    sumLL += data[0][i] * data[0][i];
    sumRR += data[1][i] * data[1][i];
  }

  corrMeter.update(sumLR, sumLL, sumRR, numSamples);

  // Complete transitions that were gliding toward identity on the processed
  // side (delay at zero, wobble and diffusion at bypass): both the side
  // swap and going idle are click-free by construction. A disengage also
  // waits for the crossover blend, so the buffer really is untouched when
  // running flips off.
  auto landed = [](const auto& smoothed) {
    return !smoothed.isSmoothing() && smoothed.getCurrentValue() <= 0.0f;
  };
  if (landed(delaySamples) && landed(wobbleDepth) && landed(diffuseMix)) {
    if (!engaged) {
      if (landed(crossoverMix))
        forceIdle();
    } else if (params.targetChannel != currentChannel) {
      currentChannel = params.targetChannel;
      delayLine.reset();
      samplesSinceClear = 0;
      diffuser.reset();
      retargetDeck();
    }
  }
}
