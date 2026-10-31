#include "../../Mod/Shared/LatencyMeasure.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace AsioProxy::LatencyMeasure
{
	namespace
	{
		void (*g_log)(const char*, ...) = nullptr;

		// Three chirps 450 ms apart, 40 ms each, 300 Hz to 2.5 kHz: the same signal as Cable mode's loopback
		// (DLL/AudioBridge/Engine/LatencyLoopback.cpp), which passes a guitar amp and cab distorted but still
		// correlating. Three, so one answer lost under menu music is outvoted. 450 ms: longer than the longest lag
		// looked for plus a chirp, so one chirp's search never finds the next one.
		constexpr int kChirps = 3;
		constexpr int kMaxInputs = 8;
		constexpr double kChirpMs = 40.0, kSpacingMs = 450.0;
		constexpr double kGameMaxLagMs = 200.0;       // the game's amp, or the amp link's return
		constexpr double kHardwareMaxLagMs = 400.0;   // an interface at a big buffer through a 64-bit host
		constexpr float kGameAmplitude = 0.35f;       // about a firm pick
		constexpr float kHardwareAmplitude = 0.1f;    // -20 dBFS: plenty back through a cable, gentle in headphones
		constexpr float kQuietPeak = 0.03f;           // -30 dBFS on every input: nobody is playing
		constexpr double kQuietMs = 300.0, kQuietTimeoutMs = 10000.0;
		constexpr float kGameMinScore = 0.2f;         // normalized correlation a distorted answer still reaches
		constexpr float kHardwareMinScore = 0.5f;     // a cable gives the chirp back almost clean
		constexpr float kNoSignalPeak = 0.01f;        // -40 dBFS: below this on every input, the chirps never arrived
		constexpr float kAgreeMs = 1.0f;              // on one sample clock the answers agree to a fraction of this

		std::mutex g_mutex;   // the stream description and the result (control and worker threads only)
		std::wstring g_driver;
		long g_bufferSize = 0;
		double g_sampleRate = 0.0;
		bool g_host64 = false;
		Result g_result{};

		std::atomic<int32_t> g_state{ static_cast<int32_t>(State::Idle) };
		std::atomic<int32_t> g_pendingOutcome{ None };   // audio thread -> worker: ended without a recording
		std::atomic<ULONGLONG> g_lastCallback{ 0 };
		HANDLE g_done = nullptr;                          // audio thread -> worker: the recording is complete

		// Written by Start, then owned by the audio thread until it hands over (State::Analysing).
		Kind g_kind = Kind::Game;
		uint32_t g_rate = 48000, g_chirpFrames = 0, g_spacing = 0, g_maxLag = 0, g_recordFrames = 0;
		std::vector<float> g_chirp;                       // one chirp at amplitude 1
		int64_t g_pos = 0;
		uint32_t g_quietFrames = 0, g_waitedFrames = 0;
		std::vector<float> g_game, g_amp;                 // Game: the game's output, and the amp link's return
		std::vector<float> g_inputs[kMaxInputs];          // Hardware: each input the game opened
		int g_inputCount = 0;

		State Now() noexcept { return static_cast<State>(g_state.load(std::memory_order_acquire)); }
		void Set(State state) noexcept { g_state.store(static_cast<int32_t>(state), std::memory_order_release); }

		float SignalAt(int64_t position) noexcept
		{
			for (int k = 0; k < kChirps; ++k)
			{
				const int64_t d = position - static_cast<int64_t>(g_spacing) * k;
				if (d >= 0 && d < g_chirpFrames) return g_chirp[static_cast<size_t>(d)];
			}
			return 0.0f;
		}

		void BuildChirp()
		{
			g_chirp.assign(g_chirpFrames, 0.0f);
			const double f0 = 300.0, f1 = 2500.0, length = g_chirpFrames / static_cast<double>(g_rate);
			const double taper = 0.1 * g_chirpFrames;   // Tukey window: no click at either end
			for (uint32_t i = 0; i < g_chirpFrames; ++i)
			{
				const double t = i / static_cast<double>(g_rate);
				const double phase = 2.0 * 3.14159265358979 * (f0 * t + (f1 - f0) * t * t / (2.0 * length));
				double w = 1.0;
				if (i < taper) w = 0.5 - 0.5 * std::cos(3.14159265358979 * i / taper);
				else if (i > g_chirpFrames - 1 - taper) w = 0.5 - 0.5 * std::cos(3.14159265358979 * (g_chirpFrames - 1 - i) / taper);
				g_chirp[i] = static_cast<float>(w * std::sin(phase));
			}
		}

		struct Found { float ms = -1.0f; int agreeing = 0; float score = 0.0f; };

		// Each chirp's best lag in [0, g_maxLag] by normalized cross-correlation (level apart: an amp's gain, a quiet
		// cable return), then the median of the confident ones and the mean of those agreeing with it.
		Found FindChirps(const std::vector<float>& recorded, float minScore, char* detail, size_t detailSize)
		{
			double chirpEnergy = 0.0;
			for (float c : g_chirp) chirpEnergy += static_cast<double>(c) * c;
			std::vector<float> lags;
			float scoreSum = 0.0f;
			size_t used = 0;
			detail[0] = 0;
			for (int k = 0; k < kChirps; ++k)
			{
				const size_t base = static_cast<size_t>(g_spacing) * k;
				double windowEnergy = 0.0;
				for (uint32_t j = 0; j < g_chirpFrames; ++j) windowEnergy += static_cast<double>(recorded[base + j]) * recorded[base + j];
				float bestScore = 0.0f;
				uint32_t bestLag = 0;
				for (uint32_t lag = 0; lag <= g_maxLag; ++lag)
				{
					const size_t at = base + lag;
					if (lag > 0)
					{
						const float gone = recorded[at - 1], added = recorded[at + g_chirpFrames - 1];
						windowEnergy += static_cast<double>(added) * added - static_cast<double>(gone) * gone;
					}
					if (windowEnergy <= 1e-12) continue;
					double dot = 0.0;
					for (uint32_t j = 0; j < g_chirpFrames; ++j) dot += static_cast<double>(g_chirp[j]) * recorded[at + j];
					const float score = static_cast<float>(std::fabs(dot) / std::sqrt(chirpEnergy * windowEnergy));
					if (score > bestScore) { bestScore = score; bestLag = lag; }
				}
				const float ms = bestLag * 1000.0f / g_rate;
				if (used < detailSize)
					used += static_cast<size_t>(std::max(0, snprintf(detail + used, detailSize - used, "%s%.2f ms (%.2f)", k ? ", " : "", ms, bestScore)));
				if (bestScore >= minScore) { lags.push_back(ms); scoreSum += bestScore; }
			}
			Found found;
			if (lags.size() < 2) return found;
			std::vector<float> sorted = lags;
			std::sort(sorted.begin(), sorted.end());
			const float median = sorted[sorted.size() / 2];
			float sum = 0.0f;
			for (float lag : lags) if (std::fabs(lag - median) <= kAgreeMs) { sum += lag; ++found.agreeing; }
			if (found.agreeing < 2) { found.agreeing = -1; return found; }   // confident but disagreeing
			found.ms = sum / found.agreeing;
			found.score = scoreSum / lags.size();
			return found;
		}

		void Finish(Outcome outcome, float gameMs, float ampMs, float roundTripMs, int inputChannel)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_result.kind = static_cast<int32_t>(g_kind);
			g_result.outcome = outcome;
			g_result.gameMs = gameMs;
			g_result.ampMs = ampMs;
			g_result.roundTripMs = roundTripMs;
			g_result.inputChannel = inputChannel;
			++g_result.sequence;
			// A patch cable may still be in: the output stays muted until the player says it is out.
			Set(g_kind == Kind::Hardware ? State::HoldMuted : State::Idle);
		}

		void Analyse()
		{
			char detail[160];
			if (g_kind == Kind::Game)
			{
				const Found game = FindChirps(g_game, kGameMinScore, detail, sizeof(detail));
				if (g_log) g_log("latency measure: game's amp %s -> %.2f ms (%d agree)", detail, game.ms, game.agreeing);
				const Found amp = FindChirps(g_amp, kGameMinScore, detail, sizeof(detail));
				if (g_log) g_log("latency measure: amp link return %s -> %.2f ms (%d agree)", detail, amp.ms, amp.agreeing);
				const bool any = game.ms >= 0.0f || amp.ms >= 0.0f;
				const bool disagreed = game.agreeing < 0 || amp.agreeing < 0;
				Finish(any ? Measured : disagreed ? Disagreed : NoAnswer, game.ms, amp.ms, -1.0f, -1);
				return;
			}
			Found best;
			int bestChannel = -1;
			bool disagreed = false;
			float loudest = 0.0f;   // the loudest input over the whole recording: tells "nothing arrived" from "arrived mangled"
			for (int c = 0; c < g_inputCount; ++c)
			{
				const Found found = FindChirps(g_inputs[c], kHardwareMinScore, detail, sizeof(detail));
				float peak = 0.0f;
				for (float v : g_inputs[c]) peak = std::max(peak, std::fabs(v));
				loudest = std::max(loudest, peak);
				if (g_log) g_log("latency measure: cable loop on input %d %s -> %.2f ms (%d agree), input peak %.1f dBFS (chirps out at %.1f dBFS)",
					c + 1, detail, found.ms, found.agreeing, peak > 1e-7f ? 20.0f * std::log10(peak) : -140.0f, 20.0f * std::log10(kHardwareAmplitude));
				disagreed = disagreed || found.agreeing < 0;
				if (found.ms >= 0.0f && (bestChannel < 0 || found.score > best.score)) { best = found; bestChannel = c; }
			}
			Finish(bestChannel >= 0 ? Measured : disagreed ? Disagreed : loudest < kNoSignalPeak ? NoSignal : NoAnswer, -1.0f, -1.0f, best.ms, bestChannel);
		}

		// One per measurement: waits for the audio thread's recording (or its giving up), then works it out off the
		// audio thread (about 30 million multiply-adds per recording).
		void Worker()
		{
			const DWORD wait = static_cast<DWORD>(kQuietTimeoutMs + 3.0 * kSpacingMs + kHardwareMaxLagMs) + 5000;
			if (WaitForSingleObject(g_done, wait) != WAIT_OBJECT_0)
			{
				if (g_log) g_log("latency measure: the stream stopped during the measurement");
				Finish(Stopped, -1.0f, -1.0f, -1.0f, -1);
				return;
			}
			const int32_t pending = g_pendingOutcome.load(std::memory_order_acquire);
			if (pending != None) { Finish(static_cast<Outcome>(pending), -1.0f, -1.0f, -1.0f, -1); return; }
			try { Analyse(); }
			catch (...) { Finish(Failed, -1.0f, -1.0f, -1.0f, -1); }
		}

		// Audio thread: the recording is complete, or it ended without one (`outcome`).
		void HandOver(Outcome outcome) noexcept
		{
			g_pendingOutcome.store(outcome, std::memory_order_release);
			Set(State::Analysing);
			SetEvent(g_done);
		}
	}

	void SetLogger(void (*log)(const char*, ...)) { g_log = log; }

	void SetStream(const wchar_t* driver, long bufferSize, double sampleRate, bool host64)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_driver = driver ? driver : L"";
		g_bufferSize = bufferSize;
		g_sampleRate = sampleRate;
		g_host64 = host64;
	}

	bool Start(Kind kind)
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (Now() != State::Idle || g_bufferSize <= 0 || g_sampleRate <= 0.0) return false;
			// A stream that stopped calling back cannot run one.
			if (GetTickCount64() - g_lastCallback.load() > 1000) return false;
			g_kind = kind;
			g_rate = static_cast<uint32_t>(g_sampleRate);
		}
		g_chirpFrames = static_cast<uint32_t>(g_rate * kChirpMs / 1000.0);
		g_spacing = static_cast<uint32_t>(g_rate * kSpacingMs / 1000.0);
		g_maxLag = static_cast<uint32_t>(g_rate * (kind == Kind::Game ? kGameMaxLagMs : kHardwareMaxLagMs) / 1000.0);
		g_recordFrames = g_spacing * (kChirps - 1) + g_maxLag + g_chirpFrames + 1;
		BuildChirp();
		if (kind == Kind::Game)
		{
			g_game.assign(g_recordFrames, 0.0f);
			g_amp.assign(g_recordFrames, 0.0f);
		}
		else for (auto& input : g_inputs) input.assign(g_recordFrames, 0.0f);
		g_inputCount = 0;
		g_pos = 0;
		g_quietFrames = g_waitedFrames = 0;
		g_pendingOutcome.store(None);
		if (!g_done) g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		ResetEvent(g_done);
		if (g_log) g_log("latency measure: %s, %u Hz, waiting for a quiet input", kind == Kind::Game ? "game (chirps on the inputs)" : "hardware (chirps out through a patch cable)", g_rate);
		Set(State::WaitingQuiet);   // release: the audio thread sees the buffers above
		std::thread(Worker).detach();
		return true;
	}

	void Release()
	{
		int32_t expected = static_cast<int32_t>(State::HoldMuted);
		if (g_state.compare_exchange_strong(expected, static_cast<int32_t>(State::Idle)) && g_log) g_log("latency measure: output unmuted (cable out)");
	}

	Result Get()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Result result = g_result;
		result.state = g_state.load();
		const bool live = GetTickCount64() - g_lastCallback.load() < 1000;
		result.sampleRate = live ? static_cast<int32_t>(g_sampleRate) : 0;
		result.bufferSize = live ? static_cast<int32_t>(g_bufferSize) : 0;
		result.host64 = g_host64 ? 1 : 0;
		wcsncpy_s(result.driver, g_driver.c_str(), _TRUNCATE);
		return result;
	}

	bool Active() noexcept
	{
		g_lastCallback.store(GetTickCount64(), std::memory_order_relaxed);
		return g_state.load(std::memory_order_relaxed) != static_cast<int32_t>(State::Idle);
	}

	bool Inputs(float* const* channels, int count, long frames) noexcept
	{
		const State state = Now();
		if (state == State::Idle || frames <= 0) return false;
		const bool hardware = g_kind == Kind::Hardware;
		if (state == State::WaitingQuiet)
		{
			float peak = 0.0f;
			for (int c = 0; c < count; ++c)
				for (long f = 0; f < frames; ++f) peak = std::max(peak, std::fabs(channels[c][f]));
			g_quietFrames = peak < kQuietPeak ? g_quietFrames + static_cast<uint32_t>(frames) : 0;
			g_waitedFrames += static_cast<uint32_t>(frames);
			if (g_quietFrames >= g_rate * kQuietMs / 1000.0) { g_pos = 0; Set(State::Running); }
			else if (g_waitedFrames >= g_rate * kQuietTimeoutMs / 1000.0) HandOver(NotQuiet);
			if (!hardware) return false;
		}
		if (Now() == State::Running && !hardware)
		{
			for (long f = 0; f < frames; ++f)
			{
				const float v = kGameAmplitude * SignalAt(g_pos + f);
				for (int c = 0; c < count; ++c) channels[c][f] = v;
			}
			return true;
		}
		if (!hardware) return false;
		// Hardware, from the start until the cable is out: the game hears nothing, so no feedback through its amp.
		if (Now() == State::Running)
		{
			g_inputCount = std::min(count, kMaxInputs);
			for (int c = 0; c < g_inputCount; ++c)
				for (long f = 0; f < frames; ++f)
				{
					const int64_t at = g_pos + f;
					if (at < g_recordFrames) g_inputs[c][static_cast<size_t>(at)] = channels[c][f];
				}
		}
		for (int c = 0; c < count; ++c) std::fill(channels[c], channels[c] + frames, 0.0f);
		return true;
	}

	void GameOutput(const float* mono, long frames) noexcept
	{
		if (Now() != State::Running || g_kind != Kind::Game) return;
		for (long f = 0; f < frames; ++f)
		{
			const int64_t at = g_pos + f;
			if (at < g_recordFrames) g_game[static_cast<size_t>(at)] = mono[f];
		}
	}

	void HeardOutput(const float* mono, long frames) noexcept
	{
		if (Now() != State::Running || g_kind != Kind::Game) return;
		for (long f = 0; f < frames; ++f)
		{
			const int64_t at = g_pos + f;
			if (at < g_recordFrames) g_amp[static_cast<size_t>(at)] = mono[f] - g_game[static_cast<size_t>(at)];
		}
	}

	bool FinalOutput(float* replacement, long frames) noexcept
	{
		const State state = Now();
		if (state == State::Idle || frames <= 0) return false;
		const bool hardware = g_kind == Kind::Hardware;
		if (!hardware && state != State::Running) return false;   // the game's amp plays while it waits, and after
		for (long f = 0; f < frames; ++f)
			replacement[f] = hardware && state == State::Running ? kHardwareAmplitude * SignalAt(g_pos + f) : 0.0f;
		if (state == State::Running)
		{
			g_pos += frames;
			if (g_pos >= g_recordFrames) HandOver(None);
		}
		return true;
	}
}
