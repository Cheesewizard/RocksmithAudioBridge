#pragma once

// Host side of the external amp link (see AmpLink.hpp): runs inside Rocksmith's copy of the proxy.
// Open/Close/SetRunning/Configure run on control threads; Publish and MixReturn run on the audio thread,
// once per buffer, and never allocate, lock or wait.

#include "AmpLink.hpp"
#include "AsioInterface.h"
#include <atomic>

namespace AmpLink
{
	class Host
	{
	public:
		~Host() { Close(); }

		// Create (or reopen) the shared block for a stream of `frames` per buffer. Safe to call again on every
		// createBuffers; a guest that is already attached stays attached.
		bool Open(long frames, long inputChannels);
		void Close();
		void SetRunning(bool running);

		// The player's External amp switch and the return level (linear). Mixing also needs a connected guest.
		void Configure(bool mixOn, float returnGain);
		// 1 = the guest's block plays on the next buffer; 2 = one buffer later, so a buffer that arrives early
		// (a busy PC, an uneven driver) still finds the guest's block ready. Clamped to 1..2.
		void SetLatencyBlocks(int blocks);
		// The Drop Pedal's input shift in semitones, passed to the guest through the shared block. Clamped to +-24.
		void SetPitchSemitones(int semitones);

		// Start of a buffer, after the host's input buffers hold this block's samples (Int32LSB): copy up to two
		// input channels into the next slot and wake the guest.
		void Publish(const ASIOBufferInfo* infos, long numChannels, long index, long frames) noexcept;

		// After the game has filled its output: add the guest's output for the PREVIOUS block to the first two
		// output channels (Int32LSB buffers; `outputBuffers` are the host's buffer-info indices).
		void MixReturn(const ASIOBufferInfo* infos, const long* outputBuffers, const long* outputTypes, long outputCount,
			long index, long frames) noexcept;

		Status GetStatus() const;

	private:
		bool GuestConnected(LONG newestSeq) const noexcept;

		HANDLE m_mapping = nullptr;
		HANDLE m_event = nullptr;
		Shared* m_shared = nullptr;
		LONG m_seq = 0;                       // audio thread: the block being published
		long m_frames = 0;
		std::atomic<bool> m_mixOn{ false };
		std::atomic<float> m_gain{ 1.0f };
		std::atomic<int> m_latencyBlocks{ 1 };
		std::atomic<int> m_pitchSemitones{ 0 };
		std::atomic<uint32_t> m_late{ 0 };
		std::atomic<float> m_inputPeak{ 0.0f };
		std::atomic<float> m_returnPeak{ 0.0f };
	};
}
