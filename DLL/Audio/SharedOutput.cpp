#include "stdafx.h"
#include "SharedOutput.hpp"
#include "AsioBufferState.hpp"
#include "AudioPacketQueue.hpp"
#include "GameAudioRecorder.hpp"
#include "DrySignalRecording.hpp"
#include "AudioControl.hpp"
#include "OutputTap.hpp"
#include "OutputDevices.hpp"
#include "PersistentInput.hpp"
#include "AsioHook.hpp"
#include "CableInput.hpp"
#include "../AsioProxy/LatencyDsp.h"
#include "../Mods/VolumeControl.hpp"
#include "../Mods/RocksmithGate.hpp"
#include "../Mods/Enumeration.hpp"
#include <vector>
#include <avrt.h>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <array>
#include "OutputBackend.hpp"
#include <wrl/client.h>
#include <functiondiscoverykeys_devpkey.h>

#pragma comment(lib, "avrt.lib")

namespace Settings
{
	bool SetNoteByNoteDetectionVisible(bool enabled);
}

namespace Audio::SharedOutput
{
	namespace
	{
		// Set for the duration of one DispatchControlPreview on the calling thread: the op applies live but writes
		// nothing to RSMods.ini (the overlay's sliders preview every drag step and save once on release).
		thread_local bool previewOnly = false;

		HRESULT PersistCustomSetting(const char* name, long value)
		{
			if (previewOnly) return S_OK;
			char executablePath[MAX_PATH]{};
			if (!GetModuleFileNameA(nullptr, executablePath, MAX_PATH))
				return HRESULT_FROM_WIN32(GetLastError());
			const auto iniPath = (std::filesystem::path(executablePath).parent_path() / "RSMods.ini").string();
			const std::string text = std::to_string(value);
			if (!WritePrivateProfileStringA("Mod Settings", name, text.c_str(), iniPath.c_str()))
				return HRESULT_FROM_WIN32(GetLastError());
			return S_OK;
		}

		HRESULT PersistStringSetting(const char* section, const char* name, const char* value)
		{
			if (previewOnly) return S_OK;
			char executablePath[MAX_PATH]{};
			if (!GetModuleFileNameA(nullptr, executablePath, MAX_PATH))
				return HRESULT_FROM_WIN32(GetLastError());
			const auto iniPath = (std::filesystem::path(executablePath).parent_path() / "RSMods.ini").string();
			if (!WritePrivateProfileStringA(section, name, value, iniPath.c_str()))
				return HRESULT_FROM_WIN32(GetLastError());
			return S_OK;
		}

		// Latency round-trip result, shared by both control handlers (a class member would not be visible to
		// the free-function passthrough handler). g_latencyProbeLen is captured when a measure is armed (op 17).
		int g_latencyProbeLen = 0;
		void WriteLatencyResult(ControlResponse& response)
		{
			if (!AsioHook::IsLatencyCaptureDone()) { wcsncpy_s(response.file, L"measuring", _TRUNCATE); return; }
			const float* cap = nullptr;
			const int filled = AsioHook::GetLatencyCapture(&cap);
			const int refLen = g_latencyProbeLen > 0 ? g_latencyProbeLen : 256;
			const int maxLag = filled - refLen;
			LatencyDsp::Match m{ -1, 0.0f };
			if (cap && maxLag > 0)
			{
				std::vector<float> ref(refLen);
				LatencyDsp::GenerateProbe(ref.data(), refLen);
				m = LatencyDsp::FindLag(ref.data(), refLen, cap, filled, maxLag);
			}
			wchar_t buf[128]{};
			if (m.lag >= 0 && m.confidence >= 0.5f)
				swprintf_s(buf, L"ms=%.2f;conf=%.2f", LatencyDsp::LagToMilliseconds(m.lag, 48000.0), m.confidence);
			else
				swprintf_s(buf, L"no_signal;conf=%.2f", m.confidence);
			wcsncpy_s(response.file, buf, _TRUNCATE);
		}
	}

	UINT32 ReadSavedPeriod(const std::wstring& endpointId)
	{
		wchar_t executable[MAX_PATH]{};
		GetModuleFileNameW(nullptr, executable, MAX_PATH);
		const auto path = std::filesystem::path(executable).parent_path() / L"RSMods.ini";
		return GetPrivateProfileIntW((L"Audio Bridge Output Buffer " + endpointId).c_str(), L"PeriodFrames", 0, path.c_str());
	}

	bool IsEnginePeriod(UINT32 value, UINT32 minimum, UINT32 fundamental, UINT32 maximum)
	{
		return fundamental && value >= minimum && value <= maximum && value % fundamental == 0;
	}

	bool ReadContainer(IMMDevice* endpoint, GUID& container)
	{
		Microsoft::WRL::ComPtr<IPropertyStore> properties;
		if (!endpoint || FAILED(endpoint->OpenPropertyStore(STGM_READ, &properties))) return false;
		PROPVARIANT value{};
		const HRESULT result = properties->GetValue(PKEY_Device_ContainerId, &value);
		const bool valid = SUCCEEDED(result) && value.vt == VT_CLSID && value.puuid && *value.puuid != GUID_NULL;
		if (valid) container = *value.puuid;
		PropVariantClear(&value);
		return valid;
	}

	HRESULT OpenEndpoint(std::wstring& endpointId, IAudioClient3** client)
	{
		using Microsoft::WRL::ComPtr;
		ComPtr<IMMDeviceEnumerator> enumerator;
		HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
		if (FAILED(result)) return result;
		ComPtr<IMMDevice> endpoint;
		result = endpointId.empty() ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint)
			: enumerator->GetDevice(endpointId.c_str(), &endpoint);
		if (FAILED(result)) return result;
		ComPtr<IMMEndpoint> direction;
		result = endpoint.As(&direction);
		EDataFlow flow = eAll;
		if (SUCCEEDED(result)) result = direction->GetDataFlow(&flow);
		if (FAILED(result)) return result;
		if (flow != eRender) return E_INVALIDARG;
		DWORD state = 0;
		result = endpoint->GetState(&state);
		if (FAILED(result)) return result;
		if (!(state & DEVICE_STATE_ACTIVE))
		{
			GUID container{};
			if (!ReadContainer(endpoint.Get(), container)) return AUDCLNT_E_DEVICE_INVALIDATED;
			ComPtr<IMMDeviceCollection> candidates;
			result = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &candidates);
			if (FAILED(result)) return result;
			UINT count = 0;
			result = candidates->GetCount(&count);
			if (FAILED(result)) return result;
			ComPtr<IMMDevice> match;
			for (UINT index = 0; index < count; ++index)
			{
				ComPtr<IMMDevice> candidate;
				GUID identity{};
				if (FAILED(candidates->Item(index, &candidate)) || !ReadContainer(candidate.Get(), identity) || identity != container) continue;
				if (match) return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
				match = candidate;
			}
			if (!match) return AUDCLNT_E_DEVICE_INVALIDATED;
			endpoint = match;
		}
		LPWSTR resolved = nullptr;
		result = endpoint->GetId(&resolved);
		if (FAILED(result)) return result;
		endpointId = resolved;
		CoTaskMemFree(resolved);
		return endpoint->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client));
	}

	bool IsGameFormat(const WAVEFORMATEX* format)
	{
		if (!format || format->nChannels != 2 || format->nSamplesPerSec != 48000) return false;
		WORD tag = format->wFormatTag;
		if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22)
		{
			const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
			if (extended->dwChannelMask != 0 && extended->dwChannelMask != (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)) return false;
			if (extended->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
			else if (extended->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
			else return false;
			if (extended->Samples.wValidBitsPerSample != format->wBitsPerSample) return false;
		}
		return ((tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32)
			|| (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16))
			&& format->nBlockAlign == format->nChannels * format->wBitsPerSample / 8
			&& format->nAvgBytesPerSec == format->nSamplesPerSec * format->nBlockAlign;
	}

	// Control ops whose handling is the same whether or not an owned output session exists. They are
	// parsed and dispatched ONCE here; OutputSession::HandleControl and PassthroughStatus only add the
	// ops that genuinely differ (recording 2/3/14, switching the game's own output 4/6, alternate-device
	// routing 21/22). The single shared op whose TARGET differs is the loudness guard (16): the session
	// applies it in its owned backend, passthrough in the proxy driver, so the caller passes that in.
	// Returns true when the op was recognised and response.result is set.
	HRESULT RouteStop();
	// True while op 25 binds a virtual proxy to the real interface; RefreshProxyOutput stands aside meanwhile so it
	// cannot re-create a managed route in the gap between the route stop and the bind.
	std::atomic<bool> g_proxyPromotionRunning{ false };

	static bool HandleSharedControl(const ControlRequest& request, ControlResponse& response,
		const std::function<bool(bool limiterOn, float ceilingLin, bool agcOn, float targetRms)>& configureGuard)
	{
		// Signed integer, optionally newline-terminated (ini-style values the GUI relays as-is).
		const auto parseInteger = [&request](long& out) -> bool
		{
			wchar_t* end = nullptr;
			out = wcstol(request.value, &end, 10);
			return end != request.value && (*end == 0 || *end == L'\n');
		};
		long integer = 0;
		switch (request.operation)
		{
		case 7: case 8: case 9: case 10: case 11: case 12: case 13:
		{
			// Playback mixer bus (op - 7) volume, digits only, 0-100. The mixer is the game's own bus
			// volumes (VolumeControl), so it is live with or without routing.
			unsigned int volume = 0;
			bool valid = request.value[0] != 0;
			for (const wchar_t* digit = request.value; *digit && valid; ++digit)
			{
				valid = *digit >= L'0' && *digit <= L'9';
				if (valid) volume = volume * 10 + (*digit - L'0');
				valid = valid && volume <= 100;
			}
			response.result = !valid ? E_INVALIDARG
				: VolumeControl::SetPlaybackVolume(request.operation - 7, static_cast<float>(volume)) ? S_OK : E_FAIL;
			return true;
		}
		case 15:
			// Guitar input make-up gain, signed tenths of a dB ("60" = +6.0 dB), as RSMods.ini stores it.
			if (!parseInteger(integer) || integer < -240 || integer > 240) response.result = E_INVALIDARG;
			else
			{
				AsioHook::SetInputGainDb(static_cast<float>(integer) / 10.0f);
				response.result = PersistCustomSetting("AsioInputGain", integer);
			}
			return true;
		case 16:
		{
			// Output loudness guard: "<limiterOn>,<ceiling>,<agcOn>,<target>" (on flags 0/1; ceiling and
			// target linear 0..1). A look-ahead brickwall limiter that holds the ceiling plus a slow loudness
			// AGC that equalises song-to-song. Malformed = E_INVALIDARG; no target to apply it to = NOT_READY.
			wchar_t* e = nullptr;
			const long limOn = wcstol(request.value, &e, 10);
			const bool hasCeiling = e && *e == L',';
			const double ceiling = hasCeiling ? wcstod(e + 1, &e) : 1.0;
			const bool hasAgc = e && *e == L',';
			const long agcOn = hasAgc ? wcstol(e + 1, &e, 10) : 0;
			const bool hasTarget = e && *e == L',';
			const double target = hasTarget ? wcstod(e + 1, &e) : 0.10;
			const bool valid = hasCeiling && hasAgc && hasTarget && e && (*e == 0 || *e == L'\n')
				&& (limOn == 0 || limOn == 1) && (agcOn == 0 || agcOn == 1);
			if (!valid) response.result = E_INVALIDARG;
			else
			{
				const HRESULT guard = configureGuard(limOn != 0, static_cast<float>(ceiling), agcOn != 0, static_cast<float>(target))
					? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_READY);
				// The ceiling cap is a deliberate saved state, like the guitar-input controls, so write it back to
				// survive a relaunch. Ceiling and target arrive linear; store them as tenths of a dBFS, the inverse
				// of the pow(10, tenths/200) the load path uses (ConfigureSavedOutputGuard / HandleAudioBridgeLimiter).
				// Persist even when the proxy is not loaded yet (NOT_READY) so the preference still sticks and the
				// saved guard is applied once the ASIO proxy comes up.
				const long ceilingTenths = ceiling > 0.0 ? std::lround(200.0 * std::log10(ceiling)) : -60;
				const long targetTenths = target > 0.0 ? std::lround(200.0 * std::log10(target)) : -200;
				HRESULT persist = PersistCustomSetting("AudioBridgeLimiter", limOn);
				if (SUCCEEDED(persist)) persist = PersistCustomSetting("AudioBridgeLimiterLevel", ceilingTenths);
				if (SUCCEEDED(persist)) persist = PersistCustomSetting("AudioBridgeLoudnessMatch", agcOn);
				if (SUCCEEDED(persist)) persist = PersistCustomSetting("AudioBridgeLoudnessTarget", targetTenths);
				response.result = FAILED(guard) ? guard : persist;
			}
			return true;
		}
		case 17:
			// Start a round-trip latency measurement: capture ~0.25 s of input, then inject the probe on
			// output. The loopback (physical out->in) carries the probe back into the capture.
			AsioHook::StartLatencyCapture(12000);
			g_latencyProbeLen = OutputTap::ArmLatencyProbe();
			response.result = g_latencyProbeLen > 0 ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_READY);
			return true;
		case 18:
			response.result = S_OK;   // the latency result is written into response.file after the common fill
			return true;
		case 19:
			// Guitar input suppressor open threshold, signed tenths of a dB ("-600" = -60 dB); 0 or above = off.
			if (!parseInteger(integer) || (integer != 0 && (integer < -900 || integer > -200))) response.result = E_INVALIDARG;
			else
			{
				AsioHook::SetNoiseGateThresholdDb(static_cast<float>(integer) / 10.0f);
				response.result = PersistCustomSetting("NoiseGateThreshold", integer);
			}
			return true;
		case 20:
			// Guitar input compressor strength, integer 0-100 (0 = off).
			if (!parseInteger(integer) || integer < 0 || integer > 100) response.result = E_INVALIDARG;
			else
			{
				AsioHook::SetCompressorStrength(static_cast<float>(integer) / 100.0f);
				response.result = PersistCustomSetting("CompressorStrength", integer);
			}
			return true;
		case 23:
		{
			// Rocksmith gate override: "<on>,<tenths>" (on 0/1; tenths = P1_NoiseFloor in tenths of a dB,
			// signed). Takes over the game's own amp noise gate; re-asserted live on the render thread
			// (RocksmithGate::ApplyPerFrame) whether or not the bridge routes.
			wchar_t* end = nullptr;
			const long on = wcstol(request.value, &end, 10);
			if (end == request.value || *end != L',' || (on != 0 && on != 1)) { response.result = E_INVALIDARG; return true; }
			wchar_t* thresholdEnd = nullptr;
			const long tenths = wcstol(end + 1, &thresholdEnd, 10);
			if (thresholdEnd == end + 1 || (*thresholdEnd != 0 && *thresholdEnd != L'\n')
				|| tenths < -1000 || tenths > 100) { response.result = E_INVALIDARG; return true; }
			RocksmithGate::SetOverride(on != 0, static_cast<float>(tenths) / 10.0f);
			response.result = PersistCustomSetting("RocksmithGateThreshold", tenths);
			if (SUCCEEDED(response.result)) response.result = PersistCustomSetting("RocksmithGateOverride", on);
			return true;
		}
		case 24:
			// Mains-hum notch base frequency (0 = off, else 50 or 60).
			if (!parseInteger(integer) || (integer != 0 && (integer < 20 || integer > 120))) response.result = E_INVALIDARG;
			else
			{
				AsioHook::SetHumFilterBaseHz(static_cast<float>(integer));
				response.result = PersistCustomSetting("HumFilter", integer);
			}
			return true;
		case 25:
		{
			// Ask the proxy to bind the named real ASIO driver. A virtual proxy promotes. A proxy that already
			// reports the device bound (2) or bound-but-stalled (3) is torn down and bound again: Apply is the
			// one explicit recovery when the real device is dead but still calling back.
			const int mode = OutputTap::ProxyOutputMode();
			const bool bound = mode == 2 || mode == 3;
			if (bound) LOG_INFO("(AUDIO ROUTING) Apply on a bound ASIO device: releasing and rebinding it" << std::endl);
			bool ok = false;
			if (bound) ok = OutputTap::TryRebindProxyOutput(request.value);
			else
			{
				// A virtual proxy is playing through a WASAPI route, often to the interface's own Windows endpoint
				// (it is not protected while the proxy is virtual). Stop the route BEFORE the ASIO driver opens:
				// a shared stream on that endpoint during the bind silences the ASIO device. The
				// promotion binds input and output together, so the guitar moves off the Real Tone Cable too.
				g_proxyPromotionRunning.store(true, std::memory_order_release);
				RouteStop();
				ok = OutputTap::TryPromoteProxyOutput(request.value);
				g_proxyPromotionRunning.store(false, std::memory_order_release);
				LOG_INFO("(AUDIO ROUTING) Binding the ASIO interface for input and output: " << (ok ? "bound" : "failed, staying virtual") << std::endl);
			}
			response.result = ok ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_READY);
			return true;
		}
		case 26:
		{
			// Live Player 2 Cable toggle ("0"/"1"); needs the persistent-input wrapper to be installed.
			wchar_t* end = nullptr;
			const long enabled = wcstol(request.value, &end, 10);
			if (end == request.value || *end != L'\0' || (enabled != 0 && enabled != 1)) response.result = E_INVALIDARG;
			else if (!PersistentInput::IsCableForPlayerTwoAvailable()) response.result = HRESULT_FROM_WIN32(ERROR_NOT_READY);
			else
			{
				PersistentInput::SetCableForPlayerTwoEnabled(enabled != 0);
				response.result = PersistStringSetting("Mod Settings", "CableForPlayerTwo", enabled != 0 ? "on" : "off");
			}
			return true;
		}
		case 27:
		{
			wchar_t* end = nullptr;
			const long enabled = wcstol(request.value, &end, 10);
			if (end == request.value || *end != L'\0' || (enabled != 0 && enabled != 1)) response.result = E_INVALIDARG;
			else
			{
				CableInput::SetOverlayEnabled(enabled != 0);
				char executablePath[MAX_PATH]{};
				GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
				const auto iniPath = (std::filesystem::path(executablePath).parent_path() / "RSMods.ini").string();
				if (!WritePrivateProfileStringA("Mod Settings", "AudioDiagnosticsOverlay", enabled != 0 ? "on" : "off", iniPath.c_str())) response.result = HRESULT_FROM_WIN32(GetLastError());
				else response.result = S_OK;
			}
			return true;
		}
		case 29:
		{
			wchar_t* end = nullptr;
			const long enabled = wcstol(request.value, &end, 10);
			if (end == request.value || *end != L'\0' || (enabled != 0 && enabled != 1)) response.result = E_INVALIDARG;
			else response.result = Settings::SetNoteByNoteDetectionVisible(enabled != 0) ? S_OK : E_FAIL;
			return true;
		}
		case 28:
			// Force the game to re-enumerate its song library (pick up newly added psarc/CDLC without a
			// restart). Same mechanism as the in-game ForceReEnumeration keypress: it flips the Steam DLC
			// service flags the enumeration hook captured. If the hook has not seen that pointer yet (the
			// game has not run its enumeration service since launch), report NOT_READY so the GUI can say so.
			if (!Enumeration::rsSteamServiceFlagsPtr) response.result = HRESULT_FROM_WIN32(ERROR_NOT_READY);
			else { Enumeration::ForceEnumeration(); response.result = S_OK; }
			return true;
		default:
			return false;
		}
	}

	class OutputSession
	{
	public:
		OutputSession() : OutputSession(OpenEndpoint, true, true) {}

		explicit OutputSession(EndpointFactory factory, bool watchDevices = false, bool allowDefaultFallback = false)
			: physical(std::make_shared<OutputBackend>(std::move(factory), watchDevices, Microsoft::WRL::Make<OutputNotifications>(), allowDefaultFallback))
		{
			packetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			gameTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			wakeFrequency = frequency.QuadPart;
			if (!packetEvent || !gameTimer || !wakeFrequency)
			{
				CloseEvents();
				throw std::runtime_error("Could not create output synchronization events");
			}
			try { physical->Launch(); worker = std::thread(&OutputSession::Run, this); }
			catch (...) { physical->Shutdown(); CloseEvents(); throw; }
		}

		~OutputSession()
		{
			Invoke([this]() { quitting = true; SetEvent(packetEvent); return S_OK; });
			worker.join();
			physical->Shutdown();
			CloseEvents();
		}

		HRESULT Invoke(std::function<HRESULT()> action)
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			try { return action(); }
			catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
			catch (...) { return E_FAIL; }
		}

		HRESULT Open(const std::wstring& endpointId)
		{
			return Invoke([&]()
			{
				activeEndpoint = endpointId;
				defaultPeriod = minimumPeriod = 100000;
				clockFrequency = 48000;
				return S_OK;
			});
		}

		HRESULT Initialize(const WAVEFORMATEX* format, DWORD flags, LPCGUID)
		{
			if (!format) return E_POINTER;
			if (!IsGameFormat(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
			if (!(flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK)) return E_INVALIDARG;
			if (initialized.load()) return AUDCLNT_E_ALREADY_INITIALIZED;
			return Invoke([this, format]()
			{
				gameFormat = *format;
				periodFrames = 480;
				gamePacketFrames.store(periodFrames);
				queue.Initialize(periodFrames * gameFormat.nBlockAlign, 2);
				converted.resize(periodFrames * 2);
				initialized.store(true, std::memory_order_release);
				ConfigureSavedOutputGuard();
				const HRESULT result = ConnectOutput(activeEndpoint, ReadSavedPeriod(activeEndpoint));

				LOG_INFO("(AUDIO ROUTING) Permanent game output initialized: 48 kHz stereo, 480-frame capacity; physical output HRESULT "
					<< std::hex << result << std::dec << std::endl);
				return S_OK;
			});
		}

		ControlResponse HandleControl(const ControlRequest& request)
		{
			ControlResponse response;
			// The loudness guard (op 16) runs inside this session's owned backend when routing owns output.
			const auto configureGuard = [this](bool limiterOn, float ceilingLin, bool agcOn, float targetRms)
			{
				return physical->ConfigureOutputGuard(limiterOn, ceilingLin, agcOn, targetRms);
			};
			// Player 2 Cable (26), overlay toggles (27/29) and Force enumeration (28) do not need the session at all, so they are
			// answered even before Initialize.
			if (request.operation == 26 || request.operation == 27 || request.operation == 28 || request.operation == 29) { HandleSharedControl(request, response, configureGuard); return response; }
			if (!initialized.load()) { response.result = AUDCLNT_E_NOT_INITIALIZED; return response; }
			if (request.operation == 2)
			{
				bool alreadyRecording = false;
				Invoke([&]() { alreadyRecording = recorder != nullptr; return S_OK; });
				if (alreadyRecording) response.result = HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
				else
				{
					auto session = std::make_shared<RecordingSession>();
					const std::filesystem::path directory(request.value);
					response.result = directory.is_absolute() ? session->Start(directory, periodFrames) : E_INVALIDARG;
					if (SUCCEEDED(response.result)) response.result = Invoke([&]()
					{
						lastRecordingPath = session->GetWetPath();
						recorder = session;
						recordedFrames = 0; recordingStarted = 0; recordingError = S_OK;
						return S_OK;
					});
					if (FAILED(response.result))
					{
						std::wstring ignoredPath;
						uint64_t ignoredFrames = 0, ignoredStarted = 0;
						session->Stop(ignoredPath, ignoredFrames, ignoredStarted);
					}
				}
			}
			else if (request.operation == 3)
			{
				std::shared_ptr<RecordingSession> finished;
				Invoke([&]()
				{
					if (recorder) { recordedFrames = recorder->GetFrames(); recordingStarted = recorder->GetStarted(); }
					finished.swap(recorder);
					return S_OK;
				});
				if (finished)
				{
					std::wstring stoppedPath;
					uint64_t stoppedFrames = 0, stoppedStarted = 0;
					finished->Stop(stoppedPath, stoppedFrames, stoppedStarted);
					response.result = finished->GetError();
					Invoke([&]() { lastRecordingPath = stoppedPath; recordingError = response.result; return S_OK; });
				}
			}
			// Never open the Windows endpoint of an interface RS_ASIO holds (it silences the interface and its
			// input does not recover). See OutputDevices.hpp.
			else if (request.operation == 4)
			{
				response.result = OutputDevices::IsProtected(request.value)
					? HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) : SwitchOutput(request.value);
				// Remember the pick: OutputDevice is what the cable-mode session opens at launch, and
				// ResolveActiveOutput falls back to the Windows default when that device is not plugged in.
				if (SUCCEEDED(response.result))
				{
					wchar_t executable[MAX_PATH]{};
					GetModuleFileNameW(nullptr, executable, MAX_PATH);
					WritePrivateProfileStringW(L"Audio Bridge", L"OutputDevice", request.value,
						(std::filesystem::path(executable).parent_path() / L"RSMods.ini").c_str());
				}
			}
			else if (request.operation == 6)
			{
				wchar_t* end = nullptr;
				const unsigned long period = wcstoul(request.value, &end, 10);
				bool recording = false;
				std::wstring endpoint;
				Invoke([&]() { recording = recorder != nullptr; endpoint = activeEndpoint; return S_OK; });
				if (recording) response.result = HRESULT_FROM_WIN32(ERROR_BUSY);
				else if (end == request.value || *end != L'\n' || !end[1] || period > 48000) response.result = E_INVALIDARG;
				else if (endpoint != end + 1) response.result = HRESULT_FROM_WIN32(ERROR_RETRY);
				else response.result = SwitchOutput(endpoint, static_cast<UINT32>(period), false);
			}
			else if (!HandleSharedControl(request, response, configureGuard)
				&& request.operation != 1 && request.operation != 5) response.result = E_INVALIDARG;
			for (unsigned int channel = 0; channel < 7; ++channel)
			{
				if (!VolumeControl::GetPlaybackVolume(channel, response.volumes[channel]))
				{
					response.mixerError = E_FAIL;
					response.volumes[channel] = -1.f;
				}
			}
			Invoke([&]()
			{
				const auto backendStatus = physical->GetStatus();
				response.outputError = FAILED(streamError.load()) ? streamError.load() : backendStatus.error;
				response.recordingError = recorder ? recorder->GetError() : recordingError;
				response.recording = recorder && SUCCEEDED(response.recordingError) ? 1 : 0;
				response.recordedFrames = recorder ? recorder->GetFrames() : recordedFrames;
				response.recordingStarted = recorder ? recorder->GetStarted() : recordingStarted;
				response.dryInputReady = DrySignalRecording::IsReady() ? 1 : 0;
				response.recordingSource = 0;
				OutputTap::ReadOutputLevels(response.outputPeak, response.outputRms, 2);
				response.proxyInputMode = static_cast<uint32_t>(OutputTap::ProxyInputMode());
				response.peak = peak;
				if (request.operation == 1) peak = 0;
				wcsncpy_s(response.file, recorder ? recorder->GetWetPath().c_str() : lastRecordingPath.c_str(), _TRUNCATE);
				wcsncpy_s(response.endpoint, (SUCCEEDED(backendStatus.error) ? backendStatus.activeEndpoint : activeEndpoint).c_str(), _TRUNCATE);
				if (request.operation == 18) WriteLatencyResult(response);   // overwrites file with the latency result
				if (request.operation == 5)
				{
					response.recording = running ? 1 : 0;
					response.recordedFrames = submittedFrames;
					response.recordingStarted = clockPosition.load();
					const std::wstring trace = L"starts=" + std::to_wstring(startCalls.load())
						+ L" stops=" + std::to_wstring(stopCalls.load())
						+ L" lifecycleHr=" + std::to_wstring(lastLifecycleResult.load())
						+ L" acquireCalls=" + std::to_wstring(acquireCalls.load())
						+ L" acquireFailures=" + std::to_wstring(acquireFailures.load())
						+ L" acquireHr=" + std::to_wstring(lastAcquireResult.load())
						+ L" requestedFrames=" + std::to_wstring(lastRequestedFrames.load())
						+ L" releaseCalls=" + std::to_wstring(releaseCalls.load())
						+ L" releaseHr=" + std::to_wstring(lastReleaseResult.load())
						+ L" queuePackets=" + std::to_wstring(queue.Count())
						+ L" padding=" + std::to_wstring(backendStatus.padding)
						+ L" repeatedGameBlocks=" + std::to_wstring(repeatedGameBlocks.load())
						+ L" inspectedGameBlocks=" + std::to_wstring(inspectedGameBlocks.load())
						+ L" emptyOutputObservations=" + std::to_wstring(backendStatus.emptyObservations)
						+ L" longestPumpGapMs=" + std::to_wstring(backendStatus.longestPumpGapMs);
					const std::wstring tuning = L" engineMinimum=" + std::to_wstring(backendStatus.minimum)
						+ L" engineFundamental=" + std::to_wstring(backendStatus.fundamental)
						+ L" engineMaximum=" + std::to_wstring(backendStatus.maximum)
						+ L" enginePeriod=" + std::to_wstring(backendStatus.period);
					const std::wstring recovery = L" backendState=" + std::to_wstring(static_cast<int>(backendStatus.state))
						+ L" generation=" + std::to_wstring(backendStatus.generation)
						+ L" fifoFrames=" + std::to_wstring(backendStatus.queuedFrames)
						+ L" expiredFrames=" + std::to_wstring(backendStatus.expiredFrames)
						+ L" overruns=" + std::to_wstring(backendStatus.overruns)
						+ L" clockMeasured=" + std::to_wstring(backendStatus.clockMeasured)
						+ L" clockPpm=" + std::to_wstring(backendStatus.clockPpm)
						+ L" correctionPpm=" + std::to_wstring(backendStatus.correctionPpm);
					const auto inputFormat = AsioBufferState::Read();
					const std::wstring input = L" asioInputFrames=" + std::to_wstring(uint32_t(inputFormat))
						+ L" asioInputRate=" + std::to_wstring(uint32_t(inputFormat >> 32));
					wcsncpy_s(response.file, (input + trace + tuning + recovery).c_str(), _TRUNCATE);
				}
				return S_OK;
			});
			return response;
		}

		HRESULT SwitchOutput(const std::wstring& endpointId, UINT32 requestedPeriod = 0, bool useSavedPeriod = true)
		{
			if (endpointId.empty()) return E_INVALIDARG;
			const UINT32 desired = requestedPeriod ? requestedPeriod : (useSavedPeriod ? ReadSavedPeriod(endpointId) : 0);
			const auto snapshot = physical->GetStatus();
			if (desired && snapshot.activeEndpoint == endpointId && !IsEnginePeriod(desired, snapshot.minimum, snapshot.fundamental, snapshot.maximum)) return E_INVALIDARG;
			return Invoke([&]() { return ConnectOutput(endpointId, desired); });
		}

		HRESULT ConnectOutput(const std::wstring& endpointId, UINT32 desired)
		{
			activeEndpoint = endpointId;
			selectedPeriod = desired;
			routeGeneration.fetch_add(1);
			physical->Request(endpointId, desired);
			return S_OK;
		}

		HRESULT Start()
		{
			startCalls.fetch_add(1);
			if (!initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			const HRESULT started = Invoke([this]()
			{
				if (FAILED(streamError.load())) return streamError.load();
				if (!gameEvent) return AUDCLNT_E_EVENTHANDLE_NOT_SET;
				if (running) return AUDCLNT_E_NOT_STOPPED;
				HRESULT result = Pump();
				if (FAILED(result)) return result;
				physical->SetRunning(true);
				if (SUCCEEDED(result)) { running = true; clockBase = clockPosition.load(); QueryPerformanceCounter(&clockStarted); lastGameWakeTick = 0; SignalGame(); }
				return result;
			});
			lastLifecycleResult.store(started);
			LOG_INFO("(AUDIO ROUTING) Start result " << std::hex << started << std::dec << std::endl);
			return started;
		}

		HRESULT Stop()
		{
			stopCalls.fetch_add(1);
			if (!initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			const HRESULT stopped = Invoke([this]()
			{
				routeGeneration.fetch_add(1);
				physical->SetRunning(false);
				HRESULT result = S_OK;
				if (SUCCEEDED(result))
				{
					UpdateClock();
					running = false;
					CancelWaitableTimer(gameTimer);
					if (gameEvent) ResetEvent(gameEvent);
					gameWakeOutstanding.store(false);
				}
				return result;
			});
			lastLifecycleResult.store(stopped);
			LOG_INFO("(AUDIO ROUTING) Stop result " << std::hex << stopped << std::dec << std::endl);
			return stopped;
		}

		HRESULT Reset()
		{
			if (!initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			if (pendingFrames != 0) return AUDCLNT_E_BUFFER_OPERATION_PENDING;
			return Invoke([this]()
			{
				if (running) return AUDCLNT_E_NOT_STOPPED;
				routeGeneration.fetch_add(1);
				physical->Request(activeEndpoint, selectedPeriod);
				HRESULT result = S_OK;
				if (SUCCEEDED(result)) { queue.Reset(); packetWriteIndex = packetReadIndex = 0; clockPosition.store(0); clockBase = 0; }
				return result;
			});
		}

		HRESULT SetGameEvent(HANDLE eventHandle)
		{
			if (!initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			if (!eventHandle) return E_INVALIDARG;
			return Invoke([this, eventHandle]()
			{
				if (gameEvent) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
				return DuplicateHandle(GetCurrentProcess(), eventHandle, GetCurrentProcess(), &gameEvent,
					0, FALSE, DUPLICATE_SAME_ACCESS) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
			});
		}

		HRESULT GetBuffer(UINT32 frames, BYTE** data)
		{
			acquireCalls.fetch_add(1, std::memory_order_relaxed);
			lastRequestedFrames.store(frames, std::memory_order_relaxed);
			const HRESULT result = AcquireBuffer(frames, data);
			lastAcquireResult.store(result, std::memory_order_relaxed);
			if (FAILED(result)) acquireFailures.fetch_add(1, std::memory_order_relaxed);
			return result;
		}

		HRESULT AcquireBuffer(UINT32 frames, BYTE** data)
		{
			if (!data) return E_POINTER;
			*data = nullptr;
			if (!initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			if (FAILED(streamError.load())) return streamError.load();
			if (pendingFrames) return AUDCLNT_E_OUT_OF_ORDER;
			if (frames == 0) return S_OK;
			if (frames > periodFrames) return AUDCLNT_E_BUFFER_SIZE_ERROR;
			BYTE* packet = queue.BeginWrite();
			if (!packet) return AUDCLNT_E_BUFFER_TOO_LARGE;
			gameWakeOutstanding.store(true);
			gamePacketFrames.store(frames);
			pendingFrames = frames;
			pendingGeneration = routeGeneration.load();
			*data = packet;
			return S_OK;
		}

		HRESULT ReleaseBuffer(UINT32 frames, DWORD flags)
		{
			releaseCalls.fetch_add(1, std::memory_order_relaxed);
			const HRESULT result = CommitBuffer(frames, flags);
			lastReleaseResult.store(result, std::memory_order_relaxed);
			return result;
		}

		HRESULT CommitBuffer(UINT32 frames, DWORD flags)
		{
			if (!initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			if (FAILED(streamError.load())) return streamError.load();
			if (frames == 0)
			{
				pendingFrames = 0;
				gameWakeOutstanding.store(false);
				SetEvent(packetEvent);
				return S_OK;
			}
			if (!pendingFrames) return AUDCLNT_E_OUT_OF_ORDER;
			if (frames != pendingFrames) return AUDCLNT_E_INVALID_SIZE;
			if (flags & ~AUDCLNT_BUFFERFLAGS_SILENT) return E_INVALIDARG;
			if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::memset(queue.BeginWrite(), 0, frames * gameFormat.nBlockAlign);
			InspectGameBlocks(queue.BeginWrite(), frames * gameFormat.nBlockAlign);
			packetGenerations[packetWriteIndex % 2] = pendingGeneration;
			packetTimes[packetWriteIndex % 2] = GetTickCount64();
			++packetWriteIndex;
			queue.CommitWrite(frames * gameFormat.nBlockAlign);
			pendingFrames = 0;
			gameWakeOutstanding.store(false);
			SetEvent(packetEvent);
			return S_OK;
		}

		std::atomic<bool> initialized{ false };
		std::atomic<HRESULT> streamError{ S_OK };
		std::atomic<UINT64> clockPosition{ 0 };
		std::atomic<UINT64> clockQpc{ 0 };
		std::atomic<uint32_t> clockSequence{ 0 };
		UINT64 clockFrequency = 0;
		UINT32 periodFrames = 0;
		REFERENCE_TIME defaultPeriod = 0;
		REFERENCE_TIME minimumPeriod = 0;
		REFERENCE_TIME streamLatency = 0;

		bool IsRecording()
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			return recorder != nullptr && SUCCEEDED(recorder->GetError());
		}

		bool HasTake()
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			return recorder != nullptr;
		}

		HRESULT RecordingError()
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			return recorder ? recorder->GetError() : recordingError;
		}

	private:
		void ConfigureSavedOutputGuard()
		{
			wchar_t executable[MAX_PATH]{};
			GetModuleFileNameW(nullptr, executable, MAX_PATH);
			const auto settingsPath = std::filesystem::path(executable).parent_path() / L"RSMods.ini";
			auto read = [&settingsPath](const wchar_t* key, long fallback)
			{
				wchar_t value[32]{};
				GetPrivateProfileStringW(L"Mod Settings", key, L"", value, 32, settingsPath.c_str());
				if (!value[0]) return fallback;
				wchar_t* end = nullptr;
				const long parsed = wcstol(value, &end, 10);
				return end != value && *end == 0 ? parsed : fallback;
			};
			const bool limiterOn = read(L"AudioBridgeLimiter", 0) != 0;
			const float ceiling = std::pow(10.0f, static_cast<float>(read(L"AudioBridgeLimiterLevel", -60)) / 200.0f);
			const bool agcOn = read(L"AudioBridgeLoudnessMatch", 0) != 0;
			const float target = std::pow(10.0f, static_cast<float>(read(L"AudioBridgeLoudnessTarget", -200)) / 200.0f);
			if (physical->ConfigureOutputGuard(limiterOn, ceiling, agcOn, target))
				LOG_INFO("(AUDIO ROUTING) Saved output limiter applied to the Cable shared-output engine" << std::endl);
		}

		void InspectGameBlocks(const BYTE* data, UINT32 bytes)
		{
			const UINT32 blockBytes = 128 * gameFormat.nBlockAlign;
			while (bytes)
			{
				const UINT32 copied = std::min(bytes, blockBytes - diagnosticBytes);
				std::memcpy(diagnosticBlock.data() + diagnosticBytes, data, copied);
				diagnosticBytes += copied; data += copied; bytes -= copied;
				if (diagnosticBytes != blockBytes) continue;
				bool nonzero = false;
				for (UINT32 index = 0; index < blockBytes; ++index) nonzero |= diagnosticBlock[index] != 0;
				if (havePreviousBlock && nonzero && std::memcmp(diagnosticBlock.data(), previousBlock.data(), blockBytes) == 0)
					repeatedGameBlocks.fetch_add(1, std::memory_order_relaxed);
				inspectedGameBlocks.fetch_add(1, std::memory_order_relaxed);
				std::memcpy(previousBlock.data(), diagnosticBlock.data(), blockBytes);
				havePreviousBlock = true;
				diagnosticBytes = 0;
			}
		}

		void SignalGame()
		{
			if (!running || !gameEvent) return;
			bool outstanding = false;
			if (!gameWakeOutstanding.compare_exchange_strong(outstanding, true)) return;
			if (queue.Count() >= 2)
			{
				gameWakeOutstanding.store(false);
				return;
			}
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			const LONGLONG interval = wakeFrequency * gamePacketFrames.load() / 48000;
			const LONGLONG due = lastGameWakeTick + interval;
			if (lastGameWakeTick && now.QuadPart < due)
			{
				LARGE_INTEGER delay{};
				delay.QuadPart = -std::max<LONGLONG>(1, (due - now.QuadPart) * 10000000 / wakeFrequency);
				gameWakeOutstanding.store(false);
				if (!SetWaitableTimer(gameTimer, &delay, 0, nullptr, nullptr, FALSE))
				{
					streamError.store(HRESULT_FROM_WIN32(GetLastError()));
					LOG_ERROR("(AUDIO ROUTING) Could not schedule the game audio wake" << std::endl);
					SetEvent(gameEvent);
				}
				return;
			}
			// Keep the wake reserved while the producer holds its uncommitted slot.
			// Preserve the sample-rate cadence through timer jitter, but do not burst
			// through missed periods after a stall.
			lastGameWakeTick = !lastGameWakeTick || now.QuadPart - due >= interval ? now.QuadPart : due;
			SetEvent(gameEvent);
		}

		void UpdateClock()
		{
			if (!running) return;
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			const auto elapsed = now.QuadPart - clockStarted.QuadPart;
			clockSequence.fetch_add(1);
			clockPosition.store(clockBase + elapsed / wakeFrequency * 48000 + elapsed % wakeFrequency * 48000 / wakeFrequency);
			clockQpc.store(now.QuadPart / wakeFrequency * 10000000 + now.QuadPart % wakeFrequency * 10000000 / wakeFrequency);
			clockSequence.fetch_add(1);
		}

		HRESULT Pump()
		{
			UpdateClock();
			for (unsigned budget = 0; budget < 2; ++budget)
			{
				uint32_t bytes = 0;
				const auto* packet = queue.BeginRead(bytes);
				if (!packet) break;
				const UINT32 packetFrames = bytes / gameFormat.nBlockAlign;
				const size_t samples = packetFrames * 2;
				if (gameFormat.wBitsPerSample == 32) std::memcpy(converted.data(), packet, samples * sizeof(float));
				else
				{
					const auto* pcm = reinterpret_cast<const int16_t*>(packet);
					for (size_t index = 0; index < samples; ++index) converted[index] = pcm[index] / 32768.0f;
				}
				if (packetGenerations[packetReadIndex % 2] == routeGeneration.load())
					physical->Submit(converted.data(), packetFrames, packetTimes[packetReadIndex % 2]);
				submittedFrames += packetFrames;
				// Held as the loudest sample since the window last asked, not the last packet alone. A
				// packet is about ten milliseconds and the window polls every hundred, so a per-packet
				// value would show the meter a one-in-ten sample of what was played.
				for (size_t index = 0; index < samples; ++index)
				{
					const float sample = converted[index];
					if (std::isfinite(sample)) peak = std::max(peak, static_cast<uint32_t>(std::min(1.0f, std::abs(sample)) * 1000));
				}
					if (recorder) recorder->SubmitWet(converted.data(), packetFrames);
				++packetReadIndex;
				queue.CommitRead();
			}
			return S_OK;
		}

		void Run()
		{
			DWORD taskIndex = 0;
			HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
			HANDLE events[] = { packetEvent, gameTimer };
			while (!quitting.load())
			{
				const DWORD wake = WaitForMultipleObjects(2, events, FALSE, 2);
				if (wake == WAIT_FAILED)
				{
					streamError.store(HRESULT_FROM_WIN32(GetLastError()));
					break;
				}
				std::lock_guard<std::mutex> guard(stateMutex);
				if (initialized.load() && SUCCEEDED(streamError.load()))
				{
					Pump();
					SignalGame();
				}
			}
			if (recorder)
			{
				std::wstring ignoredPath;
				uint64_t ignoredFrames = 0, ignoredStarted = 0;
				recorder->Stop(ignoredPath, ignoredFrames, ignoredStarted);
			}
			if (task) AvRevertMmThreadCharacteristics(task);
		}

		void CloseEvents()
		{
			for (HANDLE handle : { packetEvent, gameEvent, gameTimer })
			{
				if (handle) CloseHandle(handle);
			}
		}

		std::shared_ptr<OutputBackend> physical;
		AudioPacketQueue queue;
		std::array<uint64_t, 2> packetGenerations{}, packetTimes{};
		std::atomic<uint64_t> routeGeneration{ 0 };
		uint64_t pendingGeneration = 0;
		uint32_t packetWriteIndex = 0, packetReadIndex = 0;
		std::shared_ptr<RecordingSession> recorder;
		std::wstring activeEndpoint;
		UINT32 selectedPeriod = 0;
		std::wstring lastRecordingPath;
		HRESULT recordingError = S_OK;
		uint64_t recordedFrames = 0;
		uint64_t recordingStarted = 0;
		uint32_t peak = 0;
		uint64_t submittedFrames = 0;
		std::array<BYTE, 1024> diagnosticBlock{}, previousBlock{};
		UINT32 diagnosticBytes = 0;
		bool havePreviousBlock = false;
		std::atomic<uint64_t> repeatedGameBlocks{ 0 }, inspectedGameBlocks{ 0 };
		std::atomic<uint32_t> startCalls{ 0 }, stopCalls{ 0 }, acquireCalls{ 0 }, acquireFailures{ 0 }, releaseCalls{ 0 }, lastRequestedFrames{ 0 };
		std::atomic<HRESULT> lastLifecycleResult{ S_OK }, lastAcquireResult{ S_OK }, lastReleaseResult{ S_OK };
		UINT64 clockBase = 0;
		LARGE_INTEGER clockStarted{};
		WAVEFORMATEX gameFormat{};
		std::vector<float> converted;
		UINT32 pendingFrames = 0;
		std::atomic<bool> gameWakeOutstanding{ false };
		std::atomic<UINT32> gamePacketFrames{ 0 };
		LONGLONG wakeFrequency = 0, lastGameWakeTick = 0;
		bool running = false;
		std::atomic<bool> quitting{ false };
		HANDLE packetEvent = nullptr;
		HANDLE gameTimer = nullptr;
		HANDLE gameEvent = nullptr;
		std::mutex stateMutex;
		std::thread worker;
	};

	class RenderClient final : public IAudioRenderClient
	{
	public:
		explicit RenderClient(std::shared_ptr<OutputSession> output) : output(std::move(output))
		{
			CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(this), &marshaler);
		}
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (id == __uuidof(IUnknown) || id == __uuidof(IAudioRenderClient)) { *object = this; AddRef(); return S_OK; }
			if (id == __uuidof(IMarshal) && marshaler) return marshaler->QueryInterface(id, object);
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
		ULONG STDMETHODCALLTYPE Release() override { const ULONG count = --references; if (!count) delete this; return count; }
		HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames, BYTE** data) override { return output->GetBuffer(frames, data); }
		HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames, DWORD flags) override { return output->ReleaseBuffer(frames, flags); }
	private:
		~RenderClient() { if (marshaler) marshaler->Release(); }
		std::atomic<ULONG> references{ 1 };
		std::shared_ptr<OutputSession> output;
		IUnknown* marshaler = nullptr;
	};

	class OutputClock final : public IAudioClock
	{
	public:
		explicit OutputClock(std::shared_ptr<OutputSession> output) : output(std::move(output))
		{
			CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(this), &marshaler);
		}
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (id == __uuidof(IUnknown) || id == __uuidof(IAudioClock)) { *object = this; AddRef(); return S_OK; }
			if (id == __uuidof(IMarshal) && marshaler) return marshaler->QueryInterface(id, object);
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
		ULONG STDMETHODCALLTYPE Release() override { const ULONG count = --references; if (!count) delete this; return count; }
		HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* frequency) override
		{
			if (!frequency) return E_POINTER;
			*frequency = output->clockFrequency;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetPosition(UINT64* position, UINT64* qpc) override
		{
			if (!position) return E_POINTER;
			for (;;)
			{
				const uint32_t before = output->clockSequence.load();
				if (before & 1) continue;
				*position = output->clockPosition.load();
				const UINT64 timestamp = output->clockQpc.load();
				if (before == output->clockSequence.load())
				{
					if (qpc) *qpc = timestamp;
					break;
				}
			}
			return output->streamError.load();
		}
		HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* characteristics) override
		{
			if (!characteristics) return E_POINTER;
			*characteristics = AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ;
			return S_OK;
		}
	private:
		~OutputClock() { if (marshaler) marshaler->Release(); }
		std::atomic<ULONG> references{ 1 };
		std::shared_ptr<OutputSession> output;
		IUnknown* marshaler = nullptr;
	};

	class AudioClient final : public IAudioClient
	{
	public:
		explicit AudioClient(std::shared_ptr<OutputSession> output) : output(std::move(output))
		{
			CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(this), &marshaler);
		}
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (id == __uuidof(IUnknown) || id == __uuidof(IAudioClient)) { *object = this; AddRef(); return S_OK; }
			if (id == __uuidof(IMarshal) && marshaler) return marshaler->QueryInterface(id, object);
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
		ULONG STDMETHODCALLTYPE Release() override { const ULONG count = --references; if (!count) delete this; return count; }
		HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME, REFERENCE_TIME,
			const WAVEFORMATEX* format, LPCGUID session) override
		{
			if (mode != AUDCLNT_SHAREMODE_EXCLUSIVE) return E_INVALIDARG;
			const HRESULT result = output->Initialize(format, flags, session);
			if (FAILED(result)) LOG_ERROR("(AUDIO ROUTING) Shared output initialization failed, HRESULT " << std::hex << result << std::dec << "; requires shared 48 kHz stereo support" << std::endl);
			return result;
		}
		HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override
		{
			if (!frames) return E_POINTER;
			if (!output->initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			*frames = output->periodFrames;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* latency) override
		{
			if (!latency) return E_POINTER;
			if (!output->initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			*latency = output->streamLatency + 20000000ll * output->periodFrames / 48000;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* padding) override
		{
			if (!padding) return E_POINTER;
			if (!output->initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			*padding = 0; // Exclusive event clients process one complete buffer per signal.
			return output->streamError.load();
		}
		HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX* format, WAVEFORMATEX** closest) override
		{
			if (closest) *closest = nullptr;
			return IsGameFormat(format) ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
		}
		HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override
		{
			if (!format) return E_POINTER;
			*format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
			if (!*format) return E_OUTOFMEMORY;
			**format = { WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0 };
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* normal, REFERENCE_TIME* minimum) override
		{
			if (!normal && !minimum) return E_POINTER;
			if (normal) *normal = output->defaultPeriod;
			if (minimum) *minimum = output->minimumPeriod;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE Start() override { return output->Start(); }
		HRESULT STDMETHODCALLTYPE Stop() override { return output->Stop(); }
		HRESULT STDMETHODCALLTYPE Reset() override { return output->Reset(); }
		HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE eventHandle) override { return output->SetGameEvent(eventHandle); }
		HRESULT STDMETHODCALLTYPE GetService(REFIID id, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (!output->initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			try
			{
				if (id == __uuidof(IAudioRenderClient)) *object = new RenderClient(output);
				else if (id == __uuidof(IAudioClock)) *object = new OutputClock(output);
				else return E_NOINTERFACE;
			}
			catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
			return S_OK;
		}
	private:
		~AudioClient() { if (marshaler) marshaler->Release(); }
		std::atomic<ULONG> references{ 1 };
		std::shared_ptr<OutputSession> output;
		IUnknown* marshaler = nullptr;
	};

	Configuration ReadConfiguration()
	{
		wchar_t executable[MAX_PATH]{};
		GetModuleFileNameW(nullptr, executable, MAX_PATH);
		const auto directory = std::filesystem::path(executable).parent_path();
		const auto path = directory / L"RSMods.ini";
		Configuration configuration;
		configuration.enabled = GetPrivateProfileIntW(L"Audio Bridge", L"Enabled", 0, path.c_str()) == 1;
		wchar_t value[2048]{};
		GetPrivateProfileStringW(L"Audio Bridge", L"InputDevice", L"", value, 2048, path.c_str());
		configuration.inputDeviceId = value;
		GetPrivateProfileStringW(L"Audio Bridge", L"OutputDevice", L"", value, 2048, path.c_str());
		configuration.outputDeviceId = value;
		return configuration;
	}

	// The control pipe is hosted for the whole process life, decoupled from routing, so the GUI
	// can always find and talk to the game even when the bridge is passing audio through untouched
	// (RSMods.ini [Audio Bridge] Enabled=0). Only one server may own the pipe name (FILE_FLAG_FIRST_PIPE_INSTANCE),
	// so it lives here rather than inside the routed output object; it delegates to the live output
	// session when one exists (ROUTE engaged) and otherwise reports a benign passthrough status.
	std::mutex g_sessionMutex;
	std::weak_ptr<OutputSession> g_activeSession;
	// Intentionally leaked: its worker thread runs for the whole process life. Never joined at DLL
	// unload, which would risk a loader-lock deadlock during teardown; the OS reclaims it at exit.
	AudioControlServer* g_control = nullptr;

	bool IsRecording()
	{
		std::shared_ptr<OutputSession> session;
		{
			std::lock_guard<std::mutex> guard(g_sessionMutex);
			session = g_activeSession.lock();
		}
		if (session && session->IsRecording()) return true;
		return OutputTap::IsRecording() && SUCCEEDED(OutputTap::RecordingError());
	}

	bool HasTake()
	{
		std::shared_ptr<OutputSession> session;
		{
			std::lock_guard<std::mutex> guard(g_sessionMutex);
			session = g_activeSession.lock();
		}
		return (session && session->HasTake()) || OutputTap::IsRecording();   // OutputTap: session exists, failed or not
	}

	HRESULT RecordingError()
	{
		std::shared_ptr<OutputSession> session;
		{
			std::lock_guard<std::mutex> guard(g_sessionMutex);
			session = g_activeSession.lock();
		}
		if (session && session->IsRecording()) return session->RecordingError();
		if (OutputTap::IsRecording()) return OutputTap::RecordingError();
		if (session) return session->RecordingError();
		return OutputTap::RecordingError();
	}

	// --- Alternate-device routing (ROUTE feed) --------------------------------------------------------
	// Play the game mix out of a device the game is not bound to (e.g. laptop speakers) without a restart.
	// On an ASIO/proxy setup the game's output is bound to the real ASIO device at launch and cannot be
	// re-pointed live; but the Rocksmith Audio Bridge proxy already has the post-mix block every buffer.
	// A routing sink renders that block to a chosen WASAPI device through one shared-mode
	// OutputBackend (the same rate-matched renderer the legacy bridge uses) and mute the proxy's forward,
	// so the mix moves to the new device instead of doubling on both. Bridging two device clocks needs the
	// OutputRateMatcher (crystals drift); switching back to the ASIO device just tears the route down.
	// Only used in passthrough: when a full OutputSession owns the output, it switches devices itself.
	std::mutex g_routeMutex;
	std::shared_ptr<OutputBackend> g_routeBackend;   // non-null only while routing to another device
	std::wstring g_routeEndpoint;                    // the device the route is currently rendering to
	bool g_routeManaged = false;                     // true only for a virtual-ASIO route chosen by the host
	std::atomic<uint64_t> g_lastProxyRefreshTick{ 0 };
	std::atomic<bool> g_proxyDemotionRunning{ false };

	// Remembered output: the device last picked in the overlay's Switch output is saved as
	// RSMods.ini [Audio Bridge] LastOutput and restored once per launch when the game comes up on the ASIO
	// bridge, so a chosen output sticks the way a hardcoded RS_ASIO output does. Picking the ASIO entry
	// (op 22) forgets it. A saved device that is unplugged is skipped (the game stays on ASIO) but not
	// forgotten, so it is used again next time it is present.
	std::atomic<bool> g_lastOutputRestoreTried{ false };

	std::wstring BridgeSettingsIni()
	{
		wchar_t executable[MAX_PATH]{};
		GetModuleFileNameW(nullptr, executable, MAX_PATH);
		return (std::filesystem::path(executable).parent_path() / L"RSMods.ini").wstring();
	}

	void SaveLastOutput(const std::wstring& endpointId)
	{
		// An empty id deletes the key rather than writing "LastOutput=".
		WritePrivateProfileStringW(L"Audio Bridge", L"LastOutput", endpointId.empty() ? nullptr : endpointId.c_str(), BridgeSettingsIni().c_str());
	}

	std::wstring ReadLastOutput()
	{
		wchar_t value[2048]{};
		GetPrivateProfileStringW(L"Audio Bridge", L"LastOutput", L"", value, 2048, BridgeSettingsIni().c_str());
		return value;
	}
	// Route-source watchdog. On ASIO the routed mix is fed by the proxy's buffer callback; if the interface
	// is unplugged that callback stops firing, so the route keeps a live WASAPI backend (green light) with no
	// audio to render. These ticks let the status path tell that dead-source state from a healthy route, so
	// the GUI stops claiming sound is playing. See RouteFeedStalled.
	std::atomic<uint64_t> g_routeStartTick{ 0 };     // when the current route was started (0 = not routing)
	std::atomic<uint64_t> g_routeLastFeedTick{ 0 };  // last time RouteSink received a block (0 = none yet)

	// Convert one ASIO output block (the proxy's native format) to float stereo and feed the route
	// backend. Runs on the proxy's ASIO buffer thread; a plain copy + convert, no blocking. Mirrors the
	// recorder's ProxySink conversion (OutputTap.cpp). Sinks carry no context, so it reads the backend
	// from file scope, exactly as the recorder sink reads its recorder.
	void __cdecl RouteSink(const void* data, long frames, long channels, long asioType, double) noexcept
	{
		if (!data || frames <= 0 || channels <= 0) return;
		std::shared_ptr<OutputBackend> backend;
		{ std::lock_guard<std::mutex> guard(g_routeMutex); backend = g_routeBackend; }
		if (!backend) return;
		auto bytesOf = [](long t) -> int { switch (t) { case 0: case 16: return 2; case 1: case 17: return 3;
			case 2: case 3: case 18: case 19: case 24: case 25: case 26: case 27: return 4; default: return 0; } };
		auto toFloat = [](const BYTE* p, long t) -> float { switch (t) {
			case 16: return *reinterpret_cast<const int16_t*>(p) / 32768.0f;
			case 17: { int v = p[0] | (p[1] << 8) | (p[2] << 16); if (v & 0x800000) v |= ~0xFFFFFF; return v / 8388608.0f; }
			case 19: return *reinterpret_cast<const float*>(p);
			default: return *reinterpret_cast<const int32_t*>(p) / 2147483648.0f; } };
		const int bytes = bytesOf(asioType);
		if (bytes == 0) return;
		static thread_local std::vector<float> stereo;
		stereo.resize(static_cast<size_t>(frames) * 2);
		const BYTE* base = reinterpret_cast<const BYTE*>(data);
		for (long f = 0; f < frames; ++f)
		{
			const BYTE* fb = base + static_cast<size_t>(f) * channels * bytes;
			const float left = toFloat(fb, asioType);
			stereo[static_cast<size_t>(f) * 2] = left;
			stereo[static_cast<size_t>(f) * 2 + 1] = channels > 1 ? toFloat(fb + bytes, asioType) : left;
		}
		g_routeLastFeedTick.store(GetTickCount64(), std::memory_order_release);   // source is alive this block
		backend->Submit(stereo.data(), static_cast<UINT32>(frames), GetTickCount64());
	}

	// Start or retarget the route to endpointId. Idempotent: the first call spins up the backend, mutes
	// the proxy forward and registers the sink; a later call with a new device just retargets the backend
	// live (the rate matcher re-cools for the new clock). Needs the proxy loaded (ASIO bridge in chain).
	HRESULT RouteStart(const std::wstring& endpointId)
	{
		if (endpointId.empty()) return E_INVALIDARG;
		if (!OutputTap::ProxyAvailable()) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
		std::shared_ptr<OutputBackend> started;
		{
			std::lock_guard<std::mutex> guard(g_routeMutex);
			g_routeManaged = false;
			if (g_routeBackend)
			{
				g_routeBackend->Request(endpointId, ReadSavedPeriod(endpointId));
				g_routeEndpoint = endpointId;
				return S_OK;
			}
			started = std::make_shared<OutputBackend>(OpenEndpoint, true);
			started->Launch();
			started->Request(endpointId, ReadSavedPeriod(endpointId));
			started->SetRunning(true);
			g_routeBackend = started;
			g_routeEndpoint = endpointId;
			g_routeStartTick.store(GetTickCount64(), std::memory_order_release);   // start the source watchdog
			g_routeLastFeedTick.store(0, std::memory_order_release);
		}
		// Register the sink and mute the forward outside nothing critical; if the sink cannot attach
		// (proxy vanished), tear the backend back down and report not-ready rather than half-route.
		if (!OutputTap::AddProxySink(&RouteSink))
		{
			std::shared_ptr<OutputBackend> failed;
			{ std::lock_guard<std::mutex> guard(g_routeMutex); failed.swap(g_routeBackend); g_routeEndpoint.clear(); g_routeManaged = false; }
			g_routeStartTick.store(0, std::memory_order_release);   // route did not attach: watchdog off
			if (failed) failed->Shutdown();
			return HRESULT_FROM_WIN32(ERROR_NOT_READY);
		}
		OutputTap::SetProxyForwardMuted(true);
		LOG_INFO("(AUDIO ROUTING) ROUTE feed rendering the game mix to an alternate device" << std::endl);
		return S_OK;
	}

	// Tear the route down: unmute the forward (the ASIO device plays again), drop the sink, stop the
	// backend. Safe to call when no route is active. The backend is shut down outside the lock so the
	// audio-thread sink never blocks on a driver stop.
	HRESULT RouteStop()
	{
		std::shared_ptr<OutputBackend> backend;
		{
			std::lock_guard<std::mutex> guard(g_routeMutex);
			backend.swap(g_routeBackend);
			g_routeEndpoint.clear();
			g_routeManaged = false;
			g_routeStartTick.store(0, std::memory_order_release);   // no route: watchdog off
			g_routeLastFeedTick.store(0, std::memory_order_release);
		}
		if (!backend) return S_OK;
		OutputTap::RemoveProxySink(&RouteSink);
		OutputTap::SetProxyForwardMuted(false);
		backend->Shutdown();
		LOG_INFO("(AUDIO ROUTING) ROUTE feed stopped; game mix back on its own device" << std::endl);
		return S_OK;
	}

	bool RouteFeedStalled();

	// Health of a live route, for the overlay's switch-and-verify: S_FALSE = no route, S_OK = playing,
	// E_PENDING = still opening, a failure HRESULT = the device could not be opened or was lost.
	HRESULT RouteHealth()
	{
		std::shared_ptr<OutputBackend> backend;
		{
			std::lock_guard<std::mutex> guard(g_routeMutex);
			backend = g_routeBackend;
		}
		if (!backend) return S_FALSE;
		if (RouteFeedStalled()) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
		const OutputBackendStatus status = backend->GetStatus();
		switch (status.state)
		{
		case BackendState::Running: return S_OK;
		case BackendState::Opening:
		case BackendState::Priming:
		case BackendState::Recovering: return E_PENDING;
		default: return FAILED(status.error) ? status.error : E_FAIL;
		}
	}

	// While routing, the reported endpoint includes the route transport so the GUI can distinguish
	// real-ASIO-fed output from a route fed by the virtual proxy clock.
	std::wstring RouteEndpointTag()
	{
		std::lock_guard<std::mutex> guard(g_routeMutex);
		if (!g_routeBackend) return std::wstring();
		const wchar_t* prefix = OutputTap::ProxyOutputMode() == 1 ? L"(route-virtual)" : L"(route)";
		return std::wstring(prefix) + g_routeEndpoint;
	}

	// True when a route is live but its source (the proxy's ASIO buffer callback) has stopped delivering
	// blocks, i.e. the game's ASIO output has stalled. This is what happens when the interface is unplugged:
	// RS_ASIO opens its host once at launch and cannot re-bind a device, so the callback never resumes and
	// the still-open WASAPI backend renders silence. A start grace covers backend spin-up. Continuous silence
	// does NOT count as stalled (the callback keeps firing with zero-filled buffers, refreshing the feed tick),
	// so only a genuinely stopped callback trips this. The proxy recovery loop replaces a stalled
	// real callback source with its virtual clock.
	bool RouteFeedStalled()
	{
		const uint64_t start = g_routeStartTick.load(std::memory_order_acquire);
		if (start == 0) return false;                     // not routing
		const uint64_t now = GetTickCount64();
		if (now - start < 1500) return false;             // let the route and its first callback arrive
		const uint64_t last = g_routeLastFeedTick.load(std::memory_order_acquire);
		if (last == 0) return true;                       // past grace and never fed: source already dead
		return now - last > 800;                          // fed before, then stopped: source stalled
	}

	bool ResolveActiveOutput(const std::wstring& requested, std::wstring& resolved, bool& usedDefault)
	{
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(com) && com != RPC_E_CHANGED_MODE) return false;
		const bool uninitialize = SUCCEEDED(com);
		auto finish = [uninitialize](bool result)
		{
			if (uninitialize) CoUninitialize();
			return result;
		};
		usedDefault = requested.empty();
		if (!requested.empty())
		{
			Microsoft::WRL::ComPtr<IAudioClient3> requestedClient;
			resolved = requested;
			if (SUCCEEDED(OpenEndpoint(resolved, &requestedClient))) return finish(true);
			usedDefault = true;
		}
		Microsoft::WRL::ComPtr<IAudioClient3> defaultClient;
		resolved.clear();
		return finish(SUCCEEDED(OpenEndpoint(resolved, &defaultClient)));
	}

	// Status returned while nothing is routed. The playback mixer is the game's own bus volumes,
	// set through VolumeControl (game code), so it works with or without routing and stays live here.
	// Only the features that genuinely need an owned output, recording (2, 3) and switching the
	// physical output device (4, 6), are refused with a clear code so the GUI can say routing is
	// required. A sentinel endpoint lets the GUI show "passthrough" without changing the wire layout.
	ControlResponse PassthroughStatus(const ControlRequest& request)
	{
		ControlResponse response;
		// With nothing routed, the loudness guard (op 16) runs in the proxy ASIO driver.
		const auto configureGuard = [](bool limiterOn, float ceilingLin, bool agcOn, float targetRms)
		{
			return OutputTap::ConfigureOutputGuard(limiterOn, ceilingLin, agcOn, targetRms);
		};
		if (HandleSharedControl(request, response, configureGuard))
		{
			// handled: the input-hook DSP, mixer, latency probe, gate override, proxy promotion, Player 2 Cable
		}
		else if (request.operation == 2)
		{
			// Wet + dry takes consume the normalized output tap and input hook streams. The tap works
			// with both the ASIO-backed and cable/WASAPI output strategies.
			response.result = OutputTap::StartRecording(request.value);
		}
		std::wstring stoppedPath;
		uint64_t stoppedFrames = 0, stoppedStarted = 0;
		bool didStop = false;
		if (request.operation == 3)
		{
			response.result = OutputTap::StopRecording(stoppedPath, stoppedFrames, stoppedStarted);
			didStop = true;
		}
		else if (request.operation == 4 || request.operation == 6)
		{
			// Switching the game's OWN output device is a routing feature; passthrough leaves it alone.
			// Alternate-device routing (ops 21/22) is how passthrough plays elsewhere without a restart.
			response.result = HRESULT_FROM_WIN32(ERROR_NOT_READY);
		}
		else if (request.operation == 21)
		{
			// Route the game mix to the given WASAPI device (e.g. laptop speakers) live, no restart.
			// (Op 19 is the input suppressor, op 20 reserved; alternate-device routing is 21/22.)
			// Refused for the Windows endpoint of the interface RS_ASIO is bound to: opening it silences the ASIO
			// device and RS_ASIO input never recovers. See OutputDevices.hpp.
			response.result = OutputDevices::IsProtected(request.value)
				? HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) : RouteStart(request.value);
			if (SUCCEEDED(response.result)) SaveLastOutput(request.value);
		}
		else if (request.operation == 22)
		{
			// Stop routing: hand the mix back to the game's own (ASIO) device, and stop restoring the last pick.
			response.result = RouteStop();
			SaveLastOutput(L"");
		}
		response.outputError = S_OK;
		response.dryInputReady = DrySignalRecording::IsReady() ? 1 : 0;
		response.recordingError = OutputTap::RecordingError();
		response.recording = OutputTap::IsRecording() && SUCCEEDED(response.recordingError) ? 1 : 0;
		response.recordedFrames = OutputTap::RecordedFrames();
		response.recordingStarted = OutputTap::RecordingStarted();
		response.recordingSource = OutputTap::IsRecordingDry() ? 1 : 0;
		OutputTap::ReadOutputLevels(response.outputPeak, response.outputRms, 2);
		response.proxyInputMode = static_cast<uint32_t>(OutputTap::ProxyInputMode());
		// On stop, report the finished take's frame count and start FILETIME so a video take can align
		// and mux against it (the live recorder is already cleared, so these come from StopRecording).
		if (didStop)
		{
			response.recordedFrames = stoppedFrames;
			response.recordingStarted = stoppedStarted;
			wcsncpy_s(response.file, stoppedPath.c_str(), _TRUNCATE);
		}
		for (unsigned int channel = 0; channel < 7; ++channel)
		{
			if (!VolumeControl::GetPlaybackVolume(channel, response.volumes[channel]))
			{
				response.mixerError = E_FAIL;
				response.volumes[channel] = -1.f;
			}
		}
		// Report the real transport, not merely the presence of the proxy DLL. A virtual proxy with no
		// Windows endpoint is a healthy silent device from Rocksmith's perspective, never high-performance.
		const std::wstring routeTag = RouteEndpointTag();
		if (!routeTag.empty() && RouteFeedStalled())
		{
			// Route is up but its ASIO source has stalled (interface unplugged). Keep the device id while
			// the proxy recovery loop replaces the real callback source with its virtual clock.
			const size_t prefixEnd = routeTag.find(L')');
			const std::wstring stalled = L"(route-stalled)" +
				(prefixEnd == std::wstring::npos ? routeTag : routeTag.substr(prefixEnd + 1));
			wcsncpy_s(response.endpoint, stalled.c_str(), _TRUNCATE);
		}
		else
		{
			const int proxyMode = OutputTap::ProxyOutputMode();
			const wchar_t* idleTag = proxyMode == 3 ? L"(downgrading)" : proxyMode == 2 ? L"(passthrough)" : proxyMode == 1 ? L"(silent)" : L"(starting)";
			wcsncpy_s(response.endpoint, routeTag.empty() ? idleTag : routeTag.c_str(), _TRUNCATE);
		}
		if (request.operation == 18) WriteLatencyResult(response);   // overwrites file with the latency result
		return response;
	}

	// Apply one control request against the live engine, picking the managed output session when one owns
	// output or the passthrough backend otherwise (the exact dispatch the control pipe uses). Exposed so the
	// in-game overlay can drive the audio backend directly (no pipe, same process). Serialised so the render
	// thread (overlay) and the pipe worker thread never enter the engine handlers concurrently.
	namespace
	{
		std::mutex controlMutex;

		ControlResponse DispatchLocked(const ControlRequest& request)
		{
			std::shared_ptr<OutputSession> session;
			{
				std::lock_guard<std::mutex> guard(g_sessionMutex);
				session = g_activeSession.lock();
			}
			return session ? session->HandleControl(request) : PassthroughStatus(request);
		}
	}

	ControlResponse DispatchControl(const ControlRequest& request)
	{
		std::lock_guard<std::mutex> serialize(controlMutex);
		return DispatchLocked(request);
	}

	ControlResponse DispatchControlPreview(const ControlRequest& request)
	{
		previewOnly = true;
		const ControlResponse response = DispatchControl(request);
		previewOnly = false;
		return response;
	}

	// Some ops hold controlMutex for a long time (op 25 stops the route and loads the ASIO driver). The overlay
	// polls status from the game's render thread, and waiting there would freeze the game for the whole bind (or
	// for good with a wedged driver). Pollers use this instead and keep their last status when the engine is busy.
	bool TryDispatchControl(const ControlRequest& request, ControlResponse& response)
	{
		std::unique_lock<std::mutex> serialize(controlMutex, std::try_to_lock);
		if (!serialize.owns_lock()) return false;
		response = DispatchLocked(request);
		return true;
	}

	void StartControlServer()
	{
		if (!g_control) g_control = new AudioControlServer();
		const HRESULT result = g_control->Start([](const ControlRequest& request) -> ControlResponse
		{
			return DispatchControl(request);
		});
		if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED))
			LOG_ERROR("(AUDIO ROUTING) Control pipe could not start, HRESULT " << std::hex << result << std::dec << std::endl);
	}

	ProxyRouteAction PlanProxyRoute(int proxyMode, bool routeExists, bool routeManaged,
		bool routedEndpointAvailable, bool resolvedOutputAvailable)
	{
		if (proxyMode == 3) return ProxyRouteAction::DemoteProxy;
		if (proxyMode == 2)
			return routeExists && routeManaged ? ProxyRouteAction::StopManagedRoute : ProxyRouteAction::None;
		if (proxyMode != 1) return ProxyRouteAction::None;
		if (!resolvedOutputAvailable)
			return routeExists && routeManaged ? ProxyRouteAction::StopManagedRoute : ProxyRouteAction::None;
		if (!routeExists) return ProxyRouteAction::StartManagedRoute;
		if (routeManaged && !routedEndpointAvailable) return ProxyRouteAction::ReplaceLostManagedRoute;
		return ProxyRouteAction::None;
	}

	void RefreshProxyOutput()
	{
		const uint64_t now = GetTickCount64();
		uint64_t lastRefresh = g_lastProxyRefreshTick.load(std::memory_order_acquire);
		if (now - lastRefresh < 250 || !g_lastProxyRefreshTick.compare_exchange_strong(lastRefresh, now)) return;
		if (g_proxyPromotionRunning.load(std::memory_order_acquire)) return;
		const int mode = OutputTap::ProxyOutputMode();
		bool routeExists = false;
		bool routeManaged = false;
		std::wstring routedEndpoint;
		{
			std::lock_guard<std::mutex> guard(g_routeMutex);
			routeExists = g_routeBackend != nullptr;
			routeManaged = g_routeManaged;
			routedEndpoint = g_routeEndpoint;
		}
		if (PlanProxyRoute(mode, routeExists, routeManaged, true, true) == ProxyRouteAction::DemoteProxy)
		{
			if (!g_proxyDemotionRunning.exchange(true))
			{
				std::thread([]
				{
					OutputTap::TryDemoteProxyOutput();
					g_proxyDemotionRunning.store(false, std::memory_order_release);
				}).detach();
			}
			return;
		}
		if (mode == 2)
		{
			if (PlanProxyRoute(mode, routeExists, routeManaged, true, true) == ProxyRouteAction::StopManagedRoute) RouteStop();
			else if (!routeExists && !g_lastOutputRestoreTried.exchange(true))
			{
				// First time this launch the game is playing straight to ASIO: move it to the remembered output
				// if that device is present. Not managed (g_routeManaged stays false): it is an explicit choice.
				const std::wstring saved = ReadLastOutput();
				std::wstring resolved;
				bool usedDefault = false;
				if (!saved.empty() && !OutputDevices::IsProtected(saved) && ResolveActiveOutput(saved, resolved, usedDefault) && !usedDefault)
				{
					const HRESULT restored = RouteStart(saved);
					LOG_INFO("(AUDIO ROUTING) Restoring last output " << (SUCCEEDED(restored) ? "started" : "failed") << std::endl);
				}
				else if (!saved.empty())
					LOG_INFO("(AUDIO ROUTING) Last output not present; staying on the ASIO device" << std::endl);
			}
			return;
		}
		if (mode != 1) return;

		const auto configuration = ReadConfiguration();
		std::wstring endpoint;
		bool usedDefault = false;
		const bool hasOutput = ResolveActiveOutput(configuration.outputDeviceId, endpoint, usedDefault);
		bool routedEndpointAvailable = true;
		std::wstring survivingEndpoint;
		if (routeExists && routeManaged)
		{
			bool fellBackFromRoute = false;
			routedEndpointAvailable = ResolveActiveOutput(routedEndpoint, survivingEndpoint, fellBackFromRoute)
				&& !fellBackFromRoute;
		}
		const ProxyRouteAction action = PlanProxyRoute(mode, routeExists, routeManaged,
			routedEndpointAvailable, hasOutput);
		if (action == ProxyRouteAction::StopManagedRoute)
		{
			RouteStop();
			return;
		}
		if (action == ProxyRouteAction::StartManagedRoute || action == ProxyRouteAction::ReplaceLostManagedRoute)
		{
			const std::wstring& target = action == ProxyRouteAction::ReplaceLostManagedRoute && !survivingEndpoint.empty()
				? survivingEndpoint : endpoint;
			if (g_proxyPromotionRunning.load(std::memory_order_acquire)) return;
			if (SUCCEEDED(RouteStart(target)))
			{
				std::lock_guard<std::mutex> guard(g_routeMutex);
				g_routeManaged = true;
			}
		}
	}

	HRESULT CreateClient(const std::wstring& endpointId, IAudioClient** client)
	{
		if (!client) return E_POINTER;
		*client = nullptr;
		try
		{
			auto session = std::make_shared<OutputSession>();
			const HRESULT result = session->Open(endpointId);
			if (FAILED(result)) return result;
			*client = new AudioClient(session);
			{
				std::lock_guard<std::mutex> guard(g_sessionMutex);
				g_activeSession = session;
			}
			return S_OK;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return E_FAIL; }
	}
}
