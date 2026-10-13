#include "stdafx.h"
#include "ExclusiveOutputDevice.hpp"

#include <avrt.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Audio::SharedOutput
{
	namespace
	{
		constexpr wchar_t kPrefix[] = L"excl:";
		constexpr uint32_t kSampleRate = 48000;
		constexpr uint32_t kMaxBuffer = 4096;   // the backend's FIFO; a bigger period than this is refused

		WAVEFORMATEXTENSIBLE Format(WORD containerBits, WORD validBits, bool floating)
		{
			WAVEFORMATEXTENSIBLE format{};
			format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
			format.Format.nChannels = 2;
			format.Format.nSamplesPerSec = kSampleRate;
			format.Format.wBitsPerSample = containerBits;
			format.Format.nBlockAlign = static_cast<WORD>(2 * containerBits / 8);
			format.Format.nAvgBytesPerSec = kSampleRate * format.Format.nBlockAlign;
			format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
			format.Samples.wValidBitsPerSample = validBits;
			format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
			format.SubFormat = floating ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
			return format;
		}

		std::string Hex(HRESULT result)
		{
			char text[16];
			sprintf_s(text, "%08lX", static_cast<unsigned long>(result));
			return text;
		}

		std::string Why(HRESULT result)
		{
			if (result == AUDCLNT_E_DEVICE_IN_USE) return "another program is using it in exclusive mode";
			if (result == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
				return "Windows does not allow exclusive mode on it (Sound settings, the device's Properties, Advanced: \"Allow applications to take exclusive control\")";
			if (result == AUDCLNT_E_UNSUPPORTED_FORMAT) return "it does not play 48 kHz stereo in exclusive mode";
			if (result == AUDCLNT_E_DEVICE_INVALIDATED || result == AUDCLNT_E_ENDPOINT_CREATE_FAILED) return "it is not available (unplugged?)";
			return "Windows refused it (HRESULT " + Hex(result) + ")";
		}
	}

	bool ExclusiveOutputDevice::IsExclusiveEndpoint(const std::wstring& endpointId) { return endpointId.rfind(kPrefix, 0) == 0; }
	std::wstring ExclusiveOutputDevice::WindowsId(const std::wstring& endpointId) { return IsExclusiveEndpoint(endpointId) ? endpointId.substr(wcslen(kPrefix)) : endpointId; }
	std::wstring ExclusiveOutputDevice::EndpointOf(const std::wstring& windowsId) { return kPrefix + windowsId; }

	ExclusiveOutputDevice::~ExclusiveOutputDevice() { Close(); }

	bool ExclusiveOutputDevice::Open(const std::wstring& windowsId, const Activate& activate, uint32_t desiredFrames, bool polling, Fill fill, std::string& failure)
	{
		m_id = windowsId;
		m_polling = polling;
		const DWORD flags = polling ? 0 : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
		m_fill = std::move(fill);
		std::wstring id = windowsId;
		HRESULT result = activate(id, &m_client);
		if (SUCCEEDED(result) && !m_client) result = E_UNEXPECTED;
		if (FAILED(result)) { failure = Why(result); Close(); return false; }

		// The first 48 kHz stereo format the device takes in exclusive mode, best first.
		struct Candidate { WORD container, valid; bool floating; Sample sample; const char* name; };
		static constexpr Candidate candidates[] = {
			{ 32, 32, true, Sample::Float32, "32-bit float" },
			{ 32, 32, false, Sample::Int32, "32-bit" },
			{ 32, 24, false, Sample::Int32, "24-bit (in 32)" },
			{ 24, 24, false, Sample::Int24, "24-bit" },
			{ 16, 16, false, Sample::Int16, "16-bit" },
		};
		WAVEFORMATEXTENSIBLE format{};
		bool found = false;
		for (const Candidate& candidate : candidates)
		{
			format = Format(candidate.container, candidate.valid, candidate.floating);
			if (m_client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr) != S_OK) continue;
			m_sample = candidate.sample;
			m_formatName = candidate.name;
			m_blockAlign = format.Format.nBlockAlign;
			found = true;
			break;
		}
		if (!found) { failure = Why(AUDCLNT_E_UNSUPPORTED_FORMAT); Close(); return false; }

		// Its smallest exclusive period, or the size asked for when that is larger.
		REFERENCE_TIME normal = 0, minimum = 0;
		result = m_client->GetDevicePeriod(&normal, &minimum);
		if (FAILED(result)) { failure = Why(result); Close(); return false; }
		REFERENCE_TIME period = (std::max)(minimum, static_cast<REFERENCE_TIME>(desiredFrames) * 10000000 / kSampleRate);
		// Event driven: the buffer is one period. Polling: three, so a 1 ms timer always finds room for a whole period.
		result = m_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, polling ? period * 3 : period, period, &format.Format, nullptr);
		if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
		{
			// The driver wants a period that is a whole number of its own blocks: take the size it offers, on a fresh client
			// (a failed Initialize leaves the client unusable).
			UINT32 aligned = 0;
			m_client->GetBufferSize(&aligned);
			m_client->Release();
			m_client = nullptr;
			id = windowsId;
			result = activate(id, &m_client);
			if (SUCCEEDED(result) && !m_client) result = E_UNEXPECTED;
			const REFERENCE_TIME buffer = static_cast<REFERENCE_TIME>(10000000.0 * aligned / kSampleRate + 0.5);
			if (!polling) period = buffer;
			if (SUCCEEDED(result)) result = m_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, buffer, period, &format.Format, nullptr);
		}
		if (FAILED(result)) { failure = Why(result); Close(); return false; }

		UINT32 frames = 0;
		REFERENCE_TIME latency = 0;
		m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		m_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!m_event || !m_quit) result = E_OUTOFMEMORY;
		if (SUCCEEDED(result) && !polling) result = m_client->SetEventHandle(m_event);
		if (SUCCEEDED(result)) result = m_client->GetBufferSize(&frames);
		if (SUCCEEDED(result) && (frames == 0 || frames > kMaxBuffer)) result = AUDCLNT_E_BUFFER_SIZE_ERROR;
		if (SUCCEEDED(result)) result = m_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&m_render));
		if (SUCCEEDED(result)) m_client->GetStreamLatency(&latency);
		// One period per fill: the whole buffer when event driven, a third of it when polling.
		const uint32_t chunk = polling ? (std::max)(1u, static_cast<uint32_t>(period * kSampleRate / 10000000)) : frames;
		if (SUCCEEDED(result) && chunk > frames) result = AUDCLNT_E_BUFFER_SIZE_ERROR;
		if (SUCCEEDED(result))
		{
			// Start with a period of silence queued, as Windows asks of an exclusive event stream.
			BYTE* data = nullptr;
			result = m_render->GetBuffer(chunk, &data);
			if (SUCCEEDED(result)) result = m_render->ReleaseBuffer(chunk, AUDCLNT_BUFFERFLAGS_SILENT);
		}
		if (FAILED(result)) { failure = Why(result); Close(); return false; }

		m_bufferFrames = frames;
		m_chunkFrames = chunk;
		// What a filled period waits before it is heard: what is queued ahead of it (a period; up to the whole buffer when
		// polling), plus the driver's own latency.
		m_latencyFrames = (polling ? frames : chunk) + static_cast<uint32_t>(latency * kSampleRate / 10000000);
		m_stereo.assign(static_cast<size_t>(chunk) * 2, 0.0f);
		m_thread = std::thread(&ExclusiveOutputDevice::Run, this);
		LOG_INFO("(EXCLUSIVE OUTPUT) Opened in exclusive mode" << (polling ? " (polling)" : "") << ": " << chunk << "-frame period ("
			<< chunk * 1000.0 / kSampleRate << " ms; device minimum " << minimum / 10000.0 << " ms, buffer " << frames << " frames), "
			<< m_formatName << ", driver latency " << latency / 10000.0 << " ms" << std::endl);
		return true;
	}

	bool ExclusiveOutputDevice::Start()
	{
		if (!m_client) return false;
		m_running.store(true);
		const HRESULT result = m_client->Start();
		if (FAILED(result)) { m_running.store(false); return false; }
		return true;
	}

	void ExclusiveOutputDevice::Stop()
	{
		if (m_client && m_running.exchange(false)) m_client->Stop();
	}

	void ExclusiveOutputDevice::Close()
	{
		if (m_quit) SetEvent(m_quit);
		if (m_thread.joinable()) m_thread.join();
		Stop();
		if (m_render) { m_render->Release(); m_render = nullptr; }
		if (m_client) { m_client->Release(); m_client = nullptr; }
		if (m_event) { CloseHandle(m_event); m_event = nullptr; }
		if (m_quit) { CloseHandle(m_quit); m_quit = nullptr; }
	}

	void ExclusiveOutputDevice::Run()
	{
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		DWORD index = 0;
		HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
		if (m_polling)
		{
			// Every millisecond: top the device up a period at a time while it has room for one.
			while (WaitForSingleObject(m_quit, 1) == WAIT_TIMEOUT)
			{
				if (!m_running.load()) continue;
				UINT32 padding = 0;
				if (FAILED(m_client->GetCurrentPadding(&padding))) { m_resetRequested.store(true); break; }
				bool failed = false;
				while (padding + m_chunkFrames <= m_bufferFrames)
				{
					if (!Render(m_chunkFrames)) { failed = true; break; }
					padding += m_chunkFrames;
				}
				if (failed) { m_resetRequested.store(true); break; }
			}
		}
		else
		{
			HANDLE events[] = { m_quit, m_event };
			for (;;)
			{
				const DWORD wake = WaitForMultipleObjects(2, events, FALSE, 200);
				if (wake == WAIT_OBJECT_0 || wake == WAIT_FAILED) break;
				if (wake != WAIT_OBJECT_0 + 1 || !m_running.load()) continue;
				if (!Render(m_chunkFrames)) { m_resetRequested.store(true); break; }
			}
		}
		if (task) AvRevertMmThreadCharacteristics(task);
		if (SUCCEEDED(com)) CoUninitialize();
	}

	bool ExclusiveOutputDevice::Render(uint32_t frames) noexcept
	{
		BYTE* data = nullptr;
		if (FAILED(m_render->GetBuffer(frames, &data))) return false;
		m_fill(m_stereo.data(), frames);
		const size_t samples = static_cast<size_t>(frames) * 2;
		for (size_t i = 0; i < samples; ++i)
		{
			const float value = std::isfinite(m_stereo[i]) ? std::clamp(m_stereo[i], -1.0f, 1.0f) : 0.0f;
			switch (m_sample)
			{
			case Sample::Float32: std::memcpy(data + i * 4, &value, 4); break;
			case Sample::Int32: { const int32_t pcm = static_cast<int32_t>(std::lround(static_cast<double>(value) * 2147483647.0)); std::memcpy(data + i * 4, &pcm, 4); break; }
			case Sample::Int24:
			{
				const int32_t pcm = static_cast<int32_t>(std::lround(static_cast<double>(value) * 8388607.0));
				data[i * 3] = static_cast<BYTE>(pcm & 0xFF);
				data[i * 3 + 1] = static_cast<BYTE>((pcm >> 8) & 0xFF);
				data[i * 3 + 2] = static_cast<BYTE>((pcm >> 16) & 0xFF);
				break;
			}
			case Sample::Int16: { const int16_t pcm = static_cast<int16_t>(std::lround(value * 32767.0f)); std::memcpy(data + i * 2, &pcm, 2); break; }
			}
		}
		if (FAILED(m_render->ReleaseBuffer(frames, 0))) return false;
		m_lastCallback.store(GetTickCount64(), std::memory_order_release);
		m_callbacks.fetch_add(1, std::memory_order_acq_rel);
		return true;
	}
}
