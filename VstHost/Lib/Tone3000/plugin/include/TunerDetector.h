#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>

/**
 * Monophonic pitch detector for the tuner screen.
 *
 * The audio thread pushes raw (pre-gain, pre-gate) input samples into a ring
 * buffer whenever the tuner is enabled. The UI polls getReading() from the
 * message thread, which runs a YIN pitch analysis over the most recent window
 * and returns { frequency, confidence, level }. No locks are used: a torn
 * read across the ring wrap only ever produces a single throwaway reading.
 *
 * The search range covers 5-string bass low B through guitar high frets
 * (kMinFrequency..kMaxFrequency) at any host sample rate: prepare() picks a
 * decimation factor that lands the analysis near kTargetAnalysisRate.
 */
class TunerDetector {
public:
  TunerDetector();

  void prepare(double sampleRate);

  void setEnabled(bool shouldBeEnabled) { enabled.store(shouldBeEnabled); }
  bool isEnabled() const { return enabled.load(); }

  /** Real-time thread. Cheap copy into the ring buffer; no allocation. */
  void pushSamples(const float* samples, int numSamples);

  /**
   * Message thread. Returns a DynamicObject var:
   *   frequency  (double) detected pitch in Hz, 0.0 when no reliable pitch
   *   confidence (double) 0..1
   *   level      (double) window RMS in dBFS
   * Results are cached for a short interval so fast UI polling stays cheap.
   */
  juce::var getReading();

private:
  void analyze();

  static constexpr int kRingSize = 1 << 15;  // 32768 samples (~0.7 s at 48 kHz)
  static constexpr int kMaxDecimation = 16;  // kWindowSize * 16 == kRingSize
  static constexpr int kWindowSize = 2048;   // decimated samples per analysis

  // Decimated analysis rate the decimation factor aims for. ~12 kHz keeps
  // the YIN search window identical across host sample rates: low enough
  // that kMinFrequency fits in kWindowSize/2 lags, high enough for
  // kMaxFrequency to stay several samples per period.
  static constexpr double kTargetAnalysisRate = 12000.0;

  static constexpr double kMinFrequency = 28.0;    // below low B on a 5-string bass
  static constexpr double kMaxFrequency = 1500.0;  // above high-fret high E
  static constexpr double kSilenceDb = -55.0;
  static constexpr double kYinThreshold = 0.15;
  static constexpr int kAnalysisIntervalMs = 25;

  std::vector<float> ring;
  std::atomic<int> writePos{0};
  std::atomic<bool> enabled{false};
  double sampleRate = 48000.0;
  // sampleRate / decimation ≈ kTargetAnalysisRate; set in prepare().
  int decimation = 4;

  // Message-thread scratch + cached result
  std::vector<float> rawWindow;
  std::vector<float> window;
  std::vector<float> yin;
  double lastFrequency = 0.0;
  double lastConfidence = 0.0;
  double lastLevelDb = -120.0;
  juce::int64 lastAnalysisMs = 0; 
};
