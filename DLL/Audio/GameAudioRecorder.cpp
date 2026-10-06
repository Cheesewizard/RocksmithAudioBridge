#include "stdafx.h"
#include "GameAudioRecorder.hpp"
#include "DrySignalRecording.hpp"
#include <condition_variable>

namespace Audio
{
	GameAudioRecorder::~GameAudioRecorder()
	{
		Close();
	}

	// Readable take name: "Rocksmith-2026-09-24_14-06-49". The date sorts in filename order and the
	// time reads as a clock. Uniqueness comes from the directory, not from a process id or counter:
	// if any file of a take with this stem already exists (two takes in one second, or the same
	// second on another run), "_2", "_3"... is appended so the wet, dry and video files still
	// share one stem.
	std::wstring GameAudioRecorder::NewTakeName(const std::filesystem::path& directory)
	{
		SYSTEMTIME time{};
		GetLocalTime(&time);
		wchar_t base[64];
		swprintf_s(base, L"Rocksmith-%04u-%02u-%02u_%02u-%02u-%02u", time.wYear, time.wMonth,
			time.wDay, time.wHour, time.wMinute, time.wSecond);
		static const wchar_t* const takeFiles[] = { L"-wet.wav", L"-wet.mp4", L"-dry.wav", L".wav", L".mp4" };
		for (uint32_t attempt = 1;; ++attempt)
		{
			std::wstring stem = base;
			if (attempt > 1) stem += L"_" + std::to_wstring(attempt);
			bool taken = false;
			for (const wchar_t* suffix : takeFiles)
			{
				std::error_code ignored;
				if (std::filesystem::exists(directory / (stem + suffix), ignored)) { taken = true; break; }
			}
			if (!taken || attempt >= 1000) return stem;
		}
	}

	HRESULT GameAudioRecorder::Open(const std::filesystem::path& directory, uint32_t maximumFrames, const wchar_t* label,
		const std::wstring& takeName)
	{
		if (writer.joinable() || file != INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
		if (maximumFrames == 0 || maximumFrames > 48000) return E_INVALIDARG;
		std::error_code directoryError;
		std::filesystem::create_directories(directory, directoryError);
		if (directoryError) return HRESULT_FROM_WIN32(directoryError.value());
		const std::wstring stem = takeName.empty() ? NewTakeName(directory) : takeName;
		wchar_t name[160];
		const wchar_t* separator = (label && label[0]) ? L"-" : L"";
		swprintf_s(name, L"%ls%ls%ls.wav", stem.c_str(), separator, (label ? label : L""));
		recordingPath = (directory / name).wstring();
		file = CreateFileW(recordingPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
		packetLimit = maximumFrames;
		// Buffer enough packets to ride out disk contention (e.g. simultaneous video capture) without
		// overrunning. Input blocks can be far smaller than maximumFrames (ASIO delivers ~128 frames),
		// and each block consumes one slot, so a slot count tied only to maximumFrames leaves ~60 ms of
		// headroom, too shallow when video is also writing. Guarantee a deeper floor of packets.
		slotBytes = maximumFrames * 2 * sizeof(int16_t);
		pendingSlot = nullptr;
		pendingBytes = 0;
		queue.Initialize(slotBytes, std::max<uint32_t>(256, 96000 / maximumFrames));
		wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!wakeEvent) { const HRESULT result = HRESULT_FROM_WIN32(GetLastError()); Close(); return result; }
		dataBytes = 0;
		recordedFrames.store(0);
		recordingStarted.store(0);
		error.store(S_OK);
		stopping.store(false);
		peakSample.store(0);
		sumSquares.store(0);
		sampleCount.store(0);
		if (!WriteHeader()) { const HRESULT result = HRESULT_FROM_WIN32(GetLastError()); Close(); return result; }
		writer = std::thread(&GameAudioRecorder::WritePackets, this);
		LOG_INFO("(AUDIO ROUTING) Recording game audio to " << (directory / name).string() << std::endl);
		return S_OK;
	}

	void GameAudioRecorder::Submit(const float* stereo, uint32_t frames) noexcept
	{
		SubmitSamples(stereo, frames, 2);
	}

	void GameAudioRecorder::SubmitMono(const float* mono, uint32_t frames, uint32_t sampleRate) noexcept
	{
		if (sampleRate != 48000) { error.store(AUDCLNT_E_UNSUPPORTED_FORMAT); return; }
		SubmitSamples(mono, frames, 1);
	}

	void GameAudioRecorder::SubmitSamples(const float* samples, uint32_t frames, uint32_t channels) noexcept
	{
		if (!wakeEvent || FAILED(error.load(std::memory_order_relaxed))) return;
		if (!samples || !frames || frames > packetLimit) { error.store(E_INVALIDARG); return; }
		const uint32_t blockBytes = frames * 2 * sizeof(int16_t);
		if (pendingSlot && pendingBytes + blockBytes > slotBytes)
		{
			queue.CommitWrite(pendingBytes);
			pendingSlot = nullptr;
			pendingBytes = 0;
			SetEvent(wakeEvent);
		}
		if (!pendingSlot) pendingSlot = queue.BeginWrite();
		if (!pendingSlot)
		{
			error.store(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
			SetEvent(wakeEvent);
			return;
		}
		auto* destination = reinterpret_cast<int16_t*>(pendingSlot + pendingBytes);
		int32_t packetPeak = 0;
		uint64_t packetSquares = 0;
		for (uint32_t sample = 0; sample < frames * 2; ++sample)
		{
			const float input = samples[channels == 1 ? sample / 2 : sample];
			const float value = std::isfinite(input) ? std::clamp(input, -1.0f, 1.0f) : 0.0f;
			destination[sample] = static_cast<int16_t>(std::lround(value * 32767.0f));
			packetPeak = std::max<int32_t>(packetPeak, std::abs(static_cast<int32_t>(destination[sample])));
			packetSquares += static_cast<uint64_t>(static_cast<int64_t>(destination[sample]) * destination[sample]);
		}
		sumSquares.fetch_add(packetSquares, std::memory_order_relaxed);
		sampleCount.fetch_add(frames * 2ull, std::memory_order_relaxed);
		// Single producer thread, so load-then-store is enough to keep a running maximum.
		if (packetPeak > peakSample.load(std::memory_order_relaxed)) peakSample.store(packetPeak, std::memory_order_relaxed);
		pendingBytes += blockBytes;
		if (pendingBytes + blockBytes > slotBytes)   // the next block of this size would not fit: hand the slot over now
		{
			queue.CommitWrite(pendingBytes);
			pendingSlot = nullptr;
			pendingBytes = 0;
		}
		if (!recordingStarted.load())
		{
			FILETIME time{};
			GetSystemTimePreciseAsFileTime(&time);
			recordingStarted.store((static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime);
		}
		recordedFrames.fetch_add(frames);
		SetEvent(wakeEvent);
	}

	bool GameAudioRecorder::WriteHeader()
	{
		uint8_t header[44]{};
		std::memcpy(header, "RIFF", 4);
		std::memcpy(header + 8, "WAVEfmt ", 8);
		std::memcpy(header + 36, "data", 4);
		const uint32_t values[] = { dataBytes + 36, 16, 48000, 192000, dataBytes };
		const size_t offsets[] = { 4, 16, 24, 28, 40 };
		for (size_t index = 0; index < 5; ++index) std::memcpy(header + offsets[index], &values[index], 4);
		const uint16_t format = 1, channels = 2, alignment = 4, bits = 16;
		std::memcpy(header + 20, &format, 2);
		std::memcpy(header + 22, &channels, 2);
		std::memcpy(header + 32, &alignment, 2);
		std::memcpy(header + 34, &bits, 2);
		LARGE_INTEGER position{};
		if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
		DWORD written = 0;
		if (!WriteFile(file, header, sizeof(header), &written, nullptr) || written != sizeof(header)) return false;
		position.QuadPart = 44ull + dataBytes;
		return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE;
	}

	void GameAudioRecorder::WritePackets()
	{
		for (;;)
		{
			WaitForSingleObject(wakeEvent, 1000);
			uint32_t bytes = 0;
			while (const uint8_t* packet = queue.BeginRead(bytes))
			{
				if (dataBytes > UINT32_MAX - 36 - bytes)
				{
					error.store(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
					break;
				}
				DWORD written = 0;
				if (!WriteFile(file, packet, bytes, &written, nullptr) || written != bytes)
				{
					error.store(HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_WRITE_FAULT));
					break;
				}
				dataBytes += bytes;
				queue.CommitRead();
			}
			if (!WriteHeader() && SUCCEEDED(error.load())) error.store(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
			if (FAILED(error.load()) || stopping.load()) break;
		}
		if (FAILED(error.load())) LOG_ERROR("(AUDIO ROUTING) Recording stopped, HRESULT " << std::hex << error.load() << std::dec << std::endl);
		FlushFileBuffers(file);
	}

	void GameAudioRecorder::Close()
	{
		// Submitters are stopped before Close (wet gate / dry detach), so the producer-side open slot is ours to commit.
		if (pendingSlot && pendingBytes && SUCCEEDED(error.load()))
			queue.CommitWrite(pendingBytes);
		pendingSlot = nullptr;
		pendingBytes = 0;
		stopping.store(true);
		if (wakeEvent) SetEvent(wakeEvent);
		if (writer.joinable()) writer.join();
		if (file != INVALID_HANDLE_VALUE)
		{
			if (normalizeOnClose && SUCCEEDED(error.load())) NormalizeFile();
			CloseHandle(file);
			file = INVALID_HANDLE_VALUE;
		}
		if (wakeEvent) { CloseHandle(wakeEvent); wakeEvent = nullptr; }
	}

	namespace
	{
		std::mutex pendingClosesMutex;
		std::condition_variable pendingClosesDone;
		int pendingCloses = 0;
	}

	// Closing normalizes the dry file, which rereads and rewrites the whole WAV (~115 MB for 10 minutes).
	// Takes are stopped from the game's render thread (overlay Record button, record hotkey), so the close
	// runs in the background to keep the game from freezing.
	void GameAudioRecorder::CloseInBackground(std::shared_ptr<GameAudioRecorder> recorder)
	{
		if (!recorder) return;
		{
			std::lock_guard<std::mutex> guard(pendingClosesMutex);
			++pendingCloses;
		}
		std::thread([recorder]()
		{
			recorder->Close();
			std::lock_guard<std::mutex> guard(pendingClosesMutex);
			if (--pendingCloses == 0) pendingClosesDone.notify_all();
		}).detach();
	}

	void GameAudioRecorder::WaitForPendingCloses()
	{
		std::unique_lock<std::mutex> lock(pendingClosesMutex);
		pendingClosesDone.wait(lock, []() { return pendingCloses == 0; });
	}

	bool GameAudioRecorder::WaitForPendingCloses(DWORD timeoutMs)
	{
		std::unique_lock<std::mutex> lock(pendingClosesMutex);
		return pendingClosesDone.wait_for(lock, std::chrono::milliseconds(timeoutMs), []() { return pendingCloses == 0; });
	}

	double GameAudioRecorder::GetMeanSquare() const noexcept
	{
		const uint64_t count = sampleCount.load();
		if (count == 0) return 0.0;
		return static_cast<double>(sumSquares.load()) / count / (32767.0 * 32767.0);
	}

	// The dry take is the detector-side input (post make-up gain, pre amp), so it lands well under
	// the wet mix and sounds near-silent beside it. The whole file is scaled once at the end so its
	// mean level matches the wet take of the same session, so the pair plays back equally loud.
	// (Peak normalizing would make the gain depend on how hard the loudest note was played, and the
	// wet mix peaks lower because of the output limiter.) Limits: the dry peak never exceeds -1 dBFS, the boost
	// is capped at +30 dB so a take of pure noise is not blown up, and with no usable wet level
	// (wet silent or unset) it falls back to the -1 dBFS peak target. The applied gain is logged so
	// the detector-level signal can be recovered (divide by it).
	void GameAudioRecorder::NormalizeFile()
	{
		const int32_t peak = peakSample.load();
		const double ownMeanSquare = GetMeanSquare();
		if (peak <= 0 || dataBytes == 0 || ownMeanSquare <= 0.0) return;
		constexpr double targetPeak = 32767.0 * 0.891251;   // -1 dBFS
		constexpr double maximumGain = 31.6228;              // +30 dB
		constexpr double silentMeanSquare = 1e-6;            // -60 dBFS
		const double peakGain = targetPeak / peak;
		const bool matchWet = loudnessReference > silentMeanSquare;
		const double wantedGain = matchWet ? std::sqrt(loudnessReference / ownMeanSquare) : peakGain;
		const double gain = std::min({ wantedGain, peakGain, maximumGain });
		if (std::fabs(gain - 1.0) < 0.01) return;
		std::vector<int16_t> block(32768);
		uint64_t offset = 0;
		while (offset < dataBytes)
		{
			const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(block.size() * sizeof(int16_t), dataBytes - offset));
			LARGE_INTEGER position{};
			position.QuadPart = 44 + static_cast<LONGLONG>(offset);
			DWORD transferred = 0;
			if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN) ||
				!ReadFile(file, block.data(), chunk, &transferred, nullptr) || transferred != chunk)
			{
				LOG_ERROR("(AUDIO ROUTING) Dry normalize read failed at byte " << offset << ", file partly scaled" << std::endl);
				return;
			}
			for (size_t index = 0; index < chunk / sizeof(int16_t); ++index)
			{
				const long scaled = std::lround(block[index] * gain);
				block[index] = static_cast<int16_t>(std::clamp<long>(scaled, -32768, 32767));
			}
			if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN) ||
				!WriteFile(file, block.data(), chunk, &transferred, nullptr) || transferred != chunk)
			{
				LOG_ERROR("(AUDIO ROUTING) Dry normalize write failed at byte " << offset << ", file partly scaled" << std::endl);
				return;
			}
			offset += chunk;
		}
		FlushFileBuffers(file);
		const double gainDb = 20.0 * std::log10(gain);
		LOG_INFO("(AUDIO ROUTING) Normalized " << std::filesystem::path(recordingPath).filename().string() << " by "
			<< (gainDb >= 0 ? "+" : "") << std::fixed << std::setprecision(1) << gainDb << " dB (x" << std::setprecision(4) << gain
			<< ", " << (matchWet ? "matched wet mean " : "no wet level, peak target; ")
			<< std::setprecision(1) << (matchWet ? 10.0 * std::log10(loudnessReference) : 0.0) << (matchWet ? " dBFS" : "")
			<< ", source mean " << 10.0 * std::log10(ownMeanSquare) << " dBFS, source peak "
			<< 20.0 * std::log10(peak / 32767.0) << " dBFS" << (gain < wantedGain ? ", limited by peak/cap" : "") << ")" << std::endl);
	}

	RecordingSession::~RecordingSession()
	{
		std::wstring path;
		uint64_t frames = 0, started = 0;
		Stop(path, frames, started);
	}

	HRESULT RecordingSession::Start(const std::filesystem::path& directory, uint32_t wetMaximumFrames)
	{
		if (wetRecorder || dryRecorder) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
		if (directory.empty() || wetMaximumFrames == 0) return E_INVALIDARG;
		// Refuse before creating any file: Attach below rejects a missing/wrong-rate input too, but by then both
		// WAVs exist, and Stop only closes them, so a refused Record would leave an empty header-only pair.
		if (!DrySignalRecording::IsReady()) return AUDCLNT_E_DEVICE_INVALIDATED;
		auto wet = std::make_shared<GameAudioRecorder>();
		auto dry = std::make_shared<GameAudioRecorder>();
		dry->SetNormalizeOnClose(true);
		// One take name for both files so the pair differs only by the -wet/-dry label (the video
		// muxed from the wet file keeps that name too).
		const std::wstring takeName = GameAudioRecorder::NewTakeName(directory);
		HRESULT result = wet->Open(directory, wetMaximumFrames, L"wet", takeName);
		if (FAILED(result)) return result;
		result = dry->Open(directory, 4096, L"dry", takeName);
		if (FAILED(result)) { wet->Close(); return result; }
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			wetRecorder = std::move(wet);
			dryRecorder = std::move(dry);
			lastError = S_OK;
		}
		wetGate.Open();
		const HRESULT attached = DrySignalRecording::Attach(*this);
		if (FAILED(attached))
		{
			std::wstring ignoredPath;
			uint64_t ignoredFrames = 0, ignoredStarted = 0;
			Stop(ignoredPath, ignoredFrames, ignoredStarted);
			return attached;
		}
		dryAttached = true;
		return S_OK;
	}

	void RecordingSession::SubmitWet(const float* stereo, uint32_t frames) noexcept
	{
		CaptureCallbackScope scope(wetGate);
		if (scope && wetRecorder) wetRecorder->Submit(stereo, frames);
	}

	void RecordingSession::SubmitDry(const float* mono, uint32_t frames, uint32_t sampleRate) noexcept
	{
		if (dryRecorder) dryRecorder->SubmitMono(mono, frames, sampleRate);
	}

	void RecordingSession::Stop(std::wstring& wetPath, uint64_t& frames, uint64_t& started)
	{
		wetPath.clear();
		frames = 0;
		started = 0;
		if (dryAttached)
		{
			DrySignalRecording::Detach();
			dryAttached = false;
		}
		wetGate.CloseAndWait();   // no wet block is being submitted from here on
		std::shared_ptr<GameAudioRecorder> wet, dry;
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			wet = std::move(wetRecorder);
			dry = std::move(dryRecorder);
			if (!wet) return;
			lastError = wet->GetError();
			if (dry && FAILED(dry->GetError())) lastError = dry->GetError();
		}
		frames = wet->GetFrames();
		started = wet->GetStarted();
		wetPath = wet->GetPath();
		// The wet file is finished here (a flush): the video helper muxes it as soon as Stop returns.
		wet->Close();
		if (dry)
		{
			dry->SetLoudnessReference(wet->GetMeanSquare());
			GameAudioRecorder::CloseInBackground(std::move(dry));
		}
	}

	HRESULT RecordingSession::GetError() const noexcept
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		if (wetRecorder && FAILED(wetRecorder->GetError())) return wetRecorder->GetError();
		if (dryRecorder && FAILED(dryRecorder->GetError())) return dryRecorder->GetError();
		return lastError;
	}

	uint64_t RecordingSession::GetFrames() const noexcept
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		return wetRecorder ? wetRecorder->GetFrames() : 0;
	}

	uint64_t RecordingSession::GetStarted() const noexcept
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		return wetRecorder ? wetRecorder->GetStarted() : 0;
	}

	std::wstring RecordingSession::GetWetPath() const
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		return wetRecorder ? wetRecorder->GetPath() : std::wstring();
	}
}
