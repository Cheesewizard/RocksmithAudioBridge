#include "stdafx.h"
#include "GameToneLatency.hpp"
#include "CableGamePeriod.hpp"
#include "AudioBridgeEngine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace Audio::GameToneLatency
{
	namespace
	{
		// Both sides as 1 kHz high-passed 1 ms peaks (dBFS) on one clock: slot = time in ms, modulo the ring. The output's
		// slots are by when it will be heard, so a pick in the input and the game's answer to it sit their delay apart.
		// Analyse, once a second, times each clear pick to the first sharp rise in the output after it.
		constexpr uint32_t kChunk = 48;             // 1 ms at 48 kHz
		constexpr size_t kRing = 4096;              // ms of history
		constexpr int kMinLag = 2, kMaxLag = 150;   // ms
		constexpr float kSilentDb = -60.0f;

		struct Side
		{
			std::array<float, kRing> db{};
			std::array<int64_t, kRing> stamp{};    // which ms each slot holds (-1 = never written)
			float peak = 0.0f;
			uint32_t filled = 0;
			int64_t latest = -1;                   // newest ms written
			float hpIn = 0.0f, hpOut = 0.0f;       // one-pole 1 kHz high-pass state

			Side() { stamp.fill(-1); }

			void Feed(const float* samples, uint32_t frames, uint32_t stride, UINT64 first)
			{
				for (uint32_t i = 0; i < frames; ++i)
				{
					const float x = samples[i * stride];
					hpOut = 0.884f * (hpOut + x - hpIn);   // fc 1 kHz at 48 kHz
					hpIn = x;
					peak = (std::max)(peak, std::fabs(hpOut));
					if (++filled < kChunk) continue;
					const int64_t ms = static_cast<int64_t>((first + static_cast<UINT64>(i) * 10000000 / 48000) / 10000);
					const size_t slot = static_cast<size_t>(ms % kRing);
					db[slot] = peak > 1e-6f ? (std::max)(kSilentDb, 20.0f * std::log10(peak)) : kSilentDb;
					stamp[slot] = ms;
					latest = (std::max)(latest, ms);
					peak = 0.0f;
					filled = 0;
				}
			}

			float Level(int64_t ms) const
			{
				if (ms < 0) return kSilentDb;
				const size_t slot = static_cast<size_t>(ms % kRing);
				return stamp[slot] == ms ? db[slot] : kSilentDb;
			}
		};

		std::mutex g_mutex;
		Side g_input, g_output;
		std::array<float, 7> g_results{};
		size_t g_resultCount = 0, g_resultAt = 0;
		std::atomic<float> g_median{ 0.0f };
		std::atomic<ULONGLONG> g_lastResult{ 0 }, g_lastRun{ 0 };
		std::atomic<float> g_outputMs{ -1.0f };       // smoothed: from a game packet handed over to it being heard
		std::atomic<float> g_internalMs{ -1.0f };     // what the game adds itself, for this session's game block

		// HKCU\Software\RSMods\AsioProxy\GameToneInternal4_<block>: tenths of a ms, per game block size.
		// Filed under the block the game really runs at (a requested size the game has not taken yet must not get it).
		std::wstring InternalValueName()
		{
			const UINT32 running = SharedOutput::GameOutputBlockFrames();
			// "4": values from the first three estimators (repeats, then cable noise, taken for the delay) are never read again. They could not be
			// deleted from outside reliably (a packaged (MSIX) app sees its own copy of HKCU).
			return L"GameToneInternal4_" + std::to_wstring(running ? running : CableGamePeriod::Frames());
		}

		float Internal()
		{
			float internal = g_internalMs.load();
			if (internal >= 0.0f) return internal;
			DWORD tenths = 0, size = sizeof(tenths);
			if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy",InternalValueName().c_str(), RRF_RT_REG_DWORD, nullptr, &tenths, &size) == ERROR_SUCCESS)
				internal = tenths / 10.0f;
			else internal = -2.0f;   // nothing remembered: checked, do not read again
			g_internalMs.store(internal);
			return internal;
		}

		int64_t g_examined = -1;   // input ms already searched for picks

		// One measurement per clear pick, timed like hand measurements on recordings: the first sharp rise
		// in the game's output after the pick. Both sides are 1 kHz high-passed 1 ms peaks. A pick: a 15 dB rise over the
		// loudest of the previous 40 ms, to above -35 dBFS (noise and a backing track do not make those on the cable). Its
		// answer: the first ms, 3 to 150 ms later, where the output is 8 dB above its loudest of the 20 ms before the pick
		// and above -50 dBFS. On a recording with the music at 0 this gave 20-22 ms from the game reading the input to its
		// output for 15 of 22 plucks; matching attack shapes instead scattered (15-92 ms), and a whole-window correlation
		// read the cable's background noise as picks.
		void Analyse()
		{
			constexpr int kBefore = 40, kBaseline = 20, kSkip = 300;
			std::vector<float> in, out;
			int64_t begin = 0, end = 0, from = 0;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				end = (std::min)(g_input.latest, g_output.latest - kMaxLag);
				if (end < 0) return;
				begin = (std::max)(g_examined + 1, end - 3000);
				if (end <= begin) return;
				g_examined = end;
				from = begin - kBefore;
				in.resize(static_cast<size_t>(end - from + 1));
				out.resize(static_cast<size_t>(end + kMaxLag - from + 1));
				for (size_t k = 0; k < in.size(); ++k) in[k] = g_input.Level(from + static_cast<int64_t>(k));
				for (size_t k = 0; k < out.size(); ++k) out[k] = g_output.Level(from + static_cast<int64_t>(k));
			}
			for (int64_t ms = begin; ms <= end; ++ms)
			{
				const size_t at = static_cast<size_t>(ms - from);
				float before = -120.0f;
				for (int k = 1; k <= kBefore; ++k) before = (std::max)(before, in[at - k]);
				if (in[at] < -35.0f || in[at] - before < 15.0f) continue;
				ms += kSkip;   // the note's own ringing is not another pick
				float baseline = -120.0f;
				for (int k = 0; k <= kBaseline; ++k) baseline = (std::max)(baseline, out[at - k]);
				int lag = 0;
				for (int k = (std::max)(3, kMinLag); k <= kMaxLag && !lag; ++k)
					if (out[at + k] > -50.0f && out[at + k] > baseline + 8.0f) lag = k;
				if (!lag) continue;   // the output did not answer (game amp silenced, Tone designer playing)
				g_results[g_resultAt] = static_cast<float>(lag);
				g_resultAt = (g_resultAt + 1) % g_results.size();
				g_resultCount = (std::min)(g_resultCount + 1, g_results.size());
				std::array<float, 7> sorted = g_results;
				std::sort(sorted.begin(), sorted.begin() + g_resultCount);
				g_median.store(sorted[g_resultCount / 2]);
				g_lastResult.store(GetTickCount64());
				LOG_INFO("(GAME TONE LATENCY) pick answered after " << lag << " ms; output part " << g_outputMs.load()
					<< " ms; median of " << g_resultCount << ": " << sorted[g_resultCount / 2] << " ms" << std::endl);
				// Remember what the game adds itself (the measured whole minus the output part), for this game block.
				const float output = g_outputMs.load();
				if (g_resultCount >= 3 && output >= 0.0f)
				{
					const float internal = (std::max)(0.0f, sorted[g_resultCount / 2] - output);
					g_internalMs.store(internal);
					const DWORD tenths = static_cast<DWORD>(internal * 10.0f + 0.5f);
					RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", InternalValueName().c_str(), REG_DWORD, &tenths, sizeof(tenths));
				}
			}
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
		g_input.Feed(mono, frames, 1, first);
	}

	void Output(const float* stereo, uint32_t frames, UINT64 heard) noexcept
	{
		if (!stereo || !frames) return;
		const UINT64 now = Now100ns();
		const float ms = heard > now ? static_cast<float>(heard - now) / 10000.0f : 0.0f;
		const float before = g_outputMs.load(std::memory_order_relaxed);
		g_outputMs.store(before < 0.0f ? ms : before + (ms - before) * 0.01f, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(g_mutex);
		g_output.Feed(stereo, frames, 2, heard);
	}

	// Called by the overlay: the comparison runs there, once a second, never on an audio thread.
	bool Valid()
	{
		const ULONGLONG now = GetTickCount64();
		// The in-game HUD and the overlay both ask, every frame: one of them runs the comparison, once a second.
		ULONGLONG last = g_lastRun.load();
		if (now - last >= 1000 && g_lastRun.compare_exchange_strong(last, now)) Analyse();
		return g_resultCount >= 3 && now - g_lastResult.load() < 60000;
	}

	float MedianMs() { return g_median.load(); }

	bool Available() { return Valid() || (Internal() >= 0.0f && g_outputMs.load() >= 0.0f); }

	float CurrentMs() { return Valid() ? MedianMs() : Internal() + g_outputMs.load(); }
}
