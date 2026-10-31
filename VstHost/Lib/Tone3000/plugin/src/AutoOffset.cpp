#include "AutoOffset.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>

void AutoOffset::prepare(double newSampleRate) {
  sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
  sweepSamples = static_cast<int>(std::llround(kSweepSeconds * sampleRate));

  const int maxLag = static_cast<int>(std::ceil(kMaxLagMs * 0.001 * sampleRate));
  const int tailSamples =
      std::max(static_cast<int>(std::llround(kTailSeconds * sampleRate)), 2 * maxLag);
  capacity = sweepSamples + tailSamples;

  probeBuffer.setSize(1, sweepSamples);
  captureBuffer.setSize(2, capacity);
  buildSweep();

  written.store(0, std::memory_order_relaxed);
  setState(State::Idle);
}

void AutoOffset::buildSweep() {
  // Exponential sine sweep (Farina 2000):
  //   x(t) = A * sin(K * (exp(t/T * ln(f2/f1)) - 1)),  K = 2*pi*f1*T / ln(f2/f1)
  // Frequency rises exponentially from f1 to f2 over T seconds. Raised-cosine
  // edge fades avoid spectral splatter into the models.
  const double f1 = kSweepLowHz;
  const double f2 = kSweepHighFraction * std::min(sampleRate, 48000.0);
  const double T = sweepSamples / sampleRate;
  const double logRatio = std::log(f2 / f1);
  const double K = 2.0 * juce::MathConstants<double>::pi * f1 * T / logRatio;

  float* p = probeBuffer.getWritePointer(0);
  const int fadeSamples =
      std::min(sweepSamples / 4,
               static_cast<int>(std::llround(kProbeEdgeFadeSeconds * sampleRate)));

  for (int i = 0; i < sweepSamples; ++i) {
    const double t = i / sampleRate;
    const double phase = K * (std::exp(t / T * logRatio) - 1.0);
    float s = kProbeAmplitude * static_cast<float>(std::sin(phase));

    if (i < fadeSamples) {
      s *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * i / fadeSamples);
    } else if (i >= sweepSamples - fadeSamples) {
      const int j = sweepSamples - 1 - i;
      s *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * j / fadeSamples);
    }
    p[i] = s;
  }
}

void AutoOffset::arm() {
  if (state() != State::Idle)
    return;

  probePos = 0;
  muteStep = -1.0f / std::max(1.0f, static_cast<float>(kMuteFadeSeconds * sampleRate));
  rampStep = 1.0f / std::max(1.0f, static_cast<float>(kRampBackSeconds * sampleRate));
  // The release-store publishes the reset cursor together with the state
  // flip; the audio thread only touches it once it observes FadeOut.
  written.store(0, std::memory_order_relaxed);
  setState(State::FadeOut);
}

void AutoOffset::cancel() {
  if (state() == State::Idle)
    return;
  // Redirect to the ramp from wherever we are; applyOutputGain finishes the
  // transition to Idle (immediately if the gain never left 1).
  setState(State::RampBack);
}

void AutoOffset::resume() {
  if (state() == State::Analyzing)
    setState(State::RampBack);
}

bool AutoOffset::renderProbeInput(float* probeOut, int numSamples) {
  const State s = state();
  if (s != State::Probing && s != State::Tail)
    return false;

  const float* sweep = probeBuffer.getReadPointer(0);
  for (int i = 0; i < numSamples; ++i) {
    probeOut[i] = probePos < sweepSamples ? sweep[probePos] : 0.0f;
    ++probePos;
  }

  if (s == State::Probing && probePos >= sweepSamples)
    setState(State::Tail);
  return true;
}

void AutoOffset::captureChainOutputs(const float* chainL, const float* chainR, int numSamples) {
  const State s = state();
  if (s != State::Probing && s != State::Tail)
    return;

  const int have = written.load(std::memory_order_relaxed);
  const int take = std::min(numSamples, capacity - have);
  if (take <= 0)
    return;

  captureBuffer.copyFrom(0, have, chainL, take);
  captureBuffer.copyFrom(1, have, chainR, take);
  written.store(have + take, std::memory_order_relaxed);

  if (have + take >= capacity)
    setState(State::Captured);
}

void AutoOffset::applyOutputGain(juce::AudioBuffer<float>& output) {
  const State s = state();
  if (s == State::Idle) {
    outputGain = 1.0f;
    return;
  }

  const int n = output.getNumSamples();
  const int channels = output.getNumChannels();

  if (s == State::FadeOut) {
    for (int i = 0; i < n; ++i) {
      outputGain = std::max(0.0f, outputGain + muteStep);
      for (int c = 0; c < channels; ++c)
        output.getWritePointer(c)[i] *= outputGain;
    }
    if (outputGain <= 0.0f)
      setState(State::Probing);
    return;
  }

  if (s == State::RampBack) {
    for (int i = 0; i < n; ++i) {
      outputGain = std::min(1.0f, outputGain + rampStep);
      for (int c = 0; c < channels; ++c)
        output.getWritePointer(c)[i] *= outputGain;
    }
    if (outputGain >= 1.0f)
      setState(State::Idle);
    return;
  }

  // Probing / Tail / Captured / Analyzing: hard mute. Captured/Analyzing
  // stay muted on purpose: the live signal is back in the chains
  // (re-settling their state), but nothing is audible until resume().
  output.clear();
  outputGain = 0.0f;
}

float AutoOffset::progress() const {
  return capacity > 0
             ? juce::jlimit(0.0f, 1.0f, static_cast<float>(written.load(std::memory_order_relaxed)) /
                                            static_cast<float>(capacity))
             : 0.0f;
}

AutoOffset::Result AutoOffset::analyze() {
  Result result;
  if (state() != State::Captured) {
    cancel();
    return result;
  }
  setState(State::Analyzing);

  const int n = written.load(std::memory_order_relaxed);
  const int maxLag = static_cast<int>(std::ceil(kMaxLagMs * 0.001 * sampleRate));
  const float* l = captureBuffer.getReadPointer(0);
  const float* r = captureBuffer.getReadPointer(1);

  // Generalized cross-correlation via FFT: c = IFFT(W * FFT(L) * conj(FFT(R))),
  // where c[k] = sum_n L[n]*R[n-k]. A peak at positive k means L is a delayed
  // copy of R (the left chain lags) -> delay the right chain -> positive ms,
  // matching the StereoOffset sign convention. Zero-padding past n + maxLag
  // keeps circular wrap-around out of the searched window; negative lags
  // live at indices fftSize - k. One-shot on the message thread, so
  // allocating here is fine.
  const int fftOrder = static_cast<int>(std::ceil(std::log2(std::max(2, n + maxLag))));
  const int fftSize = 1 << fftOrder;
  juce::dsp::FFT fft(fftOrder);

  std::vector<float> specL(static_cast<size_t>(fftSize) * 2, 0.0f);
  std::vector<float> specR(static_cast<size_t>(fftSize) * 2, 0.0f);
  std::copy(l, l + n, specL.begin());
  std::copy(r, r + n, specR.begin());
  fft.performRealOnlyForwardTransform(specL.data());
  fft.performRealOnlyForwardTransform(specR.data());

  // Cross-spectrum with soft PHAT weighting (see the header): normalizing
  // each bin by |X|^rho whitens away the chains' voicing differences so the
  // peak approaches a delta at the true lag; rho < 1 keeps near-empty bins
  // from being amplified into noise.
  constexpr float eps = 1.0e-12f;
  for (int bin = 0; bin < fftSize; ++bin) {
    const float ar = specL[static_cast<size_t>(2 * bin)];
    const float ai = specL[static_cast<size_t>(2 * bin) + 1];
    const float br = specR[static_cast<size_t>(2 * bin)];
    const float bi = specR[static_cast<size_t>(2 * bin) + 1];
    const float xr = ar * br + ai * bi;
    const float xi = ai * br - ar * bi;
    const float mag = std::sqrt(xr * xr + xi * xi);
    const float w = 1.0f / std::max(std::pow(mag, kPhatRho), eps);
    specL[static_cast<size_t>(2 * bin)] = xr * w;
    specL[static_cast<size_t>(2 * bin) + 1] = xi * w;
  }

  // Keep the whitened cross-spectrum: the sub-sample refinement below
  // evaluates its inverse DFT at fractional lags.
  const std::vector<float> crossSpec(specL);

  fft.performRealOnlyInverseTransform(specL.data());

  auto corrAt = [&](int lag) -> float {
    return specL[static_cast<size_t>(lag >= 0 ? lag : fftSize + lag)];
  };

  // Peak search on the magnitude: a polarity-inverted chain correlates
  // strongly negative, and that is a valid measurement (see the header).
  int bestLag = 0;
  float bestAbs = std::abs(corrAt(0));
  for (int lag = -maxLag; lag <= maxLag; ++lag) {
    const float a = std::abs(corrAt(lag));
    if (a > bestAbs) {
      bestAbs = a;
      bestLag = lag;
    }
  }
  const bool inverted = corrAt(bestLag) < 0.0f;

  // Peak sharpness: winning |peak| against the best |peak| more than 1 ms
  // away inside the search window.
  const int guard = static_cast<int>(std::llround(0.001 * sampleRate));
  float secondAbs = 0.0f;
  for (int lag = -maxLag; lag <= maxLag; ++lag) {
    if (std::abs(lag - bestLag) > guard)
      secondAbs = std::max(secondAbs, std::abs(corrAt(lag)));
  }
  result.peakSharpness = bestAbs / std::max(secondAbs, eps);

  // Sub-sample refinement: evaluate the whitened cross-spectrum's inverse
  // DFT (exact band-limited interpolation of the correlation) on a fine
  // grid of +-1 sample around the integer peak, then parabolic-fit on the
  // fine grid, where the parabola's position-dependent bias is negligible.
  // Upper-half bins are the negative frequencies: at integer lags treating
  // them as +bin aliases invisibly (phases wrap by whole turns), but at
  // fractional lags they must be evaluated at their signed frequency or the
  // estimate gets pulled toward the nearest integer.
  auto corrFrac = [&](double tau) -> double {
    double acc = 0.0;
    const double w0 = 2.0 * juce::MathConstants<double>::pi * tau / fftSize;
    for (int bin = 0; bin < fftSize; ++bin) {
      const double xr = crossSpec[static_cast<size_t>(2 * bin)];
      const double xi = crossSpec[static_cast<size_t>(2 * bin) + 1];
      const double th = w0 * (bin <= fftSize / 2 ? bin : bin - fftSize);
      acc += xr * std::cos(th) - xi * std::sin(th);
    }
    return inverted ? -acc : acc;
  };

  const double step = 1.0 / kFineStepsPerSample;
  double fineTau = bestLag;
  double fineVal = corrFrac(fineTau);
  for (double tau = bestLag - 1.0; tau <= bestLag + 1.0 + 1e-9; tau += step) {
    const double v = corrFrac(tau);
    if (v > fineVal) {
      fineVal = v;
      fineTau = tau;
    }
  }
  {
    const double ym = corrFrac(fineTau - step);
    const double yp = corrFrac(fineTau + step);
    const double denom = ym - 2.0 * fineVal + yp;
    if (std::abs(denom) > 1e-20)
      fineTau += juce::jlimit(-0.5, 0.5, 0.5 * (ym - yp) / denom) * step;
  }

  // Confidence: normalized correlation at the (rounded) winning lag,
  // recomputed in the time domain on the raw captures, immune to FFT
  // scaling and the PHAT weighting. Its magnitude is the confidence (1 =
  // identical up to gain, shift and polarity); the sign was the polarity
  // verdict above.
  const int k = static_cast<int>(std::llround(fineTau));
  double dot = 0.0, energyL = 0.0, energyR = 0.0;
  const int from = std::max(0, k);
  const int to = std::min(n, n + k);
  for (int i = from; i < to; ++i) {
    dot += static_cast<double>(l[i]) * r[i - k];
    energyL += static_cast<double>(l[i]) * l[i];
    energyR += static_cast<double>(r[i - k]) * r[i - k];
  }
  const double denom = std::sqrt(std::max(energyL * energyR, 1.0e-24));

  result.offsetMs = juce::jlimit(-kMaxLagMs, kMaxLagMs,
                                 static_cast<float>(fineTau * 1000.0 / sampleRate));
  result.inverted = inverted;
  result.confidence = juce::jlimit(0.0f, 1.0f, static_cast<float>(std::abs(dot) / denom));
  return result;
}
