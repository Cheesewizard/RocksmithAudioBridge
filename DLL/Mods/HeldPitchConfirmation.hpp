#pragma once

#include <cstdint>

namespace NoteByNote
{
	class HeldPitchConfirmation
	{
	public:
		void Reset(uint64_t minimumSample)
		{
			this->minimumSample = minimumSample;
			lastSample = 0;
			count = 0;
		}

		uint64_t GetMinimumSample() const { return minimumSample; }

		bool Observe(uint64_t sample, uint32_t sampleRate, bool matches)
		{
			if (!matches || sampleRate == 0 || sample <= minimumSample)
			{
				lastSample = sample;
				count = 0;
				return false;
			}
			if (sample == lastSample) return count >= 2;
			if (sample < lastSample || sample - lastSample > sampleRate / 10) count = 0;
			lastSample = sample;
			if (count < 2) ++count;
			return count >= 2;
		}

	private:
		uint64_t minimumSample = 0;
		uint64_t lastSample = 0;
		uint32_t count = 0;
	};
}
