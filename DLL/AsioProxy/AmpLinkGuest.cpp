#include "AmpLinkGuest.hpp"
#include "AmpLink.hpp"
#include "../Audio/DelayLinePitchShifter.hpp"
#include <avrt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "avrt.lib")

namespace AmpLink
{
	namespace
	{
		constexpr ASIOError kOk = 0;
		constexpr ASIOError kNotPresent = -1000;
		constexpr ASIOError kInvalidParameter = -998;
		constexpr ASIOError kInvalidMode = -997;
		constexpr ASIOError kNoClock = -995;
		constexpr long kSelectorSupported = 1;
		constexpr long kResetRequest = 3;
		constexpr long kDefaultFrames = 128;

		std::wstring ModuleDir()
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&ModuleDir), &module) || !module) return std::wstring();
			wchar_t path[MAX_PATH]{};
			if (!GetModuleFileNameW(module, path, MAX_PATH)) return std::wstring();
			std::wstring p = path;
			const size_t slash = p.find_last_of(L"\\/");
			return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
		}

		// Its own log file: the game's copy rewrites RocksmithAudioBridge-log.txt per process, so a guest
		// writing there would wipe the game's log. Kept short: a few lines per stream start and link change.
		std::mutex g_logMutex;
		void GuestLog(const char* format, ...)
		{
			static const auto start = std::chrono::steady_clock::now();
			static bool truncated = false;
			std::lock_guard<std::mutex> guard(g_logMutex);
			const std::wstring dir = ModuleDir();
			if (dir.empty()) return;
			FILE* file = nullptr;
			if (_wfopen_s(&file, (dir + L"\\RocksmithAudioBridge-amp-link-log.txt").c_str(), truncated ? L"a" : L"w") != 0 || !file) return;
			truncated = true;
			fprintf(file, "%8.3f [%5lu] ", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), GetCurrentProcessId());
			va_list args;
			va_start(args, format);
			vfprintf(file, format, args);
			va_end(args);
			fputc('\n', file);
			fclose(file);
		}

		std::wstring ProcessBaseName()
		{
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			std::wstring p = path;
			const size_t slash = p.find_last_of(L"\\/");
			return slash == std::wstring::npos ? p : p.substr(slash + 1);
		}

		bool ProcessAlive(LONG pid)
		{
			HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
			if (!process) return GetLastError() == ERROR_ACCESS_DENIED;   // exists but is not ours to open
			const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
			CloseHandle(process);
			return alive;
		}

		long RememberedHostFrames()
		{
			HKEY key = nullptr;
			DWORD value = 0, size = sizeof(value), type = 0;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
			const LSTATUS status = RegQueryValueExW(key, kRegistryFrames, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
			RegCloseKey(key);
			return status == ERROR_SUCCESS && type == REG_DWORD && value > 0 && value <= static_cast<DWORD>(kMaxFrames) ? static_cast<long>(value) : 0;
		}
	}

	bool IsGuestProcess()
	{
#if defined(_WIN64)
		return true;
#elif defined(RSMODS_ASIO_PROXY_TESTING)
		wchar_t value[2]{};
		return GetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_GUEST", value, 2) > 0 && value[0] == L'1';
#else
		return _wcsicmp(ProcessBaseName().c_str(), L"Rocksmith2014.exe") != 0;
#endif
	}

	class GuestDriver final : public IAsioDriver
	{
	public:
		explicit GuestDriver(const CLSID& clsid) : m_clsid(clsid) {}
		~GuestDriver() { StopThread(); FreeBuffers(); CloseLink(); }

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
		{
			if (!object) return E_POINTER;
			if (riid == IID_IUnknown || riid == m_clsid) { *object = this; AddRef(); return S_OK; }
			*object = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
		ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --m_ref; if (!n) delete this; return n; }

		ASIOBool init(void*) override
		{
			TryOpenLink();
			GuestLog("init in %ls: link %s, offering %ld frames at 48000 Hz", ProcessBaseName().c_str(), m_shared ? "open" : "not open (Rocksmith not running)", OfferedFrames());
			return 1;   // always usable: with no game running the amp sim gets silence, never an error
		}
		void getDriverName(char* name) override { if (name) strcpy_s(name, 32, "Rocksmith Audio Bridge ASIO"); }
		long getDriverVersion() override { return 1; }
		void getErrorMessage(char* text) override { if (text) strcpy_s(text, 124, m_error.c_str()); }

		ASIOError start() override
		{
			if (m_buffers.empty() || m_running) return m_running ? kOk : kInvalidMode;
			m_running = true;
			m_thread = std::thread([this] { Run(); });
			return kOk;
		}
		ASIOError stop() override { StopThread(); return kOk; }

		ASIOError getChannels(long* in, long* out) override { if (in) *in = kChannels; if (out) *out = kChannels; return kOk; }
		ASIOError getLatencies(long* in, long* out) override
		{
			// Input: one game buffer. Output: the link's block, plus the game's own output buffer.
			const long frames = m_frames > 0 ? m_frames : OfferedFrames();
			if (in) *in = frames;
			if (out) *out = frames * 2;
			return kOk;
		}
		ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override
		{
			// Only the game's own size: blocks pass one for one, so the sizes must match.
			const long frames = OfferedFrames();
			if (mn) *mn = frames; if (mx) *mx = frames; if (pref) *pref = frames; if (gran) *gran = 0;
			return kOk;
		}
		ASIOError canSampleRate(ASIOSampleRate rate) override { return rate == kSampleRate ? kOk : kNoClock; }
		ASIOError getSampleRate(ASIOSampleRate* rate) override { if (rate) *rate = kSampleRate; return kOk; }
		ASIOError setSampleRate(ASIOSampleRate rate) override { return rate == kSampleRate || rate == 0.0 ? kOk : kNoClock; }
		ASIOError getClockSources(ASIOClockSource* clocks, long* count) override
		{
			if (!clocks || !count || *count < 1) { if (count) *count = 0; return kInvalidParameter; }
			clocks[0] = {};
			clocks[0].index = 0;
			clocks[0].associatedChannel = -1;
			clocks[0].associatedGroup = -1;
			clocks[0].isCurrentSource = 1;
			strcpy_s(clocks[0].name, "Rocksmith");
			*count = 1;
			return kOk;
		}
		ASIOError setClockSource(long reference) override { return reference == 0 ? kOk : kInvalidParameter; }
		ASIOError getSamplePosition(ASIOSamples* samples, ASIOTimeStamp* stamp) override
		{
			const uint64_t position = m_samplePosition.load(std::memory_order_acquire);
			if (samples) { samples->hi = static_cast<long>(position >> 32); samples->lo = static_cast<unsigned long>(position); }
			if (stamp)
			{
				LARGE_INTEGER counter{}, frequency{};
				QueryPerformanceCounter(&counter);
				QueryPerformanceFrequency(&frequency);
				const uint64_t ns = frequency.QuadPart > 0 ? static_cast<uint64_t>(static_cast<long double>(counter.QuadPart) * 1e9L / frequency.QuadPart) : 0;
				stamp->hi = static_cast<long>(ns >> 32);
				stamp->lo = static_cast<unsigned long>(ns);
			}
			return kOk;
		}
		ASIOError getChannelInfo(ASIOChannelInfo* info) override
		{
			if (!info || info->channel < 0 || info->channel >= kChannels) return kInvalidParameter;
			info->isActive = 0;
			for (const auto& b : m_infos) if (b.isInput == info->isInput && b.channelNum == info->channel) info->isActive = 1;
			info->channelGroup = 0;
			info->type = ASIOSTInt32LSB;
			if (info->isInput) strcpy_s(info->name, 32, info->channel == 0 ? "Rocksmith guitar" : "Rocksmith guitar 2");
			else strcpy_s(info->name, 32, info->channel == 0 ? "Rocksmith out L" : "Rocksmith out R");
			return kOk;
		}

		ASIOError createBuffers(ASIOBufferInfo* infos, long count, long frames, ASIOCallbacks* callbacks) override
		{
			if (m_running) return kInvalidMode;
			if (!infos || count <= 0 || !callbacks || !callbacks->bufferSwitch || frames < 1 || frames > kMaxFrames) return kInvalidParameter;
			FreeBuffers();
			for (long i = 0; i < count; ++i)
			{
				if (infos[i].channelNum < 0 || infos[i].channelNum >= kChannels) { FreeBuffers(); return kInvalidParameter; }
				for (int d = 0; d < 2; ++d)
				{
					void* buffer = calloc(static_cast<size_t>(frames), sizeof(int32_t));
					if (!buffer) { FreeBuffers(); return -994; }   // ASE_NoMemory
					infos[i].buffers[d] = buffer;
					m_buffers.push_back(buffer);
				}
				m_infos.push_back(infos[i]);
			}
			m_frames = frames;
			// Drop Pedal shifters start from an empty ring on every new stream (allocates; never on the stream thread).
			Audio::CaptureFormat format;
			format.sampleFormat = Audio::SampleFormat::Float32;
			format.sampleRate = kSampleRate;
			format.channelCount = 1;
			for (auto& shifter : m_shifters) shifter.Prepare(format);
			m_shiftScratch.assign(static_cast<size_t>(frames), 0.0f);
			m_callbacks = *callbacks;
			m_resetRequested = false;
			GuestLog("createBuffers: %ld channel(s), %ld frames", count, frames);
			return kOk;
		}
		ASIOError disposeBuffers() override { StopThread(); FreeBuffers(); return kOk; }
		ASIOError controlPanel() override
		{
			const std::wstring text = L"Rocksmith Audio Bridge ASIO, external amp link.\n\n"
				L"This driver plays your amp sim through Rocksmith's audio interface while Rocksmith keeps your clean "
				L"signal for note detection.\n\n1. Start Rocksmith with the Audio Bridge.\n2. Pick this driver here.\n"
				L"3. In the game overlay (press \\), open External amp and turn it on.\n\n"
				L"Sample rate and buffer size follow Rocksmith (48 kHz, " + std::to_wstring(OfferedFrames()) + L" samples).";
			MessageBoxW(nullptr, text.c_str(), L"Rocksmith Audio Bridge ASIO", MB_OK | MB_ICONINFORMATION);
			return kOk;
		}
		ASIOError future(long, void*) override { return kInvalidParameter; }
		ASIOError outputReady() override { return kNotPresent; }

	private:
		long OfferedFrames()
		{
			TryOpenLink();
			if (m_shared)
			{
				const LONG frames = Load(&m_shared->hostFrames);
				if (frames > 0 && frames <= kMaxFrames) return frames;
			}
			const long remembered = RememberedHostFrames();
			return remembered > 0 ? remembered : kDefaultFrames;
		}

		// Called from the host program's threads (buffer-size queries) and from the stream thread, so the open is
		// serialised; m_shared is published last and never cleared while the driver lives.
		bool TryOpenLink()
		{
			std::lock_guard<std::mutex> guard(m_linkMutex);
			if (m_shared) return true;
			HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMappingName);
			if (!mapping) return false;
			auto* shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
			HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, kInputEventName);
			if (!shared || !event || shared->magic != kMagic || shared->size != static_cast<LONG>(sizeof(Shared)))
			{
				if (shared) UnmapViewOfFile(shared);
				if (event) CloseHandle(event);
				CloseHandle(mapping);
				return false;
			}
			m_mapping = mapping; m_event = event;
			MemoryBarrier();
			m_shared = shared;
			return true;
		}

		void CloseLink()
		{
			ReleaseClaim();
			if (m_shared) { UnmapViewOfFile(m_shared); m_shared = nullptr; }
			if (m_mapping) { CloseHandle(m_mapping); m_mapping = nullptr; }
			if (m_event) { CloseHandle(m_event); m_event = nullptr; }
		}

		// One guest owns the output slots. A second program on this driver runs on the silent clock.
		bool TryClaim()
		{
			if (!m_shared) return false;
			const LONG me = static_cast<LONG>(GetCurrentProcessId());
			LONG owner = InterlockedCompareExchange(&m_shared->guestPid, me, 0);
			if (owner != 0 && owner != me && !ProcessAlive(owner))
				owner = InterlockedCompareExchange(&m_shared->guestPid, me, owner) == owner ? 0 : owner;
			if (owner != 0 && owner != me) return false;
			if (!m_claimed)
			{
				m_claimed = true;
				// "Nothing written back yet": far behind the game, so the host does not count a fresh claim (whose
				// last-written block is still 0 or a previous guest's) as connected.
				Store(&m_shared->guestLastSeq, Load(&m_shared->inputSeq) - 0x40000000);
				char name[64]{};
				WideCharToMultiByte(CP_UTF8, 0, ProcessBaseName().c_str(), -1, name, sizeof(name) - 1, nullptr, nullptr);
				InterlockedIncrement(&m_shared->guestNameSeq);
				memcpy(m_shared->guestName, name, sizeof(name));
				InterlockedIncrement(&m_shared->guestNameSeq);
				GuestLog("claimed the link as '%s'", name);
			}
			Store(&m_shared->guestFrames, m_frames);
			return true;
		}

		void ReleaseClaim()
		{
			if (!m_claimed || !m_shared) { m_claimed = false; return; }
			Store(&m_shared->guestFrames, 0);
			InterlockedCompareExchange(&m_shared->guestPid, 0, static_cast<LONG>(GetCurrentProcessId()));
			m_claimed = false;
		}

		void StopThread()
		{
			m_running = false;
			if (m_thread.joinable()) m_thread.join();
			ReleaseClaim();
		}

		void FreeBuffers()
		{
			for (void* buffer : m_buffers) free(buffer);
			m_buffers.clear();
			m_infos.clear();
			m_frames = 0;
		}

		// The game's frame size no longer matches ours: ask the host program to reset, which makes it query the
		// buffer size again and re-create the buffers at the game's size. Once per mismatch.
		void RequestReset(LONG hostFrames)
		{
			if (m_resetRequested || !m_callbacks.asioMessage) return;
			m_resetRequested = true;
			GuestLog("game runs %ld frames, this stream %ld; asking the host program to reset", static_cast<long>(hostFrames), m_frames);
			if (m_callbacks.asioMessage(kSelectorSupported, kResetRequest, nullptr, nullptr) == 1)
				m_callbacks.asioMessage(kResetRequest, 0, nullptr, nullptr);
		}

		// One block on the game's clock: the game's clean input in, the amp sim's output back.
		void ProcessLinked(LONG seq)
		{
			const LONG slot = SlotOf(seq);
			if (Load(&m_shared->inputSlotSeq[slot]) != seq) return;   // lapped: too late for this block
			for (const auto& info : m_infos)
			{
				if (!info.isInput) continue;
				memcpy(info.buffers[m_index], m_shared->input[slot][info.channelNum], static_cast<size_t>(m_frames) * sizeof(int32_t));
			}
			if (Load(&m_shared->inputSlotSeq[slot]) != seq) return;   // rewritten while copying
			ApplyDropPedal();
			m_callbacks.bufferSwitch(m_index, 1);
			if (m_claimed)
			{
				Store(&m_shared->outputSlotSeq[slot], 0);
				for (long ch = 0; ch < kChannels; ++ch) memset(m_shared->output[slot][ch], 0, static_cast<size_t>(m_frames) * sizeof(int32_t));
				for (const auto& info : m_infos)
				{
					if (info.isInput) continue;
					memcpy(m_shared->output[slot][info.channelNum], info.buffers[m_index], static_cast<size_t>(m_frames) * sizeof(int32_t));
				}
				Store(&m_shared->outputSlotSeq[slot], seq);
				Store(&m_shared->guestLastSeq, seq);
			}
			m_index ^= 1;
			m_samplePosition.fetch_add(static_cast<uint64_t>(m_frames), std::memory_order_release);
		}

		// The Drop Pedal shifts the game's copy of the guitar inside the game (AsioHook), after the host published
		// the DI to this link, so the amp sim would play it unshifted. The same shifter runs here on each input the
		// amp sim uses, at the pedal's semitones. At 0 it only keeps its ring warm and the samples pass
		// through untouched, exactly like the game's.
		void ApplyDropPedal()
		{
			const int semitones = static_cast<int>(Load(&m_shared->pitchSemitones));
			if (semitones != m_lastSemitones)
			{
				m_lastSemitones = semitones;
				for (auto& shifter : m_shifters) shifter.SetSemitones(semitones);
			}
			if (m_shiftScratch.size() < static_cast<size_t>(m_frames)) return;
			constexpr float kToFloat = 1.0f / 2147483648.0f;
			float* scratch = m_shiftScratch.data();
			for (const auto& info : m_infos)
			{
				if (!info.isInput) continue;
				auto* samples = static_cast<int32_t*>(info.buffers[m_index]);
				for (long i = 0; i < m_frames; ++i) scratch[i] = static_cast<float>(samples[i]) * kToFloat;
				if (!m_shifters[info.channelNum].Process(scratch, static_cast<uint32_t>(m_frames))) continue;
				for (long i = 0; i < m_frames; ++i)
				{
					const float v = (std::min)((std::max)(scratch[i], -1.0f), 1.0f) * 2147483520.0f;   // largest float below 2^31
					samples[i] = static_cast<int32_t>(v);
				}
			}
		}

		// One block on the silent clock (no game): silence in, output discarded.
		void ProcessSilent()
		{
			for (const auto& info : m_infos)
				if (info.isInput) memset(info.buffers[m_index], 0, static_cast<size_t>(m_frames) * sizeof(int32_t));
			m_callbacks.bufferSwitch(m_index, 1);
			m_index ^= 1;
			m_samplePosition.fetch_add(static_cast<uint64_t>(m_frames), std::memory_order_release);
		}

		void Run()
		{
			DWORD taskIndex = 0;
			HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
			if (!task) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
			LARGE_INTEGER frequency{}, now{};
			QueryPerformanceFrequency(&frequency);
			QueryPerformanceCounter(&now);
			const int64_t period = frequency.QuadPart * m_frames / kSampleRate;
			int64_t deadline = now.QuadPart;
			ULONGLONG nextOpenTry = 0, nextClaimTry = 0;
			bool loggedBusy = false;
			LONG lastSeq = m_shared ? Load(&m_shared->inputSeq) : 0;
			bool linked = false;
			GuestLog("stream started: %ld frames", m_frames);

			while (m_running)
			{
				if (linked)
				{
					// On the game's clock: one wake per game buffer. A quiet tenth of a second means the game stopped.
					if (WaitForSingleObject(m_event, 100) != WAIT_OBJECT_0)
					{
						linked = false;
						GuestLog("game stopped sending; silent clock");
						QueryPerformanceCounter(&now);
						deadline = now.QuadPart;
						continue;
					}
					const LONG hostFrames = Load(&m_shared->hostFrames);
					if (hostFrames != m_frames) { linked = false; RequestReset(hostFrames); continue; }
					// Catch up on every block since the last wake, oldest first: a game buffer can arrive right
					// behind the previous one (driver or scheduler jitter), and processing only the newest would drop
					// the other. More than the slots hold is skipped.
					const LONG seq = Load(&m_shared->inputSeq);
					const LONG behind = seq - lastSeq;
					if (behind <= 0) continue;
					if (behind > kSlots - 1) lastSeq = seq - (kSlots - 1);
					while (lastSeq != seq && m_running) ProcessLinked(++lastSeq);
					continue;
				}

				// Silent clock: keeps the amp sim's stream alive while there is no game to follow.
				ProcessSilent();
				const ULONGLONG tick = GetTickCount64();
				if (!m_shared && tick >= nextOpenTry) { TryOpenLink(); nextOpenTry = tick + 500; if (m_shared) lastSeq = Load(&m_shared->inputSeq); }
				if (m_shared)
				{
					const LONG hostFrames = Load(&m_shared->hostFrames);
					const LONG seq = Load(&m_shared->inputSeq);
					if (hostFrames > 0 && hostFrames != m_frames)
					{
						// Claim anyway (throttled), so the overlay can name this program and say which size it runs.
						if (tick >= nextClaimTry) { nextClaimTry = tick + 500; TryClaim(); }
						RequestReset(hostFrames);
					}
					else if (hostFrames == m_frames && seq != lastSeq)
					{
						// The game is producing blocks at our size: follow its clock from the next one. Only the
						// guest that owns the link waits on the game's event (it is auto-reset, one waiter); a second
						// program on this driver stays on the silent clock, retrying twice a second.
						lastSeq = seq;
						if (tick >= nextClaimTry)
						{
							nextClaimTry = tick + 500;
							if (TryClaim())
							{
								GuestLog("linked to the game at %ld frames", m_frames);
								linked = true;
								continue;
							}
							if (!loggedBusy) { loggedBusy = true; GuestLog("another program already uses the link; this one gets silence"); }
						}
					}
				}
				deadline += period;
				QueryPerformanceCounter(&now);
				if (now.QuadPart - deadline > period * 8) deadline = now.QuadPart;   // stalled: resync, do not burst
				const int64_t remaining = deadline - now.QuadPart;
				if (remaining <= 0) continue;
				if (timer)
				{
					LARGE_INTEGER due{};
					due.QuadPart = -(std::max<int64_t>)(1, remaining * 10000000 / frequency.QuadPart);
					if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) { WaitForSingleObject(timer, 1000); continue; }
				}
				Sleep(1);
			}
			if (timer) CloseHandle(timer);
			if (task) AvRevertMmThreadCharacteristics(task);
			GuestLog("stream stopped");
		}

		const CLSID m_clsid;
		std::atomic<ULONG> m_ref{ 1 };
		std::string m_error;
		ASIOCallbacks m_callbacks{};
		std::vector<ASIOBufferInfo> m_infos;
		std::vector<void*> m_buffers;
		long m_frames = 0;
		long m_index = 0;
		std::atomic<uint64_t> m_samplePosition{ 0 };
		std::thread m_thread;
		std::atomic<bool> m_running{ false };
		bool m_claimed = false;
		bool m_resetRequested = false;
		std::mutex m_linkMutex;
		HANDLE m_mapping = nullptr;
		HANDLE m_event = nullptr;
		Shared* m_shared = nullptr;
		std::array<Audio::DelayLinePitchShifter, kChannels> m_shifters{ Audio::DelayLinePitchShifter(0), Audio::DelayLinePitchShifter(0) };
		std::vector<float> m_shiftScratch;
		int m_lastSemitones = 0;
	};

	IAsioDriver* CreateGuestDriver(const CLSID& clsid)
	{
		return new (std::nothrow) GuestDriver(clsid);
	}
}
