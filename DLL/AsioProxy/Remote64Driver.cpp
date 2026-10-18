#include "Remote64Driver.hpp"
#include "Remote64.hpp"
#include <avrt.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "avrt.lib")

namespace Remote64
{
	namespace
	{
		constexpr ASIOError kOk = 0;
		constexpr ASIOError kNotPresent = -1000;
		constexpr ASIOError kHWMalfunction = -999;
		constexpr ASIOError kInvalidParameter = -998;
		constexpr long kResetRequest = 3;

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

		// Software\ASIO\<name> (machine, else user: the bridge's resolver reads both) in one registry view, and its CLSID's
		// InprocServer32 in the same view.
		bool ResolveIn(REGSAM view, const std::wstring& name, std::wstring& dllPath, CLSID& clsid)
		{
			HKEY key = nullptr;
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (L"SOFTWARE\\ASIO\\" + name).c_str(), 0, KEY_READ | view, &key) != ERROR_SUCCESS
				&& RegOpenKeyExW(HKEY_CURRENT_USER, (L"Software\\ASIO\\" + name).c_str(), 0, KEY_READ | view, &key) != ERROR_SUCCESS) return false;
			wchar_t clsidText[64]{};
			DWORD size = sizeof(clsidText);
			const LSTATUS read = RegQueryValueExW(key, L"CLSID", nullptr, nullptr, reinterpret_cast<BYTE*>(clsidText), &size);
			RegCloseKey(key);
			if (read != ERROR_SUCCESS || FAILED(CLSIDFromString(clsidText, &clsid))) return false;
			HKEY dllKey = nullptr;
			if (RegOpenKeyExW(HKEY_CLASSES_ROOT, (std::wstring(L"CLSID\\") + clsidText + L"\\InprocServer32").c_str(), 0, KEY_READ | view, &dllKey) != ERROR_SUCCESS) return false;
			wchar_t path[MAX_PATH]{};
			size = sizeof(path);
			const LSTATUS pathRead = RegQueryValueExW(dllKey, nullptr, nullptr, nullptr, reinterpret_cast<BYTE*>(path), &size);
			RegCloseKey(dllKey);
			if (pathRead != ERROR_SUCCESS || !path[0]) return false;
			dllPath = path;
			return true;
		}

		std::atomic<LONG> g_instance{ 0 };
	}

	bool Resolve(const std::wstring& name, std::wstring& dllPath, CLSID& clsid) { return ResolveIn(KEY_WOW64_64KEY, name, dllPath, clsid); }

	bool Prefer64()
	{
		DWORD value = 0, size = sizeof(value);
		return RegGetValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", L"Prefer64BitDrivers", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value == 1;
	}

	bool IsOnly64(const std::wstring& name)
	{
		std::wstring path; CLSID clsid{};
		return !ResolveIn(KEY_WOW64_32KEY, name, path, clsid) && ResolveIn(KEY_WOW64_64KEY, name, path, clsid);
	}

	class Remote64Driver final : public IAsioDriver
	{
	public:
		Remote64Driver(const std::wstring& name, const CLSID& clsid, LogFn log) : m_name(name), m_clsid(clsid), m_log(log) {}

		~Remote64Driver()
		{
			StopThread();
			if (m_shared && !m_dead.load()) Call(OpQuit, 2000);
			if (m_process)
			{
				if (WaitForSingleObject(m_process, 3000) == WAIT_TIMEOUT) TerminateProcess(m_process, 1);
				CloseHandle(m_process);
			}
			if (m_base) UnmapViewOfFile(m_base);
			for (HANDLE h : { m_mapping, m_request, m_reply, m_block, m_stop }) if (h) CloseHandle(h);
		}

		bool Start(const std::wstring& dllPath, std::string& failure)
		{
			const std::wstring exe = ModuleDir() + L"\\" + kHostExe;
			if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				failure = "it needs RocksmithAudioBridgeDriverHost.exe, which is missing from the game folder (run the installer's Repair)";
				return false;
			}
			const std::wstring base = std::wstring(kNamePrefix) + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++g_instance);
			m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kMappingBytes, base.c_str());
			m_request = CreateEventW(nullptr, FALSE, FALSE, (base + kRequestSuffix).c_str());
			m_reply = CreateEventW(nullptr, FALSE, FALSE, (base + kReplySuffix).c_str());
			m_block = CreateEventW(nullptr, FALSE, FALSE, (base + kBlockSuffix).c_str());
			m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!m_mapping || !m_request || !m_reply || !m_block || !m_stop) { failure = "the link to its 64-bit host could not be set up"; return false; }
			m_base = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, kMappingBytes);
			if (!m_base) { failure = "the link to its 64-bit host could not be set up"; return false; }
			m_shared = static_cast<Shared*>(m_base);
			memset(m_shared, 0, sizeof(Shared));
			m_shared->magic = kMagic;
			m_shared->version = kVersion;
			m_shared->gamePid = static_cast<LONG>(GetCurrentProcessId());

			wchar_t clsidText[64]{};
			StringFromGUID2(m_clsid, clsidText, 64);
			std::wstring command = L"\"" + exe + L"\" --parent " + std::to_wstring(GetCurrentProcessId()) + L" --mapping \"" + base
				+ L"\" --clsid " + clsidText + L" --dll \"" + dllPath + L"\"";
			STARTUPINFOW startup{ sizeof(startup) };
			PROCESS_INFORMATION info{};
			if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, ModuleDir().c_str(), &startup, &info))
			{
				failure = "its 64-bit host would not start, Windows error " + std::to_string(GetLastError()) + " (an antivirus may be blocking RocksmithAudioBridgeDriverHost.exe)";
				return false;
			}
			CloseHandle(info.hThread);
			m_process = info.hProcess;
			// The host loads the driver's DLL before it answers anything.
			for (int waited = 0; m_shared->hostReady == 0; waited += 20)
			{
				if (waited >= 10000 || WaitForSingleObject(m_process, 20) == WAIT_OBJECT_0)
				{
					failure = "its 64-bit host stopped while starting (an antivirus may have closed RocksmithAudioBridgeDriverHost.exe)";
					m_dead = true;
					return false;
				}
			}
			if (m_shared->hostReady != 1)
			{
				failure = m_shared->control.text;
				m_dead = true;
				return false;
			}
			if (m_log) m_log("Remote64: '%ls' runs in the 64-bit host (pid %ld) from '%ls'", m_name.c_str(), m_shared->hostPid, dllPath.c_str());
			return true;
		}

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
			// The game's window is not passed on: the driver lives in the host, which gives it a window of its own.
			const ASIOError result = Call(OpInit, 15000);
			std::lock_guard<std::mutex> guard(m_callMutex);
			if (m_dead.load()) { m_error = "The 64-bit driver host stopped."; return 0; }
			if (result) m_driverName = m_shared->control.text;
			else m_error = m_shared->control.text;
			m_version = m_shared->control.version;
			return result;
		}
		void getDriverName(char* name) override { if (name) strncpy_s(name, 32, m_driverName.empty() ? "64-bit ASIO" : m_driverName.c_str(), _TRUNCATE); }
		long getDriverVersion() override { return m_version; }
		void getErrorMessage(char* text) override { if (text) strncpy_s(text, 124, m_error.c_str(), _TRUNCATE); }
		ASIOError start() override { return Call(OpStart, 10000); }
		ASIOError stop() override { return Call(OpStop, 5000); }
		ASIOError getChannels(long* inputs, long* outputs) override
		{
			const ASIOError e = Call(OpGetChannels);
			if (inputs) *inputs = m_shared->control.out[0];
			if (outputs) *outputs = m_shared->control.out[1];
			return e;
		}
		ASIOError getLatencies(long* input, long* output) override
		{
			// Each block plays one buffer later than the driver itself would (see Remote64.hpp), so the output says so.
			const ASIOError e = Call(OpGetLatencies);
			if (input) *input = m_shared->control.out[0];
			if (output) *output = m_shared->control.out[1] + (m_frames > 0 ? m_frames : 0);
			return e;
		}
		ASIOError getBufferSize(long* minSize, long* maxSize, long* preferred, long* granularity) override
		{
			const ASIOError e = Call(OpGetBufferSize);
			// Capped at what the shared block carries.
			auto cap = [](long v) { return v > kMaxFrames ? kMaxFrames : v; };
			if (minSize) *minSize = cap(m_shared->control.out[0]);
			if (maxSize) *maxSize = cap(m_shared->control.out[1]);
			if (preferred) *preferred = cap(m_shared->control.out[2]);
			if (granularity) *granularity = m_shared->control.out[3];
			return e;
		}
		ASIOError canSampleRate(ASIOSampleRate rate) override { return CallValue(OpCanSampleRate, rate); }
		ASIOError getSampleRate(ASIOSampleRate* rate) override
		{
			const ASIOError e = Call(OpGetSampleRate);
			if (rate) *rate = m_shared->control.outValue;
			return e;
		}
		ASIOError setSampleRate(ASIOSampleRate rate) override { return CallValue(OpSetSampleRate, rate); }
		ASIOError getClockSources(ASIOClockSource* clocks, long* count) override
		{
			if (!clocks || !count) return kInvalidParameter;
			std::lock_guard<std::mutex> guard(m_callMutex);
			m_shared->control.arg[0] = *count;
			const ASIOError e = CallLocked(OpGetClockSources, 2000);
			const long got = (std::min)(*count, static_cast<long>(m_shared->control.out[0]));
			if (e == kOk && got > 0) memcpy(clocks, m_shared->control.payload, sizeof(ASIOClockSource) * static_cast<size_t>(got));
			*count = got;
			return e;
		}
		ASIOError setClockSource(long reference) override
		{
			std::lock_guard<std::mutex> guard(m_callMutex);
			m_shared->control.arg[0] = reference;
			return CallLocked(OpSetClockSource, 5000);
		}
		ASIOError getSamplePosition(ASIOSamples* position, ASIOTimeStamp* stamp) override
		{
			if (!m_streaming.load()) return kNotPresent;
			const LONGLONG samples = m_shared->samplePosition, time = m_shared->systemTime;
			if (position) { position->hi = static_cast<long>(samples >> 32); position->lo = static_cast<unsigned long>(samples & 0xFFFFFFFF); }
			if (stamp) { stamp->hi = static_cast<long>(time >> 32); stamp->lo = static_cast<unsigned long>(time & 0xFFFFFFFF); }
			return kOk;
		}
		ASIOError getChannelInfo(ASIOChannelInfo* info) override
		{
			if (!info) return kInvalidParameter;
			std::lock_guard<std::mutex> guard(m_callMutex);
			memcpy(m_shared->control.payload, info, sizeof(ASIOChannelInfo));
			const ASIOError e = CallLocked(OpGetChannelInfo, 2000);
			if (e == kOk) memcpy(info, m_shared->control.payload, sizeof(ASIOChannelInfo));
			return e;
		}
		ASIOError createBuffers(ASIOBufferInfo* infos, long count, long frames, ASIOCallbacks* callbacks) override
		{
			if (!infos || !callbacks || count <= 0 || count > kMaxChannels || frames <= 0 || frames > kMaxFrames) return kInvalidParameter;
			StopThread();
			// Each channel's sample type sizes its shared buffers; the bridge converts formats itself, so they stay native.
			for (long i = 0; i < count; ++i)
			{
				ASIOChannelInfo info{};
				info.channel = infos[i].channelNum;
				info.isInput = infos[i].isInput;
				if (getChannelInfo(&info) != kOk) return kInvalidParameter;
				const LONG bytes = SampleBytes(info.type);
				if (bytes == 0) return kInvalidParameter;
				m_shared->channels[i] = Channel{ infos[i].isInput, infos[i].channelNum, info.type, bytes };
			}
			std::lock_guard<std::mutex> guard(m_callMutex);
			m_shared->numChannels = count;
			m_shared->frames = frames;
			memset(Slot(m_base, 0, 0), 0, static_cast<size_t>(count) * 2 * static_cast<size_t>(kSlotBytes));
			const ASIOError e = CallLocked(OpCreateBuffers, 10000);
			if (e != kOk) return e;
			for (long i = 0; i < count; ++i)
				for (LONG half = 0; half < 2; ++half) infos[i].buffers[half] = Slot(m_base, i, half);
			m_callbacks = *callbacks;
			m_frames = frames;
			m_shared->messageRead = m_shared->messageWrite;   // nothing from before these buffers is for them
			m_streaming = true;
			ResetEvent(m_stop);
			m_thread = std::thread([this] { Run(); });
			return kOk;
		}
		ASIOError disposeBuffers() override
		{
			StopThread();
			m_streaming = false;
			m_frames = 0;
			return Call(OpDisposeBuffers, 5000);
		}
		ASIOError controlPanel() override { return Call(OpControlPanel); }
		ASIOError future(long selector, void* opt) override
		{
			if (opt) return kNotPresent;   // only selectors that carry no data cross to the host
			std::lock_guard<std::mutex> guard(m_callMutex);
			m_shared->control.arg[0] = selector;
			return CallLocked(OpFuture, 2000);
		}
		ASIOError outputReady() override { return kNotPresent; }   // the host calls the driver's own after each block

	private:
		ASIOError Call(LONG op, DWORD timeoutMs = 2000)
		{
			std::lock_guard<std::mutex> guard(m_callMutex);
			return CallLocked(op, timeoutMs);
		}
		ASIOError CallValue(LONG op, double value)
		{
			std::lock_guard<std::mutex> guard(m_callMutex);
			m_shared->control.value = value;
			return CallLocked(op, 5000);
		}
		// One request, waiting for the reply or the host's end. A host that does not answer in time is ended, so a later
		// reset request starts a fresh one instead of every call hanging on it.
		ASIOError CallLocked(LONG op, DWORD timeoutMs)
		{
			if (m_dead.load() || !m_shared) return kHWMalfunction;
			m_shared->control.op = op;
			SetEvent(m_request);
			HANDLE waits[2] = { m_reply, m_process };
			const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
			if (woke == WAIT_OBJECT_0) return m_shared->control.result;
			m_dead = true;
			if (woke == WAIT_TIMEOUT) TerminateProcess(m_process, 1);
			if (m_log) m_log("Remote64: '%ls' host %s during request %ld", m_name.c_str(), woke == WAIT_TIMEOUT ? "did not answer (ended)" : "stopped", op);
			return kHWMalfunction;
		}

		void StopThread()
		{
			if (!m_thread.joinable()) return;
			SetEvent(m_stop);
			if (m_thread.get_id() == std::this_thread::get_id()) m_thread.detach();
			else m_thread.join();
		}

		// The game side of every block: runs the bridge's bufferSwitch on the half the host just filled.
		void Run()
		{
			DWORD task = 0;
			HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			LONG last = 0;
			HANDLE waits[3] = { m_block, m_stop, m_process };
			for (;;)
			{
				const DWORD woke = WaitForMultipleObjects(3, waits, FALSE, 200);
				if (woke == WAIT_OBJECT_0 + 1) break;
				if (woke == WAIT_OBJECT_0 + 2)
				{
					// The host is gone (crashed, or closed by an antivirus). A reset request makes the bridge reopen this
					// interface, which starts a new host (ScheduleResetRebind).
					m_dead = true;
					if (m_log) m_log("Remote64: '%ls' host stopped while playing; asking the bridge to reopen it", m_name.c_str());
					if (m_callbacks.asioMessage) m_callbacks.asioMessage(kResetRequest, 0, nullptr, nullptr);
					break;
				}
				if (woke == WAIT_OBJECT_0)
				{
					LONG seq = m_shared->blockSeq, half = m_shared->blockIndex;
					if (m_shared->blockSeq != seq) { seq = m_shared->blockSeq; half = m_shared->blockIndex; }
					if (seq != last && (half == 0 || half == 1))
					{
						last = seq;
						if (m_callbacks.bufferSwitch) m_callbacks.bufferSwitch(half, 1);
						InterlockedExchange(&m_shared->outSeq[half], seq);
					}
				}
				while (m_shared->messageRead != m_shared->messageWrite)
				{
					const Message message = m_shared->messages[m_shared->messageRead % kMessages];
					InterlockedIncrement(&m_shared->messageRead);
					if (message.kind == 1 && m_callbacks.asioMessage) m_callbacks.asioMessage(message.selector, message.value, nullptr, nullptr);
					else if (message.kind == 2 && m_callbacks.sampleRateDidChange) m_callbacks.sampleRateDidChange(message.rate);
				}
			}
			if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
		}

		std::atomic<ULONG> m_ref{ 1 };
		std::wstring m_name;
		CLSID m_clsid;
		LogFn m_log;
		std::mutex m_callMutex;
		std::atomic<bool> m_dead{ false };
		std::atomic<bool> m_streaming{ false };
		HANDLE m_mapping = nullptr, m_request = nullptr, m_reply = nullptr, m_block = nullptr, m_stop = nullptr, m_process = nullptr;
		void* m_base = nullptr;
		Shared* m_shared = nullptr;
		ASIOCallbacks m_callbacks{};
		long m_frames = 0;
		std::thread m_thread;
		std::string m_driverName, m_error;
		long m_version = 0;
	};

	IAsioDriver* Create(const std::wstring& name, const std::wstring& dllPath, const CLSID& clsid, LogFn log, std::string& failure)
	{
		auto* driver = new Remote64Driver(name, clsid, log);
		if (driver->Start(dllPath, failure)) return driver;
		driver->Release();
		return nullptr;
	}
}
