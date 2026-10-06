#pragma once

#include <cstdint>

// Tier 1 of the detection stack: the game is a 32-bit process, but the
// ML pitch models worth running (CREPE/ONNX) are 64-bit-only, so the raw Player 1 route
// audio crosses to a companion process over a named shared-memory mapping instead of
// in-process inference. The exporter mirrors the tier-0 RawPitchVerifier tap: it observes
// the same post-processing samples at the same GetBuffer choke point, so the companion
// hears exactly what the game's detector hears (silent buffers included, fed as zeros to
// keep the timeline continuous). The companion writes its estimate back into a small
// seqlock mailbox in the same mapping; the host never blocks on it and treats a stale or
// absent result as "no service", so the game runs identically with no companion attached.
namespace MlAudioExporter
{
	// Mapping layout, shared verbatim with the companion process (which duplicates it as a
	// standalone build; any change here must bump ML_AUDIO_VERSION and be mirrored there). Fixed-width fields
	// and explicit padding keep the layout identical across the 32-bit host and the
	// 64-bit companion; the two uint64 fields sit at 8-aligned offsets so both sides can
	// access them atomically. The header occupies the first 64 bytes of the mapping, the
	// float ring the rest.
	//
	//   host-written:      magic, version, sampleRate, appliedShiftSemitones, writeIndex,
	//                      and the ring samples
	//   companion-written: the result mailbox (resultSeq odd while a write is in
	//                      progress, even when the fields are stable - classic seqlock)
#pragma pack(push, 4)
	struct SharedHeader
	{
		uint32_t magic;
		uint32_t version;
		uint32_t sampleRate;
		// The semitones the shared input shifter currently applies to the route: the
		// observed audio is post-shifter (same frame the tier-0 queries correct for via
		// DropPedal::GetAppliedInputShiftSemitones), so the companion needs this to map
		// its estimate back to the player's physical frame. Refreshed on every ring write.
		int32_t appliedShiftSemitones;
		// Monotonic total samples ever written; sample N lives at ring[N & (RING-1)].
		uint64_t writeIndex;
		// Companion result mailbox.
		uint32_t resultSeq;
		float resultMidi;
		float resultConfidence;
		// Loaded arrangement (ArrangementInstrument::Kind: 0 guitar, 1 bass), written under
		// expectationSequence with the expected note. Occupies former zero padding, so an older
		// companion ignores it and an older host leaves it 0 (guitar).
		int32_t expectedInstrument;
		uint64_t resultSampleIndexAtWindowEnd;
		uint32_t expectationSequence;
		int32_t expectedString;
		int32_t expectedMidi;
		uint32_t expectationTick;
	};
#pragma pack(pop)

	constexpr uint32_t ML_AUDIO_MAGIC = 0x4C4D5352u; // "RSML" read as little-endian bytes
	constexpr uint32_t ML_AUDIO_VERSION = 2;
	constexpr uint32_t ML_AUDIO_RING_SAMPLES = 1u << 17; // ~2.7 s at 48 kHz
	constexpr char ML_AUDIO_MAPPING_NAME[] = "Local\\RSModsPlus.MlAudio.v2";

	// Game-loop pump (normal thread): creates the mapping on first call - the audio
	// thread must never allocate or take the loader lock, so all setup happens here -
	// and refreshes the cached applied input shift the audio thread stamps into the
	// header. Cheap after the first call.
	void Poll(int appliedShiftSemitones);

	// The arrangement instrument Poll last read from the game's detector (guitar when it could
	// not be read). ML evidence decoded for a different instrument is refused against this.
	int32_t GetArrangementInstrument();

	// Audio-thread feed, called next to RawPitchVerifier::Observe with the identical
	// arguments and semantics: Player 1 (routeIndex 0) only, silent buffers as zeros.
	// memcpy plus one interlocked counter bump; a no-op until Poll created the mapping.
	void Observe(uint32_t routeIndex, const float* samples, uint32_t count, uint32_t sampleRate);

	// Latest companion estimate via seqlock read (4 retries). False when the mapping
	// does not exist yet, the mailbox was never written, or the seqlock never settled.
	// ageSeconds is audio-clock age: (writeIndex - resultSampleIndexAtWindowEnd) over
	// the sample rate, i.e. how much route audio arrived since the estimate's window.
	bool QueryResult(float& outMidi, float& outConfidence, double& outAgeSeconds);
	bool QueryAudioPosition(uint64_t& sampleIndex, uint32_t& sampleRate);

	// True when the companion produced an estimate within the last 1.0 s of audio.
	bool IsServiceAlive();
}
