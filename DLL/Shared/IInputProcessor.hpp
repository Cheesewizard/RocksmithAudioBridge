#pragma once

#include "CaptureFormat.h"

namespace Audio
{
	// Implementations run on the game's audio thread, inside IAudioCaptureClient::GetBuffer.
	// Process must not allocate, lock, log, or block for any reason.
	class IInputProcessor
	{
	public:
		virtual ~IInputProcessor() = default;

		// Called off the audio thread before any Process call, and again whenever the
		// capture format changes. Implementations do their allocating here.
		virtual void Prepare(const CaptureFormat& format) = 0;

		// Mono working samples. Return true only when samples were changed, so the
		// capture hook can preserve the original packet exactly during bypass.
		virtual bool Process(float* samples, uint32_t frameCount) = 0;

		// Reported processing delay in frames; zero when bypassed. This is not
		// an end-to-end hardware latency measurement.
		virtual uint32_t GetLatencyFrames() const = 0;
	};
}
