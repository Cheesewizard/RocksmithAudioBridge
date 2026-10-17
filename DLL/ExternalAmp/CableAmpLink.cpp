#include "stdafx.h"
#include "CableAmpLink.hpp"
#include "AsioProxy/AmpLinkHost.hpp"
#include "AudioBridge/Engine/OutputRateMatcher.hpp"

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

		std::mutex g_controlMutex;                   // Open / Configure / block size (control threads)
		AmpLink::Host g_host;
		std::atomic<bool> g_open{ false };
		std::atomic<long> g_block{ 128 };            // the block in use; changed only while closed (ApplyBlock)
		int g_blockSetting = 0;                      // control mutex: 0 = Automatic, else 64 / 128 / 256
		bool g_directUsb = false;                    // control mutex: the cable's last physical source was direct USB
		std::atomic<bool> g_safety{ false };         // Extra safety buffer: one more block in the return FIFO
		// FeedInput / Render in progress. ApplyBlock closes the link, then waits for this to reach 0 before it resizes;
		// Open waits for it too before OpenAt resets the state they use.
		std::atomic<int> g_inside{ 0 };
		struct Inside
		{
			Inside() { g_inside.fetch_add(1); }
			~Inside() { g_inside.fetch_sub(1); }
		};

		// Automatic on direct USB starts at 64 and steps up to 128 for the rest of the session once the amp misses blocks:
		// the link keeps AmpLink::kSlots (4) blocks, 5.3 ms at 64, so a heavy chain's processing spike or a USB hiccup of
		// that length loses blocks however much return buffer there is (seen in simulation). 128 keeps 10.7 ms.
		bool g_autoRaised = false;                   // control mutex

		long EffectiveBlock()
		{
			if (g_blockSetting == 0) return g_directUsb && !g_autoRaised ? 64 : 128;
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
		// When each FIFO frame's guitar sample came in from the cable (QPC, 100 ns units; 0 = unknown, e.g. a lost
		// block's silence). Render reads the head's stamp to measure the Tone designer's whole path in the game.
		std::array<UINT64, kReturnCapacity> g_fifoStamp{};
		uint32_t g_head = 0, g_count = 0;
		bool g_primed = false;

		// The return FIFO's speed control (Render). The cable and the output device run on different clocks, and the
		// output pulls in irregular amounts (a Windows pump that wakes late takes two periods at once). The FIFO is read
		// through a resampler whose speed is steered so that the LOWEST fill seen in each quarter second (the trough, the
		// one moment that decides whether the next pull runs dry) sits at a small margin above empty:
		//   - speed, not dropped or repeated samples: a drop/repeat servo here clicked hundreds of times a minute in
		//     simulation on Windows outputs, with no clock drift at all;
		//   - a PI controller on the trough: the integral takes up the clocks' drift, so no fill error is left over;
		//   - a backlog (the trough far above the margin for a whole quarter second, e.g. after a device switch) is
		//     dropped at once; a late Windows pull does not lift the trough, so it never triggers this;
		//   - the margin adapts: every underrun adds to it; it is given back only when the deepest dip of the last 20 s
		//     still left real room above empty, so it settles where this PC plays clean instead of learning by
		//     underrunning again and again (the first version did, a short gap every minute).
		// Tuned in simulation (simulated cable, amp and outputs; exact sample-index checking).
		Audio::SharedOutput::OutputRateMatcher g_matcher;
		std::array<float, kReturnCapacity * 2> g_scratch{};
		constexpr uint32_t kLookahead = 17;           // the resampler reads 16 frames ahead
		constexpr uint32_t kMarginCap = 2400;         // 50 ms: past this the PC cannot keep up anyway
		uint32_t g_margin = 0;                        // frames the trough is steered to
		int64_t g_windowMin = INT64_MAX;
		uint32_t g_windowFrames = 0;
		double g_integral = 0.0;
		uint64_t g_outputFrames = 0, g_lastTroubleAt = 0, g_periodStart = 0;
		int64_t g_periodLowest = INT64_MAX;          // the deepest trough of the current 20 s
		std::atomic<uint64_t> g_underflows{ 0 };
		// Tone designer loading a plugin or a tone (SetGuestLoading, every 100 ms, held 2 s past the end): the amp
		// misses blocks then because it is busy, not because this PC cannot keep up. Such misses do not grow the margin,
		// count toward Automatic's step up, or show as late buffers (otherwise a background plugin load and tone
		// switches grow the margin to 24 ms and raise Automatic to 128 for the rest of the session).
		std::atomic<bool> g_guestLoading{ false };
		std::atomic<uint32_t> g_lateWhileLoading{ 0 };
		uint64_t g_lastGrowAt = 0;                   // output frames at the last margin growth

		// The return FIFO (and the read-back state) is shared by two threads: the cable's capture thread collects
		// finished blocks every time it feeds input, the output pump collects too and plays from it. The link keeps only
		// AmpLink::kSlots (4) blocks, 10.7 ms at 128 frames, so collecting only on a 10 ms output pump lost every block
		// the guest finished early whenever the pump ran a little late (late buffers and 2.7 ms gaps even
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

		UINT64 Now100ns()
		{
			LARGE_INTEGER counter{}, frequency{};
			QueryPerformanceCounter(&counter);
			QueryPerformanceFrequency(&frequency);
			return static_cast<UINT64>(counter.QuadPart / frequency.QuadPart) * 10000000
				+ static_cast<UINT64>(counter.QuadPart % frequency.QuadPart) * 10000000 / frequency.QuadPart;
		}

		// Capture thread: when the first sample of the block being filled came in, and per published block (by seq).
		UINT64 g_fillStartStamp = 0;
		std::array<std::atomic<UINT64>, 64> g_blockStamp{};

		// The Tone designer's delay, cable in to device out, as measured in Render; logged every 30 s (LogLatency).
		std::atomic<uint64_t> g_latSumUs{ 0 }, g_latCount{ 0 }, g_latMaxUs{ 0 }, g_latMinUs{ UINT64_MAX }, g_fillSum{ 0 },
			g_deviceUs{ 0 }, g_trims{ 0 }, g_trimmedFrames{ 0 };
		std::atomic<float> g_recentMs{ 0.0f };   // the same delay, smoothed over about a second, for the Amp sim page
		// The read part of that delay (Input on the readouts): the packet's own length (its first sample waited for the
		// rest) plus the batching wait below. Smoothed over about a second.
		std::atomic<float> g_readMs{ 0.0f };

		// Batching wait, measured. The cable fills packets at a steady 48 kHz, but Windows and the USB host controller
		// can hand several over at once (or late), and each held packet waited on top of its own length. Every
		// delivery's arrival is compared with the sample clock: lateness = arrival - (frames delivered so far at 48 kHz).
		// The earliest delivery of the last 0.5-1 s (the lowest lateness) is the baseline, a packet handed over as soon
		// as it was full; anything above it is batching wait. The windowed minimum follows clock drift between the cable
		// and the PC (100 ppm is 0.1 ms per second). What it cannot see is a delay every packet shares (the cable's
		// converter, the bus): that is not measurable without a loopback and stays out, as the log says.
		// Capture thread only, except the stats.
		int64_t g_schedFrames = 0;                       // frames delivered since the schedule started
		INT64 g_schedStart = 0;                          // QPC (100 ns) when the schedule started
		INT64 g_lastArrival = 0;
		INT64 g_windowMinLate = INT64_MAX, g_prevWindowMinLate = INT64_MAX;
		int64_t g_windowStartFrames = 0;
		std::atomic<uint64_t> g_batchSumUs{ 0 }, g_batchCount{ 0 }, g_batchMaxUs{ 0 };

		// The wait (100 ns) the delivery that just arrived spent held up past the baseline.
		INT64 BatchWait(INT64 arrived, uint32_t frames)
		{
			// A gap (stream (re)started, stalled, or the link reopened) loses the count of frames: start again.
			if (!g_schedStart || arrived - g_lastArrival > 500000) {
				g_schedStart = arrived; g_schedFrames = 0; g_windowStartFrames = 0;
				g_windowMinLate = g_prevWindowMinLate = INT64_MAX;
			}
			g_lastArrival = arrived;
			g_schedFrames += frames;
			const INT64 late = arrived - g_schedStart - g_schedFrames * 625 / 3;   // frames * 10^7 / 48000
			// Much earlier than the baseline: frames were lost (a dropped packet shifts the count), so start again.
			if (g_prevWindowMinLate != INT64_MAX && late < (std::min)(g_windowMinLate, g_prevWindowMinLate) - 20000) {
				g_schedStart = arrived; g_schedFrames = 0; g_windowStartFrames = 0;
				g_windowMinLate = g_prevWindowMinLate = INT64_MAX;
				return 0;
			}
			if (late < g_windowMinLate) g_windowMinLate = late;
			if (g_schedFrames - g_windowStartFrames >= 24000) {   // 0.5 s windows; the baseline spans this one and the last
				g_prevWindowMinLate = g_windowMinLate;
				g_windowMinLate = late;
				g_windowStartFrames = g_schedFrames;
			}
			// Until a whole window has been seen the baseline is not trusted: no wait reported yet.
			if (g_prevWindowMinLate == INT64_MAX) return 0;
			const INT64 wait = late - (std::min)(g_windowMinLate, g_prevWindowMinLate);
			return wait > 0 ? wait : 0;
		}

		void Push(const float* left, const float* right, long frames, UINT64 firstStamp)
		{
			for (long f = 0; f < frames; ++f)
			{
				if (g_count == kReturnCapacity) { g_head = (g_head + 1) % kReturnCapacity; --g_count; }   // oldest out
				const uint32_t at = (g_head + g_count) % kReturnCapacity;
				g_fifo[at * 2] = left[f];
				g_fifo[at * 2 + 1] = right[f];
				g_fifoStamp[at] = firstStamp ? firstStamp + static_cast<UINT64>(f) * 10000000 / 48000 : 0;
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
				UINT64 stamp = g_blockStamp[static_cast<ULONG>(g_nextReturn) % g_blockStamp.size()].load(std::memory_order_acquire);
				if (state == AmpLink::Host::Return::Lost)
				{
					g_host.CountLate();
					std::fill_n(g_left.begin(), block, 0.0f);
					std::fill_n(g_right.begin(), block, 0.0f);
					stamp = 0;
				}
				Push(g_left.data(), g_right.data(), block, stamp);
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
			g_matcher.Reset();
			// The learned margin is kept across a block size change (what this PC needs does not change with it);
			// Open starts it afresh.
			g_windowMin = INT64_MAX;
			g_windowFrames = 0;
			g_integral = 0.0;
			g_outputFrames = g_lastTroubleAt = g_periodStart = g_lastGrowAt = 0;
			g_periodLowest = INT64_MAX;
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
		while (g_inside.load() != 0) YieldProcessor();   // closed, so any FeedInput / Render in here is on its way out
		g_margin = 0;
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

	bool IsOpen() { return g_open.load(std::memory_order_acquire); }

	void FeedInput(const float* mono, uint32_t frames) noexcept
	{
		const Inside inside;
		if (!g_open.load() || !mono || !frames) return;
		const long block = g_block.load(std::memory_order_relaxed);
		if (frames > g_burstFrames.load(std::memory_order_relaxed)) g_burstFrames.store(frames, std::memory_order_relaxed);
		const ASIOBufferInfo info{ 1, 0, { g_inputBlock.data(), g_inputBlock.data() } };
		{ FifoLock lock(false); if (lock.owned) Collect(); }   // before the slots move on
		// This delivery's last sample was ready `wait` before now (held in a batch); sample i (frames - 1 - i) samples
		// before that. Backdating by the wait puts batching into the measured total as well as into Input.
		const UINT64 now = Now100ns();
		const INT64 wait = BatchWait(static_cast<INT64>(now), frames);
		const UINT64 arrived = now - static_cast<UINT64>(wait);
		{
			const uint64_t waitUs = static_cast<uint64_t>(wait) / 10;
			g_batchSumUs.fetch_add(waitUs, std::memory_order_relaxed);
			g_batchCount.fetch_add(1, std::memory_order_relaxed);
			if (waitUs > g_batchMaxUs.load(std::memory_order_relaxed)) g_batchMaxUs.store(waitUs, std::memory_order_relaxed);
			// Time-weighted smoothing (about 1 s) so 1 ms USB packets and 10 ms Windows packets settle alike.
			const float packetMs = frames * 1000.0f / 48000.0f, inputMs = packetMs + wait / 10000.0f;
			const float read = g_readMs.load(std::memory_order_relaxed), alpha = (std::min)(1.0f, packetMs / 1000.0f);
			g_readMs.store(read > 0.0f ? read + (inputMs - read) * alpha : inputMs, std::memory_order_relaxed);
		}
		for (uint32_t i = 0; i < frames; ++i)
		{
			if (g_inputFill == 0) g_fillStartStamp = arrived - static_cast<UINT64>(frames - 1 - i) * 10000000 / 48000;
			const float sample = std::isfinite(mono[i]) ? std::clamp(mono[i], -1.0f, 1.0f) : 0.0f;
			g_inputBlock[g_inputFill++] = static_cast<int32_t>(sample * 2147483520.0f);
			if (g_inputFill >= block)
			{
				const LONG seq = g_host.PublishedSeq() + 1;   // the seq Publish is about to use (this thread publishes alone)
				g_blockStamp[static_cast<ULONG>(seq) % g_blockStamp.size()].store(g_fillStartStamp, std::memory_order_release);
				g_host.Publish(&info, 1, 0, block);
				g_inputFill = 0;
			}
		}
		{ FifoLock lock(false); if (lock.owned) Collect(); }
	}

	void Render(float* stereo, uint32_t frames, uint32_t deviceDelayFrames, uint32_t periodFrames) noexcept
	{
		const Inside inside;
		if (!g_open.load() || !stereo || !frames) return;
		FifoLock lock(true);
		Collect();

		const long block = g_block.load(std::memory_order_relaxed);
		// The smallest margin: the resampler's look-ahead, a little jitter room, and the Extra safety buffer's block.
#ifndef CABLEAMPLINK_JITTER
#define CABLEAMPLINK_JITTER 32
#endif
#ifndef CABLEAMPLINK_START
#define CABLEAMPLINK_START 64
#endif
		const uint32_t base = kLookahead + CABLEAMPLINK_JITTER + (g_safety.load(std::memory_order_relaxed) ? static_cast<uint32_t>(block) : 0u);
		// A different output (its period changed: a device switch, exclusive mode on or off) starts the margin afresh. What
		// a 10 ms shared device taught it is no measure of a 3 ms exclusive one: an 8 ms margin learnt on a
		// shared output would stay in place after switching to exclusive mode, half the delay left there.
		static uint32_t lastPeriod = 0;
		if (periodFrames != lastPeriod)
		{
			if (lastPeriod) g_margin = 0;
			lastPeriod = periodFrames;
		}
		// The Extra safety buffer moves the margin by its block at once, both ways (it only raised the floor before, so a
		// margin already above it did not change and the switch seemed to do nothing).
		static bool lastSafety = false;
		const bool safety = g_safety.load(std::memory_order_relaxed);
		if (safety != lastSafety)
		{
			if (g_margin) g_margin = safety ? g_margin + static_cast<uint32_t>(block) : (g_margin > static_cast<uint32_t>(block) ? g_margin - static_cast<uint32_t>(block) : 0);
			lastSafety = safety;
		}
		if (g_margin < base) g_margin = base + CABLEAMPLINK_START;   // a sensible start; it adapts from here
		auto underflow = [&]()
		{
#ifdef CABLEAMPLINK_TRACE
			LOG_INFO("trace UNDERFLOW out=" << g_outputFrames << " count=" << g_count << " frames=" << frames << " margin=" << g_margin << std::endl);
#endif
			// Ran dry: silence until the FIFO is back up, and a block more margin from now on (a chain's spikes are about
			// a block long; 1 ms steps left a heavy chain running dry over and over), at most once a second: a
			// stall runs dry again on every re-prime, and growing on each took the margin to 24 ms at 256.
			// Not while Tone designer is loading: that stall says nothing about what this PC needs.
			g_primed = false;
			if (!g_guestLoading.load(std::memory_order_relaxed) && (g_lastGrowAt == 0 || g_outputFrames - g_lastGrowAt >= 48000))
			{
				g_margin = (std::min)(kMarginCap, g_margin + (std::max)(48u, static_cast<uint32_t>(block)));
				g_lastGrowAt = g_outputFrames;
				g_lastTroubleAt = g_outputFrames;
			}
			g_underflows.fetch_add(1, std::memory_order_relaxed);
			g_matcher.Reseed();
		};
		g_outputFrames += frames;
		if (!g_primed)
		{
			if (g_count < g_margin + frames + kLookahead) return;   // build up first (start, or after running dry)
			// Start exactly at the target. Nothing has played while building up, so dropping what came in on top is
			// unheard; otherwise the controller took seconds to bring the delay down (8 ms high after Tone designer was
			// turned back on).
			const uint32_t start = g_margin + frames + kLookahead;
			if (g_count > start)
			{
				g_head = (g_head + (g_count - start)) % kReturnCapacity;
				g_count = start;
			}
			g_matcher.Reseed();
			g_primed = true;
			g_windowMin = INT64_MAX;
			g_windowFrames = 0;
		}
		if (g_count < frames + kLookahead) { underflow(); return; }

		// The trough: what stays after this pull. Every quarter second of output, steer it to the margin.
		g_windowMin = (std::min)(g_windowMin, static_cast<int64_t>(g_count) - static_cast<int64_t>(frames));
		g_windowFrames += frames;
		if (g_windowFrames >= 12000)
		{
			const double error = static_cast<double>(g_windowMin) - g_margin;
			const double backlogLine = static_cast<double>((std::max)(periodFrames, frames)) + 480.0;
			if (error > backlogLine)
			{
				// Even the lowest point stayed far above the margin: a backlog, not jitter. Drop it at once.
				const uint32_t excess = static_cast<uint32_t>(error);
				g_head = (g_head + excess) % kReturnCapacity;
				g_count -= excess;
				g_matcher.Reseed();
				g_integral = 0.0;
				g_trims.fetch_add(1, std::memory_order_relaxed);
				g_trimmedFrames.fetch_add(excess, std::memory_order_relaxed);
			}
			else
			{
				// The FIFO answers 1 ppm with 0.048 frames a second, so: proportional 4 (error halves in ~4 s) and integral
				// 0.2, about critically damped. The integral only has to carry the clocks' drift (a few hundred ppm at most),
				// so its share is capped at 500 ppm. Too strong an integral (1.5) wound up, overshot and drained the FIFO dry.
				constexpr double kProportional = 4.0;   // ppm per frame of trough error
				constexpr double kIntegral = 0.2;       // ppm per frame-second
				g_integral = std::clamp(g_integral + error * 0.25, -500.0 / kIntegral, 500.0 / kIntegral);
				const double ppm = std::clamp(kProportional * error + kIntegral * g_integral, -1800.0, 1800.0);
				g_matcher.Measure(-ppm, 0.0);           // the matcher's speed: +ppm reads the FIFO faster
			}
			// Every 20 s: if nothing ran dry and even the deepest dip kept half the margin (at least 96 frames) in hand, give
			// 16 frames back. A PC with plugin spikes keeps the headroom it showed it needs; a steady one comes down to the
			// base. (Giving margin back whenever the dip kept 64 frames ran a spiky chain dry every few minutes; a quarter of
			// the excess every 5 s ran a heavy chain dry 3 to 8 times in 4 minutes. A margin a
			// load used to grow is now never grown in the first place: SetGuestLoading.)
			g_periodLowest = (std::min)(g_periodLowest, g_windowMin);
			if (g_outputFrames - g_periodStart >= 20ull * 48000)
			{
				if (g_lastTroubleAt < g_periodStart && g_periodLowest > (std::max)(96ll, static_cast<long long>(g_margin / 2)) && g_margin > base)
					g_margin = (std::max)(base, g_margin - 16);
				g_periodStart = g_outputFrames;
				g_periodLowest = INT64_MAX;
			}
#ifdef CABLEAMPLINK_TRACE
			LOG_INFO("trace out=" << g_outputFrames << " count=" << g_count << " margin=" << g_margin << " lowest=" << g_periodLowest
				<< " ppm=" << g_matcher.GetCorrectionPpm() << " integral=" << g_integral << " period=" << periodFrames << " frames=" << frames << std::endl);
#endif
			g_windowMin = INT64_MAX;
			g_windowFrames = 0;
		}

		// What the player hears now came in from the cable at the head frame's stamp; it reaches the speaker after the
		// device's own delay on top (the caller's figure: Windows padding, or the ASIO driver's output latency).
		if (g_fifoStamp[g_head])
		{
			const UINT64 now = Now100ns();
			const UINT64 stamp = g_fifoStamp[g_head];
			if (now > stamp)
			{
				const uint64_t deviceUs = static_cast<uint64_t>(deviceDelayFrames) * 1000000 / 48000;
				const uint64_t us = (now - stamp) / 10 + deviceUs;
				g_latSumUs.fetch_add(us, std::memory_order_relaxed);
				g_latCount.fetch_add(1, std::memory_order_relaxed);
				if (us > g_latMaxUs.load(std::memory_order_relaxed)) g_latMaxUs.store(us, std::memory_order_relaxed);
				if (us < g_latMinUs.load(std::memory_order_relaxed)) g_latMinUs.store(us, std::memory_order_relaxed);
				g_fillSum.fetch_add(g_count, std::memory_order_relaxed);
				g_deviceUs.store(deviceUs, std::memory_order_relaxed);
				const float ms = static_cast<float>(us) / 1000.0f, recent = g_recentMs.load(std::memory_order_relaxed);
				g_recentMs.store(recent > 0.0f ? recent + (ms - recent) * 0.02f : ms, std::memory_order_relaxed);
			}
		}
		unsigned consumed = 0;
		const uint32_t capacity = (std::min)(frames, kReturnCapacity);
		const unsigned produced = g_matcher.Read([](unsigned frame, unsigned channel) { return g_fifo[((g_head + frame) % kReturnCapacity) * 2 + channel]; },
			g_count, g_scratch.data(), capacity, consumed);
		for (unsigned f = 0; f < produced; ++f)
		{
			stereo[f * 2] += g_scratch[f * 2];
			stereo[f * 2 + 1] += g_scratch[f * 2 + 1];
		}
		g_head = (g_head + consumed) % kReturnCapacity;
		g_count -= consumed;
		if (produced < frames) underflow();
	}

	void Maintain()
	{
		// Every 100 ms (ExternalAmp's thread): Automatic's step from 64 to 128 when the amp misses blocks (2 within 60 s).
		static std::array<uint32_t, 600> lateHistory{};
		static size_t tick = 0;
		if (!g_open.load()) return;
		// Late blocks while Tone designer loads are not this PC falling behind: set aside, not counted.
		static uint32_t lastLate = 0;
		const uint32_t raw = g_host.GetStatus().lateBlocks;
		if (g_guestLoading.load() && raw > lastLate) g_lateWhileLoading.fetch_add(raw - lastLate);
		lastLate = raw;
		const uint32_t late = raw - g_lateWhileLoading.load();
		const uint32_t before = lateHistory[tick % lateHistory.size()];
		lateHistory[tick % lateHistory.size()] = late;
		const bool full = ++tick > lateHistory.size();
		std::lock_guard<std::mutex> lock(g_controlMutex);
		if (g_blockSetting != 0 || g_autoRaised || g_block.load() != 64) return;
		if (late - (full ? before : 0) < 2) return;
		g_autoRaised = true;
		ApplyBlock("Automatic block size raised to 128: the amp missed blocks at 64 (a heavy chain, or a busy PC)");
	}

	void SetGuestLoading(bool loading)
	{
		static ULONGLONG lastLoading = 0;
		const ULONGLONG now = GetTickCount64();
		if (loading) lastLoading = now;
		g_guestLoading.store(lastLoading != 0 && now - lastLoading < 2000);
	}

	uint32_t LateWhileLoading() { return g_lateWhileLoading.load(); }

	float RecentLatencyMs() { return g_open.load() ? g_recentMs.load(std::memory_order_relaxed) : 0.0f; }
	float RecentReadMs() { return g_open.load() ? g_readMs.load(std::memory_order_relaxed) : 0.0f; }

	void LogLatency()
	{
		static ULONGLONG last = 0;
		const ULONGLONG now = GetTickCount64();
		if (!last) last = now;
		if (now - last < 30000) return;
		last = now;
		const uint64_t count = g_latCount.exchange(0);
		const uint64_t sum = g_latSumUs.exchange(0), maxUs = g_latMaxUs.exchange(0), minUs = g_latMinUs.exchange(UINT64_MAX);
		const uint64_t fill = g_fillSum.exchange(0), trims = g_trims.exchange(0), trimmed = g_trimmedFrames.exchange(0);
		if (!count || !g_open.load()) return;
		const AmpLink::Status status = g_host.GetStatus();
		const uint64_t batchCount = g_batchCount.exchange(0), batchSum = g_batchSumUs.exchange(0), batchMax = g_batchMaxUs.exchange(0);
		LOG_INFO("(TONE DESIGNER LATENCY) input " << g_readMs.load() << " ms (packets " << g_burstFrames.load() << " frames max; batching wait avg "
			<< (batchCount ? batchSum / batchCount / 10 / 100.0 : 0.0) << " ms, max " << batchMax / 100 / 10.0 << " ms over " << batchCount << " deliveries)" << std::endl);
		LOG_INFO("(TONE DESIGNER LATENCY) cable in to speaker out avg " << sum / count / 100 / 10.0 << " ms (min " << minUs / 100 / 10.0
			<< ", max " << maxUs / 100 / 10.0 << "); of that the output device " << g_deviceUs.load() / 100 / 10.0 << " ms and the return FIFO avg "
			<< fill / count * 10 / 48 / 10.0 << " ms; " << g_block.load() << "-sample blocks" << (g_safety.load() ? " + safety block" : "")
			<< "; backlogs dropped " << trims << " (" << trimmed * 10 / 48 / 10.0 << " ms); late buffers so far " << status.lateBlocks
			<< ". Not included: the cable's own converter and USB (about 1-2 ms)." << std::endl);
	}

	AmpLink::Status GetStatus() { return g_open.load() ? g_host.GetStatus() : AmpLink::Status{}; }
	void Configure(bool mixOn, float returnGain) { g_host.Configure(mixOn, returnGain); }
	void SetLatencyBlocks(int blocks) { g_host.SetLatencyBlocks(blocks); g_safety.store(blocks >= 2, std::memory_order_relaxed); }
	void SetPitchSemitones(int semitones) { g_host.SetPitchSemitones(semitones); }
}
