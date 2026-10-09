#include "stdafx.h"
#include "RawPitchVerifier.hpp"
#include "HfcOnsetDetector.hpp"

#include <atomic>
#include <cmath>
#include <cstring>

namespace
{
	// ~0.68 s at 48 kHz. Power of two so wrap arithmetic is a mask. The query window is
	// capped at 0.3 s, well under half the ring, so the audio thread cannot overwrite a
	// window while a reader copies it unless the reader stalls for >0.3 s - and a torn
	// tail merely perturbs an energy measurement, it cannot fault.
	constexpr uint32_t RING_CAPACITY = 32768;
	constexpr uint32_t RING_MASK = RING_CAPACITY - 1;

	std::atomic<float> ring[RING_CAPACITY] = {};
	std::atomic<uint64_t> ringWriteCount{ 0 };
	std::atomic<uint32_t> ringSampleRate{ 0 };
	std::atomic<uint64_t> ringGeneration{ 0 };

	// One Goertzel pass over a window for a single frequency. Returns power normalized
	// by (n/2)^2: a full-scale sine at the frequency reads ~1.0 for any window length.
	float GoertzelPower(const float* window, uint32_t count, double frequencyHz, double sampleRate)
	{
		if (count < 8 || frequencyHz <= 0.0 || frequencyHz >= sampleRate * 0.5) return 0.0f;
		const double omega = 6.283185307179586 * frequencyHz / sampleRate;
		const double coefficient = 2.0 * std::cos(omega);
		double s0 = 0.0;
		double s1 = 0.0;
		double s2 = 0.0;
		for (uint32_t index = 0; index < count; ++index)
		{
			s0 = static_cast<double>(window[index]) + coefficient * s1 - s2;
			s2 = s1;
			s1 = s0;
		}
		const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
		const double half = static_cast<double>(count) * 0.5;
		const double normalized = power / (half * half);
		return normalized > 0.0 ? static_cast<float>(normalized) : 0.0f;
	}
	bool FitNeighbourPowers(const float* samples, uint32_t count, double frequency, double sampleRate, float* powers, double* targetPhase = nullptr)
	{
		if (count < 8) return false;
		double sine[3] = {};
		double cosine[3] = { 1.0, 1.0, 1.0 };
		double sineStep[3] = {};
		double cosineStep[3] = {};
		for (int band = 0; band < 3; ++band)
		{
			const double probe = frequency * std::pow(2.0, (band - 1) / 12.0);
			if (probe <= 0.0 || probe >= sampleRate * 0.5) return false;
			const double step = 6.283185307179586 * probe / sampleRate;
			sineStep[band] = std::sin(step);
			cosineStep[band] = std::cos(step);
		}
		double matrix[6][7] = {};
		double weightSine = 0.0;
		double weightCosine = 1.0;
		const double weightSineStep = std::sin(6.283185307179586 / (count - 1));
		const double weightCosineStep = std::cos(6.283185307179586 / (count - 1));
		for (uint32_t index = 0; index < count; ++index)
		{
			if (!std::isfinite(samples[index])) return false;
			const double weight = 0.5 - 0.5 * weightCosine;
			double basis[6];
			for (int band = 0; band < 3; ++band)
			{
				basis[band * 2] = sine[band];
				basis[band * 2 + 1] = cosine[band];
				const double next = sine[band] * cosineStep[band] + cosine[band] * sineStep[band];
				cosine[band] = cosine[band] * cosineStep[band] - sine[band] * sineStep[band];
				sine[band] = next;
			}
			for (int row = 0; row < 6; ++row)
			{
				const double weighted = weight * basis[row];
				matrix[row][6] += weighted * samples[index];
				for (int column = row; column < 6; ++column) matrix[row][column] += weighted * basis[column];
			}
			const double nextWeightSine = weightSine * weightCosineStep + weightCosine * weightSineStep;
			weightCosine = weightCosine * weightCosineStep - weightSine * weightSineStep;
			weightSine = nextWeightSine;
		}
		for (int row = 0; row < 6; ++row)
			for (int column = 0; column < row; ++column) matrix[row][column] = matrix[column][row];
		// Fit the three tones together: independent spectral bins otherwise count
		// the target's leakage and interference as new energy in a neighbouring note.
		for (int column = 0; column < 6; ++column)
		{
			int pivot = column;
			for (int row = column + 1; row < 6; ++row)
				if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) pivot = row;
			if (std::abs(matrix[pivot][column]) < count * 1e-8) return false;
			for (int entry = column; entry < 7; ++entry)
			{
				const double temporary = matrix[column][entry];
				matrix[column][entry] = matrix[pivot][entry];
				matrix[pivot][entry] = temporary;
			}
			const double divisor = matrix[column][column];
			for (int entry = column; entry < 7; ++entry) matrix[column][entry] /= divisor;
			for (int row = 0; row < 6; ++row)
			{
				if (row == column) continue;
				const double factor = matrix[row][column];
				for (int entry = column; entry < 7; ++entry) matrix[row][entry] -= factor * matrix[column][entry];
			}
		}
		for (int band = 0; band < 3; ++band)
		{
			const double a = matrix[band * 2][6];
			const double b = matrix[band * 2 + 1][6];
			powers[band] = static_cast<float>(a * a + b * b);
			if (!std::isfinite(powers[band])) return false;
		}
		if (targetPhase != nullptr) *targetPhase = std::atan2(matrix[3][6], matrix[2][6]);
		return true;
	}

}

void RawPitchVerifier::Observe(uint32_t routeIndex, const float* samples, uint32_t count,
	uint32_t sampleRate)
{
	if (routeIndex != 0 || samples == nullptr || count == 0) return;
	ringGeneration.fetch_add(1, std::memory_order_acq_rel);
	if (sampleRate != 0) ringSampleRate.store(sampleRate, std::memory_order_relaxed);

	uint64_t writeCount = ringWriteCount.load(std::memory_order_relaxed);
	for (uint32_t index = 0; index < count; ++index)
	{
		ring[(writeCount + index) & RING_MASK].store(samples[index], std::memory_order_relaxed);
	}
	ringWriteCount.store(writeCount + count, std::memory_order_release);
	ringGeneration.fetch_add(1, std::memory_order_release);
}

namespace
{
	class RawAttackDetector
	{
		HfcOnsetDetector detector;
		bool ready = false;
		uint32_t rate = 0;
		uint32_t hop = 0;
		uint64_t cursor = 0;
		uint64_t origin = 0;
		uint64_t pending[64] = {};
		uint32_t pendingHead = 0;
		uint32_t pendingCount = 0;

		void Clear()
		{
			ready = false;
			pendingHead = 0;
			pendingCount = 0;
		}

	public:
		bool Query(const RawPitchVerifier::AudioSnapshot& snapshot, uint64_t afterSample,
			RawPitchVerifier::RawAttackBatch& out)
		{
			out.endSampleIndex = snapshot.endSampleIndex;
			out.sampleRate = snapshot.sampleRate;
			const uint64_t start = snapshot.endSampleIndex - snapshot.sampleCount;
			if (!ready || rate != snapshot.sampleRate || afterSample == 0
				|| cursor < start || cursor > snapshot.endSampleIndex || afterSample > cursor
				|| cursor - origin > 0xf0000000ULL)
			{
				Clear();
				rate = snapshot.sampleRate;
				const uint32_t window = rate <= 24000 ? 512 : (rate <= 48000 ? 1024 : 2048);
				hop = window / 4;
				if (!detector.Init(window, rate)) return false;
				detector.SetMinimumGapMs(40.0f);
				detector.SetSilenceDb(-65.0f);
				ready = true;
				// Prime spectral and peak-picking history without turning an already
				// ringing note into a new attack when practice starts or the stream resets.
				const uint32_t prime = window + 4 * hop;
				if (snapshot.sampleCount < prime) { Clear(); return false; }
				origin = snapshot.endSampleIndex - prime;
				for (cursor = origin; cursor + hop <= snapshot.endSampleIndex; cursor += hop)
					detector.Process(snapshot.samples + cursor - start);
				out.reset = 1;
				return true;
			}
			while (cursor + hop <= snapshot.endSampleIndex && out.count < 64)
			{
				const float* input = snapshot.samples + cursor - start;
				for (uint32_t index = 0; index < hop; ++index)
				{
					if (!std::isfinite(input[index])) { Clear(); return false; }
				}
				const bool onset = detector.Process(input);
				cursor += hop;
				auto& frame = out.frames[out.count++];
				frame.endSampleIndex = cursor;
				frame.inputLevelDb = HfcOnsetDetector::LevelDb(input, hop);
				if (onset)
				{
					if (pendingCount == 64) { Clear(); return false; }
					pending[(pendingHead + pendingCount++) % 64] = origin + detector.LastOnset();
				}
				// Preserve the original onset timestamp while collecting enough audio
				// to reject sustain fluctuations before they can close a valid pick.
				if (pendingCount != 0 && cursor >= pending[pendingHead] + rate / 20)
				{
					frame.attackSampleIndex = pending[pendingHead];
					pendingHead = (pendingHead + 1) % 64;
					--pendingCount;
				}
			}
			return true;
		}
	};
}

bool RawPitchVerifier::QueryAttacks(uint64_t afterSampleIndex, RawAttackBatch& out)
{
	out = {};
	static thread_local AudioSnapshot snapshot;
	static thread_local RawAttackDetector detector;
	if (!CaptureSnapshot(snapshot)) return false;
	return detector.Query(snapshot, afterSampleIndex, out);
}

bool RawPitchVerifier::CaptureSnapshot(AudioSnapshot& out)
{
	out.sampleCount = 0;
	const uint64_t generation = ringGeneration.load(std::memory_order_acquire);
	if ((generation & 1) != 0) return false;
	const uint32_t sampleRate = ringSampleRate.load(std::memory_order_relaxed);
	const uint64_t end = ringWriteCount.load(std::memory_order_acquire);
	if (sampleRate == 0 || end < static_cast<uint32_t>(0.15f * sampleRate)) return false;
	uint32_t count = static_cast<uint32_t>(0.3f * sampleRate);
	if (count > RING_CAPACITY / 2) count = RING_CAPACITY / 2;
	if (count > end) count = static_cast<uint32_t>(end);
	for (uint32_t index = 0; index < count; ++index)
	{
		out.samples[index] = ring[(end - count + index) & RING_MASK].load(std::memory_order_relaxed);
	}
	if (ringGeneration.load(std::memory_order_acquire) != generation) return false;
	out.endSampleIndex = end;
	out.sampleRate = sampleRate;
	out.sampleCount = count;
	return true;
}

bool RawPitchVerifier::MeasureSnapshot(const AudioSnapshot& snapshot, double frequencyHz,
	float windowSeconds, float& power, float& rms, uint32_t& sampleCount)
{
	power = 0.0f;
	rms = 0.0f;
	sampleCount = 0;
	if (!std::isfinite(windowSeconds) || windowSeconds < 0.05f || windowSeconds > 0.15f
		|| !std::isfinite(frequencyHz) || frequencyHz <= 0.0
		|| frequencyHz >= snapshot.sampleRate * 0.5 || snapshot.sampleCount > RING_CAPACITY / 2) return false;
	const uint32_t count = static_cast<uint32_t>(windowSeconds * snapshot.sampleRate);
	if (count < 8 || count > snapshot.sampleCount) return false;
	const float* window = snapshot.samples + snapshot.sampleCount - count;
	double sumSquares = 0.0;
	for (uint32_t index = 0; index < count; ++index)
	{
		sumSquares += static_cast<double>(window[index]) * window[index];
	}
	power = GoertzelPower(window, count, frequencyHz, snapshot.sampleRate);
	rms = static_cast<float>(std::sqrt(sumSquares / count));
	sampleCount = count;
	return true;
}

bool RawPitchVerifier::ConfirmSnapshot(const AudioSnapshot& snapshot, double frequencyHz,
	NoteConfirmation& out)
{
	out = {};
	if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0) return false;
	out.endSampleIndex = snapshot.endSampleIndex;
	out.sampleRate = snapshot.sampleRate;
	const bool useHarmonics = frequencyHz < 160.0;
	const int firstHarmonic = useHarmonics ? 2 : 1;
	const int lastHarmonic = useHarmonics ? 3 : 1;
	if (snapshot.sampleRate == 0) return false;
	const float availableWindow = static_cast<float>(snapshot.sampleCount) / snapshot.sampleRate;
	const uint32_t attackCount = static_cast<uint32_t>((frequencyHz >= 330.0 ? 0.02f : 0.15f) * snapshot.sampleRate);
	if (attackCount < 8 || snapshot.sampleCount < attackCount || snapshot.sampleCount > 16384) return false;
	float attackBands[3] = {};
	float lastBands[3] = {};
	float precedingBands[3] = {};
	// Preserve a short attack peak instead of averaging it into the following decay.
	// All three bands use identical windows so a neighbour's new energy cannot be
	// attributed to the still-ringing target when comparing with its pre-pick baseline.
	for (uint32_t end = attackCount; end <= snapshot.sampleCount; end += attackCount / 2)
	{
		float sums[3] = {};
		for (int harmonic = firstHarmonic; harmonic <= lastHarmonic; ++harmonic)
		{
			float powers[3] = {};
			if (!FitNeighbourPowers(snapshot.samples + end - attackCount, attackCount,
				frequencyHz * harmonic, snapshot.sampleRate, powers)) return false;
			for (int band = 0; band < 3; ++band) sums[band] += powers[band];
		}
		for (int band = 0; band < 3; ++band)
		{
			if (sums[band] > attackBands[band]) attackBands[band] = sums[band];
			precedingBands[band] = end == attackCount ? sums[band] : lastBands[band];
			lastBands[band] = sums[band];
		}
	}
	out.attackPower = attackBands[1];
	out.attackMinusPower = lastBands[0] < precedingBands[0] ? lastBands[0] : precedingBands[0];
	out.attackPlusPower = lastBands[2] < precedingBands[2] ? lastBands[2] : precedingBands[2];
	if (availableWindow < 0.05f) return true;
	if (frequencyHz < 330.0 && availableWindow < 0.15f) return true;
	const float fullWindow = availableWindow < 0.15f ? availableWindow : 0.15f;
	const float recentWindow = frequencyHz < 330.0 ? fullWindow : 0.05f;
	bool confirmed = true;
	float sustainedTarget = 0.0f;
	for (int harmonic = firstHarmonic; harmonic <= lastHarmonic; ++harmonic)
	{
		for (int window = 0; window < (recentWindow == fullWindow ? 1 : 2); ++window)
		{
			const float duration = window == 0 ? recentWindow : fullWindow;
			float bands[3] = {};
			for (int band = 0; band < 3; ++band)
			{
				// Give the target and BOTH adjacent notes the same intonation allowance.
				for (int detune = -1; detune <= 1; ++detune)
				{
					const double cents = (band - 1) * 100.0 + detune * 25.0;
					const double probeHz = frequencyHz * harmonic * std::pow(2.0, cents / 1200.0);
					float power = 0.0f;
					float rms = 0.0f;
					uint32_t count = 0;
					if (!MeasureSnapshot(snapshot, probeHz, duration, power, rms, count)
						|| !std::isfinite(power) || !std::isfinite(rms)) return false;
					if (power > bands[band]) bands[band] = power;
				}
			}
			const float neighbour = bands[0] > bands[2] ? bands[0] : bands[2];
			if (duration == fullWindow) sustainedTarget = bands[1];
			const bool supportsTarget = bands[1] >= 1e-6f && bands[1] > neighbour * 2.0f;
			if (!supportsTarget || out.targetPower == 0.0f)
			{
				out.targetPower = bands[1];
				out.neighbourPower = neighbour;
			}
			confirmed = confirmed && supportsTarget;
		}
	}
	// Preserve the lower-note harmonic rejection when measuring the fundamental.
	if (confirmed && !useHarmonics)
	{
		const double ratios[] = { 0.5, 1.0 / 3.0, 1.5 };
		float powers[3] = {};
		for (int band = 0; band < 3; ++band)
		{
			for (int detune = -1; detune <= 1; ++detune)
			{
				float power = 0.0f;
				float rms = 0.0f;
				uint32_t count = 0;
				const double probeHz = frequencyHz * ratios[band] * std::pow(2.0, detune * 25.0 / 1200.0);
				if (!MeasureSnapshot(snapshot, probeHz, fullWindow, power, rms, count)
					|| !std::isfinite(power) || !std::isfinite(rms)) return false;
				if (power > powers[band]) powers[band] = power;
			}
		}
		const bool lowerFundamental = (powers[0] >= 2e-5f && powers[0] >= sustainedTarget * 0.4f)
			|| (powers[1] >= 2e-5f && powers[1] >= sustainedTarget * 0.4f);
		const bool lowerOctave = powers[2] >= 1e-5f && powers[2] >= sustainedTarget * 0.1f
			&& powers[0] >= sustainedTarget * 0.02f;
		confirmed = !lowerFundamental && !lowerOctave;
	}
	out.confirmed = confirmed;
	return true;
}

namespace
{
	float MeasureAttackChange(const RawPitchVerifier::AudioSnapshot& snapshot, double frequency, uint64_t attackSample)
	{
		const uint32_t window = snapshot.sampleRate / (frequency < 330.0 ? 20 : 50);
		const uint64_t start = snapshot.endSampleIndex - snapshot.sampleCount;
		if (attackSample < start + 2 * window || attackSample + window > snapshot.endSampleIndex) return 0.0f;
		double change = 0.0;
		double reference = 0.0;
		const int firstHarmonic = frequency < 160.0 ? 2 : 1;
		const int lastHarmonic = frequency < 160.0 ? 3 : 1;
		for (int harmonic = firstHarmonic; harmonic <= lastHarmonic; ++harmonic)
		{
			double phase[3] = {};
			double amplitude[3] = {};
			for (int frame = 0; frame < 3; ++frame)
			{
				float powers[3] = {};
				const uint64_t offset = attackSample - start - 2 * window + frame * window;
				if (!FitNeighbourPowers(snapshot.samples + offset, window, frequency * harmonic,
					snapshot.sampleRate, powers, &phase[frame])) return 0.0f;
				amplitude[frame] = std::sqrt(powers[1]);
			}
			// Complex-domain prediction: a sustain continues its phase progression
			// and decay. A re-pick can break that prediction even when it is quieter.
			double predicted = amplitude[1] + (amplitude[1] < amplitude[0] ? amplitude[1] - amplitude[0] : 0.0);
			if (predicted < 0.0 || amplitude[0] < amplitude[2] * 0.1) predicted = 0.0;
			const double phaseError = phase[2] - 2.0 * phase[1] + phase[0];
			change += amplitude[2] * amplitude[2] + predicted * predicted
				- 2.0 * amplitude[2] * predicted * std::cos(phaseError);
			const double scale = amplitude[0] > amplitude[2] ? amplitude[0] : amplitude[2];
			reference += scale * scale;
		}
		return reference > 1.0e-8 ? static_cast<float>(std::sqrt((change > 0.0 ? change : 0.0) / reference)) : 0.0f;
	}
}

bool RawPitchVerifier::QueryNoteConfirmation(double frequencyHz, NoteConfirmation& out, uint64_t minimumSampleIndex, uint64_t maximumSampleIndex)
{
	static thread_local AudioSnapshot snapshot;
	out = {};
	if (!CaptureSnapshot(snapshot)) return false;
	out.endSampleIndex = snapshot.endSampleIndex;
	out.sampleRate = snapshot.sampleRate;
	if (maximumSampleIndex != 0)
	{
		if (maximumSampleIndex > snapshot.endSampleIndex) return false;
		const uint64_t excluded = snapshot.endSampleIndex - maximumSampleIndex;
		if (excluded >= snapshot.sampleCount) return false;
		snapshot.sampleCount -= static_cast<uint32_t>(excluded);
		snapshot.endSampleIndex = maximumSampleIndex;
	}
	const float attackChange = minimumSampleIndex != 0 ? MeasureAttackChange(snapshot, frequencyHz, minimumSampleIndex) : 0.0f;
	if (minimumSampleIndex != 0)
	{
		if (minimumSampleIndex >= snapshot.endSampleIndex) return false;
		const uint64_t available = snapshot.endSampleIndex - minimumSampleIndex;
		if (available < snapshot.sampleCount)
		{
			const uint32_t count = static_cast<uint32_t>(available);
			const uint32_t offset = snapshot.sampleCount - count;
			for (uint32_t index = 0; index < count; ++index) snapshot.samples[index] = snapshot.samples[offset + index];
			snapshot.sampleCount = count;
		}
	}
	const bool available = ConfirmSnapshot(snapshot, frequencyHz, out);
	out.attackChange = attackChange;
	return available;
}

bool RawPitchVerifier::IsReady()
{
	return ringSampleRate.load(std::memory_order_relaxed) != 0
		&& ringWriteCount.load(std::memory_order_acquire) >= RING_CAPACITY / 4;
}

bool RawPitchVerifier::QueryToneEvidence(double frequencyHz, float windowSeconds,
	ToneEvidence& out)
{
	out = {};
	const uint32_t sampleRate = ringSampleRate.load(std::memory_order_relaxed);
	if (sampleRate == 0 || frequencyHz <= 0.0) return false;

	if (windowSeconds < 0.05f) windowSeconds = 0.05f;
	if (windowSeconds > 0.3f) windowSeconds = 0.3f;
	uint32_t windowCount = static_cast<uint32_t>(windowSeconds * static_cast<float>(sampleRate));
	if (windowCount > RING_CAPACITY / 2) windowCount = RING_CAPACITY / 2;

	const uint64_t writeCount = ringWriteCount.load(std::memory_order_acquire);
	if (writeCount < windowCount) return false;

	// Copy the newest windowCount samples out of the ring (oldest-first order; Goertzel
	// does not care about absolute phase, only that the samples are contiguous).
	static thread_local float window[RING_CAPACITY / 2];
	const uint64_t start = writeCount - windowCount;
	for (uint32_t index = 0; index < windowCount; ++index)
	{
		window[index] = ring[(start + index) & RING_MASK].load(std::memory_order_relaxed);
	}

	double sumSquares = 0.0;
	for (uint32_t index = 0; index < windowCount; ++index)
	{
		sumSquares += static_cast<double>(window[index]) * window[index];
	}

	constexpr double SEMITONE = 1.0594630943592953;
	out.sampleRate = sampleRate;
	out.windowSampleCount = windowCount;
	out.totalRms = static_cast<float>(std::sqrt(sumSquares / windowCount));
	out.targetPower = GoertzelPower(window, windowCount, frequencyHz, sampleRate);
	out.minusOnePower = GoertzelPower(window, windowCount, frequencyHz / SEMITONE, sampleRate);
	out.plusOnePower = GoertzelPower(window, windowCount, frequencyHz * SEMITONE, sampleRate);
	out.minusTwoPower = GoertzelPower(window, windowCount, frequencyHz / (SEMITONE * SEMITONE), sampleRate);
	out.plusTwoPower = GoertzelPower(window, windowCount, frequencyHz * SEMITONE * SEMITONE, sampleRate);
	return true;
}
