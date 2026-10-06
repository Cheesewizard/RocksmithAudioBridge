#pragma once

// Adaptive mains hum / buzz remover for the guitar input (front of the input chain).
//
// Why adaptive: a fixed comb of constant-Q notches at exact multiples of 50 Hz makes each notch f/30 wide,
// so in the upper mids neighbouring notches overlap and the cascade cuts the guitar by 10-15 dB, while the
// audible, crackly part of a real buzz (odd harmonics up to ~5 kHz) sits above any low cap. A fixed 50.00 Hz
// base also misses high harmonics: 0.07 Hz of grid offset is 7 Hz at the 100th harmonic.
//
// How: the audio thread feeds channel-0 samples from quiet stretches (no notes) into a lock-free ring. An
// analysis pass (worker thread in the game, called directly in tests) averages long FFT spectra of those
// stretches, estimates the true mains fundamental from the harmonic lines (weighted least squares over every
// clear line, so precision improves with harmonic number), and picks only the harmonics that actually stand
// out above their neighbourhood. The audio thread then runs a narrow notch (1.5 Hz wide, widening slowly with
// harmonic number to cover residual tracking error, 5 Hz at most) at each of those, up to 8 kHz. Nothing is
// notched until the hum has been measured, and a harmonic that is not present is never notched, so the
// guitar tone loses almost nothing.
//
// Threading: Process / BeginBlock / EndBlock on the audio thread only; Analyze on one other thread only.
// Configs cross over through a sequence-locked double buffer; samples through an SPSC ring.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace Audio
{
	class HumRemover
	{
	public:
		static constexpr int MAX_HARMONIC = 200;
		static constexpr double MAX_NOTCH_HZ = 8000.0;
		static constexpr size_t MAX_CHANNELS = 8;
		static constexpr uint32_t MAX_BLOCK_FRAMES = 4096;
		static constexpr double LINE_ON_SNR = 40.0;          // 16 dB above the neighbourhood turns a line on
		static constexpr double LINE_OFF_SNR = 10.0;         // below 10 dB for three analyses turns it off
		static constexpr double LINE_CUT_HEADROOM_DB = 3.0;  // leave the line this far above its neighbourhood
		static constexpr double LINE_CUT_MIN_DB = 6.0;
		static constexpr double LINE_CUT_MAX_DB = 30.0;
		static constexpr int FFT_N = 32768;                 // 0.68 s at 48 kHz, 1.46 Hz bins
		static constexpr uint32_t RING_SIZE = 1u << 17;     // 2.7 s of idle samples at 48 kHz

		struct Status
		{
			double fundamentalHz = 0.0;   // measured mains fundamental (0 = not locked yet)
			int lines = 0;                // harmonics currently notched
			double highestHz = 0.0;       // highest notched frequency
			double strongestDb = 0.0;     // strongest line above its neighbourhood
			uint32_t analyses = 0;
		};

		// ---- audio thread ----------------------------------------------------------------------------

		// Call once per packet before Process. Resets everything when the rate or the nominal mains
		// frequency (50 / 60, the search centre) changes, and picks up a newly published notch set.
		void BeginBlock(uint32_t sampleRate, float nominalHz, size_t channels)
		{
			if (sampleRate != rate || nominalHz != nominal)
			{
				rate = sampleRate;
				nominal = nominalHz;
				requestedRate.store(sampleRate, std::memory_order_relaxed);
				requestedNominal.store(nominalHz, std::memory_order_relaxed);
				currentEpoch = resetEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
				active = {};
				// Mark what is already published as seen: it was measured for the old rate / nominal, and
				// re-applying it would put the old notches straight back until a new lock replaced them.
				appliedSeq = publishSeq.load(std::memory_order_acquire);
				states = {};
				floorMs = -1.0;
				wasIdle = false;
				historyEnergy.fill(0.0);
				historyFrames.fill(0);
			}
			channelCount = (std::min)(channels, MAX_CHANNELS);
			blockFrames = 0;
			const uint32_t seq = publishSeq.load(std::memory_order_acquire);
			if (seq != appliedSeq) ApplyPublished(seq);
		}

		bool Active() const { return active.count > 0; }

		inline float Process(size_t channel, float input)
		{
			// One NaN/Inf from the device would latch every active notch's state at NaN, and the input clamp would
			// then turn every later sample into 0, silencing the guitar until a rate change.
			if (!std::isfinite(input)) input = 0.0f;
			if (channel == 0 && blockFrames < MAX_BLOCK_FRAMES) block[blockFrames++] = input;
			if (active.count == 0 || channel >= channelCount) return input;
			double sample = input;
			auto& chan = states[channel];
			for (int i = 0; i < active.count; ++i)
			{
				const Notch& n = active.notch[i];
				State& st = chan[n.k];
				const double out = n.b0 * sample + n.b1 * st.x1 + n.b2 * st.x2 - n.a1 * st.y1 - n.a2 * st.y2;
				st.x2 = st.x1; st.x1 = sample;
				st.y2 = st.y1; st.y1 = out;
				sample = out;
			}
			return static_cast<float>(sample);
		}

		// Call after the packet: decide whether it was a quiet stretch and, if so, hand its (unfiltered)
		// samples to the analysis. Quiet = within 6 dB of the slowly rising running floor of packet power.
		void EndBlock()
		{
			if (blockFrames == 0) return;
			double energy = 0.0;
			for (uint32_t i = 0; i < blockFrames; ++i) energy += static_cast<double>(block[i]) * block[i];
			// Judge quietness over the last ~40 ms (two mains cycles), not one packet: a 128-frame packet is a
			// fraction of a 20 ms hum cycle, so hum alone swings well over 6 dB from packet to packet.
			historyEnergy[historyPos] = energy;
			historyFrames[historyPos] = blockFrames;
			historyPos = (historyPos + 1) % HISTORY;
			double windowEnergy = 0.0; uint32_t windowFrames = 0;
			for (int i = 0; i < HISTORY && windowFrames < rate / 25; ++i)
			{
				const int at = (historyPos - 1 - i + HISTORY) % HISTORY;
				windowEnergy += historyEnergy[at];
				windowFrames += historyFrames[at];
			}
			const double ms = windowFrames ? windowEnergy / windowFrames : 0.0;
			if (ms <= 1e-18) { MarkGap(); return; }                 // digital silence carries no hum
			if (floorMs < 0.0 || ms < floorMs) floorMs = ms;
			else floorMs *= 1.0 + 0.001 * blockFrames / 128.0;      // floor creeps up ~1.6 dB/s if the room gets louder
			if (ms > floorMs * 4.0) { MarkGap(); return; }            // 6 dB: a steady hum floor, not a decaying note
			wasIdle = true;
			const uint32_t write = ringWrite.load(std::memory_order_relaxed);
			const uint32_t read = ringRead.load(std::memory_order_acquire);
			if (write - read + blockFrames > RING_SIZE) return;      // analysis behind: skip, not a gap
			for (uint32_t i = 0; i < blockFrames; ++i) ring[(write + i) & (RING_SIZE - 1)] = block[i];
			ringWrite.store(write + blockFrames, std::memory_order_release);
		}

		// ---- analysis thread -------------------------------------------------------------------------

		// Consume queued quiet samples; each full FFT window updates the averaged spectrum and re-estimates
		// the hum. Returns true when a new notch set was published.
		bool Analyze()
		{
			const uint32_t epoch = resetEpoch.load(std::memory_order_acquire);
			if (epoch != seenEpoch)
			{
				seenEpoch = epoch;
				segment.clear();
				average.assign(FFT_N / 2 + 1, 0.0);
				spectra = 0;
				lineOn.fill(0);
				lineMiss.fill(0);
				lockedHz = candidateHz = 0.0;
				candidateAgree = 0;
				statusSeq.fetch_add(1, std::memory_order_acq_rel);   // the old lock no longer applies
				status = Status{};
				statusSeq.fetch_add(1, std::memory_order_acq_rel);
			}
			const uint32_t sr = requestedRate.load(std::memory_order_relaxed);
			const float nom = requestedNominal.load(std::memory_order_relaxed);
			if (sr == 0 || nom <= 0.0f) return false;

			bool published = false;
			uint32_t read = ringRead.load(std::memory_order_relaxed);
			const uint32_t write = ringWrite.load(std::memory_order_acquire);
			while (read != write)
			{
				const float sample = ring[read & (RING_SIZE - 1)];
				++read;
				if (std::isnan(sample)) { segment.clear(); continue; }
				segment.push_back(sample);
				if (segment.size() == static_cast<size_t>(FFT_N))
				{
					AccumulateSpectrum();
					segment.erase(segment.begin(), segment.begin() + FFT_N / 2);   // 50% overlap
					if (spectra >= 2 && EstimateAndPublish(sr, nom)) published = true;
				}
			}
			ringRead.store(read, std::memory_order_release);
			return published;
		}

		Status GetStatus() const
		{
			Status s;
			for (int attempt = 0; attempt < 4; ++attempt)
			{
				const uint32_t before = statusSeq.load(std::memory_order_acquire);
				if (before & 1u) continue;
				s = status;
				if (statusSeq.load(std::memory_order_acquire) == before) break;
			}
			return s;
		}

		// Magnitude response of the currently published notch set at frequency f (tests and diagnostics).
		double ResponseDb(double f) const
		{
			const Config& c = slots[publishSeq.load(std::memory_order_acquire) & 1u];
			if (rateForResponse == 0) return 0.0;
			const double w = 2.0 * PI * f / rateForResponse;
			const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
			std::complex<double> h = 1.0;
			for (int i = 0; i < c.count; ++i)
			{
				const Notch& n = c.notch[i];
				h *= (n.b0 + n.b1 * z1 + n.b2 * z2) / (1.0 + n.a1 * z1 + n.a2 * z2);
			}
			return 20.0 * std::log10((std::max)(std::abs(h), 1e-12));
		}

		// Energy the published filter set leaves ringing after a click, from `afterSeconds` on, in dB relative to
		// the click (test/diagnostic only; runs the cascade on an impulse off the audio path). Narrow notches ring
		// for ~1/bandwidth seconds at their own frequency; ~100 of them spaced by the mains grid ring back as a
		// 20 ms-periodic tail, heard as an echo on every pick.
		double RingingTailDb(double afterSeconds, double totalSeconds) const
		{
			const Config& c = slots[publishSeq.load(std::memory_order_acquire) & 1u];
			if (rateForResponse == 0 || c.count == 0) return -300.0;
			const size_t total = static_cast<size_t>(totalSeconds * rateForResponse);
			const size_t after = static_cast<size_t>(afterSeconds * rateForResponse);
			std::vector<double> x(total, 0.0);
			x[0] = 1.0;
			for (int i = 0; i < c.count; ++i)
			{
				const Notch& n = c.notch[i];
				double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
				for (size_t s = 0; s < total; ++s)
				{
					const double in = x[s];
					const double out = n.b0 * in + n.b1 * x1 + n.b2 * x2 - n.a1 * y1 - n.a2 * y2;
					x2 = x1; x1 = in; y2 = y1; y1 = out; x[s] = out;
				}
			}
			double tail = 0.0;
			for (size_t s = after; s < total; ++s) tail += x[s] * x[s];
			return 10.0 * std::log10((std::max)(tail, 1e-30));
		}

	private:
		static constexpr double PI = 3.14159265358979323846;

		struct Notch { int k = 0; double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
		struct Config { double f0 = 0.0; int count = 0; uint32_t epoch = 0; Notch notch[MAX_HARMONIC]; };
		struct State { double x1 = 0, x2 = 0, y1 = 0, y2 = 0; };

		void MarkGap()
		{
			if (!wasIdle) return;
			wasIdle = false;
			const uint32_t write = ringWrite.load(std::memory_order_relaxed);
			const uint32_t read = ringRead.load(std::memory_order_acquire);
			if (write - read + 1 > RING_SIZE) return;
			ring[write & (RING_SIZE - 1)] = std::numeric_limits<float>::quiet_NaN();
			ringWrite.store(write + 1, std::memory_order_release);
		}

		void ApplyPublished(uint32_t seq)
		{
			const Config& slot = slots[seq & 1u];
			const Config copy = slot;
			if (publishSeq.load(std::memory_order_acquire) != seq) return;   // republished mid-copy: next block
			// Measured before the last rate / nominal change (the analysis was mid-FFT when it happened): skip it.
			if (copy.epoch != currentEpoch) { appliedSeq = seq; return; }
			// A harmonic that newly appears starts from zero state; one that stays keeps its state, so a small
			// frequency update does not click.
			std::array<bool, MAX_HARMONIC + 1> keep{};
			for (int i = 0; i < copy.count; ++i) keep[copy.notch[i].k] = true;
			for (auto& chan : states)
				for (int k = 1; k <= MAX_HARMONIC; ++k)
					if (!keep[k]) chan[k] = State{};
			active = copy;
			appliedSeq = seq;
		}

		void AccumulateSpectrum()
		{
			std::vector<std::complex<double>> buf(FFT_N);
			for (int i = 0; i < FFT_N; ++i)
			{
				const double w = 0.5 - 0.5 * std::cos(2.0 * PI * i / FFT_N);   // Hann
				buf[i] = segment[i] * w;
			}
			Fft(buf);
			const double decay = spectra == 0 ? 0.0 : 0.6;   // exponential average, ~2.5 windows memory
			for (int b = 0; b <= FFT_N / 2; ++b)
				average[b] = decay * average[b] + (1.0 - decay) * std::norm(buf[b]);
			++spectra;
		}

		static void Fft(std::vector<std::complex<double>>& a)
		{
			const size_t n = a.size();
			for (size_t i = 1, j = 0; i < n; ++i)
			{
				size_t bit = n >> 1;
				for (; j & bit; bit >>= 1) j ^= bit;
				j ^= bit;
				if (i < j) std::swap(a[i], a[j]);
			}
			for (size_t len = 2; len <= n; len <<= 1)
			{
				const std::complex<double> wl = std::polar(1.0, -2.0 * PI / static_cast<double>(len));
				for (size_t i = 0; i < n; i += len)
				{
					std::complex<double> w = 1.0;
					for (size_t j = 0; j < len / 2; ++j)
					{
						const std::complex<double> u = a[i + j], v = a[i + j + len / 2] * w;
						a[i + j] = u + v;
						a[i + j + len / 2] = u - v;
						w *= wl;
					}
				}
			}
		}

		// Power at a fractional bin (linear interpolation) and the refined peak near a target bin.
		double PowerAt(double bin) const
		{
			const int b = static_cast<int>(bin);
			if (b < 1 || b + 1 >= static_cast<int>(average.size())) return 0.0;
			const double t = bin - b;
			return average[b] * (1.0 - t) + average[b + 1] * t;
		}

		struct Line { int k; double hz; double snr; };

		bool MeasureLine(int k, double f0, double binHz, Line& line) const
		{
			const double centre = k * f0 / binHz;
			const int c = static_cast<int>(std::lround(centre));
			if (c < 20 || c + 20 >= static_cast<int>(average.size())) return false;
			int peak = c;
			for (int b = c - 2; b <= c + 2; ++b) if (average[b] > average[peak]) peak = b;
			const double l = std::log(average[peak - 1] + 1e-30), m = std::log(average[peak] + 1e-30), r = std::log(average[peak + 1] + 1e-30);
			const double denom = l - 2.0 * m + r;
			const double offset = std::fabs(denom) > 1e-12 ? std::clamp(0.5 * (l - r) / denom, -0.5, 0.5) : 0.0;
			// Neighbourhood floor: median of +/-15 bins excluding the line's own main lobe.
			double neighbours[32]; int count = 0;
			for (int b = c - 15; b <= c + 15; ++b) if (std::abs(b - peak) > 3 && count < 32) neighbours[count++] = average[b];
			std::nth_element(neighbours, neighbours + count / 2, neighbours + count);
			const double floor = neighbours[count / 2] + 1e-30;
			line.k = k;
			line.hz = (peak + offset) * binHz;
			line.snr = average[peak] / floor;
			return true;
		}

		bool EstimateAndPublish(uint32_t sr, float nom)
		{
			const double binHz = static_cast<double>(sr) / FFT_N;
			const double maxHz = (std::min)(MAX_NOTCH_HZ, sr * 0.45);

			// 1. Coarse fundamental: harmonic sum over +/-0.5 Hz of nominal (grids are held within 1%) in 0.005 Hz
			//    steps, lines up to 4 kHz.
			double bestF = nom, bestScore = -1.0;
			for (double f = nom - 0.5; f <= nom + 0.5; f += 0.005)
			{
				double score = 0.0;
				for (int k = 1; k * f <= 4000.0 && k <= MAX_HARMONIC; ++k) score += std::sqrt(PowerAt(k * f / binHz));
				if (score > bestScore) { bestScore = score; bestF = f; }
			}

			// 2. Refine: weighted least squares of k * f0 against every clear line, twice (second pass drops
			//    lines that do not sit on the comb, e.g. a ringing open string near a harmonic).
			double f0 = bestF;
			std::vector<Line> lines;
			for (int pass = 0; pass < 2; ++pass)
			{
				lines.clear();
				double num = 0.0, den = 0.0;
				for (int k = 1; k <= MAX_HARMONIC && k * f0 <= maxHz; ++k)
				{
					Line line;
					if (!MeasureLine(k, f0, binHz, line) || line.snr < 10.0) continue;
					if (pass == 1 && std::fabs(line.hz - k * f0) > 0.25 + 0.002 * k) continue;
					const double w = (std::min)(line.snr, 1000.0);
					num += w * k * line.hz;
					den += w * static_cast<double>(k) * k;
					lines.push_back(line);
				}
				if (lines.size() < 6 || den <= 0.0) return false;   // no clear comb: publish nothing new
				f0 = num / den;
			}
			if (std::fabs(f0 - nom) > 0.5) return false;

			// 2b. Persistence: hum is steady, a note's tail is not. A decaying note whose harmonics happen to
			//     sit near multiples of the grid (G2 ~ 2 x 49.5 Hz) can briefly look like a comb; require three
			//     analyses in a row to agree (within 0.02 Hz) before the first lock or before a jump > 0.05 Hz.
			if (lockedHz == 0.0 || std::fabs(f0 - lockedHz) > 0.05)
			{
				if (std::fabs(f0 - candidateHz) <= 0.02) ++candidateAgree;
				else { candidateHz = f0; candidateAgree = 1; }
				if (candidateAgree < 3) return false;
			}
			lockedHz = f0;
			candidateAgree = 0;

			// 3. Which harmonics to cut, with hysteresis: on at >= LINE_ON_SNR (16 dB) above the neighbourhood,
			//    off only after three analyses below LINE_OFF_SNR (10 dB), so lines do not flicker in and out.
			//    Lower thresholds cut many lines that are inaudible under a guitar, and every cut line adds
			//    ringing; see LINE_CUT_* below.
			std::array<double, MAX_HARMONIC + 1> snrByK{};
			for (const Line& line : lines) snrByK[line.k] = line.snr;
			double strongest = 0.0;
			for (int k = 1; k <= MAX_HARMONIC; ++k)
			{
				double snr = snrByK[k];
				if (snr == 0.0 && lineOn[k] && k * f0 <= maxHz)
				{
					Line line;
					snr = MeasureLine(k, f0, binHz, line) ? line.snr : 0.0;
				}
				if (snr > 0.0) lineSnr[k] = snr;
				if (snr >= LINE_ON_SNR) { lineOn[k] = 1; lineMiss[k] = 0; }
				else if (lineOn[k] && snr < LINE_OFF_SNR && ++lineMiss[k] >= 3) { lineOn[k] = 0; lineMiss[k] = 0; }
				if (lineOn[k] && k * f0 > maxHz) lineOn[k] = 0;
				if (lineOn[k]) strongest = (std::max)(strongest, snr);
			}

			// 4. Build and publish the notch set.
			Config& slot = slots[(publishSeq.load(std::memory_order_relaxed) + 1) & 1u];
			slot.f0 = f0;
			slot.count = 0;
			slot.epoch = seenEpoch;
			double highest = 0.0;
			for (int k = 1; k <= MAX_HARMONIC; ++k)
			{
				if (!lineOn[k]) continue;
				const double fk = k * f0;
				const double bandwidth = (std::min)(1.5 + 0.02 * k, 5.0);   // widen with k: tracking error grows with k
				const double w0 = 2.0 * PI * fk / sr;
				const double alpha = std::sin(w0) / (2.0 * (fk / bandwidth));
				// Partial-depth cut (RBJ peaking EQ with negative gain), not a full notch: bring the line down to
				// just above its neighbourhood. A full notch removes the line to silence and rings for ~1/bandwidth
				// seconds; the ringing energy scales with how much is removed, so cutting only what sticks out keeps
				// the hum gone to the ear while the comb no longer rings back as an echo.
				const double snrDb = 10.0 * std::log10((std::max)(lineSnr[k], 1.0));
				const double cutDb = (std::min)((std::max)(snrDb - LINE_CUT_HEADROOM_DB, LINE_CUT_MIN_DB), LINE_CUT_MAX_DB);
				// Poles exactly where the full notch had them (same narrow width); zeros pulled in so the centre gain
				// is g instead of 0. (An RBJ peaking cut widens as it deepens, costing the guitar 1.5-2 dB per band and
				// ringing more.)
				const double g = std::pow(10.0, -cutDb / 20.0);
				const double a0 = 1.0 + alpha;
				Notch& n = slot.notch[slot.count++];
				n.k = k;
				n.b0 = (1.0 + alpha * g) / a0; n.b1 = -2.0 * std::cos(w0) / a0; n.b2 = (1.0 - alpha * g) / a0;
				n.a1 = n.b1; n.a2 = (1.0 - alpha) / a0;
				highest = fk;
			}
			rateForResponse = sr;
			publishSeq.fetch_add(1, std::memory_order_acq_rel);

			statusSeq.fetch_add(1, std::memory_order_acq_rel);
			status.fundamentalHz = f0;
			status.lines = slot.count;
			status.highestHz = highest;
			status.strongestDb = strongest > 0.0 ? 10.0 * std::log10(strongest) : 0.0;
			++status.analyses;
			statusSeq.fetch_add(1, std::memory_order_acq_rel);
			return true;
		}

		// Audio-thread state.
		uint32_t rate = 0;
		float nominal = 0.0f;
		size_t channelCount = 0;
		uint32_t appliedSeq = 0;
		uint32_t currentEpoch = 0;   // resetEpoch as of the audio thread's last reset
		Config active{};
		std::array<std::array<State, MAX_HARMONIC + 1>, MAX_CHANNELS> states{};
		std::array<float, MAX_BLOCK_FRAMES> block{};
		uint32_t blockFrames = 0;
		double floorMs = -1.0;
		bool wasIdle = false;
		static constexpr int HISTORY = 64;
		std::array<double, HISTORY> historyEnergy{};
		std::array<uint32_t, HISTORY> historyFrames{};
		int historyPos = 0;

		// Shared.
		std::atomic<uint32_t> requestedRate{ 0 };
		std::atomic<float> requestedNominal{ 0.0f };
		std::atomic<uint32_t> resetEpoch{ 0 };
		std::atomic<uint32_t> publishSeq{ 0 };
		Config slots[2]{};
		std::atomic<uint32_t> ringWrite{ 0 };
		std::atomic<uint32_t> ringRead{ 0 };
		std::array<float, RING_SIZE> ring{};
		std::atomic<uint32_t> statusSeq{ 0 };
		Status status{};
		uint32_t rateForResponse = 0;

		// Analysis-thread state.
		uint32_t seenEpoch = UINT32_MAX;   // never equal to resetEpoch at start, so the first Analyze sizes the buffers
		std::vector<float> segment;
		std::vector<double> average;
		int spectra = 0;
		std::array<uint8_t, MAX_HARMONIC + 1> lineOn{};
		std::array<uint8_t, MAX_HARMONIC + 1> lineMiss{};
		std::array<double, MAX_HARMONIC + 1> lineSnr{};   // last measured power ratio per line (sets its cut depth)
		double lockedHz = 0.0;
		double candidateHz = 0.0;
		int candidateAgree = 0;
	};
}
