#include "../../stdafx.h"
#include "../../GameState.hpp"
#include "../../Mods/DropPedal/DropPedal.hpp"
#include "PreRenderedPitchCache.hpp"
#include "StreamingPitchShifter.hpp"
#include "WwiseMusicHook.hpp"

namespace
{
	constexpr LONG MAX_TRACKED_SOURCES = 256;
	constexpr LONG MAX_PROCESS_EVENTS = 64;
	static_assert((MAX_TRACKED_SOURCES & (MAX_TRACKED_SOURCES - 1)) == 0,
		"source registry indexing relies on a power-of-two size");
	static_assert((MAX_PROCESS_EVENTS & (MAX_PROCESS_EVENTS - 1)) == 0,
		"event ring indexing relies on a power-of-two size");
	constexpr int DECODER_OUTPUT_VTABLE_INDEX = 10;
	constexpr AkUInt32 AUDIOKINETIC_COMPANY_ID = 0;
	constexpr AkUInt32 VORBIS_CODEC_ID = 4;
	constexpr AkUInt32 MIN_MUSIC_DURATION_SECONDS = 20;
	// A slot is never cleared (Wwise destroys sources without telling us, and clearing would break
	// the probe chains of FindDecoderSource). Once all slots are taken, a new source reuses the one
	// idle longest, provided it has produced no audio for this long: without reuse the table fills
	// in a long session (every Vorbis source claims a slot, and each mode switch restarts the music)
	// and Speaker Mode silently stops working until the game restarts.
	// The window is long because a paused song produces no audio; a short window would let a new source
	// take a paused song's slot and switch Speaker Mode off on resume. With 256 slots the table cannot
	// fill with sources all active in the last 5 minutes.
	constexpr DWORD SOURCE_REUSE_IDLE_MS = 300000;

	constexpr LONG EVENT_FREE = 0;
	constexpr LONG EVENT_WRITING = 1;
	constexpr LONG EVENT_READY = 2;
	constexpr LONG EVENT_CONSUMING = 3;

	constexpr LONG FACTORY_HOOK_INSTALLED = 1 << 0;
	constexpr LONG FACTORY_HOOK_FAILED = 1 << 1;
	constexpr LONG FACTORY_IDENTITY_INVALID = 1 << 2;
	constexpr LONG DECODER_HOOK_INSTALLED = 1 << 3;
	constexpr LONG DECODER_HOOK_FAILED = 1 << 4;
	constexpr LONG SOURCE_REGISTRY_FULL = 1 << 5;

	enum class ProcessEventKind
	{
		FailedMissingSource,
		FailedCacheCopy,
		FailedLiveShifter
	};

	struct DecoderSourceState
	{
		volatile LONG source;
		volatile LONG nextGeneration;
		volatile LONG activeGeneration;
		volatile LONG failureReported;
		volatile LONG lastActiveTick;   // GetTickCount() when the source was claimed or last produced audio
		int generationSemitones;
		PVOID activeCache;
		Audio::SongShift::StreamingPitchShifter pitchShifter;
	};

	struct ProcessEvent
	{
		volatile LONG state;
		ProcessEventKind kind;
		uintptr_t source;
		LONG generation;
		AkUInt32 sampleRate;
		int semitones;
		int inputLatency;
		int outputLatency;
	};

	typedef void(__fastcall* tDecoderOutputRaw)(void* self, void* unused, void* outputState);

	DecoderSourceState decoderSources[MAX_TRACKED_SOURCES] = {};
	ProcessEvent processEvents[MAX_PROCESS_EVENTS] = {};

	volatile LONG processEventCursor = 0;
	volatile LONG droppedProcessEvents = 0;
	volatile LONG pendingHookStatus = 0;
	volatile LONG factoryHookAttempted = 0;
	volatile LONG decoderHookAttempted = 0;
	volatile LONG missingSourceFailureReported = 0;
	PVOID selectedSongCache = nullptr;
	std::string selectedFullSongBankName;
	int selectedSongSemitones = 0;
	volatile LONG isSelectedSongTuningSynchronized = 0;
	volatile LONG isSelectedSongPlaybackExpected = 0;
	Audio::SongShift::PreparedPitchCache* probedCache = nullptr;
	bool hasLoggedFailureProbe = false;
	uint64_t loggedUnderflowWaitCount = 0;

	tRegisterCodec originalRegisterCodec = nullptr;
	AkCreateFileSourceCallback* originalVorbisFileFactory = nullptr;
	tDecoderOutputRaw originalVorbisFileDecoderOutput = nullptr;

	DecoderSourceState* FindDecoderSource(void* source)
	{
		if (source == nullptr) return nullptr;

		const LONG sourceValue = static_cast<LONG>(reinterpret_cast<uintptr_t>(source));
		const LONG startIndex = (sourceValue >> 4) & (MAX_TRACKED_SOURCES - 1);

		for (LONG offset = 0; offset < MAX_TRACKED_SOURCES; offset++)
		{
			const LONG index = (startIndex + offset) & (MAX_TRACKED_SOURCES - 1);
			auto& sourceState = decoderSources[index];
			const LONG existingSource = InterlockedCompareExchange(&sourceState.source, 0, 0);

			if (existingSource == sourceValue) return &sourceState;
			if (existingSource == 0) return nullptr;
		}

		return nullptr;
	}

	LONG BeginDecoderGeneration(void* source)
	{
		if (source == nullptr) return 0;

		const LONG sourceValue = static_cast<LONG>(reinterpret_cast<uintptr_t>(source));
		const LONG startIndex = (sourceValue >> 4) & (MAX_TRACKED_SOURCES - 1);

		for (LONG offset = 0; offset < MAX_TRACKED_SOURCES; offset++)
		{
			const LONG index = (startIndex + offset) & (MAX_TRACKED_SOURCES - 1);
			auto& sourceState = decoderSources[index];
			const LONG existingSource = InterlockedCompareExchange(
				&sourceState.source,
				sourceValue,
				0);

			if (existingSource == sourceValue || existingSource == 0)
			{
				InterlockedExchange(&sourceState.lastActiveTick, static_cast<LONG>(GetTickCount()));
				const LONG generation = InterlockedIncrement(&sourceState.nextGeneration);
				sourceState.pitchShifter.ResetGeneration();
				sourceState.generationSemitones = 0;
				InterlockedExchangePointer(&sourceState.activeCache, nullptr);
				InterlockedExchange(&sourceState.failureReported, 0);
				MemoryBarrier();
				InterlockedExchange(&sourceState.activeGeneration, generation);
				return generation;
			}
		}

		// Full: take over the slot idle longest, in place (never through zero).
		const DWORD now = GetTickCount();
		DecoderSourceState* oldest = nullptr;
		DWORD oldestIdle = 0;
		for (auto& sourceState : decoderSources)
		{
			const DWORD idle = now - static_cast<DWORD>(InterlockedCompareExchange(&sourceState.lastActiveTick, 0, 0));
			if (idle >= SOURCE_REUSE_IDLE_MS && idle > oldestIdle) { oldest = &sourceState; oldestIdle = idle; }
		}
		if (oldest != nullptr)
		{
			const LONG previous = InterlockedCompareExchange(&oldest->source, 0, 0);
			if (previous != 0 && InterlockedCompareExchange(&oldest->source, sourceValue, previous) == previous)
			{
				InterlockedExchange(&oldest->lastActiveTick, static_cast<LONG>(now));
				const LONG generation = InterlockedIncrement(&oldest->nextGeneration);
				oldest->pitchShifter.ResetGeneration();
				oldest->generationSemitones = 0;
				InterlockedExchangePointer(&oldest->activeCache, nullptr);
				InterlockedExchange(&oldest->failureReported, 0);
				MemoryBarrier();
				InterlockedExchange(&oldest->activeGeneration, generation);
				return generation;
			}
		}

		InterlockedOr(&pendingHookStatus, SOURCE_REGISTRY_FULL);
		return 0;
	}

	bool IsSupportedMusicStream(void* outputState)
	{
		if (outputState == nullptr) return false;

		auto* words = static_cast<AkUInt32*>(outputState);
		auto* frameCounts = reinterpret_cast<AkUInt16*>(words + 3);
		const auto* data = reinterpret_cast<const AkInt16*>(words[0]);
		const AkUInt32 channelMask = words[1];
		const AkUInt16 maximumFrameCount = frameCounts[0];
		const AkUInt16 frameCount = frameCounts[1];
		const AkUInt32 totalFrames = words[8];
		const AkUInt32 sampleRate = words[9];

		if (data == nullptr || channelMask != AK_SPEAKER_SETUP_STEREO) return false;
		if (sampleRate != 44100 && sampleRate != 48000) return false;
		if (totalFrames < sampleRate * MIN_MUSIC_DURATION_SECONDS) return false;
		if (frameCount == 0 || maximumFrameCount == 0 || frameCount > maximumFrameCount) return false;
		return true;
	}

	void CaptureProcessEvent(
		DecoderSourceState* sourceState,
		void* source,
		ProcessEventKind kind,
		AkUInt32 sampleRate,
		int semitones)
	{
		const LONG startIndex = InterlockedIncrement(&processEventCursor) - 1;
		ProcessEvent* claimedEvent = nullptr;

		for (LONG offset = 0; offset < MAX_PROCESS_EVENTS; offset++)
		{
			const LONG index = (startIndex + offset) & (MAX_PROCESS_EVENTS - 1);
			auto& candidate = processEvents[index];
			if (InterlockedCompareExchange(
				&candidate.state,
				EVENT_WRITING,
				EVENT_FREE) == EVENT_FREE)
			{
				claimedEvent = &candidate;
				break;
			}
		}

		if (claimedEvent == nullptr)
		{
			InterlockedIncrement(&droppedProcessEvents);
			return;
		}

		auto& event = *claimedEvent;
		event.kind = kind;
		event.source = reinterpret_cast<uintptr_t>(source);
		event.generation = sourceState != nullptr
			? InterlockedCompareExchange(&sourceState->activeGeneration, 0, 0)
			: 0;
		event.sampleRate = sampleRate;
		event.semitones = semitones;
		event.inputLatency = sourceState != nullptr
			? sourceState->pitchShifter.GetInputLatency()
			: 0;
		event.outputLatency = sourceState != nullptr
			? sourceState->pitchShifter.GetOutputLatency()
			: 0;

		MemoryBarrier();
		InterlockedExchange(&event.state, EVENT_READY);
	}

	bool CopyCachedMusic(
		DecoderSourceState& sourceState,
		AkInt16* samples,
		AkUInt16 frameCount,
		AkUInt32 positionStart,
		AkUInt32 sampleRate)
	{
		auto* activeCache = static_cast<Audio::SongShift::PreparedPitchCache*>(
			InterlockedCompareExchangePointer(&sourceState.activeCache, nullptr, nullptr));
		return Audio::SongShift::PreRenderedPitchCache::CopyFrames(
			activeCache,
			samples,
			positionStart,
			frameCount,
			sampleRate);
	}

	Audio::SongShift::PreparedPitchCache* GetMatchingSelectedCache(
		AkUInt32 totalFrames,
		AkUInt32 sampleRate)
	{
		auto* cache = static_cast<Audio::SongShift::PreparedPitchCache*>(
			InterlockedCompareExchangePointer(&selectedSongCache, nullptr, nullptr));
		return Audio::SongShift::PreRenderedPitchCache::MatchesAudio(
			cache,
			totalFrames,
			sampleRate)
			? cache
			: nullptr;
	}

	void RetireCache(Audio::SongShift::PreparedPitchCache* cache)
	{
		if (cache == nullptr) return;
		for (auto& sourceState : decoderSources)
		{
			InterlockedCompareExchangePointer(&sourceState.activeCache, nullptr, cache);
		}
		Audio::SongShift::PreRenderedPitchCache::Retire(cache);
	}

	void ProcessMusicOutput(void* source, void* outputState)
	{
		auto* sourceState = FindDecoderSource(source);
		if (sourceState != nullptr) InterlockedExchange(&sourceState->lastActiveTick, static_cast<LONG>(GetTickCount()));
		if (!IsSupportedMusicStream(outputState))
		{
			if (sourceState != nullptr)
			{
				sourceState->pitchShifter.Bypass();
			}
			return;
		}
		if (!DropPedal::IsSpeakerModeEnabled())
		{
			if (sourceState != nullptr)
			{
				sourceState->pitchShifter.Bypass();
				sourceState->generationSemitones = 0;
				InterlockedExchangePointer(&sourceState->activeCache, nullptr);
				InterlockedExchange(&sourceState->failureReported, 0);
			}
			return;
		}

		auto* words = static_cast<AkUInt32*>(outputState);
		auto* frameCounts = reinterpret_cast<AkUInt16*>(words + 3);
		auto* samples = reinterpret_cast<AkInt16*>(words[0]);
		const AkUInt16 maximumFrameCount = frameCounts[0];
		const AkUInt16 frameCount = frameCounts[1];
		const AkUInt32 positionStart = words[6];
		const AkUInt32 totalFrames = words[8];
		const AkUInt32 sampleRate = words[9];
		const int liveSemitones = -DropPedal::GetShiftSemitones();

		if (sourceState == nullptr)
		{
			DropPedal::DisableSpeakerMode();
			if (InterlockedCompareExchange(&missingSourceFailureReported, 1, 0) == 0)
			{
				CaptureProcessEvent(
					nullptr,
					source,
					ProcessEventKind::FailedMissingSource,
					sampleRate,
					liveSemitones);
			}
			return;
		}

		auto* activeCache = static_cast<Audio::SongShift::PreparedPitchCache*>(
			InterlockedCompareExchangePointer(&sourceState->activeCache, nullptr, nullptr));
		if (activeCache == nullptr)
		{
			activeCache = GetMatchingSelectedCache(totalFrames, sampleRate);
			if (activeCache != nullptr)
			{
				sourceState->generationSemitones =
					Audio::SongShift::PreRenderedPitchCache::GetSemitones(activeCache);
				InterlockedExchangePointer(&sourceState->activeCache, activeCache);
			}
		}

		if (activeCache != nullptr)
		{
			if (!CopyCachedMusic(
				*sourceState,
				samples,
				frameCount,
				positionStart,
				sampleRate))
			{
				DropPedal::DisableSpeakerMode();
				if (InterlockedCompareExchange(&sourceState->failureReported, 1, 0) == 0)
				{
					CaptureProcessEvent(
						sourceState,
						source,
						ProcessEventKind::FailedCacheCopy,
						sampleRate,
						sourceState->generationSemitones);
				}
				return;
			}

			return;
		}

		const auto result = sourceState->pitchShifter.Process(
			samples,
			frameCount,
			maximumFrameCount,
			sampleRate,
			positionStart,
			liveSemitones);

		if (result == Audio::SongShift::StreamProcessResult::Failed)
		{
			DropPedal::DisableSpeakerMode();
			if (InterlockedCompareExchange(&sourceState->failureReported, 1, 0) == 0)
			{
				CaptureProcessEvent(
					sourceState,
					source,
					ProcessEventKind::FailedLiveShifter,
					sampleRate,
					liveSemitones);
			}
			return;
		}

	}

	void __fastcall SpyVorbisFileDecoderOutput(void* self, void* unused, void* outputState)
	{
		originalVorbisFileDecoderOutput(self, unused, outputState);
		ProcessMusicOutput(self, outputState);
	}

	void HookFileDecoderOutput(IAkSoftwareCodec* source)
	{
		if (source == nullptr || MemUtil::IsBadReadPtr(source)) return;
		if (InterlockedCompareExchange(&decoderHookAttempted, 1, 0) != 0) return;

		auto* vtable = *reinterpret_cast<uintptr_t**>(source);
		if (vtable == nullptr || MemUtil::IsBadReadPtr(vtable))
		{
			InterlockedOr(&pendingHookStatus, DECODER_HOOK_FAILED);
			return;
		}

		originalVorbisFileDecoderOutput = reinterpret_cast<tDecoderOutputRaw>(DetourFunction(
			reinterpret_cast<PBYTE>(vtable[DECODER_OUTPUT_VTABLE_INDEX]),
			reinterpret_cast<PBYTE>(SpyVorbisFileDecoderOutput)));

		InterlockedOr(
			&pendingHookStatus,
			originalVorbisFileDecoderOutput != nullptr
				? DECODER_HOOK_INSTALLED
				: DECODER_HOOK_FAILED);
	}

	IAkSoftwareCodec* __cdecl SpyVorbisFileFactory(void* context)
	{
		const bool isPreparedSongExpected = InterlockedCompareExchange(
			&isSelectedSongPlaybackExpected,
			0,
			0) != 0;
		const bool hasLeftPreSongTuner = !GameState::Menus::IsInPreSongTuner();
		if (DropPedal::IsSpeakerModeEnabled() && isPreparedSongExpected
			&& hasLeftPreSongTuner
			&& InterlockedCompareExchange(&isSelectedSongTuningSynchronized, 0, 0) == 0)
		{
			LOG_ERROR("Speaker Mode did not resolve the selected chart tuning before playback" << std::endl);
			DropPedal::DisableSpeakerMode();
		}

		// Never wait for pre-rendering on Wwise's codec thread. Blocking this factory
		// starves the mixer, so the output device repeats its last buffer as a tone.
		// Decoder output uses the live shifter until the cache matches, and CopyFrames
		// returns silence whenever playback reaches beyond the rendered frontier.

		auto* source = originalVorbisFileFactory(context);
		HookFileDecoderOutput(source);
		BeginDecoderGeneration(source);
		return source;
	}

	void HookVorbisFileFactory(
		AkCreateFileSourceCallback* fileFactory,
		AkCreateBankSourceCallback* bankFactory)
	{
		if (InterlockedCompareExchange(&factoryHookAttempted, 1, 0) != 0) return;

		if (fileFactory == nullptr || bankFactory == nullptr || fileFactory == bankFactory)
		{
			InterlockedOr(&pendingHookStatus, FACTORY_IDENTITY_INVALID);
			return;
		}

		auto* trampoline = DetourFunction(
			reinterpret_cast<PBYTE>(fileFactory),
			reinterpret_cast<PBYTE>(SpyVorbisFileFactory));

		if (trampoline == nullptr)
		{
			InterlockedOr(&pendingHookStatus, FACTORY_HOOK_FAILED);
			return;
		}

		originalVorbisFileFactory = reinterpret_cast<AkCreateFileSourceCallback*>(trampoline);
		InterlockedOr(&pendingHookStatus, FACTORY_HOOK_INSTALLED);
	}

	AKRESULT __cdecl SpyRegisterCodec(
		AkUInt32 companyId,
		AkUInt32 codecId,
		AkCreateFileSourceCallback fileFactory,
		AkCreateBankSourceCallback bankFactory)
	{
		if (companyId == AUDIOKINETIC_COMPANY_ID && codecId == VORBIS_CODEC_ID)
		{
			HookVorbisFileFactory(fileFactory, bankFactory);
		}

		return originalRegisterCodec(companyId, codecId, fileFactory, bankFactory);
	}

	void LogHookStatus()
	{
		const LONG status = InterlockedExchange(&pendingHookStatus, 0);
		if ((status & FACTORY_HOOK_FAILED) != 0)
		{
			LOG_ERROR("Speaker Mode failed to hook the Wwise Vorbis file-source factory" << std::endl);
		}
		if ((status & FACTORY_IDENTITY_INVALID) != 0)
		{
			LOG_ERROR("Speaker Mode requires distinct Wwise Vorbis file and bank factories" << std::endl);
		}
		if ((status & DECODER_HOOK_FAILED) != 0)
		{
			LOG_ERROR("Speaker Mode failed to hook Wwise Vorbis file decoder output" << std::endl);
		}
		if ((status & SOURCE_REGISTRY_FULL) != 0)
		{
			LOG_ERROR("Speaker Mode source registry is full" << std::endl);
		}
	}

	void LogProcessEvents()
	{
		for (LONG index = 0; index < MAX_PROCESS_EVENTS; index++)
		{
			auto& event = processEvents[index];
			if (InterlockedCompareExchange(
				&event.state,
				EVENT_CONSUMING,
				EVENT_READY) != EVENT_READY)
			{
				continue;
			}

			if (event.kind == ProcessEventKind::FailedMissingSource)
			{
				LOG_ERROR("Speaker Mode could not find decoder state for source 0x"
					<< std::hex << event.source
					<< std::dec << " generation " << event.generation
					<< "; retaining the original decoder audio" << std::endl);
			}
			else if (event.kind == ProcessEventKind::FailedCacheCopy)
			{
				LOG_ERROR("Speaker Mode could not copy prepared PCM for source 0x"
					<< std::hex << event.source
					<< std::dec << " generation " << event.generation
					<< "; retaining the original decoder audio" << std::endl);
			}
			else if (event.kind == ProcessEventKind::FailedLiveShifter)
			{
				LOG_ERROR("Speaker Mode live preview shifter failed for source 0x"
					<< std::hex << event.source
					<< std::dec << " generation " << event.generation
					<< "; retaining the original decoder audio" << std::endl);
			}

			InterlockedExchange(&event.state, EVENT_FREE);
		}

		const LONG dropped = InterlockedExchange(&droppedProcessEvents, 0);
		if (dropped > 0)
		{
			LOG_WARNING("Speaker Mode dropped " << dropped << " status event(s)" << std::endl);
		}
	}

	void LogCacheProbes()
	{
		auto* cache = static_cast<Audio::SongShift::PreparedPitchCache*>(
			InterlockedCompareExchangePointer(&selectedSongCache, nullptr, nullptr));
		if (cache == nullptr)
		{
			probedCache = nullptr;
			return;
		}

		if (cache != probedCache)
		{
			probedCache = cache;
			hasLoggedFailureProbe = false;
			loggedUnderflowWaitCount = 0;
		}

		Audio::SongShift::PitchCacheProbeSnapshot probe;
		if (!Audio::SongShift::PreRenderedPitchCache::GetProbeSnapshot(cache, probe)) return;

		if (probe.underflowWaitCount > loggedUnderflowWaitCount)
		{
			loggedUnderflowWaitCount = probe.underflowWaitCount;
			const double requiredSeconds = probe.sampleRate == 0
				? 0.0
				: static_cast<double>(probe.lastRequiredFrame) / probe.sampleRate;
			const double readySeconds = probe.sampleRate == 0
				? 0.0
				: static_cast<double>(probe.lastReadyFrameBeforeWait) / probe.sampleRate;
			LOG_WARNING("Speaker Mode render under-run " << probe.underflowWaitCount
				<< ": playback needs " << requiredSeconds << " s, rendered "
				<< readySeconds << " s; returning silence until the render catches up"
				<< std::endl);
		}

		if (!hasLoggedFailureProbe
			&& probe.state == Audio::SongShift::PitchCacheProbeState::Failed)
		{
			hasLoggedFailureProbe = true;
			LOG_ERROR("Speaker Mode probe: temporary audio failed: "
				<< Audio::SongShift::PreRenderedPitchCache::GetError(cache) << std::endl);
		}
	}

	void UpdateFullSongCaches()
	{
		const auto songKey = GameState::GetSongKey();
		const auto fullSongBankName = songKey.empty()
			? std::string()
			: "Song_" + songKey + ".bnk";
		if (fullSongBankName != selectedFullSongBankName)
		{
			auto* cache = static_cast<Audio::SongShift::PreparedPitchCache*>(
				InterlockedExchangePointer(&selectedSongCache, nullptr));
			RetireCache(cache);
			selectedFullSongBankName = fullSongBankName;
			selectedSongSemitones = 0;
			InterlockedExchange(&isSelectedSongTuningSynchronized, 0);
			InterlockedExchange(&isSelectedSongPlaybackExpected, 0);
		}

		if (selectedFullSongBankName.empty()) return;
		if (!DropPedal::IsSpeakerModeEnabled())
		{
			auto* cache = static_cast<Audio::SongShift::PreparedPitchCache*>(
				InterlockedExchangePointer(&selectedSongCache, nullptr));
			RetireCache(cache);
			selectedSongSemitones = 0;
			InterlockedExchange(&isSelectedSongTuningSynchronized, 0);
			InterlockedExchange(&isSelectedSongPlaybackExpected, 0);
			return;
		}
		// The tuner arms the playback expectation; after it, the sync keeps polling
		// through the load screen, because a guitar already in tune passes the tuner
		// faster than the chart tuning can be confirmed there. Gameplay arms it too:
		// some transitions never show a tuner frame at all, and without this the song
		// would keep the previous song's target with no synchronization or warning.
		// A late sync corrects the live shift and detection within its first moments.
		if (GameState::Menus::IsInPreSongTuner() || GameState::IsInSong())
		{
			InterlockedExchange(&isSelectedSongPlaybackExpected, 1);
		}
		else if (InterlockedCompareExchange(&isSelectedSongPlaybackExpected, 0, 0) == 0)
		{
			return;
		}
		if (!DropPedal::TrySynchronizeSpeakerTarget(songKey)) return;
		InterlockedExchange(&isSelectedSongTuningSynchronized, 1);

		const int semitones = -DropPedal::GetShiftSemitones();
		if (InterlockedCompareExchangePointer(&selectedSongCache, nullptr, nullptr) != nullptr
			&& selectedSongSemitones == semitones)
		{
			return;
		}

		auto* previousCache = static_cast<Audio::SongShift::PreparedPitchCache*>(
			InterlockedExchangePointer(&selectedSongCache, nullptr));
		RetireCache(previousCache);
		selectedSongSemitones = semitones;
		if (semitones != 0)
		{
			auto* requestedCache = Audio::SongShift::PreRenderedPitchCache::Request(
				selectedFullSongBankName.c_str(),
				semitones);
			InterlockedExchangePointer(&selectedSongCache, requestedCache);
		}
	}
}

void Audio::SongShift::WwiseMusicHook::Install()
{
	if (!Audio::SongShift::PreRenderedPitchCache::Initialize())
	{
		LOG_ERROR("Speaker Mode could not initialize its temporary audio directory" << std::endl);
		return;
	}
	const uintptr_t target = Wwise::Exports::func_Wwise_Sound_RegisterCodec.Get();
	if (target == 0 || MemUtil::IsBadReadPtr(reinterpret_cast<void*>(target)))
	{
		LOG_ERROR("Speaker Mode could not resolve Wwise RegisterCodec" << std::endl);
		return;
	}

	originalRegisterCodec = reinterpret_cast<tRegisterCodec>(DetourFunction(
		reinterpret_cast<PBYTE>(target),
		reinterpret_cast<PBYTE>(SpyRegisterCodec)));

	if (originalRegisterCodec == nullptr)
	{
		LOG_ERROR("Speaker Mode failed to hook Wwise RegisterCodec at 0x"
			<< std::hex << target << std::dec << std::endl);
		return;
	}
}

void Audio::SongShift::WwiseMusicHook::Poll()
{
	LogHookStatus();
	LogProcessEvents();

	// The cache pass walks a pointer chain, builds strings and takes the registry
	// mutex; none of that needs frame rate, and the chart sync throttles its own
	// reads to 250 ms anyway.
	static uint64_t nextCachePassTick = 0;
	const uint64_t currentTick = GetTickCount64();
	if (currentTick < nextCachePassTick) return;
	nextCachePassTick = currentTick + 250;

	UpdateFullSongCaches();
	LogCacheProbes();
	Audio::SongShift::PreRenderedPitchCache::Maintain();
}
