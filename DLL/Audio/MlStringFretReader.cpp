#include "stdafx.h"
#include "MlStringFretReader.hpp"
#include "MlAudioExporter.hpp"
#include "../PitchNames.hpp"

#include <windows.h>
#include <cstring>

namespace
{
	// Mirror of the Python companion's StringFretResult. Cross-process, cross-bitness
	// contract: the static_assert pins the size so a stray edit fails the build instead
	// of silently reading garbage. Any change bumps SF_VERSION on both sides.
#pragma pack(push, 4)
	struct StringFretResult
	{
		uint32_t magic;
		uint32_t version;
		uint32_t seq;          // seqlock: odd while the companion is writing
		int32_t  shift;
		int32_t  fret[6];
		int32_t  physFret[6];
		float    conf[6];
		uint64_t updateTickMs; // GetTickCount64() at the companion's write
		uint64_t analyzedSampleIndex;
		uint32_t sampleRate;
		// The model's instrument, 0 guitar / 1 bass. An older companion writes 0.
		int32_t  instrument;
	};
#pragma pack(pop)
	static_assert(sizeof(StringFretResult) == 112, "must match the Python companion struct");

	constexpr uint32_t SF_MAGIC = 0x46535352u; // "RSSF"
	constexpr uint32_t SF_VERSION = 2;
	constexpr char SF_MAPPING_NAME[] = "Local\\RSModsPlus.MlStringFret.v2";

	constexpr int OPEN_STRING_MIDI[6] = { 40, 45, 50, 55, 59, 64 };

	HANDLE g_mapping = nullptr;
	const StringFretResult* g_view = nullptr;
	uint64_t g_lastOpenAttemptTick = 0;

	// Lazily open the companion's mapping, throttled to ~1 Hz so a missing companion
	// costs almost nothing on the render thread.
	const StringFretResult* EnsureView()
	{
		if (g_view != nullptr) return g_view;

		const uint64_t now = GetTickCount64();
		if (g_lastOpenAttemptTick != 0 && now - g_lastOpenAttemptTick < 1000) return nullptr;
		g_lastOpenAttemptTick = now;

		g_mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, SF_MAPPING_NAME);
		if (g_mapping == nullptr) return nullptr;

		g_view = static_cast<const StringFretResult*>(
			MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, sizeof(StringFretResult)));
		if (g_view == nullptr)
		{
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return nullptr;
		}
		return g_view;
	}
}

bool MlStringFretReader::TryGet(StringFret& out, double maxAgeSeconds, const char** failureReason)
{
	out = {};
	out.ageSeconds = -1.0;
	for (int index = 0; index < 6; ++index) out.fret[index] = -1;
	if (failureReason != nullptr) *failureReason = "mapping-unavailable";
	const StringFretResult* v = EnsureView();
	if (v == nullptr) return false;
	if (failureReason != nullptr) *failureReason = "protocol-mismatch";
	if (v->magic != SF_MAGIC || v->version != SF_VERSION) return false;

	// Seqlock read: capture an even seq, copy, re-read; retry a few times on a torn
	// read (the companion is the only writer, so this settles immediately in practice).
	StringFretResult snap{};
	bool settled = false;
	for (int attempt = 0; attempt < 4; ++attempt)
	{
		const uint32_t s1 = *const_cast<volatile uint32_t*>(&v->seq);
		if (s1 & 1u) continue; // writer mid-write
		MemoryBarrier();
		std::memcpy(&snap, v, sizeof(snap));
		MemoryBarrier();
		const uint32_t s2 = *const_cast<volatile uint32_t*>(&v->seq);
		if (s1 == s2) { settled = true; break; }
	}
	if (failureReason != nullptr) *failureReason = "concurrent-write";
	if (!settled) return false;

	const uint64_t now = GetTickCount64();
	const double age = (snap.updateTickMs != 0 && now >= snap.updateTickMs)
		? (now - snap.updateTickMs) / 1000.0
		: 1.0e9;
	out.shift = snap.shift;
	out.ageSeconds = age;
	out.analyzedSampleIndex = snap.analyzedSampleIndex;
	out.sampleRate = snap.sampleRate;
	out.instrument = snap.instrument;
	for (int i = 0; i < 6; ++i)
	{
		out.fret[i] = snap.fret[i];
		out.physFret[i] = snap.physFret[i];
		out.conf[i] = snap.conf[i];
	}
	if (failureReason != nullptr) *failureReason = "publication-stale";
	if (age > maxAgeSeconds) return false;
	uint64_t currentSampleIndex = 0;
	uint32_t currentSampleRate = 0;
	if (failureReason != nullptr) *failureReason = "audio-position-invalid";
	if (!MlAudioExporter::QueryAudioPosition(currentSampleIndex, currentSampleRate)
		|| snap.sampleRate != currentSampleRate || snap.analyzedSampleIndex > currentSampleIndex)
	{
		return false;
	}
	const double audioAge = static_cast<double>(currentSampleIndex - snap.analyzedSampleIndex)
		/ static_cast<double>(currentSampleRate);
	out.ageSeconds = audioAge > age ? audioAge : age;
	if (failureReason != nullptr) *failureReason = "audio-stale";
	if (audioAge > maxAgeSeconds) return false;
	if (failureReason != nullptr) *failureReason = "fresh";
	return true;
}

std::string MlStringFretReader::NoteNameForStringFret(int stringIndex, int fret)
{
	if (stringIndex < 0 || stringIndex > 5) return std::string();
	const int midi = OPEN_STRING_MIDI[stringIndex] + fret;
	return std::string(PitchNames::ForPitchClass(midi));   // pitch class only (no octave number)
}

void MlStringFretReader::Shutdown()
{
	if (g_view != nullptr) { UnmapViewOfFile(g_view); g_view = nullptr; }
	if (g_mapping != nullptr) { CloseHandle(g_mapping); g_mapping = nullptr; }
}
