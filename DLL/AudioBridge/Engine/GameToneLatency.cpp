#include "stdafx.h"
#include "GameToneLatency.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

namespace Audio::GameToneLatency
{
	namespace
	{
		constexpr uint32_t kChunk = 48;                 // 1 ms at 48 kHz: the detectors work on 1 ms peaks
		constexpr UINT64 kChunkTime = 10000;            // 1 ms in 100 ns
		constexpr float kRiseDb = 15.0f;                // an attack: this far above the loudest of the previous 30 ms
		constexpr float kFloorDb = -45.0f;              // and at least this loud
		constexpr UINT64 kRefractory = 800000;          // 80 ms before the next attack counts

		// One detector per side: 1 ms peaks into a 30 ms history.
		struct Onsets
		{
			std::array<float, 30> history{};
			size_t at = 0;
			float peak = 0.0f;
			uint32_t filled = 0;
			UINT64 lastOnset = 0;

			// Feeds samples (stride apart) stamped from `first`; calls found(time) for each attack.
			template<class Found>
			void Feed(const float* samples, uint32_t frames, uint32_t stride, UINT64 first, Found found)
			{
				for (uint32_t i = 0; i < frames; ++i)
				{
					peak = (std::max)(peak, std::fabs(samples[i * stride]));
					if (++filled < kChunk) continue;
					const float db = peak > 1e-6f ? 20.0f * std::log10(peak) : -120.0f;
					float before = -120.0f;
					for (float h : history) before = (std::max)(before, h);
					const UINT64 time = first + static_cast<UINT64>(i) * 10000000 / 48000;
					if (db > kFloorDb && db - before > kRiseDb && time - lastOnset > kRefractory)
					{
						lastOnset = time;
						found(time);
					}
					history[at] = db;
					at = (at + 1) % history.size();
					peak = 0.0f;
					filled = 0;
				}
			}
		};

		std::mutex g_mutex;
		Onsets g_input, g_output;
		std::array<UINT64, 8> g_picks{};                 // recent input attacks, 0 = matched or empty
		size_t g_pickAt = 0;
		std::array<float, 15> g_lags{};
		size_t g_lagCount = 0, g_lagAt = 0;
		std::atomic<float> g_median{ 0.0f };
		std::atomic<UINT64> g_lastMatch{ 0 };

		void Match(UINT64 heard)
		{
			// The latest unmatched pick 2-200 ms before this attack.
			size_t best = g_picks.size();
			for (size_t i = 0; i < g_picks.size(); ++i)
			{
				const UINT64 pick = g_picks[i];
				if (!pick || pick > heard) continue;
				const UINT64 lag = heard - pick;
				if (lag < 20000 || lag > 2000000) continue;
				if (best == g_picks.size() || pick > g_picks[best]) best = i;
			}
			if (best == g_picks.size()) return;
			const float ms = static_cast<float>(heard - g_picks[best]) / 10000.0f;
			g_picks[best] = 0;
			g_lags[g_lagAt] = ms;
			g_lagAt = (g_lagAt + 1) % g_lags.size();
			g_lagCount = (std::min)(g_lagCount + 1, g_lags.size());
			std::array<float, 15> sorted = g_lags;
			std::sort(sorted.begin(), sorted.begin() + g_lagCount);
			g_median.store(sorted[g_lagCount / 2]);
			g_lastMatch.store(GetTickCount64());
		}
	}

	UINT64 Now100ns()
	{
		LARGE_INTEGER counter{}, frequency{};
		QueryPerformanceCounter(&counter);
		QueryPerformanceFrequency(&frequency);
		return static_cast<UINT64>(counter.QuadPart / frequency.QuadPart) * 10000000
			+ static_cast<UINT64>(counter.QuadPart % frequency.QuadPart) * 10000000 / frequency.QuadPart;
	}

	void Input(const float* mono, uint32_t frames, UINT64 lastSample) noexcept
	{
		if (!mono || !frames) return;
		const UINT64 first = lastSample - static_cast<UINT64>(frames - 1) * 10000000 / 48000;
		std::lock_guard<std::mutex> lock(g_mutex);
		g_input.Feed(mono, frames, 1, first, [](UINT64 time) { g_picks[g_pickAt] = time; g_pickAt = (g_pickAt + 1) % g_picks.size(); });
	}

	void Output(const float* stereo, uint32_t frames, UINT64 heard) noexcept
	{
		if (!stereo || !frames) return;
		std::lock_guard<std::mutex> lock(g_mutex);
		g_output.Feed(stereo, frames, 2, heard, [](UINT64 time) { Match(time); });
	}

	// Shown while recent: 30 s without a match (not playing, or Tone designer playing instead) hides it.
	bool Valid() { return g_lagCount >= 5 && GetTickCount64() - g_lastMatch.load() < 30000; }
	float MedianMs() { return g_median.load(); }
}
