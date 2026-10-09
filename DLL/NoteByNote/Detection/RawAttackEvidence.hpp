#pragma once

#include <cstdint>

namespace RawPitchVerifier
{
	struct RawAttackFrame
	{
		uint64_t endSampleIndex = 0;
		uint64_t attackSampleIndex = 0;
		float inputLevelDb = -160.0f;
	};

	struct RawAttackBatch
	{
		uint64_t endSampleIndex = 0;
		uint32_t sampleRate = 0;
		uint32_t count = 0;
		uint32_t reset = 0;
		RawAttackFrame frames[64] = {};
	};

	// The raw route audio window behind chord/dyad/unison/mute attack confirmation. A shared POD
	// so it can cross the host/probe boundary (HostApi::CaptureRawSnapshot) without pulling in the
	// full verifier (and its onset detector). The verifier's real implementation fills it in the host.
	struct AudioSnapshot
	{
		uint64_t endSampleIndex = 0;
		uint32_t sampleRate = 0;
		uint32_t sampleCount = 0;
		float samples[16384] = {};
	};
}
