#pragma once

#include <Windows.h>
#include <cstdint>
#include <functional>
#include <thread>

namespace Audio
{
	struct ControlRequest
	{
		uint32_t version = 6;
		uint32_t operation = 0;
		wchar_t value[1024]{};
	};

	struct ControlResponse
	{
		uint32_t version = 6;
		HRESULT result = S_OK;
		HRESULT outputError = S_OK;
		HRESULT recordingError = S_OK;
		uint32_t recording = 0;
		uint32_t peak = 0;
		uint64_t recordedFrames = 0;
		wchar_t file[1024]{};
		wchar_t endpoint[512]{};
		uint64_t recordingStarted = 0;
		HRESULT mixerError = S_OK;
		float volumes[7]{};
		uint32_t dryInputReady = 0;
		uint32_t recordingSource = 0;
		float outputPeak[2]{};   // proxy output meter: peak-hold per channel [0,1] (0 when not metering)
		float outputRms[2]{};    // proxy output meter: smoothed RMS per channel [0,1]
		uint32_t proxyInputMode = 0; // 0 = physical ASIO/non-proxy, 1 = waiting for RTC, 2 = RTC fallback live
	};
	static_assert(sizeof(ControlResponse) == 3176, "Audio control response layout changed");

	// One bounded message per connection. Nonblocking pipe I/O keeps shutdown independent of clients.
	class AudioControlServer
	{
	public:
		~AudioControlServer() { Stop(); }
		HRESULT Start(std::function<ControlResponse(const ControlRequest&)> handler)
		{
			if (worker.joinable()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
			wchar_t name[128];
			swprintf_s(name, L"\\\\.\\pipe\\RSModsPlus.Audio.%u", GetCurrentProcessId());
			pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
				PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
				1, sizeof(ControlResponse), sizeof(ControlRequest), 0, nullptr);
			if (pipe == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
			stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!stop) { const HRESULT result = HRESULT_FROM_WIN32(GetLastError()); Stop(); return result; }
			try { worker = std::thread([this, handler]() { Run(handler); }); }
			catch (...) { Stop(); return E_OUTOFMEMORY; }
			return S_OK;
		}

		void Stop()
		{
			if (stop) SetEvent(stop);
			if (worker.joinable()) worker.join();
			if (pipe != INVALID_HANDLE_VALUE) { CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; }
			if (stop) { CloseHandle(stop); stop = nullptr; }
		}

	private:
		void Run(const std::function<ControlResponse(const ControlRequest&)>& handler)
		{
			bool replied = false;
			ULONGLONG connectedAt = 0;
			while (WaitForSingleObject(stop, 15) == WAIT_TIMEOUT)
			{
				if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
				{
					if (GetLastError() == ERROR_NO_DATA) { DisconnectNamedPipe(pipe); connectedAt = 0; replied = false; }
					continue;
				}
				if (!connectedAt) connectedAt = GetTickCount64();
				ControlRequest request{};
				DWORD bytes = 0;
				if (!replied && ReadFile(pipe, &request, sizeof(request), &bytes, nullptr))
				{
					ControlResponse response;
					if (bytes != sizeof(request) || request.version != 6 || request.value[1023] != 0) response.result = E_INVALIDARG;
					else
					{
						try { response = handler(request); }
						catch (...) { response.result = E_FAIL; }
					}
					if (!WriteFile(pipe, &response, sizeof(response), &bytes, nullptr) || bytes != sizeof(response))
					{
						DisconnectNamedPipe(pipe);
						connectedAt = 0;
					}
					replied = true;
				}
				DWORD available = 0;
				if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || GetTickCount64() - connectedAt > 3000)
				{
					DisconnectNamedPipe(pipe);
					connectedAt = 0;
					replied = false;
				}
			}
		}

		HANDLE pipe = INVALID_HANDLE_VALUE;
		HANDLE stop = nullptr;
		std::thread worker;
	};
}
