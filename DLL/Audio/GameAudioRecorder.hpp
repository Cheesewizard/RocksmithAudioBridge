#pragma once

#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include "AudioPacketQueue.hpp"
#include "CaptureProcessingGate.hpp"

namespace Audio
{
	class GameAudioRecorder
	{
	public:
		~GameAudioRecorder();
		// label tags the filename with the signal it captures ("dry" = pre-processing input
		// the detector reads, "wet" = the processed game output). Empty adds no tag.
		// takeName is the shared "Rocksmith-YYYY-MM-DD_HH-MM-SS[_N]" stem; empty draws a fresh one.
		HRESULT Open(const std::filesystem::path& directory, uint32_t maximumFrames, const wchar_t* label = L"",
			const std::wstring& takeName = std::wstring());
		static std::wstring NewTakeName(const std::filesystem::path& directory);
		// Rescale the finished file on Close so its mean level matches loudnessReference (a mean
		// square on the 0..1 full-scale range, e.g. the paired wet take's GetMeanSquare()). Peak is
		// kept at or under -1 dBFS. Set before Open; used for the dry take.
		void SetNormalizeOnClose(bool enabled) noexcept { normalizeOnClose = enabled; }
		void SetLoudnessReference(double meanSquare) noexcept { loudnessReference = meanSquare; }
		// Mean square of everything recorded so far, full scale = 1.0 (0 when nothing recorded).
		double GetMeanSquare() const noexcept;
		void Submit(const float* stereo, uint32_t frames) noexcept;
		void SubmitMono(const float* mono, uint32_t frames, uint32_t sampleRate) noexcept;
		uint64_t GetFrames() const noexcept { return recordedFrames.load(); }
		uint64_t GetStarted() const noexcept { return recordingStarted.load(); }
		void Close();
		// Close on a background thread (used for the dry take, whose normalization rereads and rewrites the
		// whole file). The recorder is kept alive until it is done.
		static void CloseInBackground(std::shared_ptr<GameAudioRecorder> recorder);
		// Block until every background close has finished (tests, and anything that reads a dry take at once).
		static void WaitForPendingCloses();
		// Same with a limit; false if closes were still running when it expired (game exit must not hang forever).
		static bool WaitForPendingCloses(DWORD timeoutMs);
		HRESULT GetError() const noexcept { return error.load(std::memory_order_relaxed); }
		const std::wstring& GetPath() const noexcept { return recordingPath; }

	private:
		void WritePackets();
		bool WriteHeader();
		void SubmitSamples(const float* samples, uint32_t frames, uint32_t channels) noexcept;
		void NormalizeFile();
		std::atomic<uint64_t> recordedFrames{ 0 }, recordingStarted{ 0 };
		AudioPacketQueue queue;
		std::atomic<HRESULT> error{ S_OK };
		std::atomic<bool> stopping{ false };
		std::atomic<int32_t> peakSample{ 0 };
		std::atomic<uint64_t> sumSquares{ 0 }, sampleCount{ 0 };
		bool normalizeOnClose = false;
		double loudnessReference = 0.0;
		HANDLE wakeEvent = nullptr;
		HANDLE file = INVALID_HANDLE_VALUE;
		std::thread writer;
		uint32_t dataBytes = 0;
		uint32_t packetLimit = 0;
		// Small blocks are packed into one queue slot (producer side only) instead of taking a slot each: at a 48-frame
		// ASIO buffer, 256 one-block slots would cover only 0.26 s, so a short disk stall would end the take. The open slot is
		// committed when the next block would not fit, and by Close (all submitters are stopped by then).
		uint32_t slotBytes = 0;
		uint8_t* pendingSlot = nullptr;
		uint32_t pendingBytes = 0;
		std::wstring recordingPath;
	};

	class RecordingSession
	{
	public:
		~RecordingSession();
		HRESULT Start(const std::filesystem::path& directory, uint32_t wetMaximumFrames);
		void SubmitWet(const float* stereo, uint32_t frames) noexcept;
		void SubmitDry(const float* mono, uint32_t frames, uint32_t sampleRate) noexcept;
		void Stop(std::wstring& wetPath, uint64_t& frames, uint64_t& started);
		bool IsRecording() const noexcept { return wetRecorder != nullptr; }
		HRESULT GetError() const noexcept;
		uint64_t GetFrames() const noexcept;
		uint64_t GetStarted() const noexcept;
		std::wstring GetWetPath() const;

	private:
		// The audio thread submits wet blocks through wetGate (never waits); Stop closes the gate and waits
		// for an in-flight block before it takes the recorder away. The getters are polled from other
		// threads (overlay, control pipe) while Stop may run, so the pointers are swapped under stateMutex.
		CaptureProcessingGate wetGate;
		mutable std::mutex stateMutex;
		std::shared_ptr<GameAudioRecorder> wetRecorder;
		std::shared_ptr<GameAudioRecorder> dryRecorder;
		HRESULT lastError = S_OK;
		bool dryAttached = false;
	};
}
