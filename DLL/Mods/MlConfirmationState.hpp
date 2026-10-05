#pragma once

#include "NoteByNoteTypes.hpp"
#include "HeldPitchConfirmation.hpp"

namespace Research
{
	class MlConfirmationState
	{
	public:
		void Reset(uint64_t minimumSampleIndex)
		{
			confirmation.Reset(minimumSampleIndex);
		}

		uint64_t GetMinimumSampleIndex() const
		{
			return confirmation.GetMinimumSample();
		}

		bool Observe(const ResearchProtocol::MlNoteEvidence& evidence)
		{
			return confirmation.Observe(evidence.analyzedSampleIndex, evidence.sampleRate,
				evidence.verdict == ResearchProtocol::MlNoteVerdict::Confirmed);
		}

	private:
		NoteByNote::HeldPitchConfirmation confirmation;
	};
}
