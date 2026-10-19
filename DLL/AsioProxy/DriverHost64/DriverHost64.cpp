// RocksmithAudioBridgeDriverHost.exe: the 64-bit side of 64-bit ASIO driver support (see ../Remote64.hpp).
//
// Started by the bridge's stand-in driver inside Rocksmith with the driver to load and the shared block to use:
//   RocksmithAudioBridgeDriverHost.exe --parent <pid> --mapping <name> --clsid <{...}> --dll <path>
// It loads the real 64-bit ASIO driver, answers the stand-in's setup calls on its main thread (an ASIO driver expects
// one thread with a message loop, the one that called init), and copies audio between the driver's buffers and the
// shared ones every block. It exits when the stand-in asks it to or the game is gone.
#include "../AsioInterface.h"
#include "../Remote64.hpp"
#include "../../Shared/ProductVersion.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
	constexpr ASIOError kOk = 0;
	constexpr ASIOError kNotPresent = -1000;
	constexpr ASIOError kInvalidParameter = -998;
	// asioMessage selectors (Steinberg SDK values).
	constexpr long kSelectorSupported = 1, kEngineVersion = 2, kResetRequest = 3, kBufferSizeChange = 4, kResyncRequest = 5,
		kLatenciesChanged = 6, kSupportsTimeInfo = 7;

	std::wstring ExeDir()
	{
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		std::wstring p = path;
		const size_t slash = p.find_last_of(L"\\/");
		return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
	}

	// Its own log file next to the exe: this process cannot write through the mod's RSMods_debug.txt.
	std::mutex g_logMutex;
	void HostLog(const char* format, ...)
	{
		static const auto start = std::chrono::steady_clock::now();
		static bool truncated = false;
		std::lock_guard<std::mutex> guard(g_logMutex);
		FILE* file = nullptr;
		if (_wfopen_s(&file, (ExeDir() + L"\\RocksmithAudioBridge-driver-host-log.txt").c_str(), truncated ? L"a" : L"w") != 0 || !file) return;
		if (!truncated) fprintf(file, "%s 64-bit ASIO driver host\n", ProductVersion::DISPLAY_NAME);
		truncated = true;
		fprintf(file, "%8.3f ", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
		va_list args;
		va_start(args, format);
		vfprintf(file, format, args);
		va_end(args);
		fputc('\n', file);
		fclose(file);
	}

	std::wstring Argument(const wchar_t* name)
	{
		int count = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
		std::wstring value;
		for (int i = 1; argv && i + 1 < count; ++i)
			if (_wcsicmp(argv[i], name) == 0) { value = argv[i + 1]; break; }
		if (argv) LocalFree(argv);
		return value;
	}

	Remote64::Shared* g_shared = nullptr;
	void* g_base = nullptr;
	IAsioDriver* g_driver = nullptr;
	ASIOBufferInfo g_infos[Remote64::kMaxChannels]{};
	LONG g_numChannels = 0, g_frames = 0;
	LONG g_seq = 0;                       // driver thread only
	bool g_outputReady = false;           // the driver wants outputReady() after each block
	std::atomic<bool> g_streaming{ false };
	HANDLE g_blockEvent = nullptr;          // signalled after each block (opened before the driver starts)

	void PostMessageToGame(LONG kind, LONG selector, LONG value, double rate)
	{
		Remote64::Shared& s = *g_shared;
		const LONG write = s.messageWrite;
		if (write - s.messageRead >= Remote64::kMessages) return;   // the game is not reading: drop rather than overwrite
		Remote64::Message& m = s.messages[write % Remote64::kMessages];
		m.kind = kind; m.selector = selector; m.value = value; m.rate = rate;
		InterlockedExchange(&s.messageWrite, write + 1);
	}

	// The real driver's thread, every block.
	void OnBufferSwitch(long index, ASIOBool)
	{
		if (!g_streaming.load(std::memory_order_acquire) || index < 0 || index > 1) return;
		Remote64::Shared& s = *g_shared;
		const LONG seq = ++g_seq;
		const size_t bytes = static_cast<size_t>(g_frames);
		// Output: what the game finished for the previous block, which it wrote into the other half. Not finished in
		// time: silence, counted (the game's half may still be half-written).
		const LONG previousHalf = index ^ 1;
		const bool ready = seq > 1 && s.outSeq[previousHalf] == seq - 1;
		for (LONG c = 0; c < g_numChannels; ++c)
		{
			if (g_infos[c].isInput || !g_infos[c].buffers[index]) continue;
			const size_t length = bytes * static_cast<size_t>(s.channels[c].bytes);
			if (ready) memcpy(g_infos[c].buffers[index], Remote64::Slot(g_base, c, previousHalf), length);
			else memset(g_infos[c].buffers[index], 0, length);
		}
		if (!ready && seq > 2) InterlockedIncrement(&s.missed);
		if (g_outputReady) g_driver->outputReady();
		// Input: this block's, into this half.
		for (LONG c = 0; c < g_numChannels; ++c)
		{
			if (!g_infos[c].isInput || !g_infos[c].buffers[index]) continue;
			memcpy(Remote64::Slot(g_base, c, index), g_infos[c].buffers[index], bytes * static_cast<size_t>(s.channels[c].bytes));
		}
		ASIOSamples position{}; ASIOTimeStamp stamp{};
		if (g_driver->getSamplePosition(&position, &stamp) == kOk)
		{
			s.samplePosition = (static_cast<LONGLONG>(position.hi) << 32) | position.lo;
			s.systemTime = (static_cast<LONGLONG>(stamp.hi) << 32) | stamp.lo;
		}
		s.blockIndex = index;
		InterlockedExchange(&s.blockSeq, seq);
		SetEvent(g_blockEvent);
	}

	ASIOTime* OnBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct) { OnBufferSwitch(index, direct); return params; }

	void OnSampleRateDidChange(ASIOSampleRate rate) { PostMessageToGame(2, 0, 0, rate); }

	long OnAsioMessage(long selector, long value, void*, double*)
	{
		switch (selector)
		{
		case kSelectorSupported:
			return value == kResetRequest || value == kResyncRequest || value == kLatenciesChanged || value == kEngineVersion ? 1 : 0;
		case kEngineVersion: return 2;
		case kSupportsTimeInfo: return 0;   // plain bufferSwitch: the sample position is read in it
		case kResetRequest: case kBufferSizeChange: case kResyncRequest: case kLatenciesChanged:
			HostLog("driver message %ld (%ld): passed to the game", selector, value);
			PostMessageToGame(1, selector, value, 0.0);
			return 1;
		default: return 0;
		}
	}

	ASIOCallbacks g_callbacks{ &OnBufferSwitch, &OnSampleRateDidChange, &OnAsioMessage, &OnBufferSwitchTimeInfo };

	bool LoadDriver(const std::wstring& dllPath, const CLSID& clsid, std::string& failure)
	{
		HMODULE module = LoadLibraryW(dllPath.c_str());
		if (!module) { failure = "its 64-bit driver file would not load, Windows error " + std::to_string(GetLastError()); return false; }
		using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
		auto getClassObject = reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject"));
		IClassFactory* factory = nullptr;
		if (!getClassObject || FAILED(getClassObject(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory))) || !factory)
		{
			failure = "its 64-bit driver file is not a working ASIO driver";
			return false;
		}
		const HRESULT created = factory->CreateInstance(nullptr, clsid, reinterpret_cast<void**>(&g_driver));
		factory->Release();
		if (FAILED(created) || !g_driver) { g_driver = nullptr; failure = "the 64-bit driver could not be created"; return false; }
		return true;
	}

	void DisposeBuffers()
	{
		g_streaming.store(false, std::memory_order_release);
		if (g_numChannels > 0 && g_driver) g_driver->disposeBuffers();
		g_numChannels = 0;
	}

	// One setup call from the stand-in, on the main thread.
	void Handle(HWND window, bool& quit, bool& openPanel)
	{
		Remote64::Control& c = g_shared->control;
		c.result = kNotPresent;
		if (!g_driver && c.op != Remote64::OpQuit) return;
		switch (c.op)
		{
		case Remote64::OpInit:
			c.result = g_driver->init(window);
			c.text[0] = 0;
			if (c.result) g_driver->getDriverName(c.text); else g_driver->getErrorMessage(c.text);
			c.text[sizeof(c.text) - 1] = 0;
			c.version = g_driver->getDriverVersion();
			HostLog("init: %s (%s)", c.result ? "started" : "refused", c.text);
			break;
		case Remote64::OpGetChannels: { long in = 0, out = 0; c.result = g_driver->getChannels(&in, &out); c.out[0] = in; c.out[1] = out; break; }
		case Remote64::OpGetLatencies: { long in = 0, out = 0; c.result = g_driver->getLatencies(&in, &out); c.out[0] = in; c.out[1] = out; break; }
		case Remote64::OpGetBufferSize:
		{
			long mn = 0, mx = 0, pref = 0, gran = 0;
			c.result = g_driver->getBufferSize(&mn, &mx, &pref, &gran);
			c.out[0] = mn; c.out[1] = mx; c.out[2] = pref; c.out[3] = gran;
			break;
		}
		case Remote64::OpCanSampleRate: c.result = g_driver->canSampleRate(c.value); break;
		case Remote64::OpGetSampleRate: { double rate = 0.0; c.result = g_driver->getSampleRate(&rate); c.outValue = rate; break; }
		case Remote64::OpSetSampleRate: c.result = g_driver->setSampleRate(c.value); break;
		case Remote64::OpGetClockSources:
		{
			long count = (std::min)(static_cast<long>(c.arg[0]), static_cast<long>(sizeof(c.payload) / sizeof(ASIOClockSource)));
			c.result = g_driver->getClockSources(reinterpret_cast<ASIOClockSource*>(c.payload), &count);
			c.out[0] = count;
			break;
		}
		case Remote64::OpSetClockSource: c.result = g_driver->setClockSource(c.arg[0]); break;
		case Remote64::OpGetChannelInfo: c.result = g_driver->getChannelInfo(reinterpret_cast<ASIOChannelInfo*>(c.payload)); break;
		case Remote64::OpCreateBuffers:
		{
			DisposeBuffers();
			const LONG count = g_shared->numChannels, frames = g_shared->frames;
			if (count <= 0 || count > Remote64::kMaxChannels || frames <= 0 || frames > Remote64::kMaxFrames) { c.result = kInvalidParameter; break; }
			for (LONG i = 0; i < count; ++i)
			{
				g_infos[i] = ASIOBufferInfo{};
				g_infos[i].isInput = g_shared->channels[i].isInput;
				g_infos[i].channelNum = g_shared->channels[i].channelNum;
			}
			g_frames = frames;
			g_seq = 0;
			g_shared->outSeq[0] = g_shared->outSeq[1] = 0;
			g_shared->blockSeq = 0;
			g_shared->missed = 0;
			c.result = g_driver->createBuffers(g_infos, count, frames, &g_callbacks);
			if (c.result == kOk)
			{
				g_numChannels = count;
				g_outputReady = g_driver->outputReady() == kOk;
				c.out[0] = g_outputReady ? 1 : 0;
				g_streaming.store(true, std::memory_order_release);
			}
			HostLog("createBuffers: %ld channels, %ld frames -> %ld%s", count, frames, c.result, g_outputReady ? " (outputReady)" : "");
			break;
		}
		case Remote64::OpDisposeBuffers: DisposeBuffers(); c.result = kOk; break;
		case Remote64::OpStart: c.result = g_driver->start(); HostLog("start -> %ld", c.result); break;
		case Remote64::OpStop: c.result = g_driver->stop(); HostLog("stop -> %ld (missed blocks %ld)", c.result, g_shared->missed); break;
		case Remote64::OpControlPanel: c.result = kOk; openPanel = true; break;
		case Remote64::OpFuture: c.result = g_driver->future(c.arg[0], nullptr); break;
		case Remote64::OpQuit: c.result = kOk; quit = true; break;
		default: break;
		}
	}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	const std::wstring mappingName = Argument(L"--mapping"), dllPath = Argument(L"--dll"), clsidText = Argument(L"--clsid");
	const DWORD parentPid = static_cast<DWORD>(_wtol(Argument(L"--parent").c_str()));
	HostLog("starting: %ls", dllPath.c_str());
	HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
	HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str());
	HANDLE request = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (mappingName + Remote64::kRequestSuffix).c_str());
	HANDLE reply = OpenEventW(EVENT_MODIFY_STATE, FALSE, (mappingName + Remote64::kReplySuffix).c_str());
	g_blockEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, (mappingName + Remote64::kBlockSuffix).c_str());
	if (!parent || !mapping || !request || !reply || !g_blockEvent) { HostLog("cannot reach the game (parent %p, mapping %p)", parent, mapping); return 1; }
	g_base = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, Remote64::kMappingBytes);
	g_shared = static_cast<Remote64::Shared*>(g_base);
	if (!g_shared || g_shared->magic != Remote64::kMagic || g_shared->version != Remote64::kVersion) { HostLog("shared block does not match this build"); return 1; }
	g_shared->hostPid = static_cast<LONG>(GetCurrentProcessId());

	// An ASIO driver expects to be initialised and called from one thread with a message loop, with a window to post to.
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	WNDCLASSW windowClass{};
	windowClass.lpfnWndProc = DefWindowProcW;
	windowClass.hInstance = instance;
	windowClass.lpszClassName = L"RocksmithAudioBridgeDriverHost";
	RegisterClassW(&windowClass);
	HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"Rocksmith Audio Bridge driver host", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);

	CLSID clsid{};
	std::string failure;
	if (FAILED(CLSIDFromString(clsidText.c_str(), &clsid))) failure = "the driver's CLSID is not valid";
	else LoadDriver(dllPath, clsid, failure);
	if (!failure.empty())
	{
		strncpy_s(g_shared->control.text, failure.c_str(), _TRUNCATE);
		HostLog("load failed: %s", failure.c_str());
	}
	InterlockedExchange(&g_shared->hostReady, failure.empty() ? 1 : 2);

	bool quit = false;
	HANDLE waits[2] = { request, parent };
	while (!quit)
	{
		const DWORD woke = MsgWaitForMultipleObjects(2, waits, FALSE, INFINITE, QS_ALLINPUT);
		if (woke == WAIT_OBJECT_0)
		{
			bool openPanel = false;
			Handle(window, quit, openPanel);
			SetEvent(reply);
			if (openPanel && g_driver) g_driver->controlPanel();   // after the reply: some panels are modal
		}
		else if (woke == WAIT_OBJECT_0 + 1) { HostLog("the game is gone"); break; }
		else if (woke == WAIT_OBJECT_0 + 2)
		{
			MSG message;
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
		}
		else break;
	}
	if (g_driver)
	{
		g_streaming.store(false, std::memory_order_release);
		g_driver->stop();
		DisposeBuffers();
		g_driver->Release();
	}
	HostLog("exiting");
	return 0;
}
