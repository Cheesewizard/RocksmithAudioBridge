#include "../../stdafx.h"
#include "FakeGuitarInjector.hpp"

#include "../../Audio/AsioHook.hpp"
#include "../../Audio/IInputProcessor.hpp"
#include "../../Audio/CaptureFormat.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace FakeGuitar
{
	namespace
	{
		constexpr float TWO_PI = 6.28318530718f;
		constexpr uint32_t QUEUE_CAPACITY = 128;            // power of two
		constexpr uint32_t QUEUE_MASK = QUEUE_CAPACITY - 1;
		constexpr int MAX_CHORD_PITCHES = 6;
		constexpr size_t PLAYER_ONE_ROUTE = 0;
		constexpr uint32_t TARGET_RATE = 48000;

		float MidiToHz(int midi)
		{
			return 440.0f * std::pow(2.0f, (static_cast<float>(midi) - 69.0f) / 12.0f);
		}

		// ---- Real-guitar sample bank ----------------------------------------------------
		// When a bank of per-note WAVs (rendered from a guitar VST) is present, the injector
		// plays those recordings instead of the synthetic tone: real harmonic content the
		// game's pitch detector reads at full quality, and bends become playback-rate glides.
		// Falls back to the synth for any note with no sample.
		struct Sample { std::vector<float> pcm; }; // mono, TARGET_RATE
		Sample g_bank[128];
		bool g_bankLoaded = false;

		bool HasSample(int midi) { return midi >= 0 && midi < 128 && !g_bank[midi].pcm.empty(); }

		// The sample used to render `midi`: the exact note if present, else the nearest loaded
		// note (its playback is resampled to reach `midi`).
		int NearestSample(int midi)
		{
			if (HasSample(midi)) return midi;
			for (int d = 1; d < 128; ++d)
			{
				if (HasSample(midi - d)) return midi - d;
				if (HasSample(midi + d)) return midi + d;
			}
			return -1;
		}

		float ReadLE(const uint8_t* p, int bytes)
		{
			int32_t v = 0;
			for (int i = 0; i < bytes; ++i) v |= static_cast<int32_t>(p[i]) << (8 * i);
			const int shift = 32 - 8 * bytes;
			v = (v << shift) >> shift;                     // sign-extend
			return static_cast<float>(v) / static_cast<float>(1u << (8 * bytes - 1));
		}

		// Minimal WAV reader -> mono float at TARGET_RATE. Handles PCM 16/24/32 and float32,
		// mono or stereo (channel 0), any sample rate (linear-resampled).
		bool LoadWav(const std::string& path, std::vector<float>& out)
		{
			FILE* f = nullptr;
			if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
			fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
			if (size < 44) { fclose(f); return false; }
			std::vector<uint8_t> buf(static_cast<size_t>(size));
			const size_t got = fread(buf.data(), 1, buf.size(), f); fclose(f);
			if (got != buf.size()) return false;
			if (std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0)
				return false;

			uint16_t fmt = 0, channels = 0, bits = 0; uint32_t rate = 0;
			const uint8_t* data = nullptr; size_t dataLen = 0;
			size_t pos = 12;
			while (pos + 8 <= buf.size())
			{
				const uint8_t* c = buf.data() + pos;
				uint32_t clen = c[4] | (c[5] << 8) | (c[6] << 16) | (static_cast<uint32_t>(c[7]) << 24);
				const uint8_t* body = c + 8;
				if (std::memcmp(c, "fmt ", 4) == 0 && clen >= 16)
				{
					fmt = body[0] | (body[1] << 8);
					channels = body[2] | (body[3] << 8);
					rate = body[4] | (body[5] << 8) | (body[6] << 16) | (static_cast<uint32_t>(body[7]) << 24);
					bits = body[14] | (body[15] << 8);
				}
				else if (std::memcmp(c, "data", 4) == 0)
				{
					data = body; dataLen = clen;
					if (body + clen > buf.data() + buf.size()) dataLen = buf.data() + buf.size() - body;
				}
				pos += 8 + clen + (clen & 1);
			}
			if (!data || channels == 0 || rate == 0 || bits == 0) return false;

			const int bytesPer = bits / 8;
			const size_t frame = static_cast<size_t>(bytesPer) * channels;
			if (frame == 0) return false;
			const size_t frames = dataLen / frame;
			std::vector<float> mono; mono.reserve(frames);
			const bool isFloat = (fmt == 3);
			for (size_t i = 0; i < frames; ++i)
			{
				const uint8_t* s = data + i * frame;              // channel 0
				float v = 0.0f;
				if (isFloat && bits == 32) std::memcpy(&v, s, 4);
				else v = ReadLE(s, bytesPer);
				mono.push_back(v);
			}

			if (rate == TARGET_RATE) { out = std::move(mono); return !out.empty(); }
			// linear resample to TARGET_RATE
			const double ratio = static_cast<double>(rate) / TARGET_RATE;
			const size_t n = static_cast<size_t>(mono.size() / ratio);
			out.clear(); out.reserve(n);
			for (size_t i = 0; i < n; ++i)
			{
				const double src = i * ratio;
				const size_t i0 = static_cast<size_t>(src);
				const float a = static_cast<float>(src - i0);
				const float s0 = mono[i0];
				const float s1 = (i0 + 1 < mono.size()) ? mono[i0 + 1] : s0;
				out.push_back(s0 + a * (s1 - s0));
			}
			return !out.empty();
		}

		// Load note_<midi>.wav from a folder into the bank. Returns how many loaded.
		int LoadSampleBank(const std::string& folder)
		{
			int loaded = 0;
			for (int midi = 0; midi < 128; ++midi)
			{
				char name[64];
				std::snprintf(name, sizeof(name), "%s\\note_%d.wav", folder.c_str(), midi);
				std::vector<float> pcm;
				if (LoadWav(name, pcm)) { g_bank[midi].pcm = std::move(pcm); ++loaded; }
			}
			g_bankLoaded = loaded > 0;
			return loaded;
		}

		// One queued strum: up to six pitches sounded together, preceded by lead silence.
		// For a bend, endPhaseIncrement differs from phaseIncrement and bendRampFrames > 0:
		// the pitch holds at the base for bendHoldFrames, then glides to the target over
		// bendRampFrames and holds there - a real string bend the game's bend detector reads.
		struct NoteCommand
		{
			int pitchCount = 0;
			int midi[MAX_CHORD_PITCHES] = { 0 };
			float phaseIncrement[MAX_CHORD_PITCHES] = { 0.0f };
			float endPhaseIncrement[MAX_CHORD_PITCHES] = { 0.0f };
			// Sample playback: when sampleIndex[i] >= 0 the pitch plays g_bank[sampleIndex] at
			// baseRate (resampled to the wanted pitch), gliding to endRate across a bend. -1
			// means fall back to the synth oscillator for that pitch.
			int sampleIndex[MAX_CHORD_PITCHES] = { -1, -1, -1, -1, -1, -1 };
			float baseRate[MAX_CHORD_PITCHES] = { 1.0f };
			float endRate[MAX_CHORD_PITCHES] = { 1.0f };
			float amplitude = 0.0f;
			uint32_t leadSilenceFrames = 0;
			uint32_t durationFrames = 0;
			uint32_t bendHoldFrames = 0;
			uint32_t bendRampFrames = 0;   // 0 = no bend (fixed pitch)
		};

		// Passthrough-by-default input processor. Process runs on the game's audio thread and
		// must not allocate, lock, log or block; all cross-thread handoff is through the
		// lock-free ring and a few atomics. The bridge thread is the sole producer.
		class FakeGuitarInjector final : public Audio::IInputProcessor
		{
		public:
			void Prepare(const Audio::CaptureFormat& format) override
			{
				sampleRate.store(format.sampleRate ? format.sampleRate : 48000u,
					std::memory_order_relaxed);
			}

			bool Process(float* samples, uint32_t frameCount) override
			{
				// Not armed: leave the real cable samples untouched.
				if (!synthEnabled.load(std::memory_order_acquire)) return false;

				if (flushRequested.exchange(false, std::memory_order_acq_rel))
				{
					active = false;
					currentMidi.store(-1, std::memory_order_relaxed);
					readIndex.store(writeIndex.load(std::memory_order_acquire),
						std::memory_order_release);
				}

				const uint32_t sr = sampleRate.load(std::memory_order_relaxed);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					samples[frame] = NextSample(sr);
				return true;
			}

			uint32_t GetLatencyFrames() const override { return 0; }

			void SetSynthEnabled(bool enabled)
			{
				synthEnabled.store(enabled, std::memory_order_release);
			}

			bool IsSynthEnabled() const
			{
				return synthEnabled.load(std::memory_order_acquire);
			}

			bool Enqueue(const int* midis, int count, float amplitude,
				uint32_t durationMs, uint32_t leadSilenceMs)
			{
				if (count <= 0) return false;
				if (count > MAX_CHORD_PITCHES) count = MAX_CHORD_PITCHES;

				const uint32_t write = writeIndex.load(std::memory_order_relaxed);
				const uint32_t read = readIndex.load(std::memory_order_acquire);
				if (write - read >= QUEUE_CAPACITY) return false;   // full

				const uint32_t sr = sampleRate.load(std::memory_order_relaxed);
				NoteCommand command;
				command.pitchCount = count;
				command.amplitude = amplitude;
				command.durationFrames = MillisecondsToFrames(durationMs, sr);
				command.leadSilenceFrames = MillisecondsToFrames(leadSilenceMs, sr);
				for (int index = 0; index < count; ++index)
				{
					command.midi[index] = midis[index];
					command.phaseIncrement[index] = TWO_PI * MidiToHz(midis[index])
						/ static_cast<float>(sr);
					command.endPhaseIncrement[index] = command.phaseIncrement[index]; // no bend
					const int samp = g_bankLoaded ? NearestSample(midis[index]) : -1;
					command.sampleIndex[index] = samp;
					if (samp >= 0)
					{
						const float r = MidiToHz(midis[index]) / MidiToHz(samp);
						command.baseRate[index] = r;
						command.endRate[index] = r;   // no bend
					}
				}

				ring[write & QUEUE_MASK] = command;
				writeIndex.store(write + 1, std::memory_order_release);
				return true;
			}

			// A single-note bend: sound baseMidi, hold briefly, then glide up (or down) to
			// endMidi and hold there for the rest of the note.
			bool EnqueueBend(int baseMidi, int endMidi, float amplitude,
				uint32_t durationMs, uint32_t bendHoldMs, uint32_t bendRampMs)
			{
				const uint32_t write = writeIndex.load(std::memory_order_relaxed);
				const uint32_t read = readIndex.load(std::memory_order_acquire);
				if (write - read >= QUEUE_CAPACITY) return false;

				const uint32_t sr = sampleRate.load(std::memory_order_relaxed);
				NoteCommand command;
				command.pitchCount = 1;
				command.amplitude = amplitude;
				command.durationFrames = MillisecondsToFrames(durationMs, sr);
				command.bendHoldFrames = MillisecondsToFrames(bendHoldMs, sr);
				command.bendRampFrames = MillisecondsToFrames(bendRampMs, sr);
				if (command.bendRampFrames == 0) command.bendRampFrames = 1;
				command.midi[0] = baseMidi;
				command.phaseIncrement[0] = TWO_PI * MidiToHz(baseMidi) / static_cast<float>(sr);
				// Overshoot the target by a fraction of a semitone: the game's pitch tracker
				// reads a rising/held bend slightly low, so bending exactly to the target lands
				// just under it and the bend never registers. Bending a touch past makes the
				// sounding pitch read AS the target every time - accept on the first attempt
				// instead of retrying until a sample happens to read high enough.
				constexpr float BEND_OVERSHOOT_SEMITONES = 0.45f;
				const float endHz = MidiToHz(endMidi)
					* std::pow(2.0f, BEND_OVERSHOOT_SEMITONES / 12.0f);
				command.endPhaseIncrement[0] = TWO_PI * endHz / static_cast<float>(sr);

				// Sample path: play the base note's recording and glide its playback rate up to
				// the (overshot) target - a real bent-guitar tone rising to pitch.
				const int samp = g_bankLoaded ? NearestSample(baseMidi) : -1;
				command.sampleIndex[0] = samp;
				if (samp >= 0)
				{
					const float sampHz = MidiToHz(samp);
					command.baseRate[0] = MidiToHz(baseMidi) / sampHz;
					command.endRate[0] = endHz / sampHz;
				}

				ring[write & QUEUE_MASK] = command;
				writeIndex.store(write + 1, std::memory_order_release);
				return true;
			}

			void Clear()
			{
				// When processing is live the audio thread owns readIndex, so ask it to drain.
				// When it is not, there is no consumer and the producer can reset directly.
				if (Audio::AsioHook::IsProcessingEnabled())
				{
					flushRequested.store(true, std::memory_order_release);
				}
				else
				{
					readIndex.store(writeIndex.load(std::memory_order_relaxed),
						std::memory_order_relaxed);
					currentMidi.store(-1, std::memory_order_relaxed);
				}
			}

			int CurrentMidi() const { return currentMidi.load(std::memory_order_relaxed); }

			uint32_t Pending() const
			{
				const uint32_t write = writeIndex.load(std::memory_order_acquire);
				const uint32_t read = readIndex.load(std::memory_order_acquire);
				return write - read;
			}

		private:
			static uint32_t MillisecondsToFrames(uint32_t milliseconds, uint32_t sampleRate)
			{
				return static_cast<uint32_t>(
					(static_cast<uint64_t>(milliseconds) * sampleRate) / 1000u);
			}

			// Attack ramp, a mild pluck decay, and a short release so the tone starts with a
			// clean rising edge (the onset evidence the game looks for) and ends without a
			// click.
			float Envelope(uint32_t toneFrame, uint32_t durationFrames, uint32_t sr) const
			{
				if (toneFrame >= durationFrames) return 0.0f;
				const uint32_t attackFrames = sr / 166;   // ~6 ms
				uint32_t releaseFrames = sr / 100;        // ~10 ms
				if (releaseFrames >= durationFrames) releaseFrames = durationFrames / 2;

				float shape = 1.0f;
				if (toneFrame < attackFrames)
				{
					shape = static_cast<float>(toneFrame) / static_cast<float>(attackFrames);
				}
				else if (releaseFrames > 0 && toneFrame > durationFrames - releaseFrames)
				{
					shape = static_cast<float>(durationFrames - toneFrame)
						/ static_cast<float>(releaseFrames);
				}

				const float decay = std::exp(-2.2f * static_cast<float>(toneFrame)
					/ static_cast<float>(sr));
				return shape * (0.4f + 0.6f * decay);
			}

			float NextSample(uint32_t sr)
			{
				if (!active)
				{
					const uint32_t read = readIndex.load(std::memory_order_relaxed);
					if (read == writeIndex.load(std::memory_order_acquire))
					{
						currentMidi.store(-1, std::memory_order_relaxed);
						return 0.0f;
					}
					current = ring[read & QUEUE_MASK];
					readIndex.store(read + 1, std::memory_order_release);
					active = true;
					framesIntoNote = 0;
					for (int index = 0; index < MAX_CHORD_PITCHES; ++index)
					{
						phase[index] = 0.0f;
						samplePos[index] = 0.0;
					}
				}

				if (framesIntoNote < current.leadSilenceFrames)
				{
					++framesIntoNote;
					currentMidi.store(-1, std::memory_order_relaxed);
					return 0.0f;
				}

				const uint32_t toneFrame = framesIntoNote - current.leadSilenceFrames;
				if (toneFrame == 0)
					currentMidi.store(current.midi[0], std::memory_order_relaxed);
				if (toneFrame >= current.durationFrames)
				{
					active = false;    // finished; the next sample pops the next note
					return 0.0f;
				}

				const float envelope = Envelope(toneFrame, current.durationFrames, sr);

				// Bend glide factor: 0 while holding the base, ramping 0->1 across the bend,
				// then 1 (held at the target). Fixed pitches have bendRampFrames 0 and stay at 0.
				float bend = 0.0f;
				if (current.bendRampFrames > 0 && toneFrame > current.bendHoldFrames)
				{
					bend = static_cast<float>(toneFrame - current.bendHoldFrames)
						/ static_cast<float>(current.bendRampFrames);
					if (bend > 1.0f) bend = 1.0f;
				}

				float mix = 0.0f;
				for (int index = 0; index < current.pitchCount; ++index)
				{
					const int si = current.sampleIndex[index];
					if (si >= 0 && si < 128 && !g_bank[si].pcm.empty())
					{
						// Real recording: read (linear-interpolated) and advance the playback
						// rate, gliding base->end across a bend. Past the sample end = silent.
						const std::vector<float>& pcm = g_bank[si].pcm;
						const double pos = samplePos[index];
						const size_t i0 = static_cast<size_t>(pos);
						if (i0 + 1 < pcm.size())
						{
							const float a = static_cast<float>(pos - i0);
							mix += pcm[i0] + a * (pcm[i0 + 1] - pcm[i0]);
						}
						const double rate = static_cast<double>(current.baseRate[index]
							+ bend * (current.endRate[index] - current.baseRate[index]));
						samplePos[index] = pos + rate;
					}
					else
					{
						const float p = phase[index];
						mix += std::sin(p) + 0.45f * std::sin(2.0f * p) + 0.30f * std::sin(3.0f * p);
						const float increment = current.phaseIncrement[index]
							+ bend * (current.endPhaseIncrement[index] - current.phaseIncrement[index]);
						float advanced = p + increment;
						if (advanced >= TWO_PI) advanced -= TWO_PI;
						phase[index] = advanced;
					}
				}

				++framesIntoNote;
				const bool usesSample = current.sampleIndex[0] >= 0;
				if (usesSample)
				{
					// The recording carries its own attack/body; only taper the very start and
					// end to avoid clicks, and do not apply the synth's 0.5 headroom (the sample
					// is already at guitar level).
					const uint32_t fade = sr / 200;   // ~5 ms
					float taper = 1.0f;
					if (toneFrame < fade) taper = static_cast<float>(toneFrame) / static_cast<float>(fade);
					else if (toneFrame > current.durationFrames - fade)
						taper = static_cast<float>(current.durationFrames - toneFrame)
							/ static_cast<float>(fade);
					return current.amplitude * taper * mix
						/ std::sqrt(static_cast<float>(current.pitchCount));
				}
				const float normalize = 0.5f
					/ std::sqrt(static_cast<float>(current.pitchCount));
				return current.amplitude * envelope * mix * normalize;
			}

			std::atomic<bool> synthEnabled{ false };
			std::atomic<bool> flushRequested{ false };
			std::atomic<uint32_t> sampleRate{ 48000 };
			std::atomic<int> currentMidi{ -1 };

			// Single-producer (bridge thread) / single-consumer (audio thread) ring.
			std::atomic<uint32_t> writeIndex{ 0 };
			std::atomic<uint32_t> readIndex{ 0 };
			NoteCommand ring[QUEUE_CAPACITY];

			// Audio-thread-only playback state.
			bool active = false;
			NoteCommand current;
			uint32_t framesIntoNote = 0;
			float phase[MAX_CHORD_PITCHES] = { 0.0f };
			double samplePos[MAX_CHORD_PITCHES] = { 0.0 };
		};

		FakeGuitarInjector injector;
		std::atomic<bool> installed{ false };
	}

	void Install()
	{
		if (installed.load(std::memory_order_relaxed)) return;

		Audio::AsioHook::Install();
		// A SOURCE stage, not the processor: it runs before whatever processor the route has
		// (the Drop Pedal shifter, if configured), so an armed synth feeds the real shifter
		// and the two coexist. Inert until armed.
		Audio::AsioHook::SetInputSource(PLAYER_ONE_ROUTE, &injector);
		installed.store(true, std::memory_order_release);
		LOG_INFO("[FakeGuitar] Synthetic input source installed on the Player 1 route (ahead of "
			"the processor chain); inert until armed." << std::endl);

		// Auto-load a guitar sample bank if one is present next to the game, so injected notes
		// are real recordings instead of the synth. Absent -> the synth is used.
		const int loaded = LoadSampleBank("RSModsResearch\\GuitarSamples");
		if (loaded > 0)
			LOG_INFO("[FakeGuitar] Loaded " << loaded << " guitar samples; injecting real recordings." << std::endl);
		else
			LOG_INFO("[FakeGuitar] No guitar sample bank found; using the synth tone." << std::endl);
	}

	bool IsInstalled() { return installed.load(std::memory_order_acquire); }

	int LoadSamples(const char* folder)
	{
		// Disarm while the bank is swapped so the audio thread is not reading it.
		const bool wasArmed = injector.IsSynthEnabled();
		injector.SetSynthEnabled(false);
		injector.Clear();
		const int n = LoadSampleBank(folder && *folder ? folder : "RSModsResearch\\GuitarSamples");
		if (wasArmed) injector.SetSynthEnabled(true);
		return n;
	}

	int SampleCount()
	{
		int n = 0;
		for (int m = 0; m < 128; ++m) if (!g_bank[m].pcm.empty()) ++n;
		return n;
	}

	void Poll()
	{
		if (installed.load(std::memory_order_acquire)) Audio::AsioHook::Poll();
	}

	void SetSynthEnabled(bool enabled)
	{
		if (!enabled) injector.Clear();
		injector.SetSynthEnabled(enabled);
		// Arm/disarm the source stage in the hook: armed replaces the real cable before the
		// processor; disarmed leaves the real input untouched (the mid-game swap).
		Audio::AsioHook::SetInputSourceActive(PLAYER_ONE_ROUTE, enabled);
	}

	bool IsSynthEnabled() { return injector.IsSynthEnabled(); }

	bool IsCaptureReady()
	{
		return installed.load(std::memory_order_acquire)
			&& Audio::AsioHook::IsInputReady(PLAYER_ONE_ROUTE)
			&& Audio::AsioHook::IsProcessingEnabled();
	}

	bool QueueNote(int midi, float amplitude, uint32_t durationMs, uint32_t leadSilenceMs)
	{
		return injector.Enqueue(&midi, 1, amplitude, durationMs, leadSilenceMs);
	}

	bool QueueChord(const int* midis, int count, float amplitude,
		uint32_t durationMs, uint32_t leadSilenceMs)
	{
		return injector.Enqueue(midis, count, amplitude, durationMs, leadSilenceMs);
	}

	bool QueueBend(int baseMidi, int endMidi, float amplitude,
		uint32_t durationMs, uint32_t bendHoldMs, uint32_t bendRampMs)
	{
		return injector.EnqueueBend(baseMidi, endMidi, amplitude, durationMs, bendHoldMs, bendRampMs);
	}

	void ClearQueue() { injector.Clear(); }

	int CurrentMidi() { return injector.CurrentMidi(); }

	uint32_t PendingCount() { return injector.Pending(); }
}
