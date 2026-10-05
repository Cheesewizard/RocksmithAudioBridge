#include "stdafx.h"
#include "MlAudioExporter.hpp"
#include "../Research/ResearchBridge.hpp"
#include "../Mods/ArrangementInstrument.hpp"

#include <atomic>
#include <cstddef>

namespace
{
	using MlAudioExporter::SharedHeader;

	// The layout is a cross-process, cross-bitness contract; pin it here so a stray
	// member edit fails the build instead of silently desynchronizing the companion.
	static_assert(sizeof(SharedHeader) == 64, "the ML shared header must stay 64 bytes");
	static_assert(offsetof(SharedHeader, writeIndex) == 16,
		"writeIndex must stay 8-aligned for interlocked access from both processes");
	static_assert(offsetof(SharedHeader, resultSeq) == 24, "mailbox layout is a contract");
	static_assert(offsetof(SharedHeader, expectedInstrument) == 36, "the companion reads the instrument at 36");
	static_assert(offsetof(SharedHeader, resultSampleIndexAtWindowEnd) == 40,
		"the mailbox sample index must stay 8-aligned");

	constexpr uint32_t RING_MASK = MlAudioExporter::ML_AUDIO_RING_SAMPLES - 1;
	constexpr size_t MAPPING_BYTES = sizeof(SharedHeader)
		+ static_cast<size_t>(MlAudioExporter::ML_AUDIO_RING_SAMPLES) * sizeof(float);

	HANDLE mappingHandle = nullptr;
	bool creationFailed = false;
	std::atomic<SharedHeader*> sharedHeader{ nullptr };
	std::atomic<float*> sharedRing{ nullptr };

	// The applied shift comes from DropPedal state that is not audio-thread-safe to walk
	// (settings-backed), so the game loop refreshes this cache and the audio thread only
	// copies the atomic into the header. Worst case the header lags the shifter by one
	// game-loop tick, well inside the companion's 100 ms analysis cadence.
	std::atomic<int32_t> cachedAppliedShift{ 0 };

	// The audio thread is the only ring writer, so this local mirror needs no interlocked
	// read; the shared writeIndex is published with InterlockedExchange64 because a plain
	// 64-bit store is not atomic in a 32-bit process and the companion reads it raw.
	std::atomic<uint64_t> localWriteCount{ 0 };

	std::atomic<int32_t> cachedInstrument{ static_cast<int32_t>(ArrangementInstrument::Kind::Guitar) };

	// Game memory outside a song (menus, loading) can be unmapped, so the pointer walk is SEH
	// guarded and falls back to guitar.
	int32_t ReadArrangementInstrument_SEH()
	{
		using namespace ArrangementInstrument;
		__try
		{
			const uintptr_t root = *reinterpret_cast<const uintptr_t*>(DETECTION_ROOT);
			if (root == 0) return static_cast<int32_t>(Kind::Guitar);
			const uintptr_t arrangement = *reinterpret_cast<const uintptr_t*>(root + PLAYER_ONE_ARRANGEMENT);
			if (arrangement == 0) return static_cast<int32_t>(Kind::Guitar);
			const uintptr_t engine = *reinterpret_cast<const uintptr_t*>(arrangement + ARRANGEMENT_ENGINE);
			if (engine == 0) return static_cast<int32_t>(Kind::Guitar);
			const uintptr_t detector = *reinterpret_cast<const uintptr_t*>(engine + ENGINE_DETECTOR);
			if (detector == 0) return static_cast<int32_t>(Kind::Guitar);
			return static_cast<int32_t>(IsBassOpenStringTable(
				reinterpret_cast<const int16_t*>(detector + DETECTOR_OPEN_STRING_MIDI)) ? Kind::Bass : Kind::Guitar);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return static_cast<int32_t>(ArrangementInstrument::Kind::Guitar);
		}
	}
}

int32_t MlAudioExporter::GetArrangementInstrument()
{
	return cachedInstrument.load(std::memory_order_relaxed);
}

void MlAudioExporter::Poll(int appliedShiftSemitones)
{
	cachedAppliedShift.store(appliedShiftSemitones, std::memory_order_relaxed);
	const int32_t instrument = ReadArrangementInstrument_SEH();
	cachedInstrument.store(instrument, std::memory_order_relaxed);
	auto* current = sharedHeader.load(std::memory_order_acquire);
	if (current != nullptr)
	{
		ResearchProtocol::NoteByNoteState state;
		const bool available = ResearchBridge::TryGetNoteByNoteState(state) && state.isInitialized;
		InterlockedIncrement(reinterpret_cast<volatile LONG*>(&current->expectationSequence));
		current->expectedString = available ? state.selectedString : -1;
		current->expectedMidi = available ? state.expectedMidi : -1;
		current->expectedInstrument = instrument;
		current->expectationTick = GetTickCount();
		InterlockedIncrement(reinterpret_cast<volatile LONG*>(&current->expectationSequence));
		return;
	}
	if (creationFailed) return;

	mappingHandle = CreateFileMappingA(
		INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
		0, static_cast<DWORD>(MAPPING_BYTES), ML_AUDIO_MAPPING_NAME);
	if (mappingHandle == nullptr)
	{
		// One shot: a per-tick retry against a hard failure (session policy, name squat)
		// would just spam the system. The exporter simply stays dormant.
		creationFailed = true;
		return;
	}

	void* view = MapViewOfFile(mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, MAPPING_BYTES);
	if (view == nullptr)
	{
		CloseHandle(mappingHandle);
		mappingHandle = nullptr;
		creationFailed = true;
		return;
	}

	auto* header = static_cast<SharedHeader*>(view);
	// A fresh mapping is zero-filled by the kernel; only the identity fields need
	// writing, and magic goes last so the companion's validation cannot race a
	// half-initialized header. The pointer publish below is the release the audio
	// thread's acquire pairs with.
	header->expectedString = -1;
	header->expectedMidi = -1;
	header->version = ML_AUDIO_VERSION;
	header->magic = ML_AUDIO_MAGIC;
	sharedRing.store(reinterpret_cast<float*>(header + 1), std::memory_order_relaxed);
	sharedHeader.store(header, std::memory_order_release);
}

void MlAudioExporter::Observe(uint32_t routeIndex, const float* samples, uint32_t count,
	uint32_t sampleRate)
{
	if (routeIndex != 0 || samples == nullptr || count == 0) return;
	SharedHeader* header = sharedHeader.load(std::memory_order_acquire);
	if (header == nullptr) return;
	float* ring = sharedRing.load(std::memory_order_relaxed);

	if (sampleRate != 0) header->sampleRate = sampleRate;
	header->appliedShiftSemitones = cachedAppliedShift.load(std::memory_order_relaxed);

	const uint64_t start = localWriteCount.load(std::memory_order_relaxed);
	for (uint32_t index = 0; index < count; ++index)
	{
		ring[(start + index) & RING_MASK] = samples[index];
	}
	localWriteCount.store(start + count, std::memory_order_relaxed);
	// Full-barrier publish: samples land before the count that makes them visible.
	InterlockedExchange64(
		reinterpret_cast<volatile LONGLONG*>(&header->writeIndex),
		static_cast<LONGLONG>(start + count));
}

bool MlAudioExporter::QueryResult(float& outMidi, float& outConfidence, double& outAgeSeconds)
{
	outMidi = 0.0f;
	outConfidence = 0.0f;
	outAgeSeconds = 0.0;

	SharedHeader* header = sharedHeader.load(std::memory_order_acquire);
	if (header == nullptr) return false;
	const uint32_t sampleRate = header->sampleRate;
	if (sampleRate == 0) return false;

	// Seqlock read of the companion's mailbox. Torn 64-bit reads need no interlocked
	// help here: the companion only writes the fields inside an odd seq, so a tear
	// implies a seq change and the retry catches it.
	for (int attempt = 0; attempt < 4; ++attempt)
	{
		const uint32_t seqBefore = *reinterpret_cast<volatile uint32_t*>(&header->resultSeq);
		if (seqBefore == 0) return false; // the mailbox was never written
		if ((seqBefore & 1) != 0) continue; // a write is in progress
		std::atomic_thread_fence(std::memory_order_acquire);
		const float midi = *reinterpret_cast<volatile float*>(&header->resultMidi);
		const float confidence = *reinterpret_cast<volatile float*>(&header->resultConfidence);
		const uint64_t windowEnd = *reinterpret_cast<volatile uint64_t*>(
			&header->resultSampleIndexAtWindowEnd);
		std::atomic_thread_fence(std::memory_order_acquire);
		const uint32_t seqAfter = *reinterpret_cast<volatile uint32_t*>(&header->resultSeq);
		if (seqBefore != seqAfter) continue;

		// Interlocked read because this runs in the 32-bit host, where a plain 64-bit
		// load can tear against the audio thread's publish.
		const uint64_t writeIndex = static_cast<uint64_t>(InterlockedCompareExchange64(
			reinterpret_cast<volatile LONGLONG*>(&header->writeIndex), 0, 0));
		outMidi = midi;
		outConfidence = confidence;
		outAgeSeconds = writeIndex > windowEnd
			? static_cast<double>(writeIndex - windowEnd) / static_cast<double>(sampleRate)
			: 0.0;
		return true;
	}
	return false;
}

bool MlAudioExporter::QueryAudioPosition(uint64_t& sampleIndex, uint32_t& sampleRate)
{
	SharedHeader* header = sharedHeader.load(std::memory_order_acquire);
	if (header == nullptr) return false;
	sampleRate = header->sampleRate;
	if (sampleRate == 0) return false;
	sampleIndex = static_cast<uint64_t>(InterlockedCompareExchange64(
		reinterpret_cast<volatile LONGLONG*>(&header->writeIndex), 0, 0));
	return true;
}

bool MlAudioExporter::IsServiceAlive()
{
	float midi = 0.0f;
	float confidence = 0.0f;
	double ageSeconds = 0.0;
	if (!QueryResult(midi, confidence, ageSeconds)) return false;
	return ageSeconds <= 1.0;
}
