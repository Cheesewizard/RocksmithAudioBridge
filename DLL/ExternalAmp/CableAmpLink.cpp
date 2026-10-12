#include "stdafx.h"
#include "CableAmpLink.hpp"
#include "AsioProxy/AmpLinkHost.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

namespace CableAmpLink
{
	namespace
	{
		// Link block size: the Amp sim page's "Block size" for cable players (ExternalAmp's CableBlockFrames). With an
		// ASIO interface the block is the interface's own buffer; the cable has no ASIO driver, so it is chosen here.
		// Automatic = 64 frames (1.3 ms) when the cable is read directly over USB (1 ms packets), 128 (2.7 ms) on the
		// Windows cable input. The Windows input delivers 480 frames at once, and the link keeps AmpLink::kSlots (4)
		// blocks, so blocks below 128 would be overwritten before the guest read them: 64 is refused there.
		constexpr long kMaxBlock = 256;
		constexpr uint32_t kReturnCapacity = 8192;   // return FIFO, stereo frames
		constexpr uint32_t kServoBand = 64;          // frames either side of the target before a frame is dropped/repeated

		std::mutex g_controlMutex;                   // Open / Close / Configure / block size (control threads)
		AmpLink::Host g_host;
		std::atomic<bool> g_open{ false };
		std::atomic<long> g_block{ 128 };            // the block in use; changed only while closed (ApplyBlock)
		int g_blockSetting = 0;                      // control mutex: 0 = Automatic, else 64 / 128 / 256
		bool g_directUsb = false;                    // control mutex: the cable's last physical source was direct USB
		std::atomic<bool> g_safety{ false };         // Extra safety buffer: one more block in the return FIFO
		// FeedInput / Render in progress. ApplyBlock closes the link, then waits for this to reach 0 before it resizes.
		std::atomic<int> g_inside{ 0 };
		struct Inside
		{
			Inside() { g_inside.fetch_add(1); }
			~Inside() { g_inside.fetch_sub(1); }
		};

		long EffectiveBlock()
		{
			if (g_blockSetting == 0) return g_directUsb ? 64 : 128;
			return !g_directUsb && g_blockSetting < 128 ? 128 : g_blockSetting;
		}

		// Capture thread: the block being filled.
		std::array<int32_t, kMaxBlock> g_inputBlock{};
		long g_inputFill = 0;
		std::atomic<uint32_t> g_burstFrames{ 0 };   // the largest single delivery seen lately (480 on WASAPI, ~48 on USB)

		// Output thread: blocks read back, and the FIFO toward the device.
		LONG g_nextReturn = 0;
		std::array<float, kMaxBlock> g_left{}, g_right{};
		std::array<float, kReturnCapacity * 2> g_fifo{};
		uint32_t g_head = 0, g_count = 0;
		bool g_primed = false;
		float g_fillAverage = 0.0f;
		unsigned g_pumpsSinceAdjust = 0;

		// The return FIFO (and the read-back state) is shared by two threads: the cable's capture thread collects
		// finished blocks every time it feeds input, the output pump collects too and plays from it. The link keeps only
		// AmpLink::kSlots (4) blocks, 10.7 ms at 128 frames, so collecting only on a 10 ms output pump lost every block
		// the guest finished early whenever the pump ran a little late (2026-10-12: late buffers and 2.7 ms gaps even
		// with a light chain). Held for microseconds; the capture thread only tries it.
		std::atomic_flag g_fifoLock = ATOMIC_FLAG_INIT;
		struct FifoLock
		{
			bool owned;
			explicit FifoLock(bool wait) : owned(!g_fifoLock.test_and_set(std::memory_order_acquire))
			{
				while (!owned && wait) { YieldProcessor(); owned = !g_fifoLock.test_and_set(std::memory_order_acquire); }
			}
			~FifoLock() { if (owned) g_fifoLock.clear(std::memory_order_release); }
		};

		void Push(const float* left, const float* right, long frames)
		{
			for (long f = 0; f < frames; ++f)
			{
				if (g_count == kReturnCapacity) { g_head = (g_head + 1) % kReturnCapacity; --g_count; }   // oldest out
				const uint32_t at = (g_head + g_count) % kReturnCapacity;
				g_fifo[at * 2] = left[f];
				g_fifo[at * 2 + 1] = right[f];
				++g_count;
			}
		}

		// Under the FIFO lock: every block the guest has finished, in order. A block it is still working on is waited
		// for; one it already overwrote is counted late and played as silence; with no guest processing (none attached,
		// or Tone designer off in standby) there is nothing to wait for and nothing is late.
		void Collect()
		{
			const long block = g_block.load(std::memory_order_relaxed);
			const LONG published = g_host.PublishedSeq();
			while (g_nextReturn - published <= 0)
			{
				const AmpLink::Host::Return state = g_host.ReadReturn(g_nextReturn, g_left.data(), g_right.data(), block);
				if (state == AmpLink::Host::Return::NotYet) return;
				if (state == AmpLink::Host::Return::NoGuest) { g_nextReturn = published + 1; return; }
				if (state == AmpLink::Host::Return::Lost)
				{
					g_host.CountLate();
					std::fill_n(g_left.begin(), block, 0.0f);
					std::fill_n(g_right.begin(), block, 0.0f);
				}
				Push(g_left.data(), g_right.data(), block);
				++g_nextReturn;
			}
		}
	}

	namespace
	{
		// Control mutex held, link closed, no FeedInput / Render running: (re)open the link at `block` frames and
		// start the read-back afresh. Host::Open keeps the mapping (it is never unmapped while the game runs, as the
		// audio threads use it without a lock) and publishes the new size; the VST host's link driver sees it and
		// has its host reset the stream at that size, as when an ASIO buffer size changes.
		bool OpenAt(long block)
		{
			if (!g_host.Open(block, 1)) return false;
			g_host.SetRunning(true);
			g_block.store(block);
			g_inputFill = 0;
			g_burstFrames.store(0);   // the source may have changed (USB / Windows): measure its deliveries again
			FifoLock lock(true);
			g_nextReturn = g_host.PublishedSeq() + 1;
			g_head = g_count = 0;
			g_primed = false;
			return true;
		}

		// Control mutex held: the setting or the source changed; reopen at the new size if it differs.
		void ApplyBlock(const char* why)
		{
			const long block = EffectiveBlock();
			if (!g_open.load() || block == g_block.load()) return;
			g_open.store(false);
			while (g_inside.load() != 0) YieldProcessor();   // a FeedInput / Render already past its open check
			if (!OpenAt(block)) { LOG_WARNING("(CABLE AMP LINK) Could not change the block size to " << block << "; the amp link is closed" << std::endl); return; }
			g_open.store(true);
			LOG_INFO("(CABLE AMP LINK) " << why << ": now " << block << "-frame blocks; Tone designer restarts its audio at that size" << std::endl);
		}
	}

	void Open()
	{
		std::lock_guard<std::mutex> lock(g_controlMutex);
		if (g_open.load()) return;
		const long block = EffectiveBlock();
		if (!OpenAt(block)) { LOG_WARNING("(CABLE AMP LINK) Could not create the amp link; Tone designer cannot play on the cable" << std::endl); return; }
		g_open.store(true);
		LOG_INFO("(CABLE AMP LINK) Amp link open on the Real Tone Cable: " << block << "-frame blocks; Tone designer can play" << std::endl);
	}

	void SetBlockSetting(int frames)
	{
		std::lock_guard<std::mutex> lock(g_controlMutex);
		g_blockSetting = frames == 64 || frames == 128 || frames == 256 ? frames : 0;
		ApplyBlock("Block size changed");
	}

	void SetDirectUsb(bool directUsb)
	{
		std::lock_guard<std::mutex> lock(g_controlMutex);
		if (g_directUsb == directUsb) return;
		g_directUsb = directUsb;
		ApplyBlock(directUsb ? "Cable now read directly over USB" : "Cable now read through Windows");
	}

	int GetBlockSetting() { std::lock_guard<std::mutex> lock(g_controlMutex); return g_blockSetting; }
	bool IsDirectUsb() { std::lock_guard<std::mutex> lock(g_controlMutex); return g_directUsb; }

	void Close()
	{
		std::lock_guard<std::mutex> lock(g_controlMutex);
		if (!g_open.exchange(false)) return;
		g_host.SetRunning(false);   // not Close(): see Open
	}

	bool IsOpen() { return g_open.load(std::memory_order_acquire); }

	void FeedInput(const float* mono, uint32_t frames) noexcept
	{
		const Inside inside;
		if (!g_open.load() || !mono || !frames) return;
		const long block = g_block.load(std::memory_order_relaxed);
		if (frames > g_burstFrames.load(std::memory_order_relaxed)) g_burstFrames.store(frames, std::memory_order_relaxed);
		const ASIOBufferInfo info{ 1, 0, { g_inputBlock.data(), g_inputBlock.data() } };
		{ FifoLock lock(false); if (lock.owned) Collect(); }   // before the slots move on
		for (uint32_t i = 0; i < frames; ++i)
		{
			const float sample = std::isfinite(mono[i]) ? std::clamp(mono[i], -1.0f, 1.0f) : 0.0f;
			g_inputBlock[g_inputFill++] = static_cast<int32_t>(sample * 2147483520.0f);
			if (g_inputFill == block)
			{
				g_host.Publish(&info, 1, 0, block);
				g_inputFill = 0;
			}
		}
		{ FifoLock lock(false); if (lock.owned) Collect(); }
	}

	void Render(float* stereo, uint32_t frames) noexcept
	{
		const Inside inside;
		if (!g_open.load() || !stereo || !frames) return;
		FifoLock lock(true);
		Collect();

		// The FIFO swings by a whole input delivery every cycle (the cable delivers 480 frames at once, the device takes its
		// period at other moments), so the drift servo works on the AVERAGE fill, with a band wider than a delivery, and
		// moves at most one frame per ten pumps (~200 ppm at a 10 ms period, plenty for two crystals). Correcting the raw
		// fill against a narrow band clicked on almost every pump (2026-10-12, heard as crackle).
		const uint32_t burst = g_burstFrames.load(std::memory_order_relaxed);
		// Extra safety buffer: one more block of return in hand (on the ASIO path it delays the read by a block instead).
		const long block = g_block.load(std::memory_order_relaxed);
		const float centre = static_cast<float>(frames + burst / 2 + block * (g_safety.load(std::memory_order_relaxed) ? 2 : 1));
		const float band = static_cast<float>(burst / 2 + kServoBand);
		if (!g_primed)
		{
			if (static_cast<float>(g_count) < centre) return;   // build up to the centre first (start, or after running dry)
			g_primed = true;
			g_fillAverage = static_cast<float>(g_count);
			g_pumpsSinceAdjust = 0;
		}
		g_fillAverage += (static_cast<float>(g_count) - g_fillAverage) * 0.05f;
		bool repeat = false;
		if (++g_pumpsSinceAdjust >= 10)
		{
			if (g_fillAverage > centre + band && g_count > frames)
			{
				g_head = (g_head + 1) % kReturnCapacity;   // the cable runs fast against the device: drop one frame
				--g_count;
				g_pumpsSinceAdjust = 0;
			}
			else if (g_fillAverage < centre - band && g_count > 1)
			{
				repeat = true;                             // runs slow: play one frame twice
				g_pumpsSinceAdjust = 0;
			}
		}
		const uint32_t available = (std::min)(frames, g_count);
		for (uint32_t f = 0; f < available; ++f)
		{
			const uint32_t at = (g_head + f) % kReturnCapacity;
			stereo[f * 2] += g_fifo[at * 2];
			stereo[f * 2 + 1] += g_fifo[at * 2 + 1];
		}
		const uint32_t consumed = repeat && available > 1 ? available - 1 : available;
		g_head = (g_head + consumed) % kReturnCapacity;
		g_count -= consumed;
		if (available < frames) g_primed = false;   // ran dry: build up again rather than crackle
	}

	AmpLink::Status GetStatus() { return g_open.load() ? g_host.GetStatus() : AmpLink::Status{}; }
	void Configure(bool mixOn, float returnGain) { g_host.Configure(mixOn, returnGain); }
	void SetLatencyBlocks(int blocks) { g_host.SetLatencyBlocks(blocks); g_safety.store(blocks >= 2, std::memory_order_relaxed); }
	void SetPitchSemitones(int semitones) { g_host.SetPitchSemitones(semitones); }
}
