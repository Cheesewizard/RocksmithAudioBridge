#pragma once

// The host side of the Tone designer page (DLL/Shared/StompboxControl.hpp): loads the Stompbox engine when the host
// starts, so the pedal catalogue is ready before the page opens; runs the page's commands on it; republishes the
// program and the preset list; and tells the host when the stream should go through Stompbox (`wanted`).
// Main thread only (the host's key timer polls it).

#include "StompboxEngine.hpp"

#include "../../DLL/Shared/StompboxControl.hpp"

#include <functional>
#include <string>

namespace VstHost
{
	class StompboxService
	{
	public:
		// `setPlays(true)` routes the stream through the engine, `false` back to the VST chain; it returns whether the
		// stream now plays as asked.
		StompboxService(StompboxEngine& engine, std::function<bool(bool)> setPlays);
		StompboxService(const StompboxService&) = delete;
		StompboxService& operator=(const StompboxService&) = delete;
		~StompboxService();

		// Opens the shared block and loads the engine on `dataPath`. With `isDevelopmentRemote` Stompbox keeps its own
		// rig layout and starts its local server for StompboxRemote (HKCU\...\VstHost StompboxDev = 1); otherwise the
		// rig is the page's one chain (StompboxControl::CHAIN_NAME).
		void Start(const std::wstring& dataPath, bool isDevelopmentRemote);
		void Poll();
		void Stop();

	private:
		void RunCommand(const std::string& command);
		void PublishCatalog();
		void PublishProgram();
		void PublishPresets();
		void SetState(StompboxControl::EngineState state);
		void SetError(const std::string& error);

		StompboxEngine& engine;
		std::function<bool(bool)> setPlays;
		HANDLE mapping = nullptr;
		StompboxControl::Shared* shared = nullptr;
		bool isPlaying = false;
		bool wasPresetLoading = false;
		bool isDevelopmentRemote = false;
	};
}
