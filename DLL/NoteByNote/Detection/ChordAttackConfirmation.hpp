#pragma once

#include <cmath>
#include <cstdint>

namespace NoteByNote
{
	inline bool ConfirmChordAttack(const float* samples, uint32_t window, uint32_t rate,
		const int* tones, int toneCount)
	{
		if (!samples || !tones || toneCount < 1 || toneCount > 6 || window < 8 || rate == 0) return false;
		constexpr int MAX_COMPONENTS = 18;
		constexpr int MAX_COEFFICIENTS = MAX_COMPONENTS * 2;
		constexpr double TWO_PI = 6.283185307179586;
		constexpr double MINIMUM_RELATIVE_EXCITATION_POWER = 0.04;
		double frequencies[MAX_COMPONENTS] = {};
		bool fundamentals[MAX_COMPONENTS] = {};
		int components = 0;
		for (int tone = 0; tone < toneCount; ++tone)
		{
			if (tones[tone] < 0 || tones[tone] > 127) return false;
			const double fundamental = 440.0 * std::pow(2.0, (tones[tone] - 69.0) / 12.0);
			for (int harmonic = 1; harmonic <= 3; ++harmonic)
			{
				const double frequency = fundamental * harmonic;
				if (frequency >= rate * 0.45) continue;
				bool duplicate = false;
				for (int other = 0; other < components; ++other)
				{
					if (std::abs(frequencies[other] - frequency) < 2.0)
					{
						duplicate = true;
						if (harmonic == 1) fundamentals[other] = true;
					}
				}
				if (!duplicate)
				{
					fundamentals[components] = harmonic == 1;
					frequencies[components++] = frequency;
				}
			}
		}
		if (components == 0) return false;
		const int coefficients = components * 2;
		double fitted[3][MAX_COEFFICIENTS] = {};
		double gram[MAX_COEFFICIENTS][MAX_COEFFICIENTS] = {};
		for (int frame = 0; frame < 3; ++frame)
		{
			double matrix[MAX_COEFFICIENTS][MAX_COEFFICIENTS + 1] = {};
			double sine[MAX_COMPONENTS] = {}, cosine[MAX_COMPONENTS] = {};
			double sineStep[MAX_COMPONENTS] = {}, cosineStep[MAX_COMPONENTS] = {};
			for (int component = 0; component < components; ++component)
			{
				const double step = TWO_PI * frequencies[component] / rate;
				sineStep[component] = std::sin(step);
				cosineStep[component] = std::cos(step);
				cosine[component] = 1.0;
			}
			for (uint32_t index = 0; index < window; ++index)
			{
				const float sample = samples[frame * window + index];
				if (!std::isfinite(sample)) return false;
				const double weight = 0.5 - 0.5 * std::cos(TWO_PI * index / (window - 1));
				double basis[MAX_COEFFICIENTS];
				for (int component = 0; component < components; ++component)
				{
					basis[component * 2] = sine[component];
					basis[component * 2 + 1] = cosine[component];
					const double nextSine = sine[component] * cosineStep[component] + cosine[component] * sineStep[component];
					cosine[component] = cosine[component] * cosineStep[component] - sine[component] * sineStep[component];
					sine[component] = nextSine;
				}
				for (int row = 0; row < coefficients; ++row)
				{
					const double weighted = weight * basis[row];
					matrix[row][coefficients] += weighted * sample;
					if (frame == 0)
						for (int column = row; column < coefficients; ++column)
							gram[row][column] += weighted * basis[column];
				}
			}
			for (int row = 0; row < coefficients; ++row)
				for (int column = 0; column < coefficients; ++column)
					matrix[row][column] = row <= column ? gram[row][column] : gram[column][row];
			for (int column = 0; column < coefficients; ++column)
			{
				int pivot = column;
				for (int row = column + 1; row < coefficients; ++row)
					if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) pivot = row;
				if (std::abs(matrix[pivot][column]) < window * 1e-8) return false;
				for (int entry = column; entry <= coefficients; ++entry)
				{
					const double temporary = matrix[column][entry];
					matrix[column][entry] = matrix[pivot][entry];
					matrix[pivot][entry] = temporary;
				}
				const double divisor = matrix[column][column];
				for (int entry = column; entry <= coefficients; ++entry) matrix[column][entry] /= divisor;
				for (int row = 0; row < coefficients; ++row)
				{
					if (row == column) continue;
					const double factor = matrix[row][column];
					for (int entry = column; entry <= coefficients; ++entry) matrix[row][entry] -= factor * matrix[column][entry];
				}
			}
			for (int coefficient = 0; coefficient < coefficients; ++coefficient)
				fitted[frame][coefficient] = matrix[coefficient][coefficients];
		}
		bool hasExcitation = false;
		for (int component = 0; component < components; ++component)
		{
			double amplitude[3], phase[3];
			for (int frame = 0; frame < 3; ++frame)
			{
				const double real = fitted[frame][component * 2], imaginary = fitted[frame][component * 2 + 1];
				amplitude[frame] = std::hypot(real, imaginary);
				phase[frame] = std::atan2(imaginary, real);
			}
			const double baseline = amplitude[0] > amplitude[1] ? amplitude[0] : amplitude[1];
			const double rise = amplitude[2] > baseline ? amplitude[2] - baseline : 0.0;
			const double error = phase[2] - 2.0 * phase[1] + phase[0];
			// Damping cannot add an attack; a quieter re-pick can change phase.
			// Shared upper harmonics can change phase when one string is damped.
			// They must gain energy; only actual chord fundamentals may use phase.
			const double novelty = rise * rise + (fundamentals[component]
				? 2.0 * amplitude[2] * amplitude[1] * (1.0 - std::cos(error)) : 0.0);
			const double scale = baseline > amplitude[2] ? baseline : amplitude[2];
			if (scale * scale > 1e-6 && novelty >= scale * scale * MINIMUM_RELATIVE_EXCITATION_POWER) hasExcitation = true;
		}
		return hasExcitation;
	}
}
