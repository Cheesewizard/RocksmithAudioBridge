// Rocksmith Audio Bridge ASIO - the proxy ASIO driver of Rocksmith Audio Bridge.
//
// It presents itself to RS_ASIO as an ASIO driver named "Rocksmith Audio Bridge ASIO", loads the user's
// real ASIO driver (the one they were using), and forwards every IAsioDriver call to it unchanged -
// so playback is still true ASIO with negligible added latency. In the buffer callback it copies the
// output channels to a sink (the RSModsPlus host records the wet mix from there). This is the only
// place the game's post-mix audio exists in a grabbable form on an ASIO setup, and sitting here means
// the game's audio-device detection never sees a wrapper.
//
// Which real driver to load is read from HKCU\Software\RSMods\AsioProxy\Target (set at setup time,
// alongside pointing RS_ASIO.ini's Driver= at this proxy). The user's real driver stays registered
// and untouched; the proxy only wraps it.

#include "AsioInterface.h"
#include "LatencyDsp.h"
#include "LimiterDsp.h"
#include "OutputGuardDsp.h"
#include "MeterDsp.h"
#include "VirtualCableCapture.hpp"
#include "AmpLinkHost.hpp"
#include "AmpLinkGuest.hpp"
#include <string>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <vector>
#include <chrono>
#include <mutex>
#include <cstdarg>
#include <cstdio>
#include <avrt.h>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")

// {7B2E5C10-9F3A-4D6B-A1C8-2E4F6A8B0D31}
static const CLSID CLSID_RocksmithAudioBridge =
	{ 0x7b2e5c10, 0x9f3a, 0x4d6b, { 0xa1, 0xc8, 0x2e, 0x4f, 0x6a, 0x8b, 0x0d, 0x31 } };

static std::atomic<long> g_lockCount{ 0 };
static std::atomic<bool> g_virtualCaptureTestMode{ false };
static std::atomic<AsioInputObserver> g_inputObserver{ nullptr };

extern "C" __declspec(dllexport) void RSModsAsio_SetInputObserver(AsioInputObserver observer)
{
	g_inputObserver.store(observer, std::memory_order_release);
}

enum class OutputMode : int { None = 0, Virtual = 1, Real = 2 };

// The RSModsPlus host registers sinks to receive each output block (interleaved copy). More than one can be
// registered so the mix fans out to several consumers at once (the recorder, plus e.g. a virtual-device feed
// for OBS/a DAW) without disturbing the primary ASIO output. Slot 0 is the primary (SetSink); AddSink uses
// the rest. The block is built once per buffer and handed to every registered sink.
typedef void(__cdecl* OutputSink)(const void* interleaved, long frames, long channels, long asioSampleType, double sampleRate);
static const int kMaxSinks = 4;
static std::atomic<OutputSink> g_sinks[kMaxSinks];
extern "C" __declspec(dllexport) void RSModsAsio_SetSink(OutputSink sink) { g_sinks[0].store(sink); }
extern "C" __declspec(dllexport) int RSModsAsio_AddSink(OutputSink sink)
{
	if (!sink) return -1;
	for (int i = 1; i < kMaxSinks; ++i) { OutputSink expected = nullptr; if (g_sinks[i].compare_exchange_strong(expected, sink)) return i; }
	return -1;   // no free slot
}
extern "C" __declspec(dllexport) void RSModsAsio_RemoveSink(OutputSink sink)
{
	for (int i = 0; i < kMaxSinks; ++i) { OutputSink cur = g_sinks[i].load(); if (cur == sink) g_sinks[i].store(nullptr); }
}

// Redirect support: when the host routes the game mix to a different device (e.g. laptop speakers) it
// registers a sink that renders the tapped copy there, and mutes the forward so the real ASIO device
// (the one the game is bound to) falls silent instead of playing the same audio twice. The tap runs
// before the mute, so the sink still receives the real post-limit audio; only the buffers handed to the
// real driver are zeroed. Off by default: the forward is the player's normal ASIO output.
static std::atomic<int> g_forwardMuted{ 0 };
extern "C" __declspec(dllexport) void RSModsAsio_SetForwardMuted(int muted) { g_forwardMuted.store(muted ? 1 : 0); }

// Round-trip latency probe: when armed, the driver writes a known impulse train into the output for the
// next few buffers (replacing the game audio briefly). Paired with a capture of the loopback on the input,
// LatencyDsp::FindLag recovers the true round-trip delay. The reference is deterministic, so the capturing
// side regenerates it via LatencyDsp::GenerateProbe(RSModsAsio_ProbeLength()) rather than reading it back.
static const int kProbeLen = 256;
static float g_probe[kProbeLen];
static std::atomic<int> g_probeRemaining{ 0 };   // samples still to inject; >0 means armed
static int g_probePos = 0;                        // next probe sample to write (audio thread only)
extern "C" __declspec(dllexport) int RSModsAsio_ProbeLength() { return kProbeLen; }
extern "C" __declspec(dllexport) void RSModsAsio_ArmLatencyProbe()
{
	LatencyDsp::GenerateProbe(g_probe, kProbeLen);
	g_probePos = 0;
	g_probeRemaining.store(kProbeLen);
}

// Static output trim, applied to the forwarded output before the real driver plays it. The requested
// linear gain lives in an atomic (set from the control thread) and is copied onto the trim each buffer.
// A gain of 1.0 is passthrough (Active() false), so it costs nothing when off. This is a clean level
// control: it scales the waveform, it does not clip or compress, so it adds no distortion or latency.
// The export keeps its two-arg shape for ABI stability; the second slot (formerly make-up) is reserved.
static LimiterDsp::Trim g_trim;
static std::atomic<float> g_trimGain{ 1.0f };
extern "C" __declspec(dllexport) void RSModsAsio_ConfigureLimiter(float gainLinear, float /*reserved*/)
{
	g_trimGain.store(gainLinear);
}

// Output loudness guard: a real look-ahead brickwall limiter (holds a ceiling) plus a slow loudness
// AGC (equalises song-to-song). Its structural config (sample rate, channel count, look-ahead/release/
// AGC time constants) is set once on the control thread in createBuffers; only the live scalars below
// change while audio runs, pushed through atomics and applied per buffer without allocation. Both stages
// default off, so with the guard unconfigured the output path is unchanged (bit-for-bit passthrough).
static OutputGuardDsp::OutputGuard g_guard;
static std::atomic<int>   g_guardLimiterOn{ 0 };
static std::atomic<float> g_guardCeiling{ 1.0f };   // linear peak ceiling in [0,1]
static std::atomic<int>   g_guardAgcOn{ 0 };
static std::atomic<float> g_guardTarget{ 0.10f };   // AGC target RMS
extern "C" __declspec(dllexport) void RSModsAsio_ConfigureOutputGuard(int limiterOn, float ceilingLin, int agcOn, float targetRms)
{
	g_guardLimiterOn.store(limiterOn ? 1 : 0);
	g_guardCeiling.store(ceilingLin);
	g_guardAgcOn.store(agcOn ? 1 : 0);
	g_guardTarget.store(targetRms);
}

// Output metering: when enabled, the driver reports each output channel's peak-hold and RMS off the same
// post-limit output the player hears. Levels are published to atomics so the control thread can read them
// without locking. Off by default (no per-sample metering cost).
static const int kMeterMaxCh = 32;
static std::atomic<int> g_meterEnabled{ 0 };
static std::atomic<int> g_meterChannels{ 0 };
static MeterDsp::ChannelMeter g_meter[kMeterMaxCh];               // audio-thread state
static std::atomic<float> g_meterPeak[kMeterMaxCh];              // published peak-hold
static std::atomic<float> g_meterRms[kMeterMaxCh];              // published RMS
extern "C" __declspec(dllexport) void RSModsAsio_EnableMeter(int on) { g_meterEnabled.store(on ? 1 : 0); }
extern "C" __declspec(dllexport) int RSModsAsio_GetOutputLevels(float* peak, float* rms, int maxCh)
{
	const int n = g_meterChannels.load();
	const int count = (n < maxCh) ? n : maxCh;
	for (int i = 0; i < count; ++i)
	{
		if (peak) peak[i] = g_meterPeak[i].load(std::memory_order_relaxed);
		if (rms) rms[i] = g_meterRms[i].load(std::memory_order_relaxed);
	}
	return count;
}

// External amp link, host side (AmpLink.hpp): the clean guitar input goes out to an amp sim running on the
// guest driver, and the amp sim's output comes back into this mix. One per process, like s_active.
static AmpLink::Host g_ampLink;
extern "C" __declspec(dllexport) void RSModsAsio_ConfigureAmpLink(int mixOn, float returnGain)
{
	g_ampLink.Configure(mixOn != 0, returnGain);
}
extern "C" __declspec(dllexport) void RSModsAsio_SetAmpLinkLatency(int blocks)
{
	g_ampLink.SetLatencyBlocks(blocks);
}
// The Drop Pedal's input shift (semitones), applied by the guest to the amp sim's input. ExternalAmp.cpp sends
// it every tick; 0 = no shift.
extern "C" __declspec(dllexport) void RSModsAsio_SetAmpLinkPitch(int semitones)
{
	g_ampLink.SetPitchSemitones(semitones);
}
// Fills `status` (sizeof(AmpLink::Status) bytes, checked against `size`). Returns 1 when filled.
extern "C" __declspec(dllexport) int RSModsAsio_GetAmpLinkStatus(AmpLink::Status* status, int size)
{
	if (!status || size != static_cast<int>(sizeof(AmpLink::Status))) return 0;
	*status = g_ampLink.GetStatus();
	return 1;
}

namespace
{
	// The proxy's own test harnesses run beside a live game in the same session, so under the testing build
	// the link is opt-in; otherwise a harness would take over the game's link.
	bool AmpLinkHostEnabled()
	{
#ifdef RSMODS_ASIO_PROXY_TESTING
		wchar_t value[2]{};
		return GetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_AMPLINK", value, 2) > 0 && value[0] == L'1';
#else
		return true;
#endif
	}

	// Read our target real-driver name, then resolve + load it the same way RS_ASIO does.
	// The directory this proxy DLL was loaded from, i.e. the game folder (where RS_ASIO.ini lives). Resolved
	// from a function address so it needs no DllMain state.
	std::wstring ThisModuleDir()
	{
		HMODULE mod = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&ThisModuleDir), &mod) || !mod)
			return std::wstring();
		wchar_t path[MAX_PATH]{};
		if (GetModuleFileNameW(mod, path, MAX_PATH) == 0) return std::wstring();
		std::wstring p = path;
		const size_t slash = p.find_last_of(L"\\/");
		return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
	}

	// Proxy log: RocksmithAudioBridge-log.txt next to RS_ASIO-log.txt in the game folder, rewritten per
	// process. The proxy has no other voice (OutputDebugString is invisible in a normal session), and
	// RS_ASIO's log alone does not show whether the real ASIO device bound at boot or which step refused.
	// Every bind step logs its result here. Cheap: a few lines per stream open.
	std::mutex g_logMutex;
	void ProxyLog(const char* format, ...)
	{
		static const auto start = std::chrono::steady_clock::now();
		static bool truncated = false;
		std::lock_guard<std::mutex> guard(g_logMutex);
		const std::wstring dir = ThisModuleDir();
		if (dir.empty()) return;
		const std::wstring path = dir + L"\\RocksmithAudioBridge-log.txt";
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), truncated ? L"a" : L"w") != 0 || !file) return;
		truncated = true;
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		fprintf(file, "%8.3f [%5lu] ", seconds, GetCurrentThreadId());
		va_list args;
		va_start(args, format);
		vfprintf(file, format, args);
		va_end(args);
		fputc('\n', file);
		fclose(file);
	}

	std::wstring ReadTargetDriverName()
	{
#ifdef RSMODS_ASIO_PROXY_TESTING
		wchar_t testTarget[512]{};
		if (GetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_TARGET", testTarget, 512) > 0) return testTarget;
#endif
		HKEY key = nullptr;
		wchar_t buffer[512]{};
		DWORD size = sizeof(buffer);
		if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, KEY_READ, &key) == ERROR_SUCCESS)
		{
			RegQueryValueExW(key, L"Target", nullptr, nullptr, reinterpret_cast<BYTE*>(buffer), &size);
			RegCloseKey(key);
		}
		if (buffer[0] != L'\0') return buffer;
		// The wrapped driver name comes only from HKCU Target (set by the GUI's Install driver). An empty
		// Target means the GUI setup was never run.
		return std::wstring();
	}

	bool PreferRealOutput()
	{
#ifdef RSMODS_ASIO_PROXY_TESTING
		wchar_t testPreference[2]{};
		if (GetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_PREFER_REAL", testPreference, 2) > 0)
			return testPreference[0] == L'1';
#endif
		HKEY key = nullptr;
		DWORD value = 0;
		DWORD valueSize = sizeof(value);
		DWORD valueType = 0;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, KEY_READ, &key) != ERROR_SUCCESS)
			return false;
		const LSTATUS status = RegQueryValueExW(key, L"PreferReal", nullptr, &valueType,
			reinterpret_cast<BYTE*>(&value), &valueSize);
		RegCloseKey(key);
		return status == ERROR_SUCCESS && valueType == REG_DWORD && value == 1;
	}

	// Resolve an ASIO driver name to its DLL path + CLSID via HKLM/HKCU Software\ASIO\<name>.
	bool ResolveAsioDriver(const std::wstring& name, std::wstring& dllPath, CLSID& clsid)
	{
		for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
		{
			HKEY key = nullptr;
			const std::wstring sub = L"Software\\ASIO\\" + name;
			if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
			wchar_t clsidStr[64]{};
			DWORD size = sizeof(clsidStr);
			LSTATUS st = RegQueryValueExW(key, L"CLSID", nullptr, nullptr, reinterpret_cast<BYTE*>(clsidStr), &size);
			RegCloseKey(key);
			if (st != ERROR_SUCCESS || FAILED(CLSIDFromString(clsidStr, &clsid))) continue;

			wchar_t clsidKey[128]{};
			swprintf_s(clsidKey, L"CLSID\\%s\\InprocServer32", clsidStr);
			HKEY dllKey = nullptr;
			if (RegOpenKeyExW(HKEY_CLASSES_ROOT, clsidKey, 0, KEY_READ, &dllKey) != ERROR_SUCCESS) continue;
			wchar_t path[MAX_PATH]{};
			size = sizeof(path);
			st = RegQueryValueExW(dllKey, nullptr, nullptr, nullptr, reinterpret_cast<BYTE*>(path), &size);
			RegCloseKey(dllKey);
			if (st != ERROR_SUCCESS) continue;
			dllPath = path;
			return true;
		}
		return false;
	}
}

class AsioProxyDriver : public IAsioDriver
{
public:
	static AsioProxyDriver* s_active;   // ASIO callbacks carry no context; only one driver is active
	AsioProxyDriver() { m_virtualCable.SetTestMode(g_virtualCaptureTestMode.load(std::memory_order_acquire)); m_virtualCable.SetLog(&ProxyLog); }
	int OutputModeValue() const
	{
		if (!m_callbackSeen.load(std::memory_order_acquire)) return 0;
		const OutputMode mode = m_mode.load(std::memory_order_acquire);
		if (mode == OutputMode::Virtual) return 1;
		if (mode != OutputMode::Real) return 0;
		// Mode 3 (stalled) drives an automatic demotion to speaker mode, so it must mean "running but silent". After the
		// host's own stop() callbacks end by design; reporting that as a stall would demote a healthy interface.
		if (!m_streamRunning.load(std::memory_order_acquire)) return 2;
		const uint64_t lastCallback = m_lastCallbackTick.load(std::memory_order_acquire);
		return lastCallback != 0 && GetTickCount64() - lastCallback > 800 ? 3 : 2;
	}
	AsioProxy::VirtualCableCapture::Stats VirtualInputStats() const { return m_virtualCable.GetStats(); }
	int InputModeValue() const
	{
		if (!m_streamRunning.load(std::memory_order_acquire) || m_inputCount == 0) return 0;
		if (m_mode.load(std::memory_order_acquire) == OutputMode::Real) return 2;
		return m_virtualCable.HasPhysicalCapture() ? 2 : 1;
	}
	bool VirtualInputPhysical() const { return m_mode.load(std::memory_order_acquire) == OutputMode::Virtual && m_virtualCable.HasPhysicalCapture(); }
	void InjectVirtualInput(const int32_t* samples, uint32_t frames) { if (m_mode.load(std::memory_order_acquire) == OutputMode::Virtual) m_virtualCable.InjectForTest(samples, frames); }
	int TryPromote(const wchar_t* driverName)
	{
		return driverName && driverName[0] != L'\0' && TryPromoteToReal(driverName) ? 1 : 0;
	}
	int TryDemote() { return TryDemoteToVirtual() ? 1 : 0; }
	int TryRebind(const wchar_t* driverName)
	{
		return driverName && driverName[0] != L'\0' && TryRebindReal(driverName) ? 1 : 0;
	}
	~AsioProxyDriver()
	{
		StopVirtual();
		FreeVirtualBuffers();
		DisableRealCallbacksAndWait();   // a host that releases without stop/dispose must not race the bridge free
		FreeBridge();
		if (s_active == this) s_active = nullptr;
		if (m_real) m_real->Release();
		if (m_realModule) FreeLibrary(m_realModule);
	}

	// IUnknown - ASIO uses the driver CLSID as the interface IID.
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
	{
		if (!object) return E_POINTER;
		if (riid == IID_IUnknown || riid == CLSID_RocksmithAudioBridge) { *object = this; AddRef(); return S_OK; }
		*object = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
	ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --m_ref; if (!n) delete this; return n; }

	// IAsioDriver - forward everything to the real driver; tap output in the buffer callback.
	ASIOBool init(void* sysHandle) override
	{
		m_sysHandle = sysHandle;
		ActivateVirtual();
		ProxyLog("init: host handle %p; starting Virtual (real device binds at createBuffers when PreferReal=1)", sysHandle);
		return 1;
	}
	void getDriverName(char* name) override { if (name) strcpy_s(name, 32, "Rocksmith Audio Bridge ASIO"); }
	long getDriverVersion() override { return m_real ? m_real->getDriverVersion() : 1; }
	void getErrorMessage(char* string) override { if (m_real) m_real->getErrorMessage(string); else if (string) string[0] = 0; }
	ASIOError start() override
	{
		std::lock_guard<std::mutex> transition(m_transitionMutex);
		if (m_mode.load(std::memory_order_acquire) == OutputMode::Real) m_realCallbacksEnabled.store(true, std::memory_order_release);
		if (m_mode.load(std::memory_order_acquire) == OutputMode::Virtual) m_virtualCable.Start();
		const ASIOError result = m_mode.load() == OutputMode::Virtual ? StartVirtual() : m_real ? m_real->start() : -1;
		if (result != 0 && m_mode.load(std::memory_order_acquire) == OutputMode::Virtual) m_virtualCable.Stop();
		if (result == 0) { m_streamRunning.store(true, std::memory_order_release); g_ampLink.SetRunning(true); }
		ProxyLog("start: mode %s -> %ld", m_mode.load() == OutputMode::Virtual ? "Virtual" : "Real", result);
		return result;
	}
	ASIOError stop() override
	{
		std::lock_guard<std::mutex> transition(m_transitionMutex);
		m_streamRunning.store(false, std::memory_order_release);
		g_ampLink.SetRunning(false);
		if (m_mode.load() == OutputMode::Virtual)
		{
			StopVirtual();
			m_virtualCable.Stop();
			return 0;
		}
		DisableRealCallbacksAndWait();
		return m_real ? m_real->stop() : -1;
	}
	ASIOError getChannels(long* in, long* out) override { if (m_mode.load() == OutputMode::Virtual) { if (in) *in = 2; if (out) *out = 2; return 0; } return m_real ? m_real->getChannels(in, out) : -1; }
	ASIOError getLatencies(long* in, long* out) override { if (m_mode.load() == OutputMode::Virtual) { const long frames = m_bufferSize > 0 ? m_bufferSize : kVirtualPreferredFrames; if (in) *in = frames; if (out) *out = frames; return 0; } return m_real ? m_real->getLatencies(in, out) : -1; }
	ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override
	{
		if (m_mode.load() == OutputMode::Virtual)
		{
			// RS_ASIO picks the buffer size (driver preferred / host / custom, clamped to this range) BEFORE
			// createBuffers, i.e. while the proxy is still Virtual. When the real device is going to be bound
			// at createBuffers, answer with the real device's own limits, so the chosen size is one the
			// hardware accepts. Otherwise a size the hardware refuses would drop a connected interface into
			// speaker mode. With no device (speaker mode) the virtual range applies.
			if (ProbeRealBeforeBind())
			{
				const ASIOError e = m_real->getBufferSize(mn, mx, pref, gran);
				if (e == 0) return e;
			}
			if (mn) *mn = kVirtualMinFrames; if (mx) *mx = kVirtualMaxFrames; if (pref) *pref = kVirtualPreferredFrames; if (gran) *gran = 1;
			return 0;
		}
		return m_real ? m_real->getBufferSize(mn, mx, pref, gran) : -1;
	}
	ASIOError canSampleRate(ASIOSampleRate r) override { if (m_mode.load() == OutputMode::Virtual) return r == 48000.0 ? 0 : -1; return m_real ? m_real->canSampleRate(r) : -1; }
	ASIOError getSampleRate(ASIOSampleRate* r) override { if (m_mode.load() == OutputMode::Virtual) { if (r) *r = 48000.0; return 0; } const ASIOError e = m_real ? m_real->getSampleRate(r) : -1; if (r && e == 0) m_sampleRate = *r; return e; }
	ASIOError setSampleRate(ASIOSampleRate r) override { if (m_mode.load() == OutputMode::Virtual) { if (r != 48000.0) return -1; m_sampleRate = r; return 0; } if (!m_real) return -1; const ASIOError e = m_real->setSampleRate(r); if (e == 0) m_sampleRate = r; return e; }
	ASIOError getClockSources(ASIOClockSource* c, long* n) override { return m_real ? m_real->getClockSources(c, n) : (n ? (*n = 0, 0) : 0); }
	ASIOError setClockSource(long r) override { return m_real ? m_real->setClockSource(r) : -1; }
	ASIOError getSamplePosition(ASIOSamples* samples, ASIOTimeStamp* timestamp) override
	{
		if (m_real) return m_real->getSamplePosition(samples, timestamp);
		const uint64_t position = m_virtualSamplePosition.load(std::memory_order_acquire);
		if (samples) { samples->hi = static_cast<long>(position >> 32); samples->lo = static_cast<unsigned long>(position); }
		if (timestamp)
		{
			LARGE_INTEGER counter{}, frequency{};
			QueryPerformanceCounter(&counter);
			QueryPerformanceFrequency(&frequency);
			const uint64_t nanoseconds = frequency.QuadPart > 0
				? static_cast<uint64_t>((static_cast<long double>(counter.QuadPart) * 1000000000.0L) / frequency.QuadPart) : 0;
			timestamp->hi = static_cast<long>(nanoseconds >> 32);
			timestamp->lo = static_cast<unsigned long>(nanoseconds);
		}
		return 0;
	}
	ASIOError getChannelInfo(ASIOChannelInfo* info) override { if (m_mode.load() == OutputMode::Virtual) { if (!info || info->channel < 0 || info->channel > 1) return -1; info->isActive = 1; info->channelGroup = 0; info->type = ASIOSTInt32LSB; strcpy_s(info->name, 32, info->isInput ? (info->channel == 0 ? "Virtual Capture Left" : "Virtual Capture Right") : (info->channel == 0 ? "Virtual Left" : "Virtual Right")); return 0; } if (!m_real) return -1; const ASIOError e = m_real->getChannelInfo(info); if (e == 0 && info) info->type = ASIOSTInt32LSB; return e; }
	ASIOError disposeBuffers() override { std::lock_guard<std::mutex> transition(m_transitionMutex); ProxyLog("disposeBuffers: mode %s", m_mode.load() == OutputMode::Virtual ? "Virtual" : "Real"); if (m_mode.load() == OutputMode::Virtual) { StopVirtual(); m_virtualCable.Stop(); FreeVirtualBuffers(); } DisableRealCallbacksAndWait(); m_bufferInfos = nullptr; m_numChannels = 0; m_callbackSeen.store(false); if (s_active == this) s_active = nullptr; const ASIOError e = m_real ? m_real->disposeBuffers() : 0; FreeBridge(); return e; }
	ASIOError controlPanel() override { return m_real ? m_real->controlPanel() : 0; }
	ASIOError future(long selector, void* opt) override { return m_real ? m_real->future(selector, opt) : -1; }
	ASIOError outputReady() override { return m_real ? m_real->outputReady() : 0; }

	ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) override
	{
		// The host's stream calls (createBuffers/start/stop/disposeBuffers) and the live transitions (TryPromote,
		// TryDemote, TryRebind; the automatic demotion runs on a detached thread) share one lock. Unserialized, a
		// demotion racing the host's stop -> dispose -> Release can free the bridge buffers twice and write fresh
		// buffer pointers into a host array RS_ASIO has already freed.
		std::lock_guard<std::mutex> transition(m_transitionMutex);
		if (!callbacks) return -1;
		m_hostCallbacks = *callbacks;
		InstallTrampolines();
		if (m_mode.load() == OutputMode::Virtual)
		{
			long inputs = 0;
			for (long i = 0; i < numChannels; ++i) if (bufferInfos[i].isInput) ++inputs;
			const bool preferReal = PreferRealOutput();
			ProxyLog("createBuffers: %ld channel(s) (%ld in, %ld out), %ld frames; PreferReal=%d",
				numChannels, inputs, numChannels - inputs, bufferSize, preferReal ? 1 : 0);
			// Stay virtual unless the user prefers the real device AND it binds cleanly. A device that already
			// refused to load or init during the buffer-size query this boot is not retried (saves ~0.4 s).
			const bool knownAbsent = m_realProbeFailed;
			m_realProbeFailed = false;
			if (knownAbsent && preferReal) ProxyLog("createBuffers: real device unavailable at the buffer-size query; not retrying");
			if (!preferReal || knownAbsent || BindReal(std::wstring(), bufferInfos, numChannels, bufferSize) != 0)
			{
				const ASIOError virtualResult = CreateVirtualBuffers(bufferInfos, numChannels, bufferSize, callbacks);
				ProxyLog("createBuffers: staying Virtual -> %ld", virtualResult);
				return virtualResult;
			}
			CompleteRealBind(bufferInfos, numChannels, bufferSize);
			ProxyLog("createBuffers: bound the real device at boot; mode Real");
			return 0;
		}
		// Already Real (the host disposed and is re-creating buffers): the driver stays loaded and
		// initialised, only its buffers are rebuilt, so this is the one path that does not go via BindReal.
		if (!m_real) return -1;
		EnforceRealSampleRate();
		const ASIOError e = CreateRealBuffers(bufferInfos, numChannels, bufferSize);
		if (e == 0) { CompleteRealBind(bufferInfos, numChannels, bufferSize); return 0; }   // ASE_OK
		// The device went away between the host's dispose and re-create (e.g. unplugged). Staying Real with no
		// buffers and no s_active would make every RSModsAsio_Try* export refuse until a restart.
		ProxyLog("createBuffers: re-creating on the real device failed (%ld); falling back to Virtual", e);
		ReleaseReal();
		ActivateVirtual();
		const ASIOError virtualResult = CreateVirtualBuffers(bufferInfos, numChannels, bufferSize, callbacks);
		ProxyLog("createBuffers: staying Virtual -> %ld", virtualResult);
		return virtualResult;
	}

private:
	// Virtual (speaker-mode) buffer sizes. The virtual clock is software, so any size works: advertise a wide
	// range with granularity 1 so RS_ASIO's BufferSizeMode (driver / host / custom) gets the size the player
	// configured instead of being clamped. Preferred is 128. The max matches the
	// host input observer's MAX_BUFFER_FRAMES (AsioHook.cpp); larger blocks would bypass the input chain.
	static const long kVirtualMinFrames = 32;
	static const long kVirtualMaxFrames = 4096;
	static const long kVirtualPreferredFrames = 128;
	// createBuffers itself takes any size up to this: the size may have come from the real device's range
	// (a bind that then failed), and refusing it would leave the player with no audio at all. Below
	// kVirtualMinFrames the software clock cannot wake that often and delivers buffers in small bursts
	// (rate still holds); above 4096 the host's input chain skips the blocks.
	static const long kVirtualHardMaxFrames = 16384;
	static const long kVirtualSampleRate = 48000;

	void ActivateVirtual() { m_mode.store(OutputMode::Virtual); m_callbackSeen.store(false); m_lastCallbackTick.store(0); m_virtualSamplePosition.store(0); m_sampleRate = 48000.0; }

	// Point the real driver's callbacks at our trampolines (the host's own callbacks are kept in
	// m_hostCallbacks and invoked from OnBufferSwitch after the proxy has filled virtual inputs).
	void InstallTrampolines()
	{
		// Static, not per instance: a quarantined (stalled, never disposed) real driver keeps this pointer, and a proxy
		// instance can be released while it still holds it. The host's own callbacks are copied into statics too, so
		// the driver's capability queries during createBuffers reach RS_ASIO before s_active is set.
		s_hostAsioMessage.store(m_hostCallbacks.asioMessage);
		s_hostSampleRateDidChange.store(m_hostCallbacks.sampleRateDidChange);
		s_ourCallbacks.bufferSwitch = &Trampoline_BufferSwitch;
		s_ourCallbacks.sampleRateDidChange = m_hostCallbacks.sampleRateDidChange ? &Trampoline_SampleRateDidChange : nullptr;
		s_ourCallbacks.asioMessage = m_hostCallbacks.asioMessage ? &Trampoline_AsioMessage : nullptr;
		s_ourCallbacks.bufferSwitchTimeInfo = m_hostCallbacks.bufferSwitchTimeInfo ? &Trampoline_BufferSwitchTimeInfo : nullptr;
	}

	// The ONE place the real-driver bind sequence lives: load (unless already loaded and initialised),
	// init, force the game's 48 kHz, create the real driver's buffers over the host's ASIOBufferInfo
	// array. Every promotion path (initial createBuffers with PreferReal, and the live TryPromote) goes
	// through here, so an invariant added to the sequence holds everywhere. On failure the real driver is
	// released again and the caller decides how to fall back (stay/return to Virtual). Returns 0 (ASE_OK)
	// or the failing ASIOError (-1 when the driver would not load or init).
	ASIOError BindReal(const std::wstring& driverName, ASIOBufferInfo* infos, long numChannels, long bufferSize)
	{
		if (!m_real)
		{
			if (!LoadReal(driverName)) { ReleaseReal(); return -1; }
			if (!m_real->init(m_sysHandle))
			{
				char message[128]{};
				m_real->getErrorMessage(message);
				ProxyLog("BindReal: the real driver refused init (host handle %p): %s", m_sysHandle, message);
				ReleaseReal();
				return -1;
			}
			ProxyLog("BindReal: real driver loaded and initialised");
		}
		EnforceRealSampleRate();
		const ASIOError e = CreateRealBuffers(infos, numChannels, bufferSize);
		if (e != 0)
		{
			char message[128]{};
			m_real->getErrorMessage(message);
			long minimum = 0, maximum = 0, preferred = 0, granularity = 0;
			m_real->getBufferSize(&minimum, &maximum, &preferred, &granularity);
			ProxyLog("BindReal: real createBuffers(%ld channels, %ld frames) -> %ld (%s); driver allows %ld..%ld preferred %ld granularity %ld",
				numChannels, bufferSize, e, message, minimum, maximum, preferred, granularity);
			ReleaseReal();
		}
		else ProxyLog("BindReal: real createBuffers(%ld channels, %ld frames) OK at %.0f Hz", numChannels, bufferSize, m_sampleRate);
		return e;
	}

	// The host (RS_ASIO) always sees ASIOSTInt32LSB: it reads the channel format while the proxy is still
	// Virtual (discovery happens before createBuffers) and sets up its PCM conversion for that type once.
	// A real device that uses another format (Float32, Int24, Int16, Int32 with 16..24 valid bits) would
	// otherwise be handed Int32 samples it reads as its own format: full-scale noise on a Float32 driver.
	// So when any requested channel is not Int32LSB, the real driver gets its own buffer array and the
	// host gets Int32 buffers from the proxy; each callback converts input (real -> host) before the host
	// runs and output (host -> real) after all output processing. Int32LSB devices keep the direct,
	// zero-copy path. A format the bridge cannot convert refuses the bind (the proxy stays Virtual) rather
	// than play garbage.
	ASIOError CreateRealBuffers(ASIOBufferInfo* infos, long numChannels, long bufferSize)
	{
		FreeBridge();
		std::vector<long> types(static_cast<size_t>(numChannels), ASIOSTInt32LSB);
		bool convert = false;
		for (long i = 0; i < numChannels; ++i)
		{
			ASIOChannelInfo info{};
			info.channel = infos[i].channelNum;
			info.isInput = infos[i].isInput;
			if (m_real->getChannelInfo(&info) != 0)
			{
				ProxyLog("BindReal: the driver would not report the format of %s channel %ld; staying Virtual rather than guess",
					infos[i].isInput ? "input" : "output", infos[i].channelNum);
				return -1;
			}
			types[i] = info.type;
			if (!BridgeableType(types[i]))
			{
				ProxyLog("BindReal: %s channel %ld uses ASIO sample type %ld, which the proxy cannot convert; staying Virtual",
					infos[i].isInput ? "input" : "output", infos[i].channelNum, types[i]);
				return -1;
			}
			if (types[i] != ASIOSTInt32LSB) convert = true;
		}
		if (!convert) return m_real->createBuffers(infos, numChannels, bufferSize, &s_ourCallbacks);

		m_realInfos.assign(infos, infos + numChannels);
		for (auto& info : m_realInfos) info.buffers[0] = info.buffers[1] = nullptr;
		const ASIOError e = m_real->createBuffers(m_realInfos.data(), numChannels, bufferSize, &s_ourCallbacks);
		if (e != 0) { m_realInfos.clear(); return e; }
		for (long i = 0; i < numChannels; ++i)
		{
			for (int d = 0; d < 2; ++d)
			{
				void* buffer = calloc(static_cast<size_t>(bufferSize), sizeof(int32_t));
				if (!buffer) { m_real->disposeBuffers(); FreeBridge(); return -1; }
				infos[i].buffers[d] = buffer;
				m_bridgeBuffers.push_back(buffer);
			}
		}
		m_realTypes = std::move(types);
		m_bridged.store(true, std::memory_order_release);
		ProxyLog("BindReal: device sample type %ld (first channel) is not Int32LSB; converting to and from the host's Int32LSB",
			m_realTypes.empty() ? -1L : m_realTypes[0]);
		return 0;
	}

	// Only called with the real callbacks disabled (or never enabled) for these buffers.
	void FreeBridge()
	{
		m_bridged.store(false, std::memory_order_release);
		for (void* buffer : m_bridgeBuffers) free(buffer);
		m_bridgeBuffers.clear();
		m_realInfos.clear();
		m_realTypes.clear();
	}

	static bool BridgeableType(long type)
	{
		switch (type)
		{
		case ASIOSTInt16LSB: case ASIOSTInt24LSB: case ASIOSTInt32LSB:
		case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
		case ASIOSTFloat32LSB: case ASIOSTFloat64LSB: return true;
		default: return false;
		}
	}

	static int32_t HostFromDouble(double value) noexcept
	{
		if (value != value) return 0;   // NaN: silence, not a full-scale spike into the game's input chain
		const double scaled = value * 2147483648.0;
		if (!(scaled > -2147483648.0)) return INT32_MIN;
		if (scaled >= 2147483647.0) return INT32_MAX;
		return static_cast<int32_t>(scaled);
	}

	// Real-device samples -> host Int32LSB (full-scale, left-justified).
	static void RealToHost(const BYTE* src, int32_t* dst, long type, long frames) noexcept
	{
		for (long f = 0; f < frames; ++f)
		{
			int32_t v = 0;
			switch (type)
			{
			case ASIOSTInt16LSB: { int16_t s; memcpy(&s, src + f * 2, 2); v = static_cast<int32_t>(static_cast<uint32_t>(s) << 16); break; }
			case ASIOSTInt24LSB: { const BYTE* p = src + f * 3; v = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 24)); break; }
			case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
			{
				const int shift = type == ASIOSTInt32LSB16 ? 16 : type == ASIOSTInt32LSB18 ? 14 : type == ASIOSTInt32LSB20 ? 12 : 8;
				int32_t s; memcpy(&s, src + f * 4, 4); v = static_cast<int32_t>(static_cast<uint32_t>(s) << shift); break;
			}
			case ASIOSTFloat32LSB: { float s; memcpy(&s, src + f * 4, 4); v = HostFromDouble(s); break; }
			case ASIOSTFloat64LSB: { double s; memcpy(&s, src + f * 8, 8); v = HostFromDouble(s); break; }
			default: { memcpy(&v, src + f * 4, 4); break; }   // ASIOSTInt32LSB
			}
			dst[f] = v;
		}
	}

	// Host Int32LSB -> real-device samples.
	static void HostToReal(const int32_t* src, BYTE* dst, long type, long frames) noexcept
	{
		for (long f = 0; f < frames; ++f)
		{
			const int32_t v = src[f];
			switch (type)
			{
			case ASIOSTInt16LSB: { const int16_t s = static_cast<int16_t>(v >> 16); memcpy(dst + f * 2, &s, 2); break; }
			case ASIOSTInt24LSB: { BYTE* p = dst + f * 3; p[0] = static_cast<BYTE>(v >> 8); p[1] = static_cast<BYTE>(v >> 16); p[2] = static_cast<BYTE>(v >> 24); break; }
			case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
			{
				const int shift = type == ASIOSTInt32LSB16 ? 16 : type == ASIOSTInt32LSB18 ? 14 : type == ASIOSTInt32LSB20 ? 12 : 8;
				const int32_t s = v >> shift; memcpy(dst + f * 4, &s, 4); break;
			}
			case ASIOSTFloat32LSB: { const float s = static_cast<float>(v / 2147483648.0); memcpy(dst + f * 4, &s, 4); break; }
			case ASIOSTFloat64LSB: { const double s = v / 2147483648.0; memcpy(dst + f * 8, &s, 8); break; }
			default: { memcpy(dst + f * 4, &v, 4); break; }   // ASIOSTInt32LSB
			}
		}
	}

	void BridgeInputs(long index) noexcept
	{
		if (!m_bridged.load(std::memory_order_acquire) || !m_bufferInfos || m_bufferSize <= 0) return;
		for (long i = 0; i < m_numChannels && i < static_cast<long>(m_realInfos.size()); ++i)
		{
			if (!m_bufferInfos[i].isInput) continue;
			const auto* src = static_cast<const BYTE*>(m_realInfos[i].buffers[index]);
			auto* dst = static_cast<int32_t*>(m_bufferInfos[i].buffers[index]);
			if (src && dst) RealToHost(src, dst, m_realTypes[i], m_bufferSize);
		}
	}

	void BridgeOutputs(long index) noexcept
	{
		if (!m_bridged.load(std::memory_order_acquire) || !m_bufferInfos || m_bufferSize <= 0) return;
		for (long i = 0; i < m_numChannels && i < static_cast<long>(m_realInfos.size()); ++i)
		{
			if (m_bufferInfos[i].isInput) continue;
			const auto* src = static_cast<const int32_t*>(m_bufferInfos[i].buffers[index]);
			auto* dst = static_cast<BYTE*>(m_realInfos[i].buffers[index]);
			if (src && dst) HostToReal(src, dst, m_realTypes[i], m_bufferSize);
		}
	}

	// Load + init the real driver early, at the host's buffer-size query, so getBufferSize can report the
	// hardware's limits. Only before the first createBuffers (no buffers yet) and only when the real device
	// is wanted. BindReal then reuses the already-initialised driver. A failure is remembered so
	// createBuffers does not pay for a second attempt in the same boot.
	bool ProbeRealBeforeBind()
	{
		if (m_real) return !m_bufferInfos;
		if (m_bufferInfos || m_realProbeFailed || !PreferRealOutput()) return false;
		if (LoadReal() && m_real->init(m_sysHandle))
		{
			long mn = 0, mx = 0, pref = 0, gran = 0;
			m_real->getBufferSize(&mn, &mx, &pref, &gran);
			ProxyLog("getBufferSize: real device answers %ld..%ld preferred %ld granularity %ld", mn, mx, pref, gran);
			return true;
		}
		if (m_real)
		{
			char message[128]{};
			m_real->getErrorMessage(message);
			ProxyLog("getBufferSize: real device refused init (%s); reporting the virtual range", message);
		}
		ReleaseReal();
		m_realProbeFailed = true;
		return false;
	}

	// Bookkeeping once the real driver owns the buffers: remember the layout, switch to Real mode, cache the
	// output channel formats, and arm the per-buffer DSP for this stream.
	void CompleteRealBind(ASIOBufferInfo* infos, long numChannels, long bufferSize)
	{
		m_bufferInfos = infos;
		m_numChannels = numChannels;
		m_bufferSize = bufferSize;
		m_inputCount = 0;
		for (long i = 0; i < numChannels; ++i)
			if (infos[i].isInput) ++m_inputCount;
		m_mode.store(OutputMode::Real, std::memory_order_release);
		CacheInputChannels();
		CacheOutputChannels();
		g_trim.Configure(g_trimGain.load());
		ConfigureGuard();
		OpenAmpLink();
		s_active = this;
	}

	void OpenAmpLink()
	{
		if (!AmpLinkHostEnabled()) return;
		if (g_ampLink.Open(m_bufferSize, m_inputCount)) ProxyLog("amp link: open at %ld frames, %ld input channel(s)", m_bufferSize, m_inputCount);
		else ProxyLog("amp link: could not open at %ld frames (external amp unavailable this stream)", m_bufferSize);
	}

	// Add the external amp's output (one block behind) to the first two output channels.
	void MixAmpReturn(long index) noexcept
	{
		long buffers[2]{}, types[2]{};
		const long count = m_outputCount < 2 ? m_outputCount : 2;
		for (long ch = 0; ch < count; ++ch) { buffers[ch] = m_outputs[ch].bufferIndex; types[ch] = m_outputs[ch].type; }
		g_ampLink.MixReturn(m_bufferInfos, buffers, types, count, index, m_bufferSize);
	}

	// Drop the real driver and its module. Safe to call when nothing is loaded.
	void ReleaseReal()
	{
		if (m_real) { m_real->Release(); m_real = nullptr; }
		if (m_realModule) { FreeLibrary(m_realModule); m_realModule = nullptr; }
	}

	// Roll a failed live promotion back onto the virtual device. Always returns false (the promotion failed)
	// so callers can `return RestoreVirtual(wasRunning);`.
	bool RestoreVirtual(bool wasRunning)
	{
		ActivateVirtual();
		const bool restored = CreateVirtualBuffers(m_bufferInfos, m_numChannels, m_bufferSize, &m_hostCallbacks) == 0;
		if (restored && wasRunning) { m_virtualCable.Start(); StartVirtual(); }
		return false;
	}

	// Rocksmith's Wwise engine renders at a fixed 48 kHz. When the game boots into its
	// unplugged / no-input path RS_ASIO can skip the 48 kHz sample-rate negotiation, leaving
	// the real device at its hardware default (the M-Track Solo/Duo defaults to 44.1 kHz). The
	// device clock then dominates the shared stream: 48 kHz audio plays at 44100/48000 = 0.919x,
	// about 1.47 semitones flat ("extra deep"), and the 48 kHz-expecting capture path never
	// lines up. Force the real driver to 48 kHz on every promotion to Real mode so the proxy
	// guarantees the rate the rest of the pipeline already assumes. ASIO locks the rate at
	// buffer creation, so this must run BEFORE m_real->createBuffers().
	void EnforceRealSampleRate()
	{
		if (!m_real) return;
		const ASIOSampleRate target = 48000.0;
		ASIOSampleRate before = 0.0;
		m_real->getSampleRate(&before);
		if (m_real->canSampleRate(target) == 0 && m_real->setSampleRate(target) == 0)
		{
			ASIOSampleRate after = 0.0;
			m_real->getSampleRate(&after);
			ProxyLog("EnforceRealSampleRate: device was at %.0f Hz, set 48000, reads back %.0f Hz", before, after);
			m_sampleRate = target;
			return;
		}
		ASIOSampleRate current = 0.0;
		if (m_real->getSampleRate(&current) == 0) m_sampleRate = current;
		const std::string message = "[Rocksmith Audio Bridge ASIO] Real ASIO device stayed at "
			+ std::to_string(static_cast<long>(m_sampleRate))
			+ " Hz instead of 48000; game audio will be pitched and input capture may not start.\n";
		OutputDebugStringA(message.c_str());
		ProxyLog("EnforceRealSampleRate: device refused 48000 Hz, stays at %.0f Hz", m_sampleRate);
	}

	ASIOError CreateVirtualBuffers(ASIOBufferInfo* infos, long numChannels, long bufferSize, ASIOCallbacks* callbacks)
	{
		if (!infos || numChannels <= 0 || !callbacks || !callbacks->bufferSwitch
			|| bufferSize < 1 || bufferSize > kVirtualHardMaxFrames) return -1;
		long inputs = 0, outputs = 0;
		bool inputChannels[2]{};
		bool outputChannels[2]{};
		for (long i = 0; i < numChannels; ++i)
		{
			const long channel = infos[i].channelNum;
			if (channel < 0 || channel > 1) return -1;
			bool* seen = infos[i].isInput ? inputChannels : outputChannels;
			if (seen[channel]) return -1;
			seen[channel] = true;
			if (infos[i].isInput) ++inputs; else ++outputs;
		}
		if (inputs > 2 || outputs > 2) return -1;
		m_bufferInfos = infos; m_numChannels = numChannels; m_bufferSize = bufferSize;
		m_hostCallbacks = *callbacks; m_outputCount = 0; m_inputCount = 0; m_virtualBuffers.clear();
		m_inputScratch.assign(static_cast<size_t>(bufferSize) * 2, 0);   // sized here, never on the audio thread
		for (long i = 0; i < numChannels; ++i)
		{
			for (int d = 0; d < 2; ++d)
			{
				void* buffer = calloc(static_cast<size_t>(bufferSize), sizeof(int32_t));
				if (!buffer) { FreeVirtualBuffers(); return -1; }
				infos[i].buffers[d] = buffer; m_virtualBuffers.push_back(buffer);
			}
			if (infos[i].isInput)
			{
				++m_inputCount;
				m_inputTypes[i] = ASIOSTInt32LSB;
			}
			else m_outputs[m_outputCount++] = { i, ASIOSTInt32LSB };
		}
		// Size and configure the limiter/AGC for Virtual (speaker-mode) streams too, so the output guard applies
		// and a Virtual re-create at a larger size does not overrun the scratch sized for the last Real bind.
		ConfigureGuard();
		OpenAmpLink();
		s_active = this;
		return 0;
	}

	// The virtual (speaker-mode) clock: plays the part of the sound card by calling the host's bufferSwitch
	// once per buffer. In speaker mode the speakers are fed by the host's WASAPI route, whose rate matcher
	// follows THIS clock, so its steadiness is what the player hears. Plain sleeps are ~1 ms granular and
	// would wake late, then fire buffers back to back (xruns, crackle on the speaker route). So:
	//   - deadlines are computed from the frame count on the QPC clock (exact, no accumulated drift, any
	//     buffer size),
	//   - the wait is a high-resolution waitable timer (Win10 1803+; falls back to a normal waitable timer
	//     with a 1 ms system timer resolution),
	//   - the thread registers with MMCSS "Pro Audio" like every other real-time audio thread here,
	//   - a stall longer than a few buffers resyncs instead of bursting a backlog of callbacks at the host.
	ASIOError StartVirtual()
	{
		if (m_virtualRunning || !m_bufferInfos || m_bufferSize <= 0) return -1;
		m_virtualRunning = true;
		const long frames = m_bufferSize;
		m_virtualThread = std::thread([this, frames]
		{
			DWORD taskIndex = 0;
			HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
			if (!task) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			const bool highResolution = timer != nullptr;
			if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
			if (!highResolution) timeBeginPeriod(1);
			ProxyLog("virtual clock: %ld frames at %ld Hz, %s timer, MMCSS %s", frames, kVirtualSampleRate,
				highResolution ? "high-resolution" : "standard", task ? "Pro Audio" : "unavailable");

			LARGE_INTEGER frequency{}, now{};
			QueryPerformanceFrequency(&frequency);
			QueryPerformanceCounter(&now);
			int64_t origin = now.QuadPart;
			uint64_t framesDue = 0;   // frames the clock has delivered since origin
			const int64_t periodTicks = frequency.QuadPart * frames / kVirtualSampleRate;
			// Resync only after a real stall: four buffers or 20 ms, whichever is longer. Tiny buffers need the
			// slack, because a timer wake-up (~0.5-1.5 ms) spans several of their periods.
			const int64_t resyncTicks = std::max<int64_t>(periodTicks * 4, frequency.QuadPart / 50);
			long index = 0;
			uint64_t resyncs = 0;
			while (m_virtualRunning)
			{
				OnBufferSwitch(index);
				m_virtualSamplePosition.fetch_add(static_cast<uint64_t>(frames), std::memory_order_release);
				index ^= 1;
				framesDue += static_cast<uint64_t>(frames);
				// Rebase every minute (exact: whole seconds of frames and ticks) so framesDue * frequency
				// never overflows, even with a GHz-rate QPC.
				if (framesDue >= static_cast<uint64_t>(kVirtualSampleRate) * 60)
				{
					framesDue -= static_cast<uint64_t>(kVirtualSampleRate) * 60;
					origin += frequency.QuadPart * 60;
				}

				// Exact deadline for the next buffer: origin + framesDue / rate, in QPC ticks.
				const int64_t deadline = origin + static_cast<int64_t>(framesDue * static_cast<uint64_t>(frequency.QuadPart) / kVirtualSampleRate);
				QueryPerformanceCounter(&now);
				if (now.QuadPart - deadline > resyncTicks)
				{
					// Stalled for several buffers (debugger, system hitch): start a fresh timeline instead
					// of firing the missed buffers back to back, which the host cannot absorb.
					origin = now.QuadPart; framesDue = 0;
					if ((++resyncs & (resyncs - 1)) == 0) ProxyLog("virtual clock: stalled, resynced (%llu so far)", resyncs);
					continue;
				}
				const int64_t remaining = deadline - now.QuadPart;
				if (remaining <= 0) continue;
				if (timer)
				{
					LARGE_INTEGER due{};
					due.QuadPart = -std::max<int64_t>(1, remaining * 10000000 / frequency.QuadPart);   // relative, 100 ns units
					if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) { WaitForSingleObject(timer, 1000); continue; }
				}
				std::this_thread::sleep_for(std::chrono::microseconds(remaining * 1000000 / frequency.QuadPart));
			}
			if (timer) CloseHandle(timer);
			if (!highResolution) timeEndPeriod(1);
			if (task) AvRevertMmThreadCharacteristics(task);
		});
		return 0;
	}

	bool TryPromoteToReal(const std::wstring& driverName)
	{
		std::lock_guard<std::mutex> transition(m_transitionMutex);
		ProxyLog("TryPromote: '%ls' requested; mode %s, buffers %s", driverName.c_str(),
			m_mode.load() == OutputMode::Virtual ? "Virtual" : "Real", m_bufferInfos ? "present" : "absent");
		if (m_mode.load() != OutputMode::Virtual || !m_bufferInfos) return false;
		const bool wasRunning = m_streamRunning.load(std::memory_order_acquire);
		// Load and init before touching the running virtual stream, so a driver that will not even load
		// costs the player nothing. BindReal then skips straight to the rate + buffer step.
		if (!LoadReal(driverName)) { ReleaseReal(); return false; }
		if (!m_real->init(m_sysHandle))
		{
			char message[128]{};
			m_real->getErrorMessage(message);
			ProxyLog("TryPromote: the real driver refused init: %s", message);
			ReleaseReal();
			return false;
		}
		if (wasRunning) StopVirtual();
		m_virtualCable.Stop();
		FreeVirtualBuffers();
		for (long i = 0; i < m_numChannels; ++i) m_bufferInfos[i].buffers[0] = m_bufferInfos[i].buffers[1] = nullptr;
		InstallTrampolines();
		if (BindReal(driverName, m_bufferInfos, m_numChannels, m_bufferSize) != 0) return RestoreVirtual(wasRunning);
		CompleteRealBind(m_bufferInfos, m_numChannels, m_bufferSize);
		m_realCallbacksEnabled.store(true, std::memory_order_release);
		m_callbackSeen.store(false);
		if (!wasRunning) return true;
		if (m_real->start() == 0)
		{
			for (int attempt = 0; attempt < 10 && !m_callbackSeen.load(std::memory_order_acquire); ++attempt)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			if (m_callbackSeen.load(std::memory_order_acquire)) { ProxyLog("TryPromote: real device running; mode Real"); return true; }
		}
		ProxyLog("TryPromote: real driver bound but never called back; back to Virtual");
		// The real driver bound but never called back: hand the stream back to the virtual clock.
		DisableRealCallbacksAndWait();
		m_real->stop();
		m_real->disposeBuffers();
		FreeBridge();
		ReleaseReal();
		return RestoreVirtual(wasRunning);
	}

	// Explicit "Apply" on a device the proxy already reports as bound: tear the real driver down properly
	// (stop, dispose, release, unload), park the host on the virtual clock so its stream never breaks, then
	// bind the named driver again through the normal promotion path. This is the only recovery when the
	// real device is dead but still calling back (callbacks at the right rate, no audio either way).
	// Nothing automatic calls this; it is the player's click.
	bool TryRebindReal(const std::wstring& driverName)
	{
		{
			std::lock_guard<std::mutex> transition(m_transitionMutex);
			ProxyLog("Rebind: '%ls' requested; mode %s", driverName.c_str(),
				m_mode.load() == OutputMode::Virtual ? "Virtual" : "Real");
			if (m_mode.load(std::memory_order_acquire) == OutputMode::Real)
			{
				if (!m_bufferInfos) return false;
				const bool wasRunning = m_streamRunning.load(std::memory_order_acquire);
				DisableRealCallbacksAndWait();
				if (m_real)
				{
					if (wasRunning) m_real->stop();
					m_real->disposeBuffers();
				}
				FreeBridge();
				ReleaseReal();
				for (long i = 0; i < m_numChannels; ++i) m_bufferInfos[i].buffers[0] = m_bufferInfos[i].buffers[1] = nullptr;
				ActivateVirtual();
				if (CreateVirtualBuffers(m_bufferInfos, m_numChannels, m_bufferSize, &m_hostCallbacks) != 0)
				{
					ProxyLog("Rebind: could not park the host on virtual buffers");
					return false;
				}
				if (wasRunning) { m_virtualCable.Start(); StartVirtual(); }
				ProxyLog("Rebind: real driver released; host parked on Virtual");
			}
		}
		return TryPromoteToReal(driverName);
	}

	bool TryDemoteToVirtual()
	{
		std::lock_guard<std::mutex> transition(m_transitionMutex);
		if (m_mode.load(std::memory_order_acquire) != OutputMode::Real || !m_real || !m_bufferInfos) return false;
		const bool wasRunning = m_streamRunning.exchange(false, std::memory_order_acq_rel);
		// A stalled ASIO driver may be inside stop/dispose or may invoke a callback concurrently.
		// Disable the trampoline first, detach both handles, and quarantine the driver/module.
		DisableRealCallbacksAndWait();
		FreeBridge();   // the host is moved onto virtual buffers below; the quarantined driver keeps its own
		m_real = nullptr;
		m_realModule = nullptr;
		ActivateVirtual();
		if (CreateVirtualBuffers(m_bufferInfos, m_numChannels, m_bufferSize, &m_hostCallbacks) != 0) return false;
		if (!wasRunning) return true;
		m_virtualCable.Start();
		const bool started = StartVirtual() == 0;
		if (!started) m_virtualCable.Stop();
		m_streamRunning.store(started, std::memory_order_release);
		return started;
	}

	void StopVirtual()
	{
		m_virtualRunning = false;
		if (m_virtualThread.joinable()) m_virtualThread.join();
	}

	void DisableRealCallbacksAndWait()
	{
		m_realCallbacksEnabled.store(false, std::memory_order_seq_cst);
		while (m_realCallbacksInFlight.load(std::memory_order_seq_cst) != 0)
			std::this_thread::yield();
	}

	void FreeVirtualBuffers()
	{
		for (void* buffer : m_virtualBuffers) free(buffer);
		m_virtualBuffers.clear();
	}

	bool LoadReal(const std::wstring& driverName = std::wstring())
	{
		if (m_real) return true;
		std::wstring dllPath; CLSID clsid;
		const std::wstring targetDriver = driverName.empty() ? ReadTargetDriverName() : driverName;
		if (!ResolveAsioDriver(targetDriver, dllPath, clsid))
		{
			ProxyLog("LoadReal: no ASIO registry entry for '%ls'", targetDriver.c_str());
			return false;
		}
		m_realModule = LoadLibraryW(dllPath.c_str());
		if (!m_realModule)
		{
			ProxyLog("LoadReal: LoadLibrary failed for '%ls' (error %lu)", dllPath.c_str(), GetLastError());
			return false;
		}
		using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
		auto dllGetClassObject = reinterpret_cast<GetClassObject>(GetProcAddress(m_realModule, "DllGetClassObject"));
		if (!dllGetClassObject) { ProxyLog("LoadReal: '%ls' exports no DllGetClassObject", dllPath.c_str()); return false; }
		IClassFactory* factory = nullptr;
		if (FAILED(dllGetClassObject(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory))) || !factory)
		{
			ProxyLog("LoadReal: '%ls' gave no class factory", dllPath.c_str());
			return false;
		}
		const HRESULT hr = factory->CreateInstance(nullptr, clsid, reinterpret_cast<void**>(&m_real));
		factory->Release();
		if (FAILED(hr) || !m_real) ProxyLog("LoadReal: CreateInstance for '%ls' failed 0x%08lX", targetDriver.c_str(), static_cast<unsigned long>(hr));
		else ProxyLog("LoadReal: '%ls' loaded from '%ls'", targetDriver.c_str(), dllPath.c_str());
		return SUCCEEDED(hr) && m_real != nullptr;
	}

	void CacheOutputChannels()
	{
		m_outputCount = 0;
		for (long i = 0; i < m_numChannels && m_outputCount < kMaxOutputs; ++i)
		{
			if (m_bufferInfos[i].isInput) continue;
			ASIOChannelInfo info{}; info.channel = m_bufferInfos[i].channelNum; info.isInput = 0;
			m_real->getChannelInfo(&info);
			m_outputs[m_outputCount].bufferIndex = i;
			m_outputs[m_outputCount].type = m_bridged.load(std::memory_order_acquire) ? ASIOSTInt32LSB : info.type;
			++m_outputCount;
		}
	}

	void CacheInputChannels()
	{
		for (long i = 0; i < m_numChannels; ++i)
		{
			if (!m_bufferInfos[i].isInput || i >= static_cast<long>(m_inputTypes.size())) continue;
			ASIOChannelInfo info{};
			info.channel = m_bufferInfos[i].channelNum;
			info.isInput = 1;
			m_inputTypes[i] = m_bridged.load(std::memory_order_acquire) ? ASIOSTInt32LSB
				: SUCCEEDED(m_real->getChannelInfo(&info)) ? info.type : ASIOSTInt32LSB;
		}
	}

	void FillVirtualInputs(long index)
	{
		if (m_mode.load(std::memory_order_acquire) != OutputMode::Virtual || m_inputCount == 0 || m_bufferSize <= 0) return;
		if (m_inputScratch.size() < static_cast<size_t>(m_bufferSize) * 2) return;
		m_virtualCable.Read(m_inputScratch.data(), static_cast<uint32_t>(m_bufferSize));
		for (long channel = 0; channel < m_numChannels; ++channel)
		{
			if (!m_bufferInfos[channel].isInput) continue;
			auto* destination = static_cast<int32_t*>(m_bufferInfos[channel].buffers[index]);
			if (!destination) continue;
			const long sourceChannel = std::clamp(m_bufferInfos[channel].channelNum, 0L, 1L);
			for (long frame = 0; frame < m_bufferSize; ++frame)
				destination[frame] = m_inputScratch[static_cast<size_t>(frame) * 2 + sourceChannel];
		}
	}

	// Called by the real driver: let the host fill the buffers, then copy the output channels to the sink.
	void OnBufferSwitch(long index)
	{
		m_callbackSeen.store(true, std::memory_order_release);
		m_lastCallbackTick.store(GetTickCount64(), std::memory_order_release);
		FillVirtualInputs(index);
		BridgeInputs(index);
		g_ampLink.Publish(m_bufferInfos, m_numChannels, index, m_bufferSize);   // raw DI, before the input chain
		NotifyInputObserver(index);
		if (m_hostCallbacks.bufferSwitch) m_hostCallbacks.bufferSwitch(index, ASIOFalseVal);
		MixAmpReturn(index);   // before the trim/limiter/meter/tap, so all of them include the amp
		InjectProbe(index);
		ApplyOutputTrim(index);
		ApplyOutputGuard(index);
		MeterOutput(index);
		TapOutput(index);
		MuteForward(index);
		BridgeOutputs(index);
	}
	ASIOTime* OnBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct)
	{
		m_callbackSeen.store(true, std::memory_order_release);
		m_lastCallbackTick.store(GetTickCount64(), std::memory_order_release);
		FillVirtualInputs(index);
		BridgeInputs(index);
		g_ampLink.Publish(m_bufferInfos, m_numChannels, index, m_bufferSize);
		NotifyInputObserver(index);
		ASIOTime* r = m_hostCallbacks.bufferSwitchTimeInfo ? m_hostCallbacks.bufferSwitchTimeInfo(params, index, direct) : nullptr;
		MixAmpReturn(index);
		InjectProbe(index);
		ApplyOutputTrim(index);
		ApplyOutputGuard(index);
		MeterOutput(index);
		TapOutput(index);
		MuteForward(index);
		BridgeOutputs(index);
		return r;
	}

	void NotifyInputObserver(long index) noexcept
	{
		const AsioInputObserver observer = g_inputObserver.load(std::memory_order_acquire);
		if (!observer || !m_bufferInfos || m_bufferSize <= 0) return;

		long count = 0;
		for (long channel = 0; channel < m_numChannels && count < static_cast<long>(m_inputChannels.size()); ++channel)
		{
			if (!m_bufferInfos[channel].isInput || channel >= static_cast<long>(m_inputTypes.size())) continue;
			m_inputChannels[count++] = {
				m_bufferInfos[channel].buffers[index],
				m_bufferInfos[channel].channelNum,
				m_inputTypes[channel] };
		}
		if (count > 0) observer(m_inputChannels.data(), count, m_bufferSize, m_sampleRate);
	}

	// Zero the output buffers handed to the real ASIO driver when the host has redirected the mix
	// elsewhere, so the game's bound device goes silent instead of doubling the routed feed. Runs last,
	// after the tap has already copied the real audio out. Passthrough (unmuted) is a no-op.
	void MuteForward(long index) noexcept
	{
		if (!g_forwardMuted.load(std::memory_order_relaxed) || m_outputCount == 0 || m_bufferSize <= 0) return;
		for (long ch = 0; ch < m_outputCount; ++ch)
		{
			const long bytes = SampleBytes(m_outputs[ch].type);
			BYTE* dst = reinterpret_cast<BYTE*>(m_bufferInfos[m_outputs[ch].bufferIndex].buffers[index]);
			if (dst && bytes > 0) memset(dst, 0, static_cast<size_t>(m_bufferSize) * bytes);
		}
	}

	// Meter the post-limit output: per channel compute the block's linear peak and RMS, feed the peak-hold /
	// RMS smoother, and publish. Runs only while a viewer has the meter enabled.
	void MeterOutput(long index) noexcept
	{
		if (!g_meterEnabled.load(std::memory_order_relaxed) || m_outputCount == 0 || m_bufferSize <= 0) return;
		const long frames = m_bufferSize;
		const long channels = (m_outputCount < kMeterMaxCh) ? m_outputCount : kMeterMaxCh;
		for (long ch = 0; ch < channels; ++ch)
		{
			const long bytes = SampleBytes(m_outputs[ch].type);
			const BYTE* src = reinterpret_cast<const BYTE*>(m_bufferInfos[m_outputs[ch].bufferIndex].buffers[index]);
			if (!src || bytes == 0) continue;
			float blockPeak = 0.0f; double sumsq = 0.0;
			for (long f = 0; f < frames; ++f)
			{
				const float v = ReadFloatSample(src + static_cast<size_t>(f) * bytes, m_outputs[ch].type);
				const float a = v < 0.0f ? -v : v;
				if (a > blockPeak) blockPeak = a;
				sumsq += static_cast<double>(v) * v;
			}
			const float blockRms = static_cast<float>(std::sqrt(sumsq / frames));
			g_meter[ch].Update(blockPeak, blockRms);
			g_meterPeak[ch].store(g_meter[ch].peakHold, std::memory_order_relaxed);
			g_meterRms[ch].store(g_meter[ch].rms, std::memory_order_relaxed);
		}
		g_meterChannels.store(static_cast<int>(channels));
	}

	// Scale the output in place by a single static gain. Reads each channel to float, multiplies by the
	// trim gain, and writes it back in the same format. No peak detection, no envelope, no clamp: the
	// waveform is scaled exactly, so it never clips or pumps and adds no distortion or latency.
	void ApplyOutputTrim(long index) noexcept
	{
		g_trim.gain = g_trimGain.load(std::memory_order_relaxed);
		if (!g_trim.Active() || m_outputCount == 0 || m_bufferSize <= 0) return;
		const long frames = m_bufferSize;
		const long channels = m_outputCount;
		for (long ch = 0; ch < channels; ++ch)
		{
			const long bytes = SampleBytes(m_outputs[ch].type);
			BYTE* buf = reinterpret_cast<BYTE*>(m_bufferInfos[m_outputs[ch].bufferIndex].buffers[index]);
			if (!buf || bytes == 0) return;
			for (long f = 0; f < frames; ++f)
			{
				BYTE* p = buf + static_cast<size_t>(f) * bytes;
				WriteFloatSample(p, m_outputs[ch].type, g_trim.Apply(ReadFloatSample(p, m_outputs[ch].type)));
			}
		}
	}

	// Set up the loudness guard for this stream: fixed structural config (sample rate, channel count,
	// look-ahead/release/AGC time constants) plus a pre-allocated float scratch buffer per channel, so the
	// audio thread never allocates. Called on the control thread from createBuffers. Live scalars (ceiling,
	// target, on/off) are pushed separately through atomics and read each buffer in ApplyOutputGuard.
	void ConfigureGuard()
	{
		OutputGuardDsp::Config cfg;
		cfg.sampleRate = m_sampleRate > 0.0 ? static_cast<float>(m_sampleRate) : 48000.0f;
		cfg.limiterOn = g_guardLimiterOn.load() != 0;
		cfg.ceilingLin = g_guardCeiling.load();
		cfg.agcOn = g_guardAgcOn.load() != 0;
		cfg.targetRms = g_guardTarget.load();
		g_guard.Configure(cfg);
		g_guard.Reset(m_outputCount);
		m_guardScratch.assign(m_outputCount, std::vector<float>(m_bufferSize > 0 ? m_bufferSize : 1, 0.0f));
	}

	// Run the loudness guard over the output in place. When both stages are off this is a cheap no-op, so
	// the default path stays untouched. When on: read each channel to float scratch (and its mean-square),
	// advance the AGC once for the block, then limit each channel and write it back in the native format.
	void ApplyOutputGuard(long index) noexcept
	{
		const bool limiterOn = g_guardLimiterOn.load(std::memory_order_relaxed) != 0;
		const bool agcOn = g_guardAgcOn.load(std::memory_order_relaxed) != 0;
		if ((!limiterOn && !agcOn) || m_outputCount == 0 || m_bufferSize <= 0) return;
		if (static_cast<int>(m_guardScratch.size()) < m_outputCount) return;   // not sized yet: skip, do not allocate here

		g_guard.SetLimiterLive(limiterOn, g_guardCeiling.load(std::memory_order_relaxed));
		g_guard.SetAgcLive(agcOn, g_guardTarget.load(std::memory_order_relaxed));

		const long frames = m_bufferSize;
		const long channels = m_outputCount;
		double meanSquare[kMaxOutputs]{};
		for (long ch = 0; ch < channels; ++ch)
		{
			const long bytes = SampleBytes(m_outputs[ch].type);
			BYTE* buf = reinterpret_cast<BYTE*>(m_bufferInfos[m_outputs[ch].bufferIndex].buffers[index]);
			float* dst = m_guardScratch[ch].data();
			if (!buf || bytes == 0) { meanSquare[ch] = 0.0; continue; }
			double sumsq = 0.0;
			for (long f = 0; f < frames; ++f)
			{
				const float v = ReadFloatSample(buf + static_cast<size_t>(f) * bytes, m_outputs[ch].type);
				dst[f] = v;
				sumsq += static_cast<double>(v) * v;
			}
			meanSquare[ch] = sumsq / frames;
		}

		g_guard.BeginBlock(meanSquare, static_cast<int>(channels), static_cast<int>(frames));

		for (long ch = 0; ch < channels; ++ch)
		{
			const long bytes = SampleBytes(m_outputs[ch].type);
			BYTE* buf = reinterpret_cast<BYTE*>(m_bufferInfos[m_outputs[ch].bufferIndex].buffers[index]);
			if (!buf || bytes == 0) continue;
			float* s = m_guardScratch[ch].data();
			g_guard.ProcessChannel(static_cast<int>(ch), s, static_cast<int>(frames));
			for (long f = 0; f < frames; ++f)
				WriteFloatSample(buf + static_cast<size_t>(f) * bytes, m_outputs[ch].type, s[f]);
		}
	}

	// One output sample (given ASIO format) as a float in [-1, 1]. Inverse of WriteFloatSample.
	static float ReadFloatSample(const BYTE* src, long asioType) noexcept
	{
		switch (asioType)
		{
		case ASIOSTInt16LSB: { int16_t s; memcpy(&s, src, 2); return s / 32768.0f; }
		case ASIOSTInt24LSB: { int32_t s = static_cast<int32_t>(src[0] | (src[1] << 8) | (src[2] << 16)); if (s & 0x800000) s |= ~0xFFFFFF; return s / 8388608.0f; }
		case ASIOSTInt32LSB: { int32_t s; memcpy(&s, src, 4); return static_cast<float>(s / 2147483648.0); }
		case ASIOSTFloat32LSB: { float v; memcpy(&v, src, 4); return v; }
		default: return 0.0f;
		}
	}

	// When armed, overwrite the leading probe samples of each output channel for this buffer. Runs after the
	// host filled the buffers, so it briefly replaces game audio with the impulse the loopback will carry back.
	void InjectProbe(long index) noexcept
	{
		if (g_probeRemaining.load(std::memory_order_relaxed) <= 0 || m_outputCount == 0 || m_bufferSize <= 0) return;
		const long frames = m_bufferSize;
		for (long ch = 0; ch < m_outputCount; ++ch)
		{
			const long type = m_outputs[ch].type;
			const long bytes = SampleBytes(type);
			if (bytes == 0) continue;
			BYTE* dst = reinterpret_cast<BYTE*>(m_bufferInfos[m_outputs[ch].bufferIndex].buffers[index]);
			if (!dst) continue;
			for (long f = 0; f < frames; ++f)
			{
				const int p = g_probePos + static_cast<int>(f);
				if (p < kProbeLen) WriteFloatSample(dst + static_cast<size_t>(f) * bytes, type, g_probe[p]);
			}
		}
		g_probePos += frames;
		const int left = g_probeRemaining.load(std::memory_order_relaxed) - frames;
		g_probeRemaining.store(left > 0 ? left : 0, std::memory_order_relaxed);
	}

	// Write one float sample [-1,1] as the given ASIO output format (common LSB layouts + float32).
	static void WriteFloatSample(BYTE* dst, long asioType, float v) noexcept
	{
		if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
		switch (asioType)
		{
		case ASIOSTInt16LSB: { const int16_t s = static_cast<int16_t>(v * 32767.0f); memcpy(dst, &s, 2); break; }
		case ASIOSTInt24LSB: { const int32_t s = static_cast<int32_t>(v * 8388607.0f); dst[0] = (BYTE)s; dst[1] = (BYTE)(s >> 8); dst[2] = (BYTE)(s >> 16); break; }
		case ASIOSTInt32LSB: { const int32_t s = static_cast<int32_t>(v * 2147483647.0); memcpy(dst, &s, 4); break; }
		case ASIOSTFloat32LSB: { memcpy(dst, &v, 4); break; }
		default: break;   // uncommon format: leave the game audio untouched rather than write garbage
		}
	}

	void TapOutput(long index) noexcept
	{
		if (m_outputCount == 0 || m_bufferSize <= 0) return;
		bool anySink = false;
		for (int i = 0; i < kMaxSinks; ++i) if (g_sinks[i].load(std::memory_order_relaxed)) { anySink = true; break; }
		if (!anySink) return;
		// Interleave the per-channel ASIO output buffers into one block for the recorder.
		const long channels = m_outputCount;
		const long frames = m_bufferSize;
		const long sampleBytes = SampleBytes(m_outputs[0].type);
		if (sampleBytes == 0) return;
		static thread_local std::string scratch;
		scratch.resize(static_cast<size_t>(frames) * channels * sampleBytes);
		BYTE* out = reinterpret_cast<BYTE*>(&scratch[0]);
		for (long ch = 0; ch < channels; ++ch)
		{
			const BYTE* src = reinterpret_cast<const BYTE*>(m_bufferInfos[m_outputs[ch].bufferIndex].buffers[index]);
			if (!src) return;
			for (long f = 0; f < frames; ++f)
				memcpy(out + (static_cast<size_t>(f) * channels + ch) * sampleBytes, src + static_cast<size_t>(f) * sampleBytes, sampleBytes);
		}
		for (int i = 0; i < kMaxSinks; ++i)
		{
			OutputSink s = g_sinks[i].load(std::memory_order_relaxed);
			if (s) s(out, frames, channels, m_outputs[0].type, m_sampleRate);
		}
	}

	static long SampleBytes(long asioType)
	{
		switch (asioType)
		{
		case ASIOSTInt16LSB: case ASIOSTInt16MSB: return 2;
		case ASIOSTInt24LSB: case ASIOSTInt24MSB: return 3;
		case ASIOSTInt32LSB: case ASIOSTInt32MSB:
		case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
		case ASIOSTFloat32LSB: case ASIOSTFloat32MSB: return 4;
		case ASIOSTFloat64LSB: case ASIOSTFloat64MSB: return 8;
		default: return 0;
		}
	}

	static void Trampoline_BufferSwitch(long index, ASIOBool) { if (s_active) s_active->OnRealBufferSwitch(index); }
	// Reset-type requests (kAsioResetRequest 3, BufferSizeChange 4, ResyncRequest 5, LatenciesChanged 6) only mean
	// something from the device the proxy is bound to. While the proxy is not on a real device they can only come
	// from a quarantined driver that woke up, and passing them on would make RS_ASIO reset a healthy speaker-mode stream.
	static long Trampoline_AsioMessage(long selector, long value, void* message, double* opt)
	{
		const auto host = s_hostAsioMessage.load();
		if (!host) return 0;
		if (selector >= 3 && selector <= 6)
		{
			const AsioProxyDriver* active = s_active;
			if (!active || active->m_mode.load(std::memory_order_acquire) != OutputMode::Real) return 0;
		}
		return host(selector, value, message, opt);
	}
	static void Trampoline_SampleRateDidChange(ASIOSampleRate rate)
	{
		const AsioProxyDriver* active = s_active;
		const auto host = s_hostSampleRateDidChange.load();
		if (host && active && active->m_mode.load(std::memory_order_acquire) == OutputMode::Real) host(rate);
	}
	static ASIOTime* Trampoline_BufferSwitchTimeInfo(ASIOTime* p, long index, ASIOBool d) { return s_active ? s_active->OnRealBufferSwitchTimeInfo(p, index, d) : nullptr; }

	static const long ASIOFalseVal = 0;
	static const int kMaxOutputs = 32;
	struct OutChannel { long bufferIndex; long type; };

	std::atomic<ULONG> m_ref{ 1 };
	std::atomic<OutputMode> m_mode{ OutputMode::None };
	void* m_sysHandle = nullptr;
	IAsioDriver* m_real = nullptr;
	HMODULE m_realModule = nullptr;
	ASIOCallbacks m_hostCallbacks{};
	static ASIOCallbacks s_ourCallbacks;
	static std::atomic<long(*)(long, long, void*, double*)> s_hostAsioMessage;
	static std::atomic<void(*)(ASIOSampleRate)> s_hostSampleRateDidChange;
	ASIOBufferInfo* m_bufferInfos = nullptr;
	long m_numChannels = 0;
	long m_bufferSize = 0;
	double m_sampleRate = 48000.0;
	OutChannel m_outputs[kMaxOutputs]{};
	long m_outputCount = 0;
	long m_inputCount = 0;
	std::vector<std::vector<float>> m_guardScratch;   // per-channel float scratch for the loudness guard (pre-allocated)
	std::vector<void*> m_virtualBuffers;
	std::thread m_virtualThread;
	std::atomic<bool> m_virtualRunning{ false };
	std::atomic<bool> m_callbackSeen{ false };
	bool m_realProbeFailed = false;   // real device refused load/init at this boot's buffer-size query
	std::atomic<bool> m_realCallbacksEnabled{ false };
	std::atomic<uint32_t> m_realCallbacksInFlight{ 0 };
	std::atomic<bool> m_streamRunning{ false };
	std::atomic<uint64_t> m_lastCallbackTick{ 0 };
	std::atomic<uint64_t> m_virtualSamplePosition{ 0 };
	std::mutex m_transitionMutex;
	std::vector<int32_t> m_inputScratch;   // interleaved stereo virtual-cable input, bufferSize * 2 samples
	std::array<AsioInputChannel, 8> m_inputChannels{};
	std::array<ASIOSampleType, 8> m_inputTypes{};
	// Format bridge (see CreateRealBuffers): set only while a non-Int32LSB real device is bound.
	std::atomic<bool> m_bridged{ false };
	std::vector<ASIOBufferInfo> m_realInfos;   // the real driver's own buffers
	std::vector<long> m_realTypes;             // the real driver's sample type per channel
	std::vector<void*> m_bridgeBuffers;        // the host's Int32 buffers while bridged
	AsioProxy::VirtualCableCapture m_virtualCable;

	void OnRealBufferSwitch(long index)
	{
		m_realCallbacksInFlight.fetch_add(1, std::memory_order_seq_cst);
		if (!m_realCallbacksEnabled.load(std::memory_order_seq_cst))
		{
			m_realCallbacksInFlight.fetch_sub(1, std::memory_order_release);
			return;
		}
		OnBufferSwitch(index);
		m_realCallbacksInFlight.fetch_sub(1, std::memory_order_release);
	}

	ASIOTime* OnRealBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct)
	{
		m_realCallbacksInFlight.fetch_add(1, std::memory_order_seq_cst);
		if (!m_realCallbacksEnabled.load(std::memory_order_seq_cst))
		{
			m_realCallbacksInFlight.fetch_sub(1, std::memory_order_release);
			return nullptr;
		}
		ASIOTime* result = OnBufferSwitchTimeInfo(params, index, direct);
		m_realCallbacksInFlight.fetch_sub(1, std::memory_order_release);
		return result;
	}
};

AsioProxyDriver* AsioProxyDriver::s_active = nullptr;
ASIOCallbacks AsioProxyDriver::s_ourCallbacks{};
std::atomic<long(*)(long, long, void*, double*)> AsioProxyDriver::s_hostAsioMessage{ nullptr };
std::atomic<void(*)(ASIOSampleRate)> AsioProxyDriver::s_hostSampleRateDidChange{ nullptr };

extern "C" __declspec(dllexport) int RSModsAsio_GetOutputMode()
{
	const AsioProxyDriver* active = AsioProxyDriver::s_active;
	return active ? active->OutputModeValue() : 0;
}

extern "C" __declspec(dllexport) int RSModsAsio_TryPromote(const wchar_t* driverName)
{
	AsioProxyDriver* active = AsioProxyDriver::s_active;
	if (!active) return 0;
	active->AddRef();
	const int result = active->TryPromote(driverName);
	active->Release();
	return result;
}

extern "C" __declspec(dllexport) int RSModsAsio_TryRebind(const wchar_t* driverName)
{
	AsioProxyDriver* active = AsioProxyDriver::s_active;
	if (!active) return 0;
	active->AddRef();
	const int result = active->TryRebind(driverName);
	active->Release();
	return result;
}

extern "C" __declspec(dllexport) int RSModsAsio_TryDemote()
{
	AsioProxyDriver* active = AsioProxyDriver::s_active;
	if (!active) return 0;
	active->AddRef();
	const int result = active->TryDemote();
	active->Release();
	return result;
}

extern "C" __declspec(dllexport) int RSModsAsio_GetInputMode()
{
	const AsioProxyDriver* active = AsioProxyDriver::s_active;
	return active ? active->InputModeValue() : 0;
}

// Virtual-mode cable input health: underruns (silent blocks), overflow drops, backlog skips, WASAPI
// discontinuities, current fill and drift correction. Out-array order matches the Stats fields.
extern "C" __declspec(dllexport) int RSModsAsio_GetVirtualInputStats(long* values, long count)
{
	const AsioProxyDriver* active = AsioProxyDriver::s_active;
	if (!active || !values || count <= 0) return 0;
	const auto stats = active->VirtualInputStats();
	const long all[] = { static_cast<long>(stats.underruns), static_cast<long>(stats.overflowDrops), static_cast<long>(stats.skips),
		static_cast<long>(stats.deviceGlitches), stats.fillFrames, stats.correctionPpm, stats.primed, stats.marginFrames };
	const long n = count < 8 ? count : 8;
	for (long i = 0; i < n; ++i) values[i] = all[i];
	return static_cast<int>(n);
}

extern "C" __declspec(dllexport) void RSModsAsio_SetVirtualCaptureTestMode(int enabled)
{
	g_virtualCaptureTestMode.store(enabled != 0, std::memory_order_release);
}

extern "C" __declspec(dllexport) void RSModsAsio_TestInjectInput(const int32_t* samples, long frames)
{
	AsioProxyDriver* active = AsioProxyDriver::s_active;
	if (active && frames > 0) active->InjectVirtualInput(samples, static_cast<uint32_t>(frames));
}

// ---- COM class factory + registration ----

class ProxyClassFactory : public IClassFactory
{
public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** o) override
	{
		if (!o) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IClassFactory) { *o = this; AddRef(); return S_OK; }
		*o = nullptr; return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return 2; }   // singleton, never freed
	ULONG STDMETHODCALLTYPE Release() override { return 1; }
	HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object) override
	{
		if (outer) return CLASS_E_NOAGGREGATION;
		// Only Rocksmith's copy owns the audio interface. Any other program (AmpliTube on the 64-bit build, a
		// DAW) gets the external amp link's guest driver, which runs on the game's clock instead of opening the
		// interface a second time.
		IAsioDriver* driver = AmpLink::IsGuestProcess() ? AmpLink::CreateGuestDriver(CLSID_RocksmithAudioBridge)
			: static_cast<IAsioDriver*>(new (std::nothrow) AsioProxyDriver());
		if (!driver) return E_OUTOFMEMORY;
		const HRESULT hr = driver->QueryInterface(riid, object);
		driver->Release();
		return hr;
	}
	HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { g_lockCount += lock ? 1 : -1; return S_OK; }
};

static ProxyClassFactory g_factory;

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID riid, void** object)
{
	if (clsid == CLSID_RocksmithAudioBridge) return g_factory.QueryInterface(riid, object);
	if (object) *object = nullptr;
	return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow() { return g_lockCount.load() == 0 ? S_OK : S_FALSE; }

static HMODULE g_module = nullptr;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) { g_module = module; DisableThreadLibraryCalls(module); }
	return TRUE;
}

namespace
{
	// The ASIO list name (what RS_ASIO.ini Driver= matches) must equal getDriverName() and
	// AsioProxySetup.ProxyName in the GUI, and fit ASIO's 32-byte driver name buffer.
	const wchar_t* kAsioName = L"Rocksmith Audio Bridge ASIO";
	const wchar_t* kClsidString = L"{7B2E5C10-9F3A-4D6B-A1C8-2E4F6A8B0D31}";

	LSTATUS SetValue(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* value)
	{
		HKEY key = nullptr;
		LSTATUS st = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
		if (st != ERROR_SUCCESS) return st;
		st = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
			static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
		RegCloseKey(key);
		return st;
	}
}

// Register machine-wide (HKLM): the COM server under Software\Classes\CLSID and the ASIO list entry under
// Software\ASIO. HKLM is required because ASIO hosts (RS_ASIO included) enumerate drivers ONLY from
// HKLM\Software\ASIO; a per-user (HKCU) entry is invisible to them. This needs elevation, so it is invoked
// via an elevated regsvr32 by the RSModsPlus setup, once per build: the 32-bit DLL with the 32-bit regsvr32
// (Rocksmith and RS_ASIO see it) and the 64-bit DLL, RocksmithAudioBridgeAsio64.dll, with the 64-bit
// regsvr32 (64-bit amp sims such as AmpliTube 5 see it; it is always the external amp link's guest). Each
// writes its own registry view. Selecting the driver (RS_ASIO.ini Driver=) and setting the Target real
// driver is done by the setup, not here.
extern "C" HRESULT STDAPICALLTYPE DllRegisterServer()
{
	wchar_t path[MAX_PATH]{};
	if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());

	std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidString;
	if (SetValue(HKEY_LOCAL_MACHINE, clsidKey.c_str(), nullptr, kAsioName) != ERROR_SUCCESS) return E_ACCESSDENIED;
	std::wstring inproc = clsidKey + L"\\InprocServer32";
	if (SetValue(HKEY_LOCAL_MACHINE, inproc.c_str(), nullptr, path) != ERROR_SUCCESS) return E_ACCESSDENIED;
	if (SetValue(HKEY_LOCAL_MACHINE, inproc.c_str(), L"ThreadingModel", L"Apartment") != ERROR_SUCCESS) return E_ACCESSDENIED;

	std::wstring asioKey = std::wstring(L"Software\\ASIO\\") + kAsioName;
	if (SetValue(HKEY_LOCAL_MACHINE, asioKey.c_str(), L"CLSID", kClsidString) != ERROR_SUCCESS) return E_ACCESSDENIED;
	if (SetValue(HKEY_LOCAL_MACHINE, asioKey.c_str(), L"Description", kAsioName) != ERROR_SUCCESS) return E_ACCESSDENIED;
	return S_OK;
}

extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer()
{
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"Software\\ASIO\\") + kAsioName).c_str());
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsidString).c_str());
	return S_OK;
}
