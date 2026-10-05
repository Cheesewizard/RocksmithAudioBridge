#include "PreRenderedPitchCache.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../ThirdParty/SignalsmithStretch/signalsmith-stretch.h"

namespace
{
	constexpr uint32_t CACHE_CHANNELS = 2;
	constexpr uint32_t PROCESS_CHUNK_FRAMES = 8192;
	constexpr uint32_t PLAYABLE_SECONDS = 10;
	constexpr uint64_t CACHE_COOLDOWN_MILLISECONDS = 120000;
	constexpr uint64_t MINIMUM_EVICTION_GRACE_MILLISECONDS = 5000;
	constexpr uint64_t MAX_RETAINED_CACHE_BYTES = 256ULL * 1024ULL * 1024ULL;
	constexpr float INT16_NORMALIZER = 1.0f / 32768.0f;
	constexpr float INT16_SCALE = 32768.0f;

	// The song is never mapped whole: a 32-bit process cannot afford 100+ MiB
	// contiguous reservations, and fragmentation would eventually fail them.
	// Rendering and playback each hold one sliding window instead.
	constexpr uint64_t MAP_WINDOW_BYTES = 16ULL * 1024ULL * 1024ULL;
	constexpr uint64_t MAP_ALIGNMENT = 64ULL * 1024ULL; // MapViewOfFile offset granularity.
	static_assert(MAP_WINDOW_BYTES % MAP_ALIGNMENT == 0,
		"map windows must start on an allocation-granularity boundary");

	constexpr LONG CACHE_PREPARING = 0;
	constexpr LONG CACHE_READY = 1;
	constexpr LONG CACHE_FAILED = 2;
	constexpr LONG CACHE_CANCELLED = 3;
	constexpr LONG CACHE_EVICTED = 4;

	class CacheCancelledException final : public std::runtime_error
	{
	public:
		CacheCancelledException()
			: std::runtime_error("Speaker Mode audio preparation was cancelled")
		{
		}
	};

	class TemporarySourceFiles final
	{
	public:
		TemporarySourceFiles(
			std::filesystem::path wavePath,
			std::filesystem::path errorPath)
			: wavePath(std::move(wavePath)),
			errorPath(std::move(errorPath))
		{
		}

		~TemporarySourceFiles()
		{
			DeleteFileW(wavePath.c_str());
			DeleteFileW(errorPath.c_str());
		}

	private:
		std::filesystem::path wavePath;
		std::filesystem::path errorPath;
	};

	struct WaveSource
	{
		std::ifstream stream;
		uint32_t sampleRate = 0;
		uint64_t totalFrames = 0;
		uint64_t dataOffset = 0;
	};

	std::mutex cacheRegistryMutex;
	std::mutex sourceExtractionMutex;
	std::map<std::string, std::unique_ptr<Audio::SongShift::PreparedPitchCache>> cacheRegistry;
	std::vector<std::unique_ptr<Audio::SongShift::PreparedPitchCache>> supersededCaches;
	std::filesystem::path sessionCacheDirectory;
	bool isCacheInitialized = false;
	volatile LONG64 cacheSequence = 0;

	uint16_t ReadUInt16(std::ifstream& stream)
	{
		uint16_t value = 0;
		stream.read(reinterpret_cast<char*>(&value), sizeof(value));
		return value;
	}

	uint32_t ReadUInt32(std::ifstream& stream)
	{
		uint32_t value = 0;
		stream.read(reinterpret_cast<char*>(&value), sizeof(value));
		return value;
	}

	std::wstring Utf8ToWide(const std::string& value)
	{
		if (value.empty()) return {};
		const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
		if (length <= 1) return {};

		std::wstring result(static_cast<size_t>(length), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), length);
		result.resize(static_cast<size_t>(length - 1));
		return result;
	}

	std::wstring QuoteArgument(const std::wstring& value)
	{
		if (value.find(L'"') != std::wstring::npos)
		{
			throw std::invalid_argument("Speaker Mode path contains an unsupported quote character");
		}

		return L"\"" + value + L"\"";
	}

	std::filesystem::path GetRocksmithDirectory()
	{
		wchar_t executablePath[MAX_PATH] = {};
		const DWORD length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
		if (length == 0 || length == MAX_PATH)
		{
			throw std::runtime_error("could not resolve the Rocksmith executable directory");
		}

		return std::filesystem::path(executablePath).parent_path();
	}

	std::string SanitizeBankName(const std::string& bankName)
	{
		std::string result;
		result.reserve(bankName.size());
		for (const char character : bankName)
		{
			const bool isSafe = (character >= 'a' && character <= 'z')
				|| (character >= 'A' && character <= 'Z')
				|| (character >= '0' && character <= '9')
				|| character == '-' || character == '_';
			result.push_back(isSafe ? character : '_');
		}
		return result;
	}

	uint64_t HashString(const std::string& value)
	{
		uint64_t hash = 14695981039346656037ULL;
		for (const unsigned char character : value)
		{
			hash ^= character;
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	std::wstring GetCacheBaseName(const std::string& bankName)
	{
		std::wstringstream name;
		name << Utf8ToWide(SanitizeBankName(bankName)) << L"."
			<< std::hex << std::setw(16) << std::setfill(L'0') << HashString(bankName);
		return name.str();
	}

	void ThrowIfCancelled(const volatile LONG* cancellationRequested)
	{
		if (cancellationRequested != nullptr && InterlockedCompareExchange(
			const_cast<volatile LONG*>(cancellationRequested),
			0,
			0) != 0)
		{
			throw CacheCancelledException();
		}
	}

	int16_t FloatToInt16(float value)
	{
		const float scaled = value * INT16_SCALE;
		const float clipped = std::max(-32768.0f, std::min(32767.0f, scaled));
		return static_cast<int16_t>(std::lround(clipped));
	}

	void* MapWindow(HANDLE mappingHandle, uint64_t startByte, uint64_t sizeBytes, bool writable)
	{
		ULARGE_INTEGER offset;
		offset.QuadPart = startByte;
		return MapViewOfFile(
			mappingHandle,
			writable ? FILE_MAP_WRITE : FILE_MAP_READ,
			offset.HighPart,
			offset.LowPart,
			static_cast<SIZE_T>(sizeBytes));
	}

	// Sequential writer over the cache's file mapping. The render only moves
	// forward, so a single sliding window is remapped as it advances.
	class WindowedCacheWriter final
	{
	public:
		WindowedCacheWriter(HANDLE mappingHandle, uint64_t totalFrames)
			: mappingHandle(mappingHandle),
			totalBytes(totalFrames * CACHE_CHANNELS * sizeof(int16_t))
		{
		}

		~WindowedCacheWriter()
		{
			if (view != nullptr) UnmapViewOfFile(view);
		}

		// A pointer valid for frameCount frames starting at frameIndex, or null.
		int16_t* Acquire(uint64_t frameIndex, uint32_t frameCount)
		{
			const uint64_t startByte = frameIndex * CACHE_CHANNELS * sizeof(int16_t);
			const uint64_t endByte = startByte
				+ static_cast<uint64_t>(frameCount) * CACHE_CHANNELS * sizeof(int16_t);
			if (endByte > totalBytes) return nullptr;

			if (view == nullptr || startByte < windowStartByte
				|| endByte > windowStartByte + windowBytes)
			{
				if (view != nullptr)
				{
					UnmapViewOfFile(view);
					view = nullptr;
				}
				windowStartByte = startByte & ~(MAP_ALIGNMENT - 1);
				windowBytes = std::min(MAP_WINDOW_BYTES, totalBytes - windowStartByte);
				if (endByte > windowStartByte + windowBytes) return nullptr;
				view = MapWindow(mappingHandle, windowStartByte, windowBytes, true);
				if (view == nullptr) return nullptr;
			}

			return reinterpret_cast<int16_t*>(
				static_cast<char*>(view) + (startByte - windowStartByte));
		}

	private:
		HANDLE mappingHandle;
		uint64_t totalBytes;
		void* view = nullptr;
		uint64_t windowStartByte = 0;
		uint64_t windowBytes = 0;
	};

	WaveSource OpenWaveSource(const std::filesystem::path& path)
	{
		WaveSource source;
		source.stream.open(path, std::ios::binary);
		if (!source.stream) throw std::runtime_error("could not open the decoded Speaker Mode WAV");

		char riff[4] = {};
		source.stream.read(riff, sizeof(riff));
		ReadUInt32(source.stream);
		char wave[4] = {};
		source.stream.read(wave, sizeof(wave));
		if (memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0)
		{
			throw std::runtime_error("decoded Speaker Mode audio is not a RIFF WAVE file");
		}

		bool foundFormat = false;
		bool foundData = false;
		uint32_t dataSize = 0;
		while (source.stream && !foundData)
		{
			char chunkId[4] = {};
			source.stream.read(chunkId, sizeof(chunkId));
			if (source.stream.gcount() != sizeof(chunkId)) break;
			const uint32_t chunkSize = ReadUInt32(source.stream);
			const auto chunkStart = source.stream.tellg();

			if (memcmp(chunkId, "fmt ", 4) == 0)
			{
				const uint16_t format = ReadUInt16(source.stream);
				const uint16_t channels = ReadUInt16(source.stream);
				source.sampleRate = ReadUInt32(source.stream);
				ReadUInt32(source.stream);
				ReadUInt16(source.stream);
				const uint16_t bitsPerSample = ReadUInt16(source.stream);
				if (format != WAVE_FORMAT_PCM || channels != CACHE_CHANNELS || bitsPerSample != 16)
				{
					throw std::runtime_error("Speaker Mode requires decoded stereo 16-bit PCM");
				}
				foundFormat = true;
			}
			else if (memcmp(chunkId, "data", 4) == 0)
			{
				source.dataOffset = static_cast<uint64_t>(source.stream.tellg());
				dataSize = chunkSize;
				foundData = true;
			}

			if (!foundData)
			{
				source.stream.seekg(chunkStart + static_cast<std::streamoff>(chunkSize + (chunkSize & 1)));
			}
		}

		if (!foundFormat || !foundData || source.sampleRate == 0 || dataSize % 4 != 0)
		{
			throw std::runtime_error("decoded Speaker Mode WAV has an invalid format or data chunk");
		}

		source.totalFrames = dataSize / 4;
		source.stream.seekg(static_cast<std::streamoff>(source.dataOffset));
		return source;
	}

	void WriteProcessedFrames(
		WindowedCacheWriter& writer,
		const std::vector<float>& left,
		const std::vector<float>& right,
		uint32_t frameCount,
		uint64_t& framesToSkip,
		uint64_t& framesWritten,
		uint64_t totalFrames)
	{
		const uint32_t skip = static_cast<uint32_t>(std::min<uint64_t>(framesToSkip, frameCount));
		framesToSkip -= skip;
		const uint32_t available = frameCount - skip;
		const uint32_t writeCount = static_cast<uint32_t>(std::min<uint64_t>(
			available,
			totalFrames - framesWritten));
		if (writeCount == 0) return;

		int16_t* destination = writer.Acquire(framesWritten, writeCount);
		if (destination == nullptr)
		{
			throw std::runtime_error("could not map the Speaker Mode render window");
		}

		for (uint32_t frame = 0; frame < writeCount; frame++)
		{
			destination[frame * 2] = FloatToInt16(left[skip + frame]);
			destination[frame * 2 + 1] = FloatToInt16(right[skip + frame]);
		}
		framesWritten += writeCount;
	}

	void RenderPitchCache(
		const std::filesystem::path& wavePath,
		int semitones,
		HANDLE mappingHandle,
		volatile LONG64* contiguousFramesReady,
		volatile LONG64* openingReadyTick,
		const volatile LONG* cancellationRequested)
	{
		ThrowIfCancelled(cancellationRequested);
		auto source = OpenWaveSource(wavePath);
		WindowedCacheWriter writer(mappingHandle, source.totalFrames);
		signalsmith::stretch::SignalsmithStretch<float> stretch(0);
		stretch.presetDefault(CACHE_CHANNELS, static_cast<float>(source.sampleRate));
		stretch.setTransposeSemitones(static_cast<float>(semitones));

		const uint32_t seekFrames = static_cast<uint32_t>(stretch.outputSeekLength(1.0f));
		if (source.totalFrames < seekFrames)
		{
			throw std::runtime_error("decoded Speaker Mode audio is too short for exact pitch alignment");
		}

		const uint32_t bufferFrames = std::max(PROCESS_CHUNK_FRAMES, seekFrames);
		std::vector<int16_t> inputInterleaved(static_cast<size_t>(bufferFrames) * CACHE_CHANNELS);
		std::vector<float> inputLeft(bufferFrames);
		std::vector<float> inputRight(bufferFrames);
		std::vector<float> outputLeft(bufferFrames);
		std::vector<float> outputRight(bufferFrames);
		source.stream.read(
			reinterpret_cast<char*>(inputInterleaved.data()),
			static_cast<std::streamsize>(seekFrames * CACHE_CHANNELS * sizeof(int16_t)));
		if (source.stream.gcount() != static_cast<std::streamsize>(seekFrames * CACHE_CHANNELS * sizeof(int16_t)))
		{
			throw std::runtime_error("decoded Speaker Mode WAV ended during alignment pre-roll");
		}
		for (uint32_t frame = 0; frame < seekFrames; frame++)
		{
			inputLeft[frame] = inputInterleaved[frame * 2] * INT16_NORMALIZER;
			inputRight[frame] = inputInterleaved[frame * 2 + 1] * INT16_NORMALIZER;
		}
		float* seekChannels[] = { inputLeft.data(), inputRight.data() };
		stretch.outputSeek(seekChannels, seekFrames);

		uint64_t framesToSkip = 0;
		uint64_t framesWritten = 0;
		uint64_t framesRead = seekFrames;

		while (framesRead < source.totalFrames)
		{
			ThrowIfCancelled(cancellationRequested);
			const uint32_t frameCount = static_cast<uint32_t>(std::min<uint64_t>(
				PROCESS_CHUNK_FRAMES,
				source.totalFrames - framesRead));
			source.stream.read(
				reinterpret_cast<char*>(inputInterleaved.data()),
				static_cast<std::streamsize>(frameCount * CACHE_CHANNELS * sizeof(int16_t)));
			if (source.stream.gcount() != static_cast<std::streamsize>(frameCount * CACHE_CHANNELS * sizeof(int16_t)))
			{
				throw std::runtime_error("decoded Speaker Mode WAV ended unexpectedly");
			}

			for (uint32_t frame = 0; frame < frameCount; frame++)
			{
				inputLeft[frame] = inputInterleaved[frame * 2] * INT16_NORMALIZER;
				inputRight[frame] = inputInterleaved[frame * 2 + 1] * INT16_NORMALIZER;
			}

			float* inputChannels[] = { inputLeft.data(), inputRight.data() };
			float* outputChannels[] = { outputLeft.data(), outputRight.data() };
			stretch.process(inputChannels, frameCount, outputChannels, frameCount);
			WriteProcessedFrames(
				writer,
				outputLeft,
				outputRight,
				frameCount,
				framesToSkip,
				framesWritten,
				source.totalFrames);
			InterlockedExchange64(contiguousFramesReady, static_cast<LONG64>(framesWritten));
			const uint64_t openingFrames = std::min<uint64_t>(
				source.totalFrames,
				static_cast<uint64_t>(source.sampleRate) * PLAYABLE_SECONDS);
			if (framesWritten >= openingFrames)
			{
				InterlockedCompareExchange64(
					openingReadyTick,
					static_cast<LONG64>(GetTickCount64()),
					0);
			}
			framesRead += frameCount;
		}

		ThrowIfCancelled(cancellationRequested);
		const uint32_t flushFrames = seekFrames;
		outputLeft.resize(flushFrames);
		outputRight.resize(flushFrames);
		float* flushChannels[] = { outputLeft.data(), outputRight.data() };
		stretch.flush(flushChannels, flushFrames, 1.0f);
		WriteProcessedFrames(
			writer,
			outputLeft,
			outputRight,
			flushFrames,
			framesToSkip,
			framesWritten,
			source.totalFrames);
		InterlockedExchange64(contiguousFramesReady, static_cast<LONG64>(framesWritten));
		InterlockedCompareExchange64(
			openingReadyTick,
			static_cast<LONG64>(GetTickCount64()),
			0);

		if (framesWritten != source.totalFrames)
		{
			throw std::runtime_error("Speaker Mode pitch render did not produce the source frame count");
		}

	}

	void InvokeExtractor(
		const std::filesystem::path& rocksmithDirectory,
		const std::string& bankName,
		const std::filesystem::path& wavePath,
		const std::filesystem::path& errorPath,
		const volatile LONG* cancellationRequested)
	{
		const auto helperPath = rocksmithDirectory / L"RSMods.exe";
		if (!std::filesystem::exists(helperPath))
		{
			throw std::runtime_error("RSMods.exe is required to prepare Speaker Mode audio");
		}

		std::wstring commandLine = QuoteArgument(helperPath.wstring())
			+ L" --speaker-cache-extract "
			+ QuoteArgument(rocksmithDirectory.wstring()) + L" "
			+ QuoteArgument(Utf8ToWide(bankName)) + L" "
			+ QuoteArgument(wavePath.wstring()) + L" "
			+ QuoteArgument(errorPath.wstring());
		std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
		mutableCommand.push_back(L'\0');

		STARTUPINFOW startupInfo = {};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInfo = {};
		if (!CreateProcessW(
			helperPath.c_str(),
			mutableCommand.data(),
			nullptr,
			nullptr,
			FALSE,
			CREATE_NO_WINDOW,
			nullptr,
			rocksmithDirectory.c_str(),
			&startupInfo,
			&processInfo))
		{
			throw std::runtime_error("could not start the Speaker Mode audio extractor");
		}

		CloseHandle(processInfo.hThread);
		DWORD waitResult = WAIT_TIMEOUT;
		const DWORD startedTick = GetTickCount();
		while (waitResult == WAIT_TIMEOUT && GetTickCount() - startedTick < 120000)
		{
			waitResult = WaitForSingleObject(processInfo.hProcess, 25);
			if (InterlockedCompareExchange(
				const_cast<volatile LONG*>(cancellationRequested),
				0,
				0) != 0)
			{
				TerminateProcess(processInfo.hProcess, ERROR_CANCELLED);
				WaitForSingleObject(processInfo.hProcess, 5000);
				CloseHandle(processInfo.hProcess);
				throw CacheCancelledException();
			}
		}
		DWORD exitCode = 1;
		if (waitResult == WAIT_OBJECT_0)
		{
			GetExitCodeProcess(processInfo.hProcess, &exitCode);
		}
		else
		{
			// Timed out: stop it, or a hung extractor keeps running (and outlives the game; it is in no job object).
			TerminateProcess(processInfo.hProcess, ERROR_TIMEOUT);
			WaitForSingleObject(processInfo.hProcess, 5000);
		}
		CloseHandle(processInfo.hProcess);

		if (waitResult != WAIT_OBJECT_0)
		{
			throw std::runtime_error("Speaker Mode audio extraction exceeded two minutes");
		}
		if (exitCode != 0)
		{
			std::ifstream errorStream(errorPath);
			std::string error(
				(std::istreambuf_iterator<char>(errorStream)),
				std::istreambuf_iterator<char>());
			throw std::runtime_error(error.empty() ? "Speaker Mode audio extraction failed" : error);
		}
	}
}

namespace Audio::SongShift
{
	class PreparedPitchCache final
	{
	public:
		PreparedPitchCache(std::string bankName, int semitones)
			: bankName(std::move(bankName)), semitones(semitones), completedEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr))
		{
			if (completedEvent == nullptr) throw std::runtime_error("could not create the Speaker Mode audio event");
			const auto currentTick = static_cast<LONG64>(GetTickCount64());
			InterlockedExchange64(&requestTick, currentTick);
		}

		~PreparedPitchCache()
		{
			if (readWindowView != nullptr) UnmapViewOfFile(readWindowView);
			if (mappingHandle != nullptr) CloseHandle(mappingHandle);
			if (fileHandle != INVALID_HANDLE_VALUE) CloseHandle(fileHandle);
			if (completedEvent != nullptr) CloseHandle(completedEvent);
		}

		std::string bankName;
		std::string error;
		int semitones = 0;
		uint32_t sampleRate = 0;
		uint64_t totalFrames = 0;
		__declspec(align(8)) volatile LONG64 storageBytes = 0;
		volatile LONG state = CACHE_PREPARING;
		volatile LONG cancellationRequested = 0;
		volatile LONG metadataReady = 0;
		volatile LONG activeReaders = 0;
		volatile LONG isEvicting = 0;
		__declspec(align(8)) volatile LONG64 contiguousFramesReady = 0;
		HANDLE completedEvent = nullptr;
		HANDLE fileHandle = INVALID_HANDLE_VALUE;
		HANDLE mappingHandle = nullptr;

		// Playback's sliding view of the mapped song. Guarded by readWindowLock;
		// eviction never runs concurrently because readers hold activeReaders.
		SRWLOCK readWindowLock = SRWLOCK_INIT;
		void* readWindowView = nullptr;
		uint64_t readWindowStartByte = 0;
		uint64_t readWindowBytes = 0;
		__declspec(align(8)) volatile LONG64 retiredTick = 0;
		__declspec(align(8)) volatile LONG64 requestTick = 0;
		__declspec(align(8)) volatile LONG64 extractionStartedTick = 0;
		__declspec(align(8)) volatile LONG64 extractionCompletedTick = 0;
		__declspec(align(8)) volatile LONG64 renderStartedTick = 0;
		__declspec(align(8)) volatile LONG64 openingReadyTick = 0;
		__declspec(align(8)) volatile LONG64 preparationCompletedTick = 0;
		__declspec(align(8)) volatile LONG64 underflowWaitCount = 0;
		volatile LONG lastPlaybackFrame = 0;
		__declspec(align(8)) volatile LONG64 lastRequiredFrame = 0;
		__declspec(align(8)) volatile LONG64 lastReadyFrameBeforeWait = 0;
	};
}

namespace
{
	class CacheReadGuard final
	{
	public:
		CacheReadGuard(volatile LONG* activeReaders, volatile LONG* isEvicting)
			: activeReaders(activeReaders)
		{
			if (InterlockedCompareExchange(isEvicting, 0, 0) != 0) return;
			InterlockedIncrement(activeReaders);
			if (InterlockedCompareExchange(isEvicting, 0, 0) == 0)
			{
				isAcquired = true;
				return;
			}

			InterlockedDecrement(activeReaders);
		}

		~CacheReadGuard()
		{
			if (isAcquired) InterlockedDecrement(activeReaders);
		}

		bool IsAcquired() const
		{
			return isAcquired;
		}

	private:
		volatile LONG* activeReaders;
		bool isAcquired = false;
	};

	void CreateTemporaryMappedCache(
		Audio::SongShift::PreparedPitchCache& cache,
		const std::filesystem::path& path,
		uint32_t sampleRate,
		uint64_t totalFrames)
	{
		const uint64_t fileSize = totalFrames * CACHE_CHANNELS * sizeof(int16_t);
		if (fileSize > static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max()))
		{
			throw std::runtime_error("Speaker Mode temporary audio is too large to map");
		}

		const HANDLE fileHandle = CreateFileW(
			path.c_str(),
			GENERIC_READ | GENERIC_WRITE | DELETE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
			nullptr);
		if (fileHandle == INVALID_HANDLE_VALUE)
		{
			throw std::runtime_error("could not create temporary Speaker Mode audio");
		}

		LARGE_INTEGER endPosition = {};
		endPosition.QuadPart = static_cast<LONGLONG>(fileSize);
		if (!SetFilePointerEx(fileHandle, endPosition, nullptr, FILE_BEGIN)
			|| !SetEndOfFile(fileHandle))
		{
			CloseHandle(fileHandle);
			throw std::runtime_error("could not size temporary Speaker Mode audio");
		}

		const HANDLE mappingHandle = CreateFileMappingW(fileHandle, nullptr, PAGE_READWRITE, 0, 0, nullptr);
		if (mappingHandle == nullptr)
		{
			CloseHandle(fileHandle);
			throw std::runtime_error("could not map temporary Speaker Mode audio");
		}

		cache.fileHandle = fileHandle;
		cache.mappingHandle = mappingHandle;
		cache.sampleRate = sampleRate;
		cache.totalFrames = totalFrames;
		InterlockedExchange64(&cache.storageBytes, static_cast<LONG64>(fileSize));
		MemoryBarrier();
		InterlockedExchange(&cache.metadataReady, 1);
	}

	bool TryReleaseCacheStorage(Audio::SongShift::PreparedPitchCache& cache)
	{
		InterlockedExchange(&cache.isEvicting, 1);
		if (InterlockedCompareExchange(&cache.activeReaders, 0, 0) != 0)
		{
			InterlockedExchange(&cache.isEvicting, 0);
			return false;
		}

		InterlockedExchange(&cache.metadataReady, 0);
		InterlockedExchange64(&cache.contiguousFramesReady, 0);
		MemoryBarrier();

		auto* readWindowView = cache.readWindowView;
		const HANDLE mappingHandle = cache.mappingHandle;
		const HANDLE fileHandle = cache.fileHandle;
		cache.readWindowView = nullptr;
		cache.readWindowStartByte = 0;
		cache.readWindowBytes = 0;
		cache.mappingHandle = nullptr;
		cache.fileHandle = INVALID_HANDLE_VALUE;
		InterlockedExchange64(&cache.storageBytes, 0);
		InterlockedExchange(&cache.state, CACHE_EVICTED);

		if (readWindowView != nullptr) UnmapViewOfFile(readWindowView);
		if (mappingHandle != nullptr) CloseHandle(mappingHandle);
		if (fileHandle != INVALID_HANDLE_VALUE) CloseHandle(fileHandle);
		return true;
	}

	void PrepareCache(Audio::SongShift::PreparedPitchCache* cache)
	{
		try
		{
			ThrowIfCancelled(&cache->cancellationRequested);
			const auto rocksmithDirectory = GetRocksmithDirectory();
			const auto safeName = GetCacheBaseName(cache->bankName);
			const auto sequence = InterlockedIncrement64(&cacheSequence);
			const auto uniqueName = safeName + L"." + std::to_wstring(GetCurrentProcessId())
				+ L"." + std::to_wstring(sequence);
			const auto wavePath = sessionCacheDirectory / (uniqueName + L".decoded.wav");
			const auto errorPath = sessionCacheDirectory / (uniqueName + L".error.txt");
			const auto cachePath = sessionCacheDirectory / (uniqueName + L".prepared.pcm");
			TemporarySourceFiles temporarySourceFiles(wavePath, errorPath);

			{
				std::lock_guard<std::mutex> extractionLock(sourceExtractionMutex);
				InterlockedExchange64(
					&cache->extractionStartedTick,
					static_cast<LONG64>(GetTickCount64()));
				InvokeExtractor(
					rocksmithDirectory,
					cache->bankName,
					wavePath,
					errorPath,
					&cache->cancellationRequested);
				InterlockedExchange64(
					&cache->extractionCompletedTick,
					static_cast<LONG64>(GetTickCount64()));
			}
			ThrowIfCancelled(&cache->cancellationRequested);
			{
				auto source = OpenWaveSource(wavePath);
				CreateTemporaryMappedCache(
					*cache,
					cachePath,
					source.sampleRate,
					source.totalFrames);
			}
			InterlockedExchange64(
				&cache->renderStartedTick,
				static_cast<LONG64>(GetTickCount64()));
			RenderPitchCache(
				wavePath,
				cache->semitones,
				cache->mappingHandle,
				&cache->contiguousFramesReady,
				&cache->openingReadyTick,
				&cache->cancellationRequested);

			InterlockedExchange64(
				&cache->preparationCompletedTick,
				static_cast<LONG64>(GetTickCount64()));
			InterlockedExchange(&cache->state, CACHE_READY);
		}
		catch (const CacheCancelledException&)
		{
			InterlockedExchange64(
				&cache->preparationCompletedTick,
				static_cast<LONG64>(GetTickCount64()));
			InterlockedExchange(&cache->metadataReady, 0);
			InterlockedExchange64(&cache->contiguousFramesReady, 0);
			cache->error.clear();
			InterlockedExchange(&cache->state, CACHE_CANCELLED);
		}
		catch (const std::exception& exception)
		{
			InterlockedExchange64(
				&cache->preparationCompletedTick,
				static_cast<LONG64>(GetTickCount64()));
			InterlockedExchange(&cache->metadataReady, 0);
			InterlockedExchange64(&cache->contiguousFramesReady, 0);
			cache->error = exception.what();
			InterlockedExchange(&cache->state, CACHE_FAILED);
		}
		catch (...)
		{
			InterlockedExchange64(
				&cache->preparationCompletedTick,
				static_cast<LONG64>(GetTickCount64()));
			InterlockedExchange(&cache->metadataReady, 0);
			InterlockedExchange64(&cache->contiguousFramesReady, 0);
			cache->error = "unknown Speaker Mode audio preparation failure";
			InterlockedExchange(&cache->state, CACHE_FAILED);
		}

		SetEvent(cache->completedEvent);
	}
}

bool Audio::SongShift::PreRenderedPitchCache::Initialize()
{
	std::lock_guard<std::mutex> lock(cacheRegistryMutex);
	if (isCacheInitialized) return true;

	try
	{
		sessionCacheDirectory = GetRocksmithDirectory()
			/ L"RSMods" / L"Cache" / L"SpeakerMode";
		std::filesystem::remove_all(sessionCacheDirectory);
		std::filesystem::create_directories(sessionCacheDirectory);
		isCacheInitialized = true;
		return true;
	}
	catch (...)
	{
		sessionCacheDirectory.clear();
		return false;
	}
}

Audio::SongShift::PreparedPitchCache* Audio::SongShift::PreRenderedPitchCache::Request(
	const char* bankName,
	int semitones)
{
	if (bankName == nullptr || bankName[0] == '\0' || semitones == 0) return nullptr;

	const std::string key = std::string(bankName) + "\n" + std::to_string(semitones);
	PreparedPitchCache* cache = nullptr;
	{
		std::lock_guard<std::mutex> lock(cacheRegistryMutex);
		if (!isCacheInitialized) return nullptr;
		auto existing = cacheRegistry.find(key);
		if (existing != cacheRegistry.end())
		{
			cache = existing->second.get();
			const LONG state = InterlockedCompareExchange(&cache->state, 0, 0);
			const bool isActivePreparation = state == CACHE_PREPARING
				&& InterlockedCompareExchange(&cache->cancellationRequested, 0, 0) == 0;
			if (state == CACHE_READY || isActivePreparation)
			{
				InterlockedExchange64(&cache->retiredTick, 0);
				return cache;
			}

			supersededCaches.push_back(std::move(existing->second));
			auto created = std::make_unique<PreparedPitchCache>(bankName, semitones);
			cache = created.get();
			existing->second = std::move(created);
		}
		else
		{
			auto created = std::make_unique<PreparedPitchCache>(bankName, semitones);
			cache = created.get();
			cacheRegistry.emplace(key, std::move(created));
		}
	}

	try
	{
		std::thread worker(PrepareCache, cache);
		try
		{
			worker.detach();
		}
		catch (...)
		{
			worker.join();
		}
	}
	catch (const std::exception& exception)
	{
		cache->error = exception.what();
		InterlockedExchange(&cache->state, CACHE_FAILED);
		SetEvent(cache->completedEvent);
	}
	return cache;
}

void Audio::SongShift::PreRenderedPitchCache::Retire(PreparedPitchCache* cache)
{
	if (cache == nullptr) return;
	InterlockedCompareExchange64(
		&cache->retiredTick,
		static_cast<LONG64>(GetTickCount64()),
		0);
	if (InterlockedCompareExchange(&cache->state, 0, 0) == CACHE_PREPARING)
	{
		InterlockedExchange(&cache->cancellationRequested, 1);
	}
}

uint64_t Audio::SongShift::PreRenderedPitchCache::Maintain()
{
	std::lock_guard<std::mutex> lock(cacheRegistryMutex);
	const uint64_t currentTick = GetTickCount64();
	uint64_t totalRetainedBytes = 0;
	uint64_t releasedBytesTotal = 0;

	const auto readStorageBytes = [](const PreparedPitchCache& cache)
	{
		return static_cast<uint64_t>(InterlockedCompareExchange64(
			const_cast<volatile LONG64*>(&cache.storageBytes),
			0,
			0));
	};
	const auto releaseExpired = [currentTick, &readStorageBytes, &totalRetainedBytes, &releasedBytesTotal](
		PreparedPitchCache& cache)
	{
		const uint64_t storageBytes = readStorageBytes(cache);
		if (storageBytes == 0) return;
		const auto retiredTick = static_cast<uint64_t>(InterlockedCompareExchange64(
			&cache.retiredTick,
			0,
			0));
		const LONG state = InterlockedCompareExchange(&cache.state, 0, 0);
		if (retiredTick != 0
			&& currentTick - retiredTick >= CACHE_COOLDOWN_MILLISECONDS
			&& state != CACHE_PREPARING)
		{
			if (TryReleaseCacheStorage(cache))
			{
				releasedBytesTotal += storageBytes;
				return;
			}
		}
		totalRetainedBytes += storageBytes;
	};

	for (auto& entry : cacheRegistry) releaseExpired(*entry.second);
	for (auto& cache : supersededCaches) releaseExpired(*cache);

	while (totalRetainedBytes > MAX_RETAINED_CACHE_BYTES)
	{
		PreparedPitchCache* oldestCache = nullptr;
		uint64_t oldestRetiredTick = std::numeric_limits<uint64_t>::max();
		const auto consider = [currentTick, &readStorageBytes, &oldestCache, &oldestRetiredTick](
			PreparedPitchCache& cache)
		{
			if (readStorageBytes(cache) == 0) return;
			const auto retiredTick = static_cast<uint64_t>(InterlockedCompareExchange64(
				&cache.retiredTick,
				0,
				0));
			const LONG state = InterlockedCompareExchange(&cache.state, 0, 0);
			if (retiredTick == 0 || state == CACHE_PREPARING
				|| currentTick - retiredTick < MINIMUM_EVICTION_GRACE_MILLISECONDS)
			{
				return;
			}
			if (retiredTick < oldestRetiredTick)
			{
				oldestRetiredTick = retiredTick;
				oldestCache = &cache;
			}
		};

		for (auto& entry : cacheRegistry) consider(*entry.second);
		for (auto& cache : supersededCaches) consider(*cache);
		if (oldestCache == nullptr) break;

		const uint64_t releasedBytes = readStorageBytes(*oldestCache);
		if (!TryReleaseCacheStorage(*oldestCache)) break;
		totalRetainedBytes -= std::min(totalRetainedBytes, releasedBytes);
		releasedBytesTotal += releasedBytes;
	}

	return releasedBytesTotal;
}

bool Audio::SongShift::PreRenderedPitchCache::IsReady(const PreparedPitchCache* cache)
{
	return cache != nullptr && InterlockedCompareExchange(
		const_cast<volatile LONG*>(&cache->state),
		0,
		0) == CACHE_READY;
}

const char* Audio::SongShift::PreRenderedPitchCache::GetError(const PreparedPitchCache* cache)
{
	if (cache == nullptr) return "prepared-audio request is missing";
	return cache->error.empty()
		? "audio preparation was cancelled or did not complete"
		: cache->error.c_str();
}

int Audio::SongShift::PreRenderedPitchCache::GetSemitones(const PreparedPitchCache* cache)
{
	return cache != nullptr ? cache->semitones : 0;
}

bool Audio::SongShift::PreRenderedPitchCache::GetProbeSnapshot(
	const PreparedPitchCache* cache,
	PitchCacheProbeSnapshot& snapshot)
{
	if (cache == nullptr) return false;

	const LONG state = InterlockedCompareExchange(
		const_cast<volatile LONG*>(&cache->state),
		0,
		0);
	switch (state)
	{
		case CACHE_READY:
			snapshot.state = PitchCacheProbeState::Ready;
			break;
		case CACHE_FAILED:
			snapshot.state = PitchCacheProbeState::Failed;
			break;
		case CACHE_CANCELLED:
		case CACHE_EVICTED:
			snapshot.state = PitchCacheProbeState::Cancelled;
			break;
		case CACHE_PREPARING:
		default:
			snapshot.state = PitchCacheProbeState::Preparing;
			break;
	}

	snapshot.hasMetadata = InterlockedCompareExchange(
		const_cast<volatile LONG*>(&cache->metadataReady),
		0,
		0) != 0;
	if (snapshot.hasMetadata)
	{
		MemoryBarrier();
		snapshot.sampleRate = cache->sampleRate;
		snapshot.totalFrames = cache->totalFrames;
	}

	const auto readValue = [](const volatile LONG64* value)
	{
		return static_cast<uint64_t>(InterlockedCompareExchange64(
			const_cast<volatile LONG64*>(value),
			0,
			0));
	};

	snapshot.contiguousFramesReady = readValue(&cache->contiguousFramesReady);
	snapshot.requestTick = readValue(&cache->requestTick);
	snapshot.extractionStartedTick = readValue(&cache->extractionStartedTick);
	snapshot.extractionCompletedTick = readValue(&cache->extractionCompletedTick);
	snapshot.renderStartedTick = readValue(&cache->renderStartedTick);
	snapshot.openingReadyTick = readValue(&cache->openingReadyTick);
	snapshot.preparationCompletedTick = readValue(&cache->preparationCompletedTick);
	snapshot.underflowWaitCount = readValue(&cache->underflowWaitCount);
	snapshot.lastPlaybackFrame = static_cast<uint64_t>(InterlockedCompareExchange(
		const_cast<volatile LONG*>(&cache->lastPlaybackFrame),
		0,
		0));
	snapshot.lastRequiredFrame = readValue(&cache->lastRequiredFrame);
	snapshot.lastReadyFrameBeforeWait = readValue(&cache->lastReadyFrameBeforeWait);

	return true;
}

bool Audio::SongShift::PreRenderedPitchCache::MatchesAudio(
	const PreparedPitchCache* cache,
	uint64_t totalFrames,
	uint32_t sampleRate)
{
	if (cache == nullptr) return false;
	if (InterlockedCompareExchange(
		const_cast<volatile LONG*>(&cache->isEvicting),
		0,
		0) != 0)
	{
		return false;
	}
	const LONG state = InterlockedCompareExchange(
		const_cast<volatile LONG*>(&cache->state),
		0,
		0);
	if (state != CACHE_PREPARING && state != CACHE_READY) return false;
	if (InterlockedCompareExchange(
		const_cast<volatile LONG*>(&cache->metadataReady),
		0,
		0) == 0)
	{
		return false;
	}

	return cache->sampleRate == sampleRate && cache->totalFrames == totalFrames;
}

bool Audio::SongShift::PreRenderedPitchCache::CopyFrames(
	const PreparedPitchCache* cache,
	int16_t* destination,
	uint32_t positionStart,
	uint16_t frameCount,
	uint32_t sampleRate)
{
	if (cache == nullptr || destination == nullptr) return false;
	CacheReadGuard readGuard(
		const_cast<volatile LONG*>(&cache->activeReaders),
		const_cast<volatile LONG*>(&cache->isEvicting));
	if (!readGuard.IsAcquired()) return false;
	if (!MatchesAudio(cache, cache->totalFrames, sampleRate)) return false;
	if (positionStart > cache->totalFrames || frameCount > cache->totalFrames - positionStart) return false;
	const uint64_t frameEnd = static_cast<uint64_t>(positionStart) + frameCount;
	InterlockedExchange(
		const_cast<volatile LONG*>(&cache->lastPlaybackFrame),
		static_cast<LONG>(frameEnd));
	uint64_t readyFrames = static_cast<uint64_t>(InterlockedCompareExchange64(
		const_cast<volatile LONG64*>(&cache->contiguousFramesReady),
		0,
		0));
	if (frameEnd > readyFrames)
	{
		const LONG state = InterlockedCompareExchange(
			const_cast<volatile LONG*>(&cache->state),
			0,
			0);
		if (state == CACHE_FAILED || state == CACHE_CANCELLED) return false;

		// The request is beyond the rendered frontier -- a seek, such as Riff
		// Repeater picking a late section or a skip. The render is sequential, so
		// waiting here would stall Wwise decode for however long the catch-up
		// takes; hand back silence instead and resume the real audio once the
		// render reaches this position.
		InterlockedExchange64(
			const_cast<volatile LONG64*>(&cache->lastRequiredFrame),
			static_cast<LONG64>(frameEnd));
		InterlockedExchange64(
			const_cast<volatile LONG64*>(&cache->lastReadyFrameBeforeWait),
			static_cast<LONG64>(readyFrames));
		InterlockedIncrement64(const_cast<volatile LONG64*>(&cache->underflowWaitCount));
		memset(
			destination,
			0,
			static_cast<size_t>(frameCount) * CACHE_CHANNELS * sizeof(int16_t));
		return true;
	}

	// Serve the copy through the cache's sliding read window, remapping when the
	// request leaves it. Sequential playback remaps every ~95 seconds of audio;
	// a seek remaps once. The active-reader guard keeps eviction from closing the
	// mapping underneath this.
	auto* mutableCache = const_cast<PreparedPitchCache*>(cache);
	const uint64_t startByte = static_cast<uint64_t>(positionStart)
		* CACHE_CHANNELS * sizeof(int16_t);
	const uint64_t endByte = frameEnd * CACHE_CHANNELS * sizeof(int16_t);

	AcquireSRWLockExclusive(&mutableCache->readWindowLock);
	if (mutableCache->readWindowView == nullptr
		|| startByte < mutableCache->readWindowStartByte
		|| endByte > mutableCache->readWindowStartByte + mutableCache->readWindowBytes)
	{
		if (mutableCache->readWindowView != nullptr)
		{
			UnmapViewOfFile(mutableCache->readWindowView);
			mutableCache->readWindowView = nullptr;
			mutableCache->readWindowStartByte = 0;
			mutableCache->readWindowBytes = 0;
		}

		const uint64_t totalBytes = cache->totalFrames * CACHE_CHANNELS * sizeof(int16_t);
		const uint64_t windowStart = startByte & ~(MAP_ALIGNMENT - 1);
		const uint64_t windowBytes = std::min(MAP_WINDOW_BYTES, totalBytes - windowStart);
		if (endByte <= windowStart + windowBytes && cache->mappingHandle != nullptr)
		{
			auto* view = MapWindow(cache->mappingHandle, windowStart, windowBytes, false);
			if (view != nullptr)
			{
				mutableCache->readWindowView = view;
				mutableCache->readWindowStartByte = windowStart;
				mutableCache->readWindowBytes = windowBytes;
			}
		}
	}

	bool copied = false;
	if (mutableCache->readWindowView != nullptr)
	{
		memcpy(
			destination,
			static_cast<const char*>(mutableCache->readWindowView)
				+ (startByte - mutableCache->readWindowStartByte),
			static_cast<size_t>(frameCount) * CACHE_CHANNELS * sizeof(int16_t));
		copied = true;
	}
	ReleaseSRWLockExclusive(&mutableCache->readWindowLock);
	return copied;
}
