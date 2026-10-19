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
#include "SplitInput.hpp"
#include "AmpLinkHost.hpp"
#include "AmpLinkGuest.hpp"
#include "../Shared/ProductVersion.hpp"
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
// The Input page's noise suppressor threshold (linear, 0 = off) for the amp sim's input (AmpFeedConditioner.hpp).
// ExternalAmp.cpp sends it every tick.
extern "C" __declspec(dllexport) void RSModsAsio_ConfigureAmpFeed(float gateThreshold)
{
	g_ampLink.ConfigureConditioner(gateThreshold);
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
	bool AmpLinkHostEnabled()
	{
		return true;
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

	// Proxy log: written into the mod's RSMods_debug.txt through RSMods_BridgeDriverLog, exported by the mod
	// (xinput1_3.dll, in this DLL's folder), so a report needs one log, not two. RS_ASIO's log alone does not
	// show whether the real ASIO device bound at boot or which step refused; every bind step logs here. Cheap:
	// a few lines per stream open. Looked up per line by full path (the mod also loads the system xinput1_3.dll,
	// same base name), so a line written while the mod unloads is dropped, not sent to a stale address. The
	// driver is only meant to run inside Rocksmith: anywhere else the lines are dropped.
	using BridgeDriverLogFn = void(__cdecl*)(const char*);
	BridgeDriverLogFn FindModLog()
	{
		static const std::wstring modPath = ThisModuleDir() + L"\\xinput1_3.dll";
		const HMODULE mod = GetModuleHandleW(modPath.c_str());
		return mod ? reinterpret_cast<BridgeDriverLogFn>(GetProcAddress(mod, "RSMods_BridgeDriverLog")) : nullptr;
	}

	void ProxyLog(const char* format, ...)
	{
		const BridgeDriverLogFn log = FindModLog();
		if (!log) return;
		static std::atomic<bool> announced{ false };
		if (!announced.exchange(true))
		{
			char banner[128]{};
			_snprintf_s(banner, _TRUNCATE, "%s ASIO bridge driver loaded", ProductVersion::DISPLAY_NAME);
			log(banner);
		}
		char line[1024]{};
		va_list args;
		va_start(args, format);
		_vsnprintf_s(line, _TRUNCATE, format, args);
		va_end(args);
		log(line);
	}

	std::wstring ReadTargetDriverName()
	{
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

	// Why the wanted real device did not bind this launch. Without this a driver that refuses to open (the
	// device is busy in its own control app, a DAW, OBS) leaves the proxy silently on Virtual and the player
	// only sees "no input". Kept per process for the mod's overlay (RSModsAsio_GetBindError) and persisted
	// under HKCU so the settings app can say it after the game has closed. Written only from bind steps on
	// host / export threads, never from the audio callback.
	std::mutex g_bindErrorMutex;
	std::wstring g_bindErrorDriver;
	std::wstring g_bindErrorReason;

	std::wstring Widen(const std::string& text)
	{
		if (text.empty()) return std::wstring();
		const int n = MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
		std::wstring wide(static_cast<size_t>(n > 0 ? n : 0), L'\0');
		if (n > 0) MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), &wide[0], n);
		return wide;
	}

	void PersistBindError(const std::wstring& driver, const std::wstring& reason)
	{
		HKEY key = nullptr;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
			return;
		const auto setString = [key](const wchar_t* name, const std::wstring& value)
		{
			RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
				static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
		};
		setString(L"LastBindError", reason);
		setString(L"LastBindErrorDriver", driver);
		SYSTEMTIME now{};
		GetLocalTime(&now);
		wchar_t stamp[32]{};
		swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
		setString(L"LastBindErrorTime", stamp);
		RegCloseKey(key);
	}

	void ClearBindError()
	{
		{
			std::lock_guard<std::mutex> lock(g_bindErrorMutex);
			g_bindErrorDriver.clear();
			g_bindErrorReason.clear();
		}
		HKEY key = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
		RegDeleteValueW(key, L"LastBindError");
		RegDeleteValueW(key, L"LastBindErrorDriver");
		RegDeleteValueW(key, L"LastBindErrorTime");
		RegCloseKey(key);
	}

	// Channel routing. RS_ASIO asks the bridge for fixed channel numbers (RS_ASIO.ini Channel= /
	// BaseChannel=); the player picks the real channel per role in the overlay, and the bridge maps one onto the
	// other when it creates the real device's buffers. A change re-creates only those buffers (no driver reload)
	// and never touches RS_ASIO.ini. Saved per device under HKCU\Software\RSMods\AsioProxy\Routes\<driver>, one
	// DWORD per role; a missing value means "the channel RS_ASIO asked for", so an untouched setup is unchanged.
	enum RouteRole : int { RouteOutput = 0, RoutePlayer1 = 1, RoutePlayer2 = 2, RouteMic = 3, RouteRoleCount = 4 };
	const wchar_t* const kRouteValueNames[RouteRoleCount] = { L"Output", L"Player1", L"Player2", L"Mic" };
	const wchar_t* const kRouteIniSections[RouteRoleCount] = { L"Asio.Output", L"Asio.Input.0", L"Asio.Input.1", L"Asio.Input.Mic" };

	struct RouteRoleInfo { bool viaBridge = false; long hostChannel = 0; };

	// Whether this RS_ASIO.ini section points at the bridge, and the channel it asks for (the output's is the first
	// of its stereo pair). Read per call: the file is tiny and only read on bind and on overlay polls.
	RouteRoleInfo ReadRouteRole(int role)
	{
		RouteRoleInfo info;
		const std::wstring ini = ThisModuleDir() + L"\\RS_ASIO.ini";
		wchar_t driver[256]{};
		GetPrivateProfileStringW(kRouteIniSections[role], L"Driver", L"", driver, 256, ini.c_str());
		info.viaBridge = _wcsicmp(driver, L"Rocksmith Audio Bridge ASIO") == 0 || _wcsicmp(driver, L"Rocksmith Audio Bridge") == 0;
		info.hostChannel = static_cast<long>(GetPrivateProfileIntW(kRouteIniSections[role], role == RouteOutput ? L"BaseChannel" : L"Channel", 0, ini.c_str()));
		if (info.hostChannel < 0) info.hostChannel = 0;
		return info;
	}

	std::wstring RoutesKey(std::wstring driver)
	{
		std::replace(driver.begin(), driver.end(), L'\\', L'_');
		return L"Software\\RSMods\\AsioProxy\\Routes\\" + driver;
	}

	// The saved real channel for a role on this device, or -1 when none is saved.
	long ReadSavedRoute(const std::wstring& driver, int role)
	{
		if (driver.empty()) return -1;
		HKEY key = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, RoutesKey(driver).c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return -1;
		DWORD value = 0, size = sizeof(value), type = 0;
		const LSTATUS status = RegQueryValueExW(key, kRouteValueNames[role], nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
		RegCloseKey(key);
		return status == ERROR_SUCCESS && type == REG_DWORD && value < 1024 ? static_cast<long>(value) : -1;
	}

	// channel < 0 forgets the role's route (back to the channel RS_ASIO asks for).
	bool WriteSavedRoute(const std::wstring& driver, int role, long channel)
	{
		if (driver.empty()) return false;
		HKEY key = nullptr;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, RoutesKey(driver).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
			return false;
		LSTATUS status = ERROR_SUCCESS;
		if (channel < 0) RegDeleteValueW(key, kRouteValueNames[role]);
		else
		{
			const DWORD value = static_cast<DWORD>(channel);
			status = RegSetValueExW(key, kRouteValueNames[role], 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
		}
		RegCloseKey(key);
		return status == ERROR_SUCCESS;
	}

	// The roles and saved routes for one bind, and the host channel -> real channel rule built from them.
	struct RouteTable
	{
		RouteRoleInfo roles[RouteRoleCount];
		long saved[RouteRoleCount]{ -1, -1, -1, -1 };

		static RouteTable Load(const std::wstring& driver)
		{
			RouteTable table;
			for (int role = 0; role < RouteRoleCount; ++role)
			{
				table.roles[role] = ReadRouteRole(role);
				table.saved[role] = ReadSavedRoute(driver, role);
			}
			return table;
		}

		// The real channel for a host channel; the host channel itself when no role routes it. A host channel two
		// roles share (both ask for the same Channel=) follows the first of them.
		long Map(bool isInput, long hostChannel) const
		{
			if (!isInput)
			{
				const RouteRoleInfo& out = roles[RouteOutput];
				if (out.viaBridge && saved[RouteOutput] >= 0 && hostChannel >= out.hostChannel && hostChannel <= out.hostChannel + 1)
					return saved[RouteOutput] + (hostChannel - out.hostChannel);
				return hostChannel;
			}
			for (int role = RoutePlayer1; role < RouteRoleCount; ++role)
				if (roles[role].viaBridge && saved[role] >= 0 && hostChannel == roles[role].hostChannel) return saved[role];
			return hostChannel;
		}
	};

	// Real channel names of the bound device, for the overlay's pickers. Filled at each bind, emptied on Virtual.
	std::mutex g_channelNamesMutex;
	std::vector<std::string> g_inputNames;
	std::vector<std::string> g_roleInputNames[RouteRoleCount];   // per input role: its interface's input names
	std::vector<std::string> g_outputNames;
	std::wstring g_boundTarget;   // the bound device's driver name, empty while Virtual

	// The device whose routes the overlay reads and writes: the bound one, else the configured Target.
	std::wstring CurrentRouteTarget()
	{
		{
			std::lock_guard<std::mutex> lock(g_channelNamesMutex);
			if (!g_boundTarget.empty()) return g_boundTarget;
		}
		return ReadTargetDriverName();
	}

	// Input interfaces (SplitInput.hpp), per input role: HKCU AsioProxy\InputTarget.Player1 / .Player2 / .Mic names
	// the ASIO interface that role's input comes from; the plain InputTarget value is the default for every role that
	// has none of its own (and was the only setting before per-role interfaces). Empty, or the output interface,
	// means the role's input comes from the output interface. So the bridge just routes: each input from any
	// interface and channel, the output to any interface and pair, the way RS_ASIO.ini itself can.
	constexpr const wchar_t* kSameAsOutput = L"(output)";
	std::wstring InputTargetValueName(int role) { return role == RoutePlayer1 || role == RoutePlayer2 || role == RouteMic ? std::wstring(L"InputTarget.") + kRouteValueNames[role] : L"InputTarget"; }

	std::wstring ReadRegistryString(const wchar_t* name)
	{
		// RRF_RT_REG_SZ: only a string, always terminated; anything else (missing, another type, too long) reads as empty.
		wchar_t buffer[512]{};
		DWORD size = sizeof(buffer);
		if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return std::wstring();
		return buffer;
	}

	// The role's own setting; else, for Player 2 and the mic, Player 1's (an unset second player is on the same
	// interface as the first); else the shared default.
	std::wstring ReadInputTargetDriverName(int role)
	{
		const std::wstring own = ReadRegistryString(InputTargetValueName(role).c_str());
		if (!own.empty()) return own;
		if (role == RoutePlayer2 || role == RouteMic)
		{
			const std::wstring first = ReadRegistryString(InputTargetValueName(RoutePlayer1).c_str());
			if (!first.empty()) return first;
		}
		return ReadRegistryString(L"InputTarget");
	}

	// role < 0 sets the shared default and clears every role's own setting (one interface for all inputs).
	bool WriteInputTargetDriverName(int role, const std::wstring& driver)
	{
		HKEY key = nullptr;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
		const std::wstring name = InputTargetValueName(role);
		// A role set to "same as output" must still override a shared default, so it is stored as the marker
		// kSameAsOutput rather than deleted (not as the output's name: that would turn into a real input interface
		// once the output moved to another interface).
		const std::wstring value = driver.empty() && role >= 0 ? std::wstring(kSameAsOutput) : driver;
		const LSTATUS status = value.empty() ? RegDeleteValueW(key, name.c_str())
			: RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
		if (role < 0)
			for (int each : { RoutePlayer1, RoutePlayer2, RouteMic }) RegDeleteValueW(key, InputTargetValueName(each).c_str());
		RegCloseKey(key);
		return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
	}

	// The input interface to open for `role` beside `outputTarget`, or empty when it comes from the output interface.
	std::wstring SplitInputTarget(int role, const std::wstring& outputTarget)
	{
		const std::wstring input = ReadInputTargetDriverName(role);
		return input.empty() || input == kSameAsOutput || _wcsicmp(input.c_str(), outputTarget.c_str()) == 0 ? std::wstring() : input;
	}

	// Why the input interface would not open (one line), shown on the overlay; the guitar then comes from the output
	// interface. Cleared when it opens or is set back to "same as output".
	std::string WideToUtf8(const std::wstring& text)
	{
		if (text.empty()) return std::string();
		const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
		if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
		return out;
	}

	std::mutex g_inputErrorMutex;
	std::string g_inputError;
	void SetInputError(const std::string& text) { std::lock_guard<std::mutex> lock(g_inputErrorMutex); g_inputError = text; }

	// ---- Last known good setup ------------------------------------------------------------------
	// A broken ASIO driver can take the whole game down (Realtek ASIO corrupts the heap when opened input-only, proven
	// outside the game), and nothing in-process can catch that. So the bridge never boots twice into a setup that
	// crashed: before any interface opens it saves the exact setup it is trying as "Pending"; ten seconds of clean
	// running (or a clean exit) promote it to "LastGood" and clear Pending (a setup that fails to open without crashing
	// just clears Pending). At boot, a saved setup equal to Pending crashed last time: the last good one comes back, or
	// with none (or when that one is the one that crashed) nothing opens until the player picks an interface. A setup
	// changed in the settings app while the game was closed no longer matches Pending, so it is tried normally.
	// Chosen over testing drivers in a helper process: far less code, and a crash points at the player's own setup.
	constexpr const wchar_t* kSetupValues[] = { L"Target", L"InputTarget", L"InputTarget.Player1", L"InputTarget.Player2", L"InputTarget.Mic" };
	std::mutex g_safeBootMutex;
	std::wstring g_attemptSetup;           // the setup being tried, until it is confirmed
	uint64_t g_attemptTick = 0;            // GetTickCount64 when it started; 0 = nothing pending from this process
	std::string g_safeBootNotice;          // shown on the overlay's Playing to card

	void WriteRegistryString(const wchar_t* name, const std::wstring& value)
	{
		HKEY key = nullptr;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
		if (value.empty()) RegDeleteValueW(key, name);
		else RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
		RegCloseKey(key);
	}

	// The saved setup as one string (values joined by newlines); `target` replaces the output when not empty.
	std::wstring CurrentSetup(const std::wstring& target = std::wstring())
	{
		std::wstring setup;
		for (const wchar_t* name : kSetupValues)
		{
			const bool isTarget = wcscmp(name, L"Target") == 0;
			setup += (isTarget && !target.empty() ? target : ReadRegistryString(name)) + L"\n";
		}
		return setup;
	}

	void RestoreSetup(const std::wstring& setup)
	{
		size_t start = 0;
		for (const wchar_t* name : kSetupValues)
		{
			const size_t end = setup.find(L'\n', start);
			WriteRegistryString(name, setup.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
			start = end == std::wstring::npos ? setup.size() : end + 1;
		}
	}

	// Marks `setup` as being tried now (before any driver for it loads).
	void BeginSetupAttempt(const std::wstring& setup)
	{
		std::lock_guard<std::mutex> lock(g_safeBootMutex);
		g_attemptSetup = setup;
		g_attemptTick = GetTickCount64();
		WriteRegistryString(L"Pending", setup);
	}

	// The device whose routes an input role uses: its input interface when it has one, else the output's.
	std::wstring CurrentInputRouteTarget(int role)
	{
		const std::wstring output = CurrentRouteTarget();
		const std::wstring input = SplitInputTarget(role, output);
		return input.empty() ? output : input;
	}

	// The input role a host input channel belongs to: the RS_ASIO.ini section that asks the bridge for that channel
	// (Player 1 first when two share one, as RouteTable::Map does). A channel no section names counts as Player 1.
	int InputRoleOfHostChannel(long hostChannel)
	{
		for (int role : { RoutePlayer1, RoutePlayer2, RouteMic })
		{
			const RouteRoleInfo info = ReadRouteRole(role);
			if (info.viaBridge && info.hostChannel == hostChannel) return role;
		}
		return RoutePlayer1;
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

	// Boot only, once per process: whether this launch may open the saved setup (see "Last known good setup").
	static bool SafeBootAllowsReal()
	{
		static int decided = -1;
		if (decided >= 0) return decided == 1;
		const std::wstring setup = CurrentSetup();
		const std::wstring pending = ReadRegistryString(L"Pending");
		if (!pending.empty() && pending == setup)
		{
			const std::wstring lastGood = ReadRegistryString(L"LastGood");
			if (!lastGood.empty() && lastGood != setup)
			{
				RestoreSetup(lastGood);
				ProxyLog("SafeBoot: the saved setup did not survive its last launch; back on the last one that worked");
				std::lock_guard<std::mutex> lock(g_safeBootMutex);
				g_safeBootNotice = "The game closed unexpectedly with the last setup, so the last one that worked is back.";
			}
			else
			{
				if (!lastGood.empty()) WriteRegistryString(L"LastGood", std::wstring());   // it crashed too
				ProxyLog("SafeBoot: the saved setup did not survive its last launch and nothing else is known to work; opening no interface");
				std::lock_guard<std::mutex> lock(g_safeBootMutex);
				g_safeBootNotice = "The game closed unexpectedly with the saved setup, so no interface was opened. Pick one in Switch output.";
				decided = 0;
				return false;   // Pending stays: the next launch holds too until the setup changes
			}
		}
		BeginSetupAttempt(CurrentSetup());
		StartSafeBootWatch();
		decided = 1;
		return true;
	}

	// A live change in the overlay: `target` is the output being tried (empty = the saved one).
	static void BeginLiveAttempt(const std::wstring& target)
	{
		BeginSetupAttempt(CurrentSetup(target));
		{
			std::lock_guard<std::mutex> lock(g_safeBootMutex);
			g_safeBootNotice.clear();
		}
		StartSafeBootWatch();
	}

	// Ends the attempt being tried; `worked` saves it as the last good one.
	static void FinishSetupAttempt(bool worked)
	{
		std::lock_guard<std::mutex> lock(g_safeBootMutex);
		if (!g_attemptTick) return;
		if (worked) WriteRegistryString(L"LastGood", g_attemptSetup);
		WriteRegistryString(L"Pending", std::wstring());
		ProxyLog("SafeBoot: setup %s", worked ? "confirmed working; saved as the last good one" : "did not open, but did not crash either");
		g_attemptTick = 0;
	}

	static void StartSafeBootWatch()
	{
		static std::atomic<bool> started{ false };
		if (started.exchange(true)) return;
		// The thread lives as long as the process, so this module must never unload under it.
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&StartSafeBootWatch), &self);
		std::thread([] {
			for (;;)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				uint64_t begun = 0;
				{
					std::lock_guard<std::mutex> lock(g_safeBootMutex);
					begun = g_attemptTick;
				}
				if (!begun || GetTickCount64() - begun < 10000) continue;
				// Held while asked, as the RSModsAsio_Try* exports do: the host may release the driver meanwhile.
				AsioProxyDriver* active = s_active;
				if (active) active->AddRef();
				const bool running = active && active->RunningReal();
				if (active) active->Release();
				FinishSetupAttempt(running);
			}
		}).detach();
	}

	// Real, and its callbacks arrived in the last second.
	bool RunningReal() const
	{
		const uint64_t last = m_lastCallbackTick.load(std::memory_order_acquire);
		return IsReal() && last != 0 && GetTickCount64() - last < 1000;
	}
	AsioProxyDriver()
	{
		m_virtualCable.SetTestMode(g_virtualCaptureTestMode.load(std::memory_order_acquire));
		m_virtualCable.SetLog(&ProxyLog);
		for (auto& slot : m_splitInputs) slot.store(new AsioProxy::SplitInput(), std::memory_order_release);   // never allocated on the audio thread
	}
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
	int Reroute() { return RerouteReal() ? 1 : 0; }
	// The input that runs on another clock for `role` and how well it is being kept in time: 1 its input interface,
	// 2 the Real Tone Cable (speaker mode), 0 none (it shares the output's clock, so there is nothing to correct).
	int InputClock(int role, AsioProxy::VirtualCableCapture::Stats& stats) const
	{
		const long code = role >= 0 && role < RouteRoleCount ? m_roleSplit[static_cast<size_t>(role)] : 0;
		const long slots = static_cast<long>(AsioProxy::SplitInput::kMaxChannels);
		if (code < 0 && (-1 - code) / slots < static_cast<long>(m_splitInputs.size()))
		{
			const auto& input = Split(static_cast<size_t>((-1 - code) / slots));
			if (input.IsOpen()) { stats = input.Stats(static_cast<size_t>((-1 - code) % slots)); return 1; }
		}
		if (m_mode.load(std::memory_order_acquire) == OutputMode::Virtual && m_virtualCable.HasPhysicalCapture()) { stats = m_virtualCable.GetStats(); return 2; }
		return 0;
	}
	bool IsReal() const { return m_mode.load(std::memory_order_acquire) == OutputMode::Real; }
	std::wstring BoundTarget() const { return m_bindTarget; }
	~AsioProxyDriver()
	{
		StopVirtual();
		FreeVirtualBuffers();
		if (IsReal() && m_callbackSeen.load(std::memory_order_acquire)) FinishSetupAttempt(true);   // closed cleanly while running: it worked
		DisableRealCallbacksAndWait();   // a host that releases without stop/dispose must not race the bridge free
		FreeBridge();
		if (s_active == this) s_active = nullptr;
		for (auto& slot : m_splitInputs)
		{
			AsioProxy::SplitInput* input = slot.exchange(nullptr);
			if (input && !input->IsQuarantined()) delete input;   // a quarantined one may still be inside its callback
		}
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
	// RS_ASIO checks RS_ASIO.ini's Channel= / BaseChannel= against these counts while the proxy is still Virtual
	// (discovery runs before createBuffers). When the real device is going to be bound, answer with its own
	// channels, as getBufferSize does, so a Channel= above 1 is accepted; speaker mode alone has two of each.
	ASIOError getChannels(long* in, long* out) override
	{
		if (m_mode.load() == OutputMode::Virtual)
		{
			long realIn = 0, realOut = 0;
			if (ProbeRealBeforeBind() && m_real->getChannels(&realIn, &realOut) == 0)
			{
				if (in) *in = (std::max)(realIn, 2L);
				if (out) *out = (std::max)(realOut, 2L);
				return 0;
			}
			if (in) *in = 2; if (out) *out = 2;
			return 0;
		}
		return m_real ? m_real->getChannels(in, out) : -1;
	}
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
	ASIOError getChannelInfo(ASIOChannelInfo* info) override { if (m_mode.load() == OutputMode::Virtual && info && info->channel > 1 && ProbeRealBeforeBind()) { const ASIOError e = m_real->getChannelInfo(info); if (e == 0) info->type = ASIOSTInt32LSB; return e; } if (m_mode.load() == OutputMode::Virtual) { if (!info || info->channel < 0 || info->channel > 1) return -1; info->isActive = 1; info->channelGroup = 0; info->type = ASIOSTInt32LSB; strcpy_s(info->name, 32, info->isInput ? (info->channel == 0 ? "Virtual Capture Left" : "Virtual Capture Right") : (info->channel == 0 ? "Virtual Left" : "Virtual Right")); return 0; } if (!m_real) return -1; const ASIOError e = m_real->getChannelInfo(info); if (e == 0 && info) info->type = ASIOSTInt32LSB; return e; }
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
			const bool preferReal = PreferRealOutput() && SafeBootAllowsReal();
			ProxyLog("createBuffers: %ld channel(s) (%ld in, %ld out), %ld frames; PreferReal=%d",
				numChannels, inputs, numChannels - inputs, bufferSize, preferReal ? 1 : 0);
			// Stay virtual unless the user prefers the real device AND it binds cleanly. A device that already
			// refused to load or init during the buffer-size query this boot is not retried (saves ~0.4 s).
			const bool knownAbsent = m_realProbeFailed;
			m_realProbeFailed = false;
			if (!knownAbsent) m_bindFailure.clear();   // a probe refusal keeps its reason for the report below
			if (knownAbsent && preferReal) ProxyLog("createBuffers: real device unavailable at the buffer-size query; not retrying");
			if (!preferReal || knownAbsent || BindReal(std::wstring(), bufferInfos, numChannels, bufferSize) != 0)
			{
				if (preferReal) PublishBindFailure();
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
		m_bindFailure.clear();
		const ASIOError e = CreateRealBuffers(bufferInfos, numChannels, bufferSize);
		if (e == 0) { CompleteRealBind(bufferInfos, numChannels, bufferSize); return 0; }   // ASE_OK
		// The device went away between the host's dispose and re-create (e.g. unplugged). Staying Real with no
		// buffers and no s_active would make every RSModsAsio_Try* export refuse until a restart.
		ProxyLog("createBuffers: re-creating on the real device failed (%ld); falling back to Virtual", e);
		if (m_bindFailure.empty()) NoteBindFailure("it stopped accepting buffers, error %ld (was it unplugged?)", e);
		PublishBindFailure();
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

	void ActivateVirtual() { m_mode.store(OutputMode::Virtual); m_callbackSeen.store(false); m_lastCallbackTick.store(0); m_virtualSamplePosition.store(0); m_sampleRate = 48000.0; CacheChannelNames(); }

	// The bound device's channel names for the overlay's pickers; none while Virtual.
	void CacheChannelNames()
	{
		std::vector<std::string> inputs, outputs;
		long in = 0, out = 0;
		if (m_real && m_mode.load(std::memory_order_acquire) == OutputMode::Real && m_real->getChannels(&in, &out) == 0)
		{
			for (int isInput = 0; isInput < 2; ++isInput)
			{
				const long count = (std::min)(isInput ? in : out, 256L);
				auto& names = isInput ? inputs : outputs;
				for (long channel = 0; channel < count; ++channel)
				{
					ASIOChannelInfo info{};
					info.channel = channel;
					info.isInput = isInput;
					const bool named = m_real->getChannelInfo(&info) == 0;
					info.name[sizeof(info.name) - 1] = '\0';
					names.push_back(named && info.name[0] != '\0' ? std::string(info.name) : (isInput ? "In " : "Out ") + std::to_string(channel + 1));
				}
			}
		}
		// Each input role's picker lists its own interface's channels: an input interface's when it has one open.
		std::vector<std::string> roleNames[RouteRoleCount];
		const long slots = static_cast<long>(AsioProxy::SplitInput::kMaxChannels);
		for (int role = RoutePlayer1; role < RouteRoleCount; ++role)
		{
			const long code = m_roleSplit[static_cast<size_t>(role)];
			const bool split = code < 0 && (-1 - code) / slots < static_cast<long>(m_splitInputs.size()) && Split(static_cast<size_t>((-1 - code) / slots)).IsOpen();
			roleNames[role] = split ? Split(static_cast<size_t>((-1 - code) / slots)).ChannelNames() : inputs;
		}
		std::lock_guard<std::mutex> lock(g_channelNamesMutex);
		for (int role = 0; role < RouteRoleCount; ++role) g_roleInputNames[role] = std::move(roleNames[role]);
		g_inputNames = std::move(inputs);
		g_outputNames = std::move(outputs);
		g_boundTarget = m_real && m_mode.load(std::memory_order_acquire) == OutputMode::Real ? m_bindTarget : std::wstring();
	}

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
				NoteInitRefused(message);
				ReleaseReal();
				return -1;
			}
			ProxyLog("BindReal: real driver loaded and initialised");
		}
		EnforceRealSampleRate();
		m_bindFailure.clear();   // so a format refusal noted inside CreateRealBuffers is kept below
		const ASIOError e = CreateRealBuffers(infos, numChannels, bufferSize);
		if (e != 0)
		{
			char message[128]{};
			m_real->getErrorMessage(message);
			long minimum = 0, maximum = 0, preferred = 0, granularity = 0;
			m_real->getBufferSize(&minimum, &maximum, &preferred, &granularity);
			ProxyLog("BindReal: real createBuffers(%ld channels, %ld frames) -> %ld (%s); driver allows %ld..%ld preferred %ld granularity %ld",
				numChannels, bufferSize, e, message, minimum, maximum, preferred, granularity);
			// A format refusal noted by CreateRealBuffers is the real reason; otherwise use the driver's text.
			if (m_bindFailure.empty())
			{
				if (message[0] != '\0') NoteBindFailure("it refused %ld frame buffers: %s", bufferSize, message);
				else NoteBindFailure("it refused %ld frame buffers, error %ld", bufferSize, e);
			}
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
	//
	// Channel routing uses the same split: when the player's routes send a host channel to a different real
	// channel (RouteTable), the real driver gets buffers for the routed channels and each callback copies between
	// them. Two host channels routed to the same real input share one real buffer. With no routes and an
	// Int32LSB device the host keeps the driver's own buffers, exactly as before routing existed. A routed bind
	// the driver refuses is retried once on the channels RS_ASIO asked for, so a bad route never costs the sound.
	ASIOError CreateRealBuffers(ASIOBufferInfo* infos, long numChannels, long bufferSize)
	{
		bool routed = false;
		const ASIOError e = CreateRealBuffersRouted(infos, numChannels, bufferSize, true, routed);
		if (e == 0 || !routed) return e;
		ProxyLog("BindReal: the routed channels were refused (%ld); retrying on the channels RS_ASIO asked for", e);
		return CreateRealBuffersRouted(infos, numChannels, bufferSize, false, routed);
	}

	ASIOError CreateRealBuffersRouted(ASIOBufferInfo* infos, long numChannels, long bufferSize, bool useRoutes, bool& routed)
	{
		FreeBridge();
		routed = false;
		const std::wstring outputTarget = m_bindTarget.empty() ? ReadTargetDriverName() : m_bindTarget;
		const RouteTable routes = useRoutes ? RouteTable::Load(outputTarget) : RouteTable{};
		long realInputs = 0, realOutputs = 0;
		m_real->getChannels(&realInputs, &realOutputs);
		std::vector<ASIOBufferInfo> real;
		std::vector<long> types;
		std::vector<long> hostToReal(static_cast<size_t>(numChannels), 0);
		bool convert = false;

		// Input interfaces (SplitInput.hpp), per input role: each host input belongs to the RS_ASIO.ini section that asks
		// for its channel (Player 1, Player 2, mic), and that role's input interface, if it has one, supplies it. Each
		// distinct interface is one SplitInput (up to three), each distinct channel on it one capture slot; a host input
		// served that way maps to -1 - (device * kMaxChannels + slot) instead of a real buffer. An interface that will
		// not open leaves its roles on the output interface as before, with the reason shown on the overlay.
		constexpr long kSlots = static_cast<long>(AsioProxy::SplitInput::kMaxChannels);
		std::vector<long> hostSplit(static_cast<size_t>(numChannels), 0);   // 0 = output interface, else the code above
		m_roleSplit.fill(0);
		{
			struct Pending { std::wstring driver; std::vector<long> channels; std::vector<long> hosts; };
			std::vector<Pending> wanted;
			for (long i = 0; i < numChannels; ++i)
			{
				if (!infos[i].isInput) continue;
				const int role = InputRoleOfHostChannel(infos[i].channelNum);
				const std::wstring driver = SplitInputTarget(role, outputTarget);
				if (driver.empty()) continue;
				auto at = std::find_if(wanted.begin(), wanted.end(), [&](const Pending& p) { return _wcsicmp(p.driver.c_str(), driver.c_str()) == 0; });
				if (at == wanted.end())
				{
					if (wanted.size() >= m_splitInputs.size()) continue;   // more than three interfaces: the rest stay on the output's
					wanted.push_back(Pending{ driver, {}, {} });
					at = wanted.end() - 1;
				}
				const RouteTable inputRoutes = useRoutes ? RouteTable::Load(driver) : RouteTable{};
				const long channel = inputRoutes.Map(true, infos[i].channelNum);
				if (std::find(at->channels.begin(), at->channels.end(), channel) == at->channels.end() && at->channels.size() < static_cast<size_t>(kSlots))
					at->channels.push_back(channel);
				at->hosts.push_back(i);
			}
			std::string errors;
			for (size_t device = 0; device < wanted.size(); ++device)
			{
				const Pending& pending = wanted[device];
				std::wstring dllPath; CLSID clsid{};
				std::string failure;
				if (!ResolveAsioDriver(pending.driver, dllPath, clsid)) failure = "it is not installed as an ASIO driver";
				else if (Split(device).Open(pending.driver, dllPath, clsid, m_sysHandle, pending.channels, &ProxyLog, failure)) failure.clear();
				if (!Split(device).IsOpen())
				{
					errors += (errors.empty() ? "" : " ") + WideToUtf8(pending.driver) + " would not open as an input interface: " + failure + ". Its inputs come from the output interface.";
					ProxyLog("BindReal: input interface '%ls' not used: %s", pending.driver.c_str(), failure.c_str());
					continue;
				}
				const RouteTable inputRoutes = useRoutes ? RouteTable::Load(pending.driver) : RouteTable{};
				for (long host : pending.hosts)
				{
					const long channel = inputRoutes.Map(true, infos[host].channelNum);
					const auto slot = std::find(pending.channels.begin(), pending.channels.end(), channel);
					const long slotIndex = slot == pending.channels.end() ? 0 : static_cast<long>(slot - pending.channels.begin());
					hostSplit[static_cast<size_t>(host)] = -1 - (static_cast<long>(device) * kSlots + slotIndex);
					const int role = InputRoleOfHostChannel(infos[host].channelNum);
					if (m_roleSplit[static_cast<size_t>(role)] == 0) m_roleSplit[static_cast<size_t>(role)] = hostSplit[static_cast<size_t>(host)];
					ProxyLog("BindReal: host input channel %ld -> '%ls' channel %ld", infos[host].channelNum, pending.driver.c_str(), channel);
				}
			}
			SetInputError(errors);
		}

		for (long i = 0; i < numChannels; ++i)
		{
			const bool isInput = infos[i].isInput != 0;
			const long hostChannel = infos[i].channelNum;
			if (isInput && hostSplit[static_cast<size_t>(i)] < 0)
			{
				hostToReal[static_cast<size_t>(i)] = hostSplit[static_cast<size_t>(i)];
				routed = true;
				continue;
			}
			// A route past the device's channels (another device, fewer channels) falls back to the channel RS_ASIO
			// asked for, then wraps onto the device, so it plays something instead of refusing.
			const long count = isInput ? realInputs : realOutputs;
			long channel = routes.Map(isInput, hostChannel);
			if (count > 0 && (channel < 0 || channel >= count)) channel = hostChannel < count ? hostChannel : hostChannel % count;
			if (channel != hostChannel)
			{
				routed = true;
				ProxyLog("BindReal: host %s channel %ld -> device channel %ld", isInput ? "input" : "output", hostChannel, channel);
			}
			long slot = -1;
			for (size_t r = 0; r < real.size(); ++r)
				if ((real[r].isInput != 0) == isInput && real[r].channelNum == channel) { slot = static_cast<long>(r); break; }
			if (slot >= 0) { routed = true; hostToReal[static_cast<size_t>(i)] = slot; continue; }

			ASIOChannelInfo info{};
			info.channel = channel;
			info.isInput = isInput ? 1 : 0;
			if (m_real->getChannelInfo(&info) != 0)
			{
				ProxyLog("BindReal: the driver would not report the format of %s channel %ld; staying Virtual rather than guess",
					isInput ? "input" : "output", channel);
				NoteBindFailure("it would not report the audio format of %s channel %ld", isInput ? "input" : "output", channel);
				return -1;
			}
			if (!BridgeableType(info.type))
			{
				ProxyLog("BindReal: %s channel %ld uses ASIO sample type %ld, which the proxy cannot convert; staying Virtual",
					isInput ? "input" : "output", channel, info.type);
				NoteBindFailure("it uses audio format %ld, which the bridge cannot convert", info.type);
				return -1;
			}
			if (info.type != ASIOSTInt32LSB) convert = true;
			ASIOBufferInfo realInfo{};
			realInfo.isInput = isInput ? 1 : 0;
			realInfo.channelNum = channel;
			hostToReal[static_cast<size_t>(i)] = static_cast<long>(real.size());
			real.push_back(realInfo);
			types.push_back(info.type);
		}
		if (!convert && !routed) return m_real->createBuffers(infos, numChannels, bufferSize, &s_ourCallbacks);

		m_realInfos = std::move(real);
		const ASIOError e = m_real->createBuffers(m_realInfos.data(), static_cast<long>(m_realInfos.size()), bufferSize, &s_ourCallbacks);
		if (e != 0) { m_realInfos.clear(); CloseSplitInputs(); return e; }
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
		m_hostToReal = std::move(hostToReal);
		m_bridged.store(true, std::memory_order_release);
		if (convert)
			ProxyLog("BindReal: device sample type %ld (first channel) is not Int32LSB; converting to and from the host's Int32LSB",
				m_realTypes.empty() ? -1L : m_realTypes[0]);
		if (routed) ProxyLog("BindReal: %ld host channel(s) routed onto %ld device channel(s)", numChannels, static_cast<long>(m_realInfos.size()));
		return 0;
	}

	// Only called with the real callbacks disabled (or never enabled) for these buffers.
	void FreeBridge()
	{
		m_bridged.store(false, std::memory_order_release);
		CloseSplitInputs();
		m_roleSplit.fill(0);
		for (void* buffer : m_bridgeBuffers) free(buffer);
		m_bridgeBuffers.clear();
		m_realInfos.clear();
		m_realTypes.clear();
		m_hostToReal.clear();
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
		for (size_t device = 0; device < m_splitInputs.size(); ++device) Split(device).BeginBlock();   // each input-interface channel is drained once per block, however many players share it
		for (long i = 0; i < m_numChannels && i < static_cast<long>(m_hostToReal.size()); ++i)
		{
			if (!m_bufferInfos[i].isInput) continue;
			const long r = m_hostToReal[static_cast<size_t>(i)];
			auto* dst = static_cast<int32_t*>(m_bufferInfos[i].buffers[index]);
			if (r < 0)   // an input interface: -1 - (device * kMaxChannels + slot)
			{
				const long code = -1 - r;
				const long slots = static_cast<long>(AsioProxy::SplitInput::kMaxChannels);
				if (dst && code / slots < static_cast<long>(m_splitInputs.size()))
					Split(static_cast<size_t>(code / slots)).Read(static_cast<size_t>(code % slots), dst, static_cast<uint32_t>(m_bufferSize));
				continue;
			}
			const auto* src = static_cast<const BYTE*>(m_realInfos[static_cast<size_t>(r)].buffers[index]);
			if (src && dst) RealToHost(src, dst, m_realTypes[static_cast<size_t>(r)], m_bufferSize);
		}
	}

	void BridgeOutputs(long index) noexcept
	{
		if (!m_bridged.load(std::memory_order_acquire) || !m_bufferInfos || m_bufferSize <= 0) return;
		for (long i = 0; i < m_numChannels && i < static_cast<long>(m_hostToReal.size()); ++i)
		{
			if (m_bufferInfos[i].isInput) continue;
			const long r = m_hostToReal[static_cast<size_t>(i)];
			const auto* src = static_cast<const int32_t*>(m_bufferInfos[i].buffers[index]);
			auto* dst = static_cast<BYTE*>(m_realInfos[static_cast<size_t>(r)].buffers[index]);
			if (src && dst) HostToReal(src, dst, m_realTypes[static_cast<size_t>(r)], m_bufferSize);
		}
	}

	// Load + init the real driver early, at the host's buffer-size query, so getBufferSize can report the
	// hardware's limits. Only before the first createBuffers (no buffers yet) and only when the real device
	// is wanted. BindReal then reuses the already-initialised driver. A failure is remembered so
	// createBuffers does not pay for a second attempt in the same boot.
	bool ProbeRealBeforeBind()
	{
		if (m_real) return !m_bufferInfos;
		if (m_bufferInfos || m_realProbeFailed || !PreferRealOutput() || !SafeBootAllowsReal()) return false;
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
			NoteInitRefused(message);
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
		// Bound: a refusal recorded on an earlier launch (or earlier this launch) no longer applies.
		m_bindFailure.clear();
		ClearBindError();
		CacheInputChannels();
		CacheOutputChannels();
		CacheChannelNames();
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
	// Bind failure reporting (see g_bindErrorMutex). Each refusing step notes a short reason; the caller that
	// decides "staying Virtual" publishes it, so the overlay and settings app can say why the guitar is silent.
	void NoteBindFailure(const char* format, ...)
	{
		char text[256]{};
		va_list args;
		va_start(args, format);
		_vsnprintf_s(text, _TRUNCATE, format, args);
		va_end(args);
		m_bindFailure = text;
	}

	// The driver's own getErrorMessage text (e.g. "ASIO Driver open Failure!") is the most useful part.
	void NoteInitRefused(const char* driverMessage)
	{
		if (driverMessage && driverMessage[0] != '\0') NoteBindFailure("it refused to start: %s", driverMessage);
		else NoteBindFailure("it refused to start");
	}

	void PublishBindFailure()
	{
		const std::wstring driver = m_bindTarget.empty() ? ReadTargetDriverName() : m_bindTarget;
		const std::string reason = m_bindFailure.empty() ? std::string("it would not start") : m_bindFailure;
		m_bindFailure.clear();
		// It refused without taking the game down: not a crash, so the next launch must not hold this setup back
		// (a refusal shortly before the game was closed would leave Pending set, and the next boot would open nothing).
		FinishSetupAttempt(false);
		if (driver.empty()) return;   // no Target set: the settings app already reports "not set up"
		ProxyLog("ERROR: guitar input and output are silent because '%ls' would not open: %s", driver.c_str(), reason.c_str());
		const std::wstring wideReason = Widen(reason);
		{
			std::lock_guard<std::mutex> lock(g_bindErrorMutex);
			g_bindErrorDriver = driver;
			g_bindErrorReason = wideReason;
		}
		PersistBindError(driver, wideReason);
	}

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
		// Any channel number is taken: RS_ASIO may have asked for a real device's channel 3 at discovery and then
		// fallen back here (device refused, unplugged, demoted). Even channels play the left side, odd the right.
		long inputs = 0, outputs = 0;
		for (long i = 0; i < numChannels; ++i)
		{
			if (infos[i].channelNum < 0) return -1;
			for (long j = 0; j < i; ++j)
				if (infos[j].isInput == infos[i].isInput && infos[j].channelNum == infos[i].channelNum) return -1;
			if (infos[i].isInput) ++inputs; else ++outputs;
		}
		if (inputs > static_cast<long>(m_inputTypes.size()) || outputs > kMaxOutputs) return -1;
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
				// By buffer index, as CacheInputChannels: past the table when outputs come first in the list.
				if (i < static_cast<long>(m_inputTypes.size())) m_inputTypes[i] = ASIOSTInt32LSB;
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
		// Each refusal is published like a boot-time one (RSModsAsio_GetBindError), so the overlay's interface switch
		// can say why before it falls back; the bind that follows clears it again (CompleteRealBind).
		m_bindFailure.clear();
		BeginLiveAttempt(driverName);
		if (!LoadReal(driverName)) { ReleaseReal(); PublishBindFailure(); return false; }
		if (!m_real->init(m_sysHandle))
		{
			char message[128]{};
			m_real->getErrorMessage(message);
			ProxyLog("TryPromote: the real driver refused init: %s", message);
			NoteInitRefused(message);
			ReleaseReal();
			PublishBindFailure();
			return false;
		}
		if (wasRunning) StopVirtual();
		m_virtualCable.Stop();
		FreeVirtualBuffers();
		for (long i = 0; i < m_numChannels; ++i) m_bufferInfos[i].buffers[0] = m_bufferInfos[i].buffers[1] = nullptr;
		InstallTrampolines();
		if (BindReal(driverName, m_bufferInfos, m_numChannels, m_bufferSize) != 0) { PublishBindFailure(); return RestoreVirtual(wasRunning); }
		CompleteRealBind(m_bufferInfos, m_numChannels, m_bufferSize);
		m_realCallbacksEnabled.store(true, std::memory_order_release);
		m_callbackSeen.store(false);
		if (!wasRunning) return true;
		if (m_real->start() == 0)
		{
			// Up to a second for the first callback: ASIO4ALL (kernel streaming) and some USB drivers take a few hundred
			// ms to start, and 50 ms rejected ASIO4ALL as "never started playing".
			for (int attempt = 0; attempt < 100 && !m_callbackSeen.load(std::memory_order_acquire); ++attempt)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			if (m_callbackSeen.load(std::memory_order_acquire)) { ProxyLog("TryPromote: real device running; mode Real"); return true; }
		}
		ProxyLog("TryPromote: real driver bound but never called back; back to Virtual");
		NoteBindFailure("it opened but never started playing");
		PublishBindFailure();
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
			BeginLiveAttempt(driverName);
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

	// A route changed in the overlay: re-create only the real device's buffers on the new channels. The driver
	// stays loaded and initialised and the host keeps its stream, so the player hears a gap of a few buffers,
	// not a device reload. Refused routed channels fall back inside CreateRealBuffers; a device that refuses
	// even those is dropped to Virtual like an unplug, with the reason in the bind error.
	bool RerouteReal()
	{
		std::lock_guard<std::mutex> transition(m_transitionMutex);
		if (m_mode.load(std::memory_order_acquire) != OutputMode::Real || !m_real || !m_bufferInfos) return false;
		const bool wasRunning = m_streamRunning.load(std::memory_order_acquire);
		ProxyLog("Reroute: re-creating the device's buffers on the new channels (stream %s)", wasRunning ? "running" : "stopped");
		DisableRealCallbacksAndWait();
		if (wasRunning) m_real->stop();
		m_real->disposeBuffers();
		FreeBridge();
		m_bindFailure.clear();
		if (CreateRealBuffers(m_bufferInfos, m_numChannels, m_bufferSize) == 0)
		{
			CompleteRealBind(m_bufferInfos, m_numChannels, m_bufferSize);
			m_realCallbacksEnabled.store(true, std::memory_order_release);
			if (!wasRunning || m_real->start() == 0) { ProxyLog("Reroute: done"); return true; }
			ProxyLog("Reroute: the device would not start again");
			DisableRealCallbacksAndWait();
			m_real->disposeBuffers();
			FreeBridge();
			if (m_bindFailure.empty()) NoteBindFailure("it would not start again after a channel change");
		}
		else if (m_bindFailure.empty()) NoteBindFailure("it refused the new channels");
		PublishBindFailure();
		ReleaseReal();
		for (long i = 0; i < m_numChannels; ++i) m_bufferInfos[i].buffers[0] = m_bufferInfos[i].buffers[1] = nullptr;
		ActivateVirtual();
		if (CreateVirtualBuffers(m_bufferInfos, m_numChannels, m_bufferSize, &m_hostCallbacks) == 0 && wasRunning)
		{
			m_virtualCable.Start();
			StartVirtual();
		}
		return false;
	}

	// The output stalled. If an input interface opened just before, it most likely stopped the output (Realtek ASIO
	// opened as Player 1's input can take the sound chip ASIO4ALL is playing through, and the output goes quiet
	// seconds later). Its players go back to the output interface, saved, so the next launch does not repeat the clash, and
	// the overlay says why.
	void BlameRecentInputInterface()
	{
		static const char* const kRoleLabels[RouteRoleCount] = { "Output", "Player 1", "Player 2", "Mic" };
		const uint64_t now = GetTickCount64();
		const long slots = static_cast<long>(AsioProxy::SplitInput::kMaxChannels);
		std::string message;
		for (size_t device = 0; device < m_splitInputs.size(); ++device)
		{
			AsioProxy::SplitInput& input = Split(device);
			if (!input.IsOpen() || input.OpenedTick() == 0 || now - input.OpenedTick() > 10000) continue;
			std::string players;
			for (int role = 0; role < RouteRoleCount; ++role)
			{
				const long code = m_roleSplit[static_cast<size_t>(role)];
				if (code >= 0 || static_cast<size_t>((-1 - code) / slots) != device) continue;
				WriteInputTargetDriverName(role, std::wstring());   // same as output
				players += (players.empty() ? "" : " and ") + std::string(kRoleLabels[role]);
			}
			const std::string name = WideToUtf8(input.DriverName());
			ProxyLog("Output stalled %llu ms after input interface '%ls' opened; %s set back to the output interface",
				static_cast<unsigned long long>(now - input.OpenedTick()), input.DriverName().c_str(), players.empty() ? "no role" : players.c_str());
			message += (message.empty() ? "" : " ") + std::string("The output stopped right after ") + name + " opened as an input; they probably share the same sound hardware. "
				+ (players.empty() ? std::string("Its inputs") : players) + " now come from the output interface.";
		}
		if (!message.empty()) SetInputError(message);
	}

	bool TryDemoteToVirtual()
	{
		std::lock_guard<std::mutex> transition(m_transitionMutex);
		if (m_mode.load(std::memory_order_acquire) != OutputMode::Real || !m_real || !m_bufferInfos) return false;
		const bool wasRunning = m_streamRunning.exchange(false, std::memory_order_acq_rel);
		// A stalled ASIO driver may be inside stop/dispose or may invoke a callback concurrently.
		// Disable the trampoline first, detach both handles, and quarantine the driver/module.
		DisableRealCallbacksAndWait();
		BlameRecentInputInterface();
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
		m_bindTarget = targetDriver;   // named in the bind error if a later step refuses
		if (!ResolveAsioDriver(targetDriver, dllPath, clsid))
		{
			ProxyLog("LoadReal: no ASIO registry entry for '%ls'", targetDriver.c_str());
			NoteBindFailure("it is not installed as an ASIO driver (reinstall its driver)");
			return false;
		}
		m_realModule = LoadLibraryW(dllPath.c_str());
		// Pinned: an ASIO driver's own threads can outlive Release() (a USB worker waking late), and once its DLL is
		// unmapped they run into freed code and take the game down (an execute fault after switching interfaces live).
		// Pinned, every FreeLibrary below is only a reference count; the code stays
		// mapped until the game exits. A driver loaded again reuses the same mapping.
		HMODULE pinned = nullptr;
		if (m_realModule) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(m_realModule), &pinned);
		if (!m_realModule)
		{
			const DWORD error = GetLastError();
			ProxyLog("LoadReal: LoadLibrary failed for '%ls' (error %lu)", dllPath.c_str(), error);
			NoteBindFailure("its driver file would not load, Windows error %lu", error);
			return false;
		}
		using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
		auto dllGetClassObject = reinterpret_cast<GetClassObject>(GetProcAddress(m_realModule, "DllGetClassObject"));
		if (!dllGetClassObject)
		{
			ProxyLog("LoadReal: '%ls' exports no DllGetClassObject", dllPath.c_str());
			NoteBindFailure("its driver file is not a working ASIO driver");
			return false;
		}
		IClassFactory* factory = nullptr;
		if (FAILED(dllGetClassObject(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory))) || !factory)
		{
			ProxyLog("LoadReal: '%ls' gave no class factory", dllPath.c_str());
			NoteBindFailure("its driver file is not a working ASIO driver");
			return false;
		}
		const HRESULT hr = factory->CreateInstance(nullptr, clsid, reinterpret_cast<void**>(&m_real));
		factory->Release();
		if (FAILED(hr) || !m_real)
		{
			ProxyLog("LoadReal: CreateInstance for '%ls' failed 0x%08lX", targetDriver.c_str(), static_cast<unsigned long>(hr));
			NoteBindFailure("the driver could not be created, error 0x%08lX", static_cast<unsigned long>(hr));
		}
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
			const long sourceChannel = m_bufferInfos[channel].channelNum & 1;
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
			// kAsioResetRequest: the interface changed under us (ASIO4ALL after a device is switched on or off in its
			// panel, a buffer size changed in a control panel). RS_ASIO does not reopen anything for it, so the bridge
			// kept playing into the old channel list ("Not Connected 1 / 2", silence). The bridge reopens
			// the same interface itself, the live switch path, off this driver thread.
			if (selector == 3) { ScheduleResetRebind(); return 1; }
		}
		return host(selector, value, message, opt);
	}
	static void ScheduleResetRebind()
	{
		static std::atomic<bool> pending{ false };
		if (pending.exchange(true)) return;
		HMODULE self = nullptr;   // the thread may outlive a host release: never let this module unload under it
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&ScheduleResetRebind), &self);
		std::thread([] {
			std::this_thread::sleep_for(std::chrono::milliseconds(300));   // drivers often send several in a row
			pending.store(false);
			AsioProxyDriver* active = s_active;
			if (!active || !active->IsReal()) return;
			active->AddRef();
			const std::wstring target = active->BoundTarget();
			ProxyLog("Reset request from '%ls': reopening it with its new settings", target.c_str());
			// A driver is often still resetting when asked to open again (some refuse to start right after their own
			// reset request): try again after 1 s and 3 s before leaving it to the player.
			bool reopened = !target.empty() && active->TryRebind(target.c_str()) != 0;
			for (int wait : { 1000, 3000 })
			{
				if (reopened || target.empty()) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(wait));
				ProxyLog("Reset request: '%ls' would not open yet; trying again", target.c_str());
				reopened = active->TryRebind(target.c_str()) != 0;
			}
			active->Release();
		}).detach();
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
	std::wstring m_bindTarget;        // the real driver the last LoadReal tried
	std::string m_bindFailure;        // why the last bind step refused, user-readable; empty when none
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
	std::vector<long> m_realTypes;             // the real driver's sample type per real buffer
	std::vector<long> m_hostToReal;            // host buffer index -> m_realInfos index (channel routing); -1 - slot = input interface
	// Input interfaces, open only while set and bound. Owned by pointer so one whose driver stays wedged in its callback
	// can be abandoned (leaked, never freed under that callback) and replaced; see CloseSplitInputs.
	std::array<std::atomic<AsioProxy::SplitInput*>, AsioProxy::SplitInput::kMaxDevices> m_splitInputs{};
	AsioProxy::SplitInput& Split(size_t device) const
	{
		AsioProxy::SplitInput* input = m_splitInputs[device].load(std::memory_order_acquire);
		if (!input)
		{
			// First use: create it (only the bind/transition path reaches here with no object yet).
			auto* created = new AsioProxy::SplitInput();
			AsioProxy::SplitInput* expected = nullptr;
			if (const_cast<std::atomic<AsioProxy::SplitInput*>&>(m_splitInputs[device]).compare_exchange_strong(expected, created)) input = created;
			else { delete created; input = expected; }
		}
		return *input;
	}
	void CloseSplitInputs()
	{
		for (size_t device = 0; device < m_splitInputs.size(); ++device)
		{
			AsioProxy::SplitInput* input = m_splitInputs[device].load(std::memory_order_acquire);
			if (!input || input->Close()) continue;
			// Quarantined: abandon it (never deleted) and give the slot a fresh object.
			m_splitInputs[device].store(new AsioProxy::SplitInput(), std::memory_order_release);
		}
	}
	std::array<long, RouteRoleCount> m_roleSplit{};   // per input role: its input-interface code (-1 - device * slots - slot), 0 = output's
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

// Why the real device would not open this launch, as one user-readable line ("<driver> would not open: <reason>.
// Guitar input and output are silent."). Returns 1 and fills buffer when a failure is recorded, else 0.
extern "C" __declspec(dllexport) int RSModsAsio_GetBindError(wchar_t* buffer, int chars)
{
	std::lock_guard<std::mutex> lock(g_bindErrorMutex);
	if (g_bindErrorDriver.empty()) return 0;
	if (buffer && chars > 0)
		_snwprintf_s(buffer, static_cast<size_t>(chars), _TRUNCATE, L"%ls would not open: %ls. Guitar input and output are silent.",
			g_bindErrorDriver.c_str(), g_bindErrorReason.c_str());
	return 1;
}

// Channel routing, for the overlay's per-role channel pickers. Roles: 0 output (the first channel
// of its stereo pair), 1 Player 1, 2 Player 2, 3 Mic. Channels are 0-based device channels.

// Returns 1 when RS_ASIO.ini sends the role through the bridge, else 0. Fills the channel RS_ASIO asks for and the
// device channel the role plays on (its saved route, else that same channel).
extern "C" __declspec(dllexport) int RSModsAsio_GetRoute(int role, long* hostChannel, long* deviceChannel)
{
	if (role < 0 || role >= RouteRoleCount) return 0;
	const RouteRoleInfo info = ReadRouteRole(role);
	const long saved = ReadSavedRoute(role == RouteOutput ? CurrentRouteTarget() : CurrentInputRouteTarget(role), role);
	if (hostChannel) *hostChannel = info.hostChannel;
	if (deviceChannel) *deviceChannel = saved >= 0 ? saved : info.hostChannel;
	return info.viaBridge ? 1 : 0;
}

// The bound device's input (isInput 1) or output (0) channel count; 0 while the bridge is on speaker mode.
extern "C" __declspec(dllexport) int RSModsAsio_GetChannelCount(int isInput)
{
	std::lock_guard<std::mutex> lock(g_channelNamesMutex);
	return static_cast<int>(isInput ? g_inputNames.size() : g_outputNames.size());
}

// The device's own name for a channel (UTF-8 as the driver reports it). Returns 1 when filled.
extern "C" __declspec(dllexport) int RSModsAsio_GetChannelName(int isInput, long channel, char* buffer, int chars)
{
	if (!buffer || chars <= 0) return 0;
	std::lock_guard<std::mutex> lock(g_channelNamesMutex);
	const auto& names = isInput ? g_inputNames : g_outputNames;
	if (channel < 0 || channel >= static_cast<long>(names.size())) return 0;
	strncpy_s(buffer, static_cast<size_t>(chars), names[static_cast<size_t>(channel)].c_str(), _TRUNCATE);
	return 1;
}

// Saves the role's device channel for the current device (-1 = back to the channel RS_ASIO asks for) and applies
// it now. Returns 1 when applied live, 2 when saved for the next bind (speaker mode), 0 on failure (a device that
// refused the change drops to speaker mode with the reason in RSModsAsio_GetBindError). Re-creating the device's
// buffers takes a moment: call it off the render thread.
extern "C" __declspec(dllexport) int RSModsAsio_SetRoute(int role, long deviceChannel)
{
	if (role < 0 || role >= RouteRoleCount) return 0;
	const std::wstring target = role == RouteOutput ? CurrentRouteTarget() : CurrentInputRouteTarget(role);
	if (!WriteSavedRoute(target, role, deviceChannel < 0 ? -1 : deviceChannel)) return 0;
	ProxyLog("SetRoute: '%ls' %ls -> device channel %ld", target.c_str(), kRouteValueNames[role], deviceChannel);
	AsioProxyDriver* active = AsioProxyDriver::s_active;
	if (!active || !active->IsReal()) return 2;
	active->AddRef();
	const int result = active->Reroute();
	active->Release();
	return result;
}

// Input interfaces (SplitInput.hpp), per input role: 1 Player 1, 2 Player 2, 3 Mic. The role's input interface
// name, or "" when its input comes from the output interface. Returns 1 when it has one.
extern "C" __declspec(dllexport) int RSModsAsio_GetRoleInputTarget(int role, wchar_t* buffer, int chars)
{
	if (role < RoutePlayer1 || role >= RouteRoleCount) return 0;
	const std::wstring input = SplitInputTarget(role, CurrentRouteTarget());
	if (buffer && chars > 0) wcsncpy_s(buffer, static_cast<size_t>(chars), input.c_str(), _TRUNCATE);
	return input.empty() ? 0 : 1;
}

// Saves a role's input interface (null or "" = the output interface; role -1 = every input role) and applies it now
// by re-creating the bound interface's buffers. 1 applied live, 2 saved for the next bind (speaker mode), 0 failed.
// Whether it actually opened is RSModsAsio_GetInputError. Blocks for the re-bind: call it off the render thread.
extern "C" __declspec(dllexport) int RSModsAsio_SetRoleInputTarget(int role, const wchar_t* driver)
{
	if (role != -1 && (role < RoutePlayer1 || role >= RouteRoleCount)) return 0;
	const std::wstring name = driver ? driver : L"";
	if (!WriteInputTargetDriverName(role, name)) return 0;
	ProxyLog("SetInputTarget: %ls -> '%ls'", role < 0 ? L"every input" : kRouteValueNames[role], name.empty() ? L"(same as output)" : name.c_str());
	AsioProxyDriver* active = AsioProxyDriver::s_active;
	if (!active || !active->IsReal()) return 2;
	AsioProxyDriver::BeginLiveAttempt(std::wstring());
	active->AddRef();
	const int result = active->Reroute();
	active->Release();
	return result;
}

// What the last-known-good guard did at boot (UTF-8, one line), for the overlay's Playing to card. Returns 1 with text.
extern "C" __declspec(dllexport) int RSModsAsio_GetSafeBootNotice(char* buffer, int chars)
{
	std::lock_guard<std::mutex> lock(g_safeBootMutex);
	if (g_safeBootNotice.empty()) return 0;
	if (buffer && chars > 0) strncpy_s(buffer, static_cast<size_t>(chars), g_safeBootNotice.c_str(), _TRUNCATE);
	return 1;
}

// Why an input interface would not open this time (UTF-8, one line per interface). Returns 1 and fills the buffer
// when there is one.
extern "C" __declspec(dllexport) int RSModsAsio_GetInputError(char* buffer, int chars)
{
	std::lock_guard<std::mutex> lock(g_inputErrorMutex);
	if (g_inputError.empty()) return 0;
	if (buffer && chars > 0) strncpy_s(buffer, static_cast<size_t>(chars), g_inputError.c_str(), _TRUNCATE);
	return 1;
}

// How a role's input on another clock is kept in time, for the overlay's drift pill. Returns 1 for an input
// interface, 2 for the Real Tone Cable, 0 when it shares the output's clock (nothing to correct). ppm: the correction
// applied now; fill and margin in frames; underruns since it opened; primed 1 while it is delivering sound.
extern "C" __declspec(dllexport) int RSModsAsio_GetRoleInputClock(int role, int* ppm, int* fill, int* margin, unsigned* underruns, int* primed)
{
	AsioProxyDriver* active = AsioProxyDriver::s_active;
	if (!active) return 0;
	AsioProxy::VirtualCableCapture::Stats stats{};
	const int source = active->InputClock(role, stats);
	if (ppm) *ppm = stats.correctionPpm;
	if (fill) *fill = stats.fillFrames;
	if (margin) *margin = stats.marginFrames;
	if (underruns) *underruns = stats.underruns;
	if (primed) *primed = stats.primed;
	return source;
}

// A role's input channel names (its own interface's), for the overlay's per-role pickers.
extern "C" __declspec(dllexport) int RSModsAsio_GetRoleChannelCount(int role)
{
	if (role < 0 || role >= RouteRoleCount) return 0;
	std::lock_guard<std::mutex> lock(g_channelNamesMutex);
	return static_cast<int>(role == RouteOutput ? g_outputNames.size() : g_roleInputNames[role].size());
}

extern "C" __declspec(dllexport) int RSModsAsio_GetRoleChannelName(int role, long channel, char* buffer, int chars)
{
	if (!buffer || chars <= 0 || role < 0 || role >= RouteRoleCount) return 0;
	std::lock_guard<std::mutex> lock(g_channelNamesMutex);
	const auto& names = role == RouteOutput ? g_outputNames : g_roleInputNames[role];
	if (channel < 0 || channel >= static_cast<long>(names.size())) return 0;
	strncpy_s(buffer, static_cast<size_t>(chars), names[static_cast<size_t>(channel)].c_str(), _TRUNCATE);
	return 1;
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
