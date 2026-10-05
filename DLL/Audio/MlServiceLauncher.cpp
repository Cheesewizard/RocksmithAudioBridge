#include "stdafx.h"
#include "MlServiceLauncher.hpp"
#include "MlServiceControl.hpp"
#include "../Log.hpp"

#include <windows.h>
#include <filesystem>
#include <string>

namespace
{
	bool attempted = false;
	HANDLE serviceJob = nullptr;
	HANDLE serviceProcess = nullptr;
	HANDLE controlMapping = nullptr;
	HANDLE restartEvent = nullptr;
	MlServiceControl::Status* control = nullptr;
	bool controlAttempted = false;
	bool restarting = false;
	uint64_t startedTick = 0;
	MlServiceControl::State serviceState = MlServiceControl::State::Stopped;
	DWORD serviceError = 0;

	void PublishStatus()
	{
		if (control == nullptr) return;
		InterlockedIncrement(&control->sequence);
		control->version = 1;
		control->tick = GetTickCount64();
		control->processId = GetCurrentProcessId();
		control->state = serviceState;
		control->error = serviceError;
		InterlockedIncrement(&control->sequence);
	}

	void InitializeControl()
	{
		if (controlAttempted) return;
		controlAttempted = true;
		const auto suffix = std::to_wstring(GetCurrentProcessId());
		controlMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
			sizeof(MlServiceControl::Status), (L"Local\\RSModsPlus.MlControl.v1." + suffix).c_str());
		if (controlMapping != nullptr)
			control = static_cast<MlServiceControl::Status*>(MapViewOfFile(controlMapping, FILE_MAP_WRITE, 0, 0, sizeof(MlServiceControl::Status)));
		restartEvent = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\RSModsPlus.MlRestart.v1." + suffix).c_str());
		if (control == nullptr || restartEvent == nullptr)
		{
			LOG_ERROR("[MlServiceLauncher] Cannot create ML status/restart controls, error=" << GetLastError() << std::endl);
		}
	}
}

void MlServiceLauncher::EnsureStarted()
{
	InitializeControl();
	if (restartEvent != nullptr && WaitForSingleObject(restartEvent, 0) == WAIT_OBJECT_0 && !restarting)
	{
		restarting = true;
		serviceState = MlServiceControl::State::Restarting;
		serviceError = 0;
		if (serviceJob != nullptr) CloseHandle(serviceJob);
		serviceJob = nullptr;
		LOG_INFO("[MlServiceLauncher] ML restart requested from settings." << std::endl);
	}
	if (serviceProcess != nullptr && WaitForSingleObject(serviceProcess, 0) == WAIT_OBJECT_0)
	{
		DWORD exitCode = 0;
		GetExitCodeProcess(serviceProcess, &exitCode);
		if (!restarting)
		{
			LOG_ERROR("[MlServiceLauncher] Bundled ML service exited, code=" << exitCode
				<< ". See %LOCALAPPDATA%\\RSModsPlus\\Logs\\ml-service.log and startup-error.log." << std::endl);
		}
		CloseHandle(serviceProcess);
		serviceProcess = nullptr;
		serviceError = exitCode;
		serviceState = MlServiceControl::State::Stopped;
	}
	if (restarting)
	{
		if (serviceProcess != nullptr) { PublishStatus(); return; }
		restarting = false;
		attempted = false;
	}
	if (attempted)
	{
		if (serviceProcess != nullptr)
			serviceState = GetTickCount64() - startedTick < 1000 ? MlServiceControl::State::Starting : MlServiceControl::State::Waiting;
		PublishStatus();
		return;
	}
	attempted = true;
	serviceState = MlServiceControl::State::Failed;
	serviceError = 0;

	wchar_t gamePath[32768] = {};
	const DWORD length = GetModuleFileNameW(nullptr, gamePath, static_cast<DWORD>(std::size(gamePath)));
	if (length == 0 || length >= std::size(gamePath))
	{
		serviceError = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
		LOG_ERROR("[MlServiceLauncher] Cannot resolve the game installation directory." << std::endl);
		PublishStatus();
		return;
	}
	const auto directory = std::filesystem::path(gamePath).parent_path();
	const auto executable = directory / L"RSMods.exe";
	std::error_code error;
	if (!std::filesystem::is_regular_file(executable, error)
		|| !std::filesystem::is_regular_file(directory / L"RocksmithAudioBridge.dll", error))
	{
		LOG_ERROR("[MlServiceLauncher] Bundled ML service is missing. Install the complete matching release package." << std::endl);
		serviceError = ERROR_FILE_NOT_FOUND;
		PublishStatus();
		return;
	}

	if (serviceJob != nullptr) CloseHandle(serviceJob);
	serviceJob = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (serviceJob == nullptr || !SetInformationJobObject(serviceJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
	{
		serviceError = GetLastError();
		LOG_ERROR("[MlServiceLauncher] Cannot establish game-owned ML service lifetime, error=" << serviceError << std::endl);
		if (serviceJob != nullptr) CloseHandle(serviceJob);
		serviceJob = nullptr;
		PublishStatus();
		return;
	}

	std::wstring command = L"\"" + executable.wstring() + L"\" --ml-service " + std::to_wstring(GetCurrentProcessId());
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
		CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process))
	{
		serviceError = GetLastError();
		LOG_ERROR("[MlServiceLauncher] Cannot start bundled ML service, error=" << serviceError << std::endl);
		CloseHandle(serviceJob);
		serviceJob = nullptr;
		PublishStatus();
		return;
	}
	if (!AssignProcessToJobObject(serviceJob, process.hProcess) || ResumeThread(process.hThread) == static_cast<DWORD>(-1))
	{
		serviceError = GetLastError();
		LOG_ERROR("[MlServiceLauncher] Cannot bind/start bundled ML service, error=" << serviceError << std::endl);
		TerminateProcess(process.hProcess, 1);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		CloseHandle(serviceJob);
		serviceJob = nullptr;
		PublishStatus();
		return;
	}
	CloseHandle(process.hThread);
	serviceProcess = process.hProcess;
	startedTick = GetTickCount64();
	serviceState = MlServiceControl::State::Starting;
	PublishStatus();
	LOG_INFO("[MlServiceLauncher] Started bundled C# FretNet service with embedded model; bound to game lifetime." << std::endl);
}

void MlServiceLauncher::Poll(bool wanted)
{
	InitializeControl();
	if (!attempted && !wanted)
	{
		// Pressing Restart in the overlay before Note by Note was used starts it now.
		if (restartEvent == nullptr || WaitForSingleObject(restartEvent, 0) != WAIT_OBJECT_0) return;
		LOG_INFO("[MlServiceLauncher] ML service start requested from settings." << std::endl);
	}
	EnsureStarted();
}

void MlServiceLauncher::Shutdown()
{
	if (control != nullptr) UnmapViewOfFile(control);
	control = nullptr;
	if (controlMapping != nullptr) CloseHandle(controlMapping);
	controlMapping = nullptr;
	if (restartEvent != nullptr) CloseHandle(restartEvent);
	restartEvent = nullptr;
	if (serviceJob != nullptr)
	{
		CloseHandle(serviceJob);
		serviceJob = nullptr;
	}
	if (serviceProcess != nullptr)
	{
		CloseHandle(serviceProcess);
		serviceProcess = nullptr;
	}
}

namespace
{
	// Mirrors MlServiceConnection.ReadResults: results are "fresh" when the service's last result is under
	// 500 ms old and within half a second of the audio the game is currently publishing.
	bool ReadResultsFresh(std::string& message)
	{
		HANDLE resultMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\RSModsPlus.MlStringFret.v2");
		HANDLE audioMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\RSModsPlus.MlAudio.v2");
		const uint8_t* result = resultMapping ? static_cast<const uint8_t*>(MapViewOfFile(resultMapping, FILE_MAP_READ, 0, 0, 112)) : nullptr;
		const uint8_t* audio = audioMapping ? static_cast<const uint8_t*>(MapViewOfFile(audioMapping, FILE_MAP_READ, 0, 0, 64)) : nullptr;
		bool fresh = false;
		if (result == nullptr || audio == nullptr)
			message = "ML service running, initializing or waiting for game audio.";
		else if (*reinterpret_cast<const uint32_t*>(result) != 0x46535352 || *reinterpret_cast<const uint32_t*>(result + 4) != 2
			|| *reinterpret_cast<const uint32_t*>(audio) != 0x4C4D5352 || *reinterpret_cast<const uint32_t*>(audio + 4) != 2)
			message = "ML data version mismatch. Install the matching package.";
		else
		{
			message = "ML service running, waiting for a consistent result.";
			for (int attempt = 0; attempt < 4; ++attempt)
			{
				const uint32_t sequence = *reinterpret_cast<const volatile uint32_t*>(result + 8);
				if (sequence & 1) continue;
				MemoryBarrier();
				const uint64_t tick = *reinterpret_cast<const volatile uint64_t*>(result + 88);
				const uint64_t sample = *reinterpret_cast<const volatile uint64_t*>(result + 96);
				const uint32_t rate = *reinterpret_cast<const volatile uint32_t*>(result + 104);
				MemoryBarrier();
				if (*reinterpret_cast<const volatile uint32_t*>(result + 8) != sequence) continue;
				const uint32_t currentRate = *reinterpret_cast<const volatile uint32_t*>(audio + 8);
				const uint64_t currentSample = *reinterpret_cast<const volatile uint64_t*>(audio + 16);
				const uint64_t now = GetTickCount64();
				fresh = tick != 0 && now >= tick && now - tick <= 500 && rate != 0 && rate == currentRate
					&& currentSample >= sample && currentSample - sample <= rate / 2;
				message = fresh ? "Connected, receiving fresh ML results." : "ML service running, waiting for fresh audio/results.";
				break;
			}
		}
		if (result) UnmapViewOfFile(result);
		if (audio) UnmapViewOfFile(audio);
		if (resultMapping) CloseHandle(resultMapping);
		if (audioMapping) CloseHandle(audioMapping);
		return fresh;
	}
}

void MlServiceLauncher::Describe(std::string& message, bool& connected, bool& canRestart)
{
	connected = false;
	canRestart = false;
	if (restartEvent == nullptr)
	{
		message = "ML controls unavailable. The service has not been started in this session.";
		return;
	}
	if (!attempted && serviceProcess == nullptr && !restarting)
	{
		message = "ML service not running yet: it starts when you open Riff Repeater for Note by Note. Restart starts it now.";
		canRestart = true;
		return;
	}
	switch (restarting ? MlServiceControl::State::Restarting : serviceState)
	{
	case MlServiceControl::State::Starting: message = "Starting ML service..."; return;
	case MlServiceControl::State::Restarting: message = "Restarting ML service..."; return;
	case MlServiceControl::State::Stopped:
		message = "ML service stopped (exit code " + std::to_string(serviceError) + ").";
		canRestart = true;
		return;
	case MlServiceControl::State::Failed:
		message = serviceError == ERROR_FILE_NOT_FOUND ? "ML files missing. Install the complete matching package."
			: "ML service could not start (Windows error " + std::to_string(serviceError) + ").";
		canRestart = true;
		return;
	case MlServiceControl::State::Waiting:
		connected = ReadResultsFresh(message);
		canRestart = true;
		return;
	}
	message = "Unknown ML status.";
}

bool MlServiceLauncher::RequestRestart()
{
	return restartEvent != nullptr && SetEvent(restartEvent) != FALSE;
}
