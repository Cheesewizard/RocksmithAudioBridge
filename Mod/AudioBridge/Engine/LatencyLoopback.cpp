#include "stdafx.h"
#include "LatencyLoopback.hpp"
#include "GameToneLatency.hpp"
#include "../Cable/CableInput.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace Audio::LatencyLoopback
{
	namespace
	{
		constexpr uint32_t kRate = 48000;
		// Three chirps, 450 ms apart, so one answer lost under the music or a gate is outvoted. 450 ms: more than the
		// longest answer looked for plus a chirp, so one chirp's window never finds the next chirp. 40 ms, 300 Hz to 2.5 kHz:
		// inside a guitar amp's band, so the amp and cab pass it (distorted, but its sweep still correlates), and long
		// enough to stand above a backing track. 0.35 peak: about a firm pick on the cable.
		constexpr int kChirps = 3;
		constexpr uint32_t kChirpFrames = kRate * 40 / 1000;
		constexpr uint32_t kSpacing = kRate * 450 / 1000;
		constexpr uint32_t kInjectFrames = kSpacing * (kChirps - 1) + kChirpFrames + kRate * 60 / 1000;
		// The game's answer is looked for 1 to 200 ms after each chirp; the recording runs until the last one's window ends.
		constexpr uint32_t kMinLag = kRate / 1000, kMaxLag = kRate * 200 / 1000;
		constexpr uint32_t kRecordFrames = kSpacing * (kChirps - 1) + kMaxLag + kChirpFrames + kRate * 20 / 1000;
		constexpr float kAmplitude = 0.35f;
		constexpr float kMinScore = 0.2f;        // normalized correlation a real answer reaches
		constexpr float kAgreeMs = 1.5f;         // answers further than this from the median are thrown out
		constexpr ULONGLONG kTimeoutMs = 5000;   // the cable or the output never came: give up and unmute

		enum class State { Idle, Armed, Injecting, Recording, Analysing };

		std::mutex g_mutex;
		State g_state = State::Idle;
		ULONGLONG g_armedTick = 0;
		UINT64 g_injectStart = 0;        // capture time of the first replaced sample (100 ns)
		uint32_t g_injectPos = 0;
		std::vector<float> g_signal;     // what is injected, kInjectFrames long
		std::vector<float> g_recorded;   // the game's output on the injection's timeline, kRecordFrames long
		std::vector<uint8_t> g_have;     // which recorded samples arrived
		// Output packets are laid end to end (the stream is continuous), not each at its own stamp: a stamp is taken when
		// the output thread wakes, so it jitters by up to a millisecond, which offline tests showed as 1-2 ms of error.
		// The jitter is averaged out instead (g_offsetSum / g_offsetCount: stamped minus laid position, in samples) and
		// added to the result. A packet more than 3 ms off where it should follow (a trimmed backlog, a gap) starts afresh.
		int64_t g_nextPos = INT64_MIN;
		double g_offsetSum = 0.0;
		uint32_t g_offsetCount = 0;
		uint32_t g_reanchors = 0;
		std::string g_status = "Not measured this launch.";
		std::atomic<float> g_lastMs{ -1.0f };
		std::atomic<bool> g_busy{ false };   // lock-free gate for the audio threads

		const std::vector<float>& Chirp()
		{
			static const std::vector<float> chirp = [] {
				std::vector<float> c(kChirpFrames);
				const double f0 = 300.0, f1 = 2500.0, length = kChirpFrames / static_cast<double>(kRate);
				const double taper = 0.1 * kChirpFrames;   // Tukey window: 10% fades, no click at either end
				for (uint32_t i = 0; i < kChirpFrames; ++i)
				{
					const double t = i / static_cast<double>(kRate);
					const double phase = 2.0 * 3.14159265358979 * (f0 * t + (f1 - f0) * t * t / (2.0 * length));
					double w = 1.0;
					if (i < taper) w = 0.5 - 0.5 * std::cos(3.14159265358979 * i / taper);
					else if (i > kChirpFrames - 1 - taper) w = 0.5 - 0.5 * std::cos(3.14159265358979 * (kChirpFrames - 1 - i) / taper);
					c[i] = static_cast<float>(kAmplitude * w * std::sin(phase));
				}
				return c;
			}();
			return chirp;
		}

		// Called with g_mutex held.
		void Finish(State next, const std::string& status)
		{
			g_state = next;
			g_status = status;
			if (next == State::Idle) g_busy.store(false);
		}

		// Called with g_mutex held: the cable or the output stopped coming.
		void CheckTimeout()
		{
			if (g_state == State::Idle || g_state == State::Analysing) return;
			if (GetTickCount64() - g_armedTick < kTimeoutMs) return;
			const char* why = g_state == State::Armed ? "no cable input arrived" : "the game's output stopped";
			LOG_WARNING("(LATENCY LOOPBACK) gave up: " << why << std::endl);
			Finish(State::Idle, std::string("Not measured: ") + why + ".");
		}

		void Analyse(std::vector<float> recorded, std::vector<uint8_t> have, double offsetSamples, uint32_t reanchors)
		{
			const std::vector<float>& chirp = Chirp();
			double chirpEnergy = 0.0;
			for (float c : chirp) chirpEnergy += static_cast<double>(c) * c;
			std::vector<float> lags;
			std::ostringstream detail;
			detail << std::fixed << std::setprecision(2);
			for (int k = 0; k < kChirps; ++k)
			{
				const uint32_t base = kSpacing * k;
				// Running energy of the output window, so each lag's score is normalized (amp gain and music level apart).
				double windowEnergy = 0.0;
				size_t missing = 0;
				for (uint32_t j = 0; j < kChirpFrames; ++j)
				{
					const float o = recorded[base + kMinLag + j];
					windowEnergy += static_cast<double>(o) * o;
				}
				float bestScore = 0.0f;
				uint32_t bestLag = 0;
				for (uint32_t lag = kMinLag; lag <= kMaxLag; ++lag)
				{
					const size_t at = base + lag;
					if (lag > kMinLag)
					{
						const float gone = recorded[at - 1], added = recorded[at + kChirpFrames - 1];
						windowEnergy += static_cast<double>(added) * added - static_cast<double>(gone) * gone;
					}
					if (!have[at]) { ++missing; continue; }
					if (windowEnergy <= 1e-12) continue;
					double dot = 0.0;
					for (uint32_t j = 0; j < kChirpFrames; ++j) dot += static_cast<double>(chirp[j]) * recorded[at + j];
					const float score = static_cast<float>(std::fabs(dot) / std::sqrt(chirpEnergy * windowEnergy));
					if (score > bestScore) { bestScore = score; bestLag = lag; }
				}
				const float ms = bestLag * 1000.0f / kRate;
				detail << (k ? "; " : "") << "chirp " << k + 1 << ": " << ms << " ms score " << bestScore;
				if (missing) detail << " (" << missing << " lags without output)";
				if (bestScore >= kMinScore) lags.push_back(ms);
			}

			std::lock_guard<std::mutex> lock(g_mutex);
			if (lags.size() < 2)
			{
				LOG_WARNING("(LATENCY LOOPBACK) no clear answer: " << detail.str() << std::endl);
				Finish(State::Idle, "Not measured: the game's output did not answer clearly. Is the game's amp playing (Compare on, or Tone designer off)?");
				return;
			}
			std::sort(lags.begin(), lags.end());
			const float median = lags[lags.size() / 2];
			std::vector<float> agreeing;
			for (float lag : lags) if (std::fabs(lag - median) <= kAgreeMs) agreeing.push_back(lag);
			if (agreeing.size() < 2)
			{
				LOG_WARNING("(LATENCY LOOPBACK) answers disagree: " << detail.str() << std::endl);
				Finish(State::Idle, "Not measured: the answers disagreed. Try again with the music paused.");
				return;
			}
			float sum = 0.0f;
			for (float lag : agreeing) sum += lag;
			const float offsetMs = static_cast<float>(offsetSamples * 1000.0 / kRate);
			const float total = sum / agreeing.size() + offsetMs;
			detail << "; stamp offset " << offsetMs << " ms, re-anchored " << reanchors << "x";
			g_lastMs.store(total);
			LOG_INFO("(LATENCY LOOPBACK) game tones " << std::fixed << std::setprecision(2) << total << " ms from the cable sample to the speaker ("
				<< agreeing.size() << " of " << kChirps << " chirps agree; " << detail.str() << ")" << std::endl);
			GameToneLatency::NoteMeasuredMs(total);
			std::ostringstream status;
			status << "Measured " << std::fixed << std::setprecision(1) << total << " ms (cable to speaker; the cable's converter and USB excluded).";
			Finish(State::Idle, status.str());
		}
	}

	bool Start(std::string& why)
	{
		if (CableInput::IsAsioPath()) { why = "Measuring works in Cable mode only for now."; return false; }
		std::lock_guard<std::mutex> lock(g_mutex);
		CheckTimeout();
		if (g_state != State::Idle) { why = "A measurement is already running."; return false; }
		if (g_signal.empty())
		{
			g_signal.assign(kInjectFrames, 0.0f);
			const std::vector<float>& chirp = Chirp();
			for (int k = 0; k < kChirps; ++k) std::copy(chirp.begin(), chirp.end(), g_signal.begin() + kSpacing * k);
		}
		g_recorded.assign(kRecordFrames + kMaxLag + kChirpFrames, 0.0f);
		g_have.assign(g_recorded.size(), 0);
		g_injectStart = 0;
		g_injectPos = 0;
		g_nextPos = INT64_MIN;
		g_offsetSum = 0.0;
		g_offsetCount = 0;
		g_reanchors = 0;
		g_armedTick = GetTickCount64();
		g_state = State::Armed;
		g_status = "Measuring...";
		g_busy.store(true);
		LOG_INFO("(LATENCY LOOPBACK) armed: " << kChirps << " chirps on Player 1's cable, output muted until the answer is recorded" << std::endl);
		return true;
	}

	bool Running()
	{
		if (!g_busy.load()) return false;
		std::lock_guard<std::mutex> lock(g_mutex);
		CheckTimeout();
		return g_state != State::Idle;
	}

	std::string Status()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		CheckTimeout();
		return g_status;
	}

	float LastMs() { return g_lastMs.load(); }

	bool Inject(float* samples, uint32_t frames, UINT64 firstSample) noexcept
	{
		if (!g_busy.load(std::memory_order_relaxed) || !samples || !frames) return false;
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_state == State::Armed)
		{
			g_injectStart = firstSample;
			g_injectPos = 0;
			g_state = State::Injecting;
		}
		if (g_state != State::Injecting) return false;
		for (uint32_t i = 0; i < frames; ++i)
			samples[i] = g_injectPos + i < kInjectFrames ? g_signal[g_injectPos + i] : 0.0f;
		g_injectPos += frames;
		if (g_injectPos >= kInjectFrames) g_state = State::Recording;
		return true;
	}

	void Capture(const float* stereo, uint32_t frames, UINT64 heard) noexcept
	{
		if (!g_busy.load(std::memory_order_relaxed) || !stereo || !frames) return;
		std::unique_lock<std::mutex> lock(g_mutex);
		if ((g_state != State::Injecting && g_state != State::Recording) || !g_injectStart) return;
		// Where this packet sits on the injection's timeline (sample 0 = the first injected sample's capture time).
		const int64_t stamped = (static_cast<int64_t>(heard) - static_cast<int64_t>(g_injectStart)) * kRate / 10000000;
		int64_t first = g_nextPos;
		if (g_nextPos == INT64_MIN || std::llabs(stamped - g_nextPos) > kRate * 3 / 1000)
		{
			if (g_nextPos != INT64_MIN) ++g_reanchors;
			first = stamped;
			g_offsetSum = 0.0;
			g_offsetCount = 0;
		}
		g_offsetSum += static_cast<double>(stamped - first);
		++g_offsetCount;
		g_nextPos = first + frames;
		for (uint32_t i = 0; i < frames; ++i)
		{
			const int64_t at = first + i;
			if (at < 0 || at >= static_cast<int64_t>(g_recorded.size())) continue;
			g_recorded[static_cast<size_t>(at)] = 0.5f * (stereo[i * 2] + stereo[i * 2 + 1]);
			g_have[static_cast<size_t>(at)] = 1;
		}
		if (g_state != State::Recording || first + frames < static_cast<int64_t>(kRecordFrames + kChirpFrames)) return;
		g_state = State::Analysing;
		g_status = "Measuring... (working it out)";
		std::vector<float> recorded = std::move(g_recorded);
		std::vector<uint8_t> have = std::move(g_have);
		const double offset = g_offsetCount ? g_offsetSum / g_offsetCount : 0.0;
		const uint32_t reanchors = g_reanchors;
		g_recorded.clear();
		g_have.clear();
		lock.unlock();
		// Off the audio thread: about 80 million multiply-adds.
		std::thread([recorded = std::move(recorded), have = std::move(have), offset, reanchors]() mutable {
			try { Analyse(std::move(recorded), std::move(have), offset, reanchors); }
			catch (...)
			{
				std::lock_guard<std::mutex> guard(g_mutex);
				Finish(State::Idle, "Not measured: the analysis failed.");
			}
		}).detach();
	}

	bool MuteOutput() noexcept
	{
		if (!g_busy.load(std::memory_order_relaxed)) return false;
		std::lock_guard<std::mutex> lock(g_mutex);
		CheckTimeout();
		return g_state == State::Armed || g_state == State::Injecting || g_state == State::Recording;
	}
}
