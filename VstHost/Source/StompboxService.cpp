#include "StompboxService.hpp"

#include "AudioFormat.hpp"
#include "HostLog.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace VstHost
{
	namespace
	{
		bool StartsWith(const std::string& text, const char* prefix)
		{
			const size_t length = std::strlen(prefix);
			return text.size() >= length && text.compare(0, length, prefix) == 0;
		}
	}

	StompboxService::StompboxService(StompboxEngine& engineToUse, std::function<bool(bool)> setPlaysCallback)
		: engine(engineToUse), setPlays(std::move(setPlaysCallback))
	{
		if (!setPlays) throw std::invalid_argument("StompboxService needs a setPlays callback.");
	}

	StompboxService::~StompboxService() { Stop(); }

	void StompboxService::Start(const std::wstring& dataPath, bool developmentRemote)
	{
		if (shared) return;
		shared = StompboxControl::Open(mapping);
		if (!shared)
		{
			Log("stompbox: the Tone designer's shared block could not be opened (%lu); the page cannot reach the engine", GetLastError());
			return;
		}
		if (shared->version != StompboxControl::VERSION)
		{
			Log("stompbox: shared block version %u, expected %u; the page cannot reach the engine", shared->version, StompboxControl::VERSION);
			Stop();
			return;
		}
		isDevelopmentRemote = developmentRemote;
		InterlockedExchange(&shared->hostPid, static_cast<LONG>(GetCurrentProcessId()));
		InterlockedExchange(&shared->playing, 0);
		SetState(StompboxControl::EngineState::Loading);

		std::string error;
		if (!engine.Load(dataPath, error))
		{
			SetError(error);
			SetState(StompboxControl::EngineState::Failed);
			return;
		}
		if (isDevelopmentRemote) engine.StartRemoteServer();
		else engine.SendCommand(std::string("SetGlobalChain MasterChain MasterIn Chain ") + StompboxControl::CHAIN_NAME + " MasterChain MasterOut");
		PublishCatalog();
		PublishPresets();
		PublishProgram();
		SetError({});
		SetState(StompboxControl::EngineState::Ready);
		Log("stompbox: ready for the Tone designer page%s", isDevelopmentRemote ? " (StompboxDev: Stompbox's own layout and remote server)" : "");
	}

	void StompboxService::Poll()
	{
		if (!shared) return;

		const bool wanted = StompboxControl::Load(&shared->wanted) != 0 && engine.IsLoaded();
		if (wanted != isPlaying)
		{
			const bool plays = setPlays(wanted);
			isPlaying = wanted && plays;
			InterlockedExchange(&shared->playing, isPlaying ? 1 : 0);
			if (wanted && !plays) SetError("The Tone designer could not switch the stream to its engine.");
		}

		// One command at a time: the page writes the next only after commandDone has caught up.
		const LONG sequence = StompboxControl::Load(&shared->commandSeq);
		if (sequence != StompboxControl::Load(&shared->commandDone))
		{
			RunCommand(std::string(shared->command, strnlen(shared->command, StompboxControl::COMMAND_CHARS)));
			InterlockedExchange(&shared->commandDone, sequence);
		}

		if (!engine.IsLoaded()) return;
		// Stompbox loads presets and models on its own thread: republish once it has finished.
		const bool loading = engine.IsPresetLoading();
		if (loading != wasPresetLoading)
		{
			wasPresetLoading = loading;
			InterlockedExchange(&shared->presetLoading, loading ? 1 : 0);
			if (!loading)
			{
				PublishProgram();
				PublishPresets();
			}
		}
	}

	void StompboxService::PublishLoad(const BlockTiming& timing, int frames)
	{
		if (!shared || frames <= 0) return;
		const double periodMicroseconds = 1000000.0 * frames / SAMPLE_RATE;
		const auto permille = [&](double microseconds) { return static_cast<LONG>(std::lround(1000.0 * microseconds / periodMicroseconds)); };
		InterlockedExchange(&shared->blockFrames, frames);
		InterlockedExchange(&shared->medianPermille, permille(timing.medianMicroseconds));
		InterlockedExchange(&shared->loadPermille, permille(timing.percentile99Microseconds));
		InterlockedExchange(&shared->peakPermille, permille(timing.maximumMicroseconds));
		InterlockedIncrement(&shared->loadSeq);
	}

	void StompboxService::RunCommand(const std::string& command)
	{
		if (!engine.IsLoaded())
		{
			SetError("The Stompbox engine is not loaded.");
			return;
		}
		SetError({});
		try
		{
			engine.SendCommand(command);
		}
		catch (const std::exception& exception)
		{
			SetError(exception.what());
			Log("stompbox: command failed (%s): %s", exception.what(), command.c_str());
			return;
		}
		PublishProgram();
		if (StartsWith(command, "SavePreset") || StartsWith(command, "DeletePreset") || StartsWith(command, "LoadPreset")) PublishPresets();
	}

	void StompboxService::PublishCatalog()
	{
		const std::string catalog = engine.DumpConfig();
		if (!StompboxControl::WriteText(&shared->catalogSeq, shared->catalog, StompboxControl::CATALOG_CHARS, catalog))
		{
			SetError("The pedal catalogue is too large for the Tone designer page.");
			Log("stompbox: catalogue of %zu bytes does not fit (%d)", catalog.size(), StompboxControl::CATALOG_CHARS);
		}
	}

	void StompboxService::PublishProgram()
	{
		const std::string program = engine.DumpProgram();
		if (!StompboxControl::WriteText(&shared->programSeq, shared->program, StompboxControl::PROGRAM_CHARS, program))
		{
			SetError("This tone is too large for the Tone designer page.");
			Log("stompbox: program of %zu bytes does not fit (%d)", program.size(), StompboxControl::PROGRAM_CHARS);
		}
	}

	void StompboxService::PublishPresets()
	{
		std::string presets = engine.CurrentPreset() + "\n";
		for (const std::string& name : engine.ListPresets()) presets += name + "\n";
		if (!StompboxControl::WriteText(&shared->presetSeq, shared->presets, StompboxControl::PRESET_CHARS, presets))
		{
			SetError("There are too many saved tones to list.");
			Log("stompbox: preset list of %zu bytes does not fit (%d)", presets.size(), StompboxControl::PRESET_CHARS);
		}
	}

	void StompboxService::SetState(StompboxControl::EngineState state)
	{
		InterlockedExchange(&shared->engineState, static_cast<LONG>(state));
	}

	void StompboxService::SetError(const std::string& error)
	{
		const size_t length = std::min(error.size(), static_cast<size_t>(StompboxControl::ERROR_CHARS - 1));
		std::memcpy(shared->error, error.data(), length);
		shared->error[length] = '\0';
	}

	void StompboxService::Stop()
	{
		if (shared)
		{
			InterlockedExchange(&shared->playing, 0);
			InterlockedExchange(&shared->hostPid, 0);
			SetState(StompboxControl::EngineState::Off);
			UnmapViewOfFile(shared);
		}
		shared = nullptr;
		if (mapping) CloseHandle(mapping);
		mapping = nullptr;
	}
}
