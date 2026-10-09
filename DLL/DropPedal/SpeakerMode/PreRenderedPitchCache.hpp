#pragma once

#include <cstdint>

namespace Audio::SongShift
{
	class PreparedPitchCache;

	enum class PitchCacheProbeState
	{
		Preparing,
		Ready,
		Failed,
		Cancelled
	};

	struct PitchCacheProbeSnapshot
	{
		PitchCacheProbeState state = PitchCacheProbeState::Preparing;
		bool hasMetadata = false;
		uint32_t sampleRate = 0;
		uint64_t totalFrames = 0;
		uint64_t contiguousFramesReady = 0;
		uint64_t requestTick = 0;
		uint64_t extractionStartedTick = 0;
		uint64_t extractionCompletedTick = 0;
		uint64_t renderStartedTick = 0;
		uint64_t openingReadyTick = 0;
		uint64_t preparationCompletedTick = 0;
		uint64_t underflowWaitCount = 0;
		uint64_t lastPlaybackFrame = 0;
		uint64_t lastRequiredFrame = 0;
		uint64_t lastReadyFrameBeforeWait = 0;
	};

	namespace PreRenderedPitchCache
	{
		bool Initialize();
		PreparedPitchCache* Request(const char* bankName, int semitones);
		void Retire(PreparedPitchCache* cache);
		uint64_t Maintain();
		bool IsReady(const PreparedPitchCache* cache);
		const char* GetError(const PreparedPitchCache* cache);
		int GetSemitones(const PreparedPitchCache* cache);
		bool GetProbeSnapshot(
			const PreparedPitchCache* cache,
			PitchCacheProbeSnapshot& snapshot);
		bool MatchesAudio(
			const PreparedPitchCache* cache,
			uint64_t totalFrames,
			uint32_t sampleRate);
		bool CopyFrames(
			const PreparedPitchCache* cache,
			int16_t* destination,
			uint32_t positionStart,
			uint16_t frameCount,
			uint32_t sampleRate);
	}
}
