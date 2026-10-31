#pragma once

#include <array>
#include <algorithm>
#include <cmath>

namespace Audio::SharedOutput
{
	// QPC is the session clock. Ratio is source frames per physical output frame.
	class OutputRateMatcher
	{
	public:
		OutputRateMatcher()
		{
			constexpr double PI = 3.14159265358979323846;
			for (unsigned phaseIndex = 0; phaseIndex <= PHASES; ++phaseIndex)
			{
				double sum = 0;
				for (unsigned tap = 0; tap < TAPS; ++tap)
				{
					const double distance = double(tap) - 15 - double(phaseIndex) / PHASES;
					const double sinc = std::abs(distance) < 1e-12 ? 0.94 : std::sin(PI * distance * 0.94) / (PI * distance);
					const double window = 0.42 + 0.5 * std::cos(PI * distance / 16) + 0.08 * std::cos(2 * PI * distance / 16);
					coefficients[phaseIndex][tap] = sinc * window;
					sum += coefficients[phaseIndex][tap];
				}
				for (auto& coefficient : coefficients[phaseIndex]) coefficient /= sum;
			}
		}

		void Reset()
		{
			history.fill(0);
			phase = 0;
			ratio = 1;
			correctionPpm = 0;
			enabled = false;
		}

		// Recover from a FIFO discontinuity (an overrun drop or a stale-frame
		// expiry) without re-cooling the servo. The sample stream jumped, so the
		// interpolation history and sub-frame phase are stale and cleared; the two
		// clocks did not change, so the learned ratio and correction are kept.
		// A cold reset here would stall convergence, let the FIFO back up again,
		// and turn a single drop into sustained crackle.
		void Reseed()
		{
			history.fill(0);
			phase = 0;
		}

		void Measure(double hardwarePpm, double queueErrorFrames)
		{
			if (!std::isfinite(hardwarePpm) || std::abs(hardwarePpm) > 5000) return;
			const double target = std::clamp(-hardwarePpm + std::clamp(queueErrorFrames * 0.5, -500.0, 500.0), -2000.0, 2000.0);
			correctionPpm += std::clamp(target - correctionPpm, -50.0, 50.0);
			ratio = 1 + correctionPpm / 1000000;
			enabled = true;
		}

		double GetCorrectionPpm() const { return correctionPpm; }

		template<class Sample>
		unsigned Read(Sample sample, unsigned available, float* output, unsigned capacity, unsigned& consumed)
		{
			unsigned produced = 0;
			double position = phase;
			while (produced < capacity)
			{
				const auto frame = static_cast<unsigned>(position);
				if (frame >= available || (enabled && frame + 16 >= available)) break;
				for (unsigned channel = 0; channel < 2; ++channel)
				{
					double value = 0;
					if (!enabled) value = sample(frame, channel);
					else
					{
						const double fractional = (position - frame) * PHASES;
						const auto index = static_cast<unsigned>(fractional);
						const double blend = fractional - index;
						for (unsigned tap = 0; tap < TAPS; ++tap)
						{
							const int offset = static_cast<int>(frame + tap) - 15;
							const float input = offset < 0 ? history[(16 + offset) * 2 + channel] : sample(static_cast<unsigned>(offset), channel);
							const double coefficient = coefficients[index][tap] * (1 - blend) + coefficients[index + 1][tap] * blend;
							value += input * coefficient;
						}
					}
					output[produced * 2 + channel] = static_cast<float>(value);
				}
				++produced;
				position += ratio;
			}
			consumed = std::min(available, static_cast<unsigned>(position));
			std::array<float, 32> nextHistory{};
			for (int frame = 0; frame < 16; ++frame)
			{
				const int offset = static_cast<int>(consumed) - 16 + frame;
				for (unsigned channel = 0; channel < 2; ++channel)
					nextHistory[frame * 2 + channel] = offset < 0 ? history[(16 + offset) * 2 + channel] : sample(static_cast<unsigned>(offset), channel);
			}
			history = nextHistory;
			phase = position - consumed;
			return produced;
		}

	private:
		static constexpr unsigned PHASES = 256;
		static constexpr unsigned TAPS = 32;
		std::array<std::array<double, TAPS>, PHASES + 1> coefficients{};
		std::array<float, 32> history{};
		double phase = 0, ratio = 1, correctionPpm = 0;
		bool enabled = false;
	};
}
