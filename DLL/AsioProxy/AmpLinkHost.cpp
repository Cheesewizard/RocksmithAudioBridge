#include "AmpLinkHost.hpp"
#include <sddl.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "advapi32.lib")

namespace AmpLink
{
	namespace
	{
		// Peak-hold that falls about 20 dB a second, updated once per block.
		float DecayedPeak(float previous, float blockPeak, long frames)
		{
			const float fall = std::pow(10.0f, -20.0f * static_cast<float>(frames) / kSampleRate / 20.0f);
			return (std::max)(blockPeak, previous * fall);
		}

		float PeakOf(const int32_t* samples, long frames)
		{
			int64_t peak = 0;
			for (long f = 0; f < frames; ++f)
			{
				const int64_t v = samples[f] < 0 ? -static_cast<int64_t>(samples[f]) : samples[f];
				if (v > peak) peak = v;
			}
			return static_cast<float>(peak / 2147483648.0);
		}

		int32_t Saturate(int64_t v)
		{
			if (v > INT32_MAX) return INT32_MAX;
			if (v < INT32_MIN) return INT32_MIN;
			return static_cast<int32_t>(v);
		}

		void WriteFramesToRegistry(long frames)
		{
			HKEY key = nullptr;
			if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
			const DWORD value = static_cast<DWORD>(frames);
			RegSetValueExW(key, kRegistryFrames, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
			RegCloseKey(key);
		}
	}

	bool Host::Open(long frames, long inputChannels)
	{
		if (frames <= 0 || frames > kMaxFrames) { Close(); return false; }
		if (!m_shared)
		{
			// Any process of this user may open it, so an elevated game and a normal AmpliTube (or the other
			// way round) still meet. The Local namespace keeps it to this logon session.
			SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
			PSECURITY_DESCRIPTOR sd = nullptr;
			if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;AU)(A;;GA;;;SY)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr))
				sa.lpSecurityDescriptor = sd;
			m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0, sizeof(Shared), kMappingName);
			m_event = CreateEventW(sd ? &sa : nullptr, FALSE, FALSE, kInputEventName);
			if (sd) LocalFree(sd);
			if (!m_mapping || !m_event) { Close(); return false; }
			m_shared = static_cast<Shared*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
			if (!m_shared) { Close(); return false; }
			// A block left over from an earlier game run (a guest kept it alive) keeps its guest claim and
			// sequence; a fresh block is zero-filled by the system.
			if (m_shared->magic != kMagic || m_shared->size != static_cast<LONG>(sizeof(Shared)))
			{
				m_shared->version = kVersion;
				m_shared->size = static_cast<LONG>(sizeof(Shared));
				m_shared->magic = kMagic;
			}
			m_seq = Load(&m_shared->inputSeq);
		}
		m_frames = frames;
		Store(&m_shared->hostPid, static_cast<LONG>(GetCurrentProcessId()));
		Store(&m_shared->inputChannels, std::clamp<LONG>(inputChannels, 0, kChannels));
		Store(&m_shared->hostFrames, frames);
		WriteFramesToRegistry(frames);
		return true;
	}

	void Host::Close()
	{
		if (m_shared)
		{
			Store(&m_shared->hostRunning, 0);
			Store(&m_shared->hostFrames, 0);
			Store(&m_shared->hostPid, 0);
			UnmapViewOfFile(m_shared);
			m_shared = nullptr;
		}
		if (m_mapping) { CloseHandle(m_mapping); m_mapping = nullptr; }
		if (m_event) { CloseHandle(m_event); m_event = nullptr; }
		m_frames = 0;
	}

	void Host::SetRunning(bool running)
	{
		if (m_shared) Store(&m_shared->hostRunning, running ? 1 : 0);
	}

	void Host::Configure(bool mixOn, float returnGain)
	{
		m_mixOn.store(mixOn, std::memory_order_relaxed);
		m_gain.store(std::isfinite(returnGain) ? std::clamp(returnGain, 0.0f, 4.0f) : 1.0f, std::memory_order_relaxed);
	}

	void Host::SetLatencyBlocks(int blocks)
	{
		m_latencyBlocks.store(blocks >= 2 ? 2 : 1, std::memory_order_relaxed);
	}

	void Host::SetPitchSemitones(int semitones)
	{
		m_pitchSemitones.store(std::clamp(semitones, -24, 24), std::memory_order_relaxed);
	}

	bool Host::GuestConnected(LONG newestSeq) const noexcept
	{
		if (!m_shared || Load(&m_shared->guestPid) == 0 || m_frames <= 0) return false;
		// Connected = the guest has written back one of the blocks of the last quarter second.
		const LONG window = std::max<LONG>(8, kSampleRate / 4 / m_frames);
		const LONG behind = newestSeq - Load(&m_shared->guestLastSeq);
		return behind >= 0 && behind <= window;
	}

	void Host::Publish(const ASIOBufferInfo* infos, long numChannels, long index, long frames) noexcept
	{
		if (!m_shared || !infos || frames != m_frames) return;
		const LONG seq = ++m_seq;
		const LONG slot = SlotOf(seq);
		const int32_t* sources[kChannels]{};
		long found = 0;
		for (long i = 0; i < numChannels && found < kChannels; ++i)
			if (infos[i].isInput && infos[i].buffers[index]) sources[found++] = static_cast<const int32_t*>(infos[i].buffers[index]);
		if (found == 1) sources[1] = sources[0];   // a mono guitar input feeds both of the guest's inputs
		Store(&m_shared->inputSlotSeq[slot], 0);   // tag invalid while the slot is rewritten
		for (long ch = 0; ch < kChannels; ++ch)
		{
			int32_t* destination = m_shared->input[slot][ch];
			if (sources[ch]) memcpy(destination, sources[ch], static_cast<size_t>(frames) * sizeof(int32_t));
			else memset(destination, 0, static_cast<size_t>(frames) * sizeof(int32_t));
		}
		Store(&m_shared->pitchSemitones, m_pitchSemitones.load(std::memory_order_relaxed));
		Store(&m_shared->inputSlotSeq[slot], seq);
		Store(&m_shared->inputSeq, seq);
		SetEvent(m_event);
		const float peak = sources[0] ? PeakOf(sources[0], frames) : 0.0f;
		m_inputPeak.store(DecayedPeak(m_inputPeak.load(std::memory_order_relaxed), peak, frames), std::memory_order_relaxed);
	}

	void Host::MixReturn(const ASIOBufferInfo* infos, const long* outputBuffers, const long* outputTypes, long outputCount,
		long index, long frames) noexcept
	{
		if (!m_shared || !infos || frames != m_frames || outputCount <= 0) return;
		const float gain = m_gain.load(std::memory_order_relaxed);
		const bool mixOn = m_mixOn.load(std::memory_order_relaxed);
		if (!mixOn || !GuestConnected(m_seq))
		{
			m_returnPeak.store(DecayedPeak(m_returnPeak.load(std::memory_order_relaxed), 0.0f, frames), std::memory_order_relaxed);
			return;
		}

		// One block of link latency (two with the safety buffer): this buffer plays what the guest made from
		// the previous block's input. The guest rewrites a slot only kSlots blocks later, so both are safe.
		const LONG wanted = m_seq - m_latencyBlocks.load(std::memory_order_relaxed);
		const LONG slot = SlotOf(wanted);
		if (Load(&m_shared->outputSlotSeq[slot]) != wanted)
		{
			m_late.fetch_add(1, std::memory_order_relaxed);
			m_returnPeak.store(DecayedPeak(m_returnPeak.load(std::memory_order_relaxed), 0.0f, frames), std::memory_order_relaxed);
			return;
		}

		float blockPeak = 0.0f;
		const long targets = (std::min<long>)(outputCount, kChannels);
		for (long ch = 0; ch < targets; ++ch)
		{
			if (outputTypes[ch] != ASIOSTInt32LSB) continue;   // the host side is always Int32LSB; never guess
			auto* destination = static_cast<int32_t*>(infos[outputBuffers[ch]].buffers[index]);
			if (!destination) continue;
			const int32_t* left = m_shared->output[slot][0];
			const int32_t* right = m_shared->output[slot][1];
			int64_t peak = 0;
			for (long f = 0; f < frames; ++f)
			{
				// One output channel gets the mono sum; otherwise left to the first, right to the second.
				const double source = targets == 1 ? (static_cast<double>(left[f]) + right[f]) * 0.5 : (ch == 0 ? left[f] : right[f]);
				const int64_t added = static_cast<int64_t>(source * gain);
				const int64_t magnitude = added < 0 ? -added : added;
				if (magnitude > peak) peak = magnitude;
				destination[f] = Saturate(static_cast<int64_t>(destination[f]) + added);
			}
			blockPeak = (std::max)(blockPeak, static_cast<float>(peak / 2147483648.0));
		}
		m_returnPeak.store(DecayedPeak(m_returnPeak.load(std::memory_order_relaxed), blockPeak, frames), std::memory_order_relaxed);
	}

	Status Host::GetStatus() const
	{
		Status status{};
		status.sampleRate = kSampleRate;
		status.lateBlocks = m_late.load(std::memory_order_relaxed);
		status.inputPeak = m_inputPeak.load(std::memory_order_relaxed);
		status.returnPeak = m_returnPeak.load(std::memory_order_relaxed);
		status.latencyBlocks = m_latencyBlocks.load(std::memory_order_relaxed);
		if (!m_shared) return status;
		status.linkOpen = 1;
		status.frames = m_frames;
		status.guestPresent = Load(&m_shared->guestPid) != 0 ? 1 : 0;
		status.connected = GuestConnected(Load(&m_shared->inputSeq)) ? 1 : 0;
		status.guestFrames = Load(&m_shared->guestFrames);
		// Copy the name twice-checked against its sequence so a rename mid-copy is not shown half written.
		for (int attempt = 0; attempt < 3; ++attempt)
		{
			const LONG before = Load(&m_shared->guestNameSeq);
			memcpy(status.guestName, m_shared->guestName, sizeof(status.guestName));
			if (Load(&m_shared->guestNameSeq) == before) break;
		}
		status.guestName[sizeof(status.guestName) - 1] = '\0';
		return status;
	}
}
