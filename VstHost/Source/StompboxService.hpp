#pragma once

// The host side of the Tone designer page (DLL/Shared/StompboxControl.hpp):
//   - loads the Stompbox engine when the host starts, so the pedal catalogue is ready before the page opens;
//   - runs the page's commands on it and republishes the program and the preset list;
//   - tells the host when the stream should go through the Tone designer (`wanted`);
//   - song tones: when a song is chosen, every saved tone assigned to one of its game tones is loaded into an engine
//     of its own and run on silence until it is ready (SceneSet), before the song plays. During the song the stream
//     follows the game's own tone key (the chart's tone changes, seeks, Riff Repeater, the 1-4 keys), processing only
//     the active tone. Assignments are kept per song in <data>\SongTones\<song key>.txt ("<tone key>\t<saved tone>").
// Main thread only (the host's key timer polls it; StartAudio calls PrepareStream with the stream stopped).

#include "SceneSet.hpp"
#include "SourceRouter.hpp"
#include "StompboxEngine.hpp"

#include "../../DLL/Shared/StompboxControl.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace VstHost
{
	class StompboxService
	{
	public:
		// `setPlays(true)` routes the stream through the Tone designer (Sink()), `false` back to the VST chain; it
		// returns whether the stream now plays as asked.
		StompboxService(StompboxEngine& engine, SourceRouter& router, std::function<bool(bool)> setPlays);
		StompboxService(const StompboxService&) = delete;
		StompboxService& operator=(const StompboxService&) = delete;
		~StompboxService();

		// Opens the shared block and loads the engine on `dataPath`. With `isDevelopmentRemote` Stompbox keeps its own
		// rig layout and starts its local server for StompboxRemote (HKCU\...\VstHost StompboxDev = 1); otherwise the
		// rig is the page's one chain (StompboxControl::CHAIN_NAME).
		void Start(const std::wstring& dataPath, bool isDevelopmentRemote);
		void Poll();
		// The stream is stopped and opens at `frames` per block: prepares the engine and the song's tones for it.
		void PrepareStream(int frames);
		// What the stream plays while the Tone designer is chosen: the song's tones once they are ready, else the board.
		AudioSink* Sink();
		// The last second's block timing at `frames` per block, for the page's load meter.
		void PublishLoad(const BlockTiming& timing, int frames);
		float TakeInputPeak();
		float TakeOutputPeak();
		void Stop();

	private:
		struct RetiredScenes
		{
			std::unique_ptr<SceneSet> scenes;
			uint64_t freeAfterBlock = 0;
		};

		bool IsWanted() const;
		void RunCommand(const std::string& command);
		void RunSongToneCommand(const std::string& command);
		void PollGame();
		void PollScenes();
		void ChangeSong(const std::string& songKey);
		void RebuildScenes();
		void FinishScenes();
		void ClearScenes();
		void ApplyTarget();
		void RestoreBypassedAmps();
		void Retire(std::unique_ptr<SceneSet> scenes);
		std::unique_ptr<StompboxEngine> CreateEngine(std::string& error);
		std::wstring SongTonesFile(const std::string& songKey) const;
		std::map<std::string, std::string> ReadAssignments(const std::string& song) const;
		void WriteAssignments(const std::string& song, const std::map<std::string, std::string>& tones) const;
		void PublishCatalog();
		void PublishProgram();
		void PublishPresets();
		void PublishSong();
		void SetState(StompboxControl::EngineState state);
		void SetSongState(StompboxControl::SongState state);
		void SetError(const std::string& error);

		StompboxEngine& engine;
		SourceRouter& router;
		std::function<bool(bool)> setPlays;
		HANDLE mapping = nullptr;
		StompboxControl::Shared* shared = nullptr;
		std::wstring dataPath;
		bool isPlaying = false;
		bool wasPresetLoading = false;
		LONG inputTrimCentidB = 0;
		bool isDevelopmentRemote = false;
		int streamFrames = 0;

		// Song tones.
		LONG gameSeen = 0;
		std::string songKey;
		std::string toneKey;
		std::map<std::string, std::string> assignments;     // the current song's: game tone key -> saved tone
		std::vector<std::unique_ptr<StompboxEngine>> preparing;
		std::vector<std::string> preparingTones;
		int preparedBlocks = 0;
		std::unique_ptr<SceneSet> scenes;
		std::vector<std::string> sceneTones;                // scene i + 1 plays sceneTones[i]
		std::vector<RetiredScenes> retired;
		int bypassedScene = -1;                             // a ringing-out scene whose amps are bypassed
		std::vector<std::string> bypassedAmps;
	};
}
