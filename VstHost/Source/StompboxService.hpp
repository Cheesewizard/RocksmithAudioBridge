#pragma once

// The host side of the Tone designer page (Mod/Shared/StompboxControl.hpp):
//   - loads the Stompbox engine when the host starts, so the pedal catalogue is ready before the page opens;
//   - runs the page's commands on it and republishes the program and the preset list;
//   - tells the host when the stream should go through the Tone designer (`wanted`);
//   - song tones: when a song is chosen, every saved tone assigned to one of its game tones is loaded into an engine
//     of its own and run on silence until it is ready (SceneSet), before the song plays. During the song the stream
//     follows the game's own tone key (the chart's tone changes, seeks, Riff Repeater, the 1-4 keys), processing only
//     the active tone. Assignments are kept per song in <data>\SongTones\<song key>.txt ("<tone key>\t<saved tone>");
//   - Rescan (the page's command): a fresh Stompbox processor lists NAM\, Cabinets\ and Presets\ again, takes the
//     board's tone, is run on silence until ready and then replaces the board's processor (StompboxEngine::Adopt);
//     the catalogue, presets and song tones are published again and <data>\Catalogue.txt (the DumpConfig text) is
//     written, as at start;
//   - undo: the page's board edits (SetChain on the Main chain, SetParam on its pedals, from the page or the pedal
//     window) are juce::UndoableActions in a juce::UndoManager, each holding the command and its inverse computed
//     from the program before it (the old chain, the old value, a removed pedal's settings); Undo and Redo are page
//     commands, the labels go to the page (undoLabel, redoLabel). LoadPreset and NewTone clear the history;
//   - safe failure: saved tones, captures and cabinets are tried before Stompbox loads them (StompboxChecks); a
//     refused one leaves the rig as it was and the page shows why (the shared block's error).
// Main thread only (the host's key timer polls it; StartAudio calls PrepareStream with the stream stopped).

#include "SceneSet.hpp"
#include "StompboxChecks.hpp"
#include "SourceRouter.hpp"
#include "StompboxEngine.hpp"
#include "Tone3000Engine.hpp"

#include "../../Mod/Shared/StompboxControl.hpp"

#include <juce_data_structures/juce_data_structures.h>

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

		// The host's pedal window (PedalEditor). The page asks for it with EditPedal "<pedal id>" (an empty id closes
		// it): this is called with the id. Main thread.
		std::function<void(const std::string& pedalId)> onEditPedal;
		// The pedal window's edits: a Stompbox command run as if the page had sent it (checked, run, the program
		// published again so the page shows it). False, and nothing run, while a Rescan holds commands or the engine
		// is not loaded; the caller keeps it for later.
		bool RunLocalCommand(const std::string& command);
		// The last catalogue (DumpConfig) and program (DumpProgram) published, and counters that move when they change.
		const std::string& Catalog() const { return publishedCatalog; }
		const std::string& Program() const { return publishedProgram; }
		uint64_t CatalogVersion() const { return catalogVersion; }
		uint64_t ProgramVersion() const { return programVersion; }
		const std::wstring& DataPath() const { return dataPath; }   // NAM\, Cabinets\, Presets\, StarterTones\

	private:
		// Rescan, over several main-thread ticks while the page's commands wait.
		enum class RescanStep { None, LoadingPreset, Warming, Switching };

		struct RetiredScenes
		{
			std::unique_ptr<SceneSet> scenes;
			uint64_t freeAfterBlock = 0;
		};

		bool IsWanted() const;
		bool LoadEngine();
		void PollEngine();
		void RunCommand(const std::string& command);
		void StartRescan();
		void PollRescan();
		void RestoreBoard();
		void FinishRescan();
		void AbortRescan(const std::string& error);
		void ReportFaults();
		void WriteCatalogueFile(const std::string& catalog) const;
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
		void ApplyToTone3000(const std::string& program);
		void PublishPresets();
		void PublishSong();
		void PublishAutoTrim();
		void SetState(StompboxControl::EngineState state);
		void SetSongState(StompboxControl::SongState state);
		void SetError(const std::string& error);
		// Undo (see the top). RecordBoardEdit runs a board edit as an undoable step; false when it failed.
		bool IsBoardEdit(const std::vector<std::string>& words) const;
		bool RecordBoardEdit(const std::string& command, const std::vector<std::string>& words);
		bool ApplyBoardLines(const std::vector<std::string>& lines);
		std::string DefaultValue(const std::string& pedalId, const std::string& parameter);
		void UndoOrRedo(bool isUndo);
		void ClearUndo(const char* why);
		void PublishUndo();

		StompboxEngine& engine;
		// Engine swap phase 1: the board plays through the TONE3000 engine (Tone3000Engine.hpp); Stompbox still
		// holds the tone the page edits and is run on silence so its changes apply.
		std::unique_ptr<Tone3000Engine> tone3000;
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
		StompboxChecks checks;
		std::string publishedProgram;                       // the board's last DumpProgram, kept for a Rescan
		std::string publishedPreset;
		std::string publishedCatalog;                       // for the pedal window
		uint64_t catalogVersion = 0;
		uint64_t programVersion = 0;
		bool isFaultReported = false;
		ULONGLONG autoTrimCheckedAt = 0;
		LONG publishedAutoTrim = 0;
		bool hasPublishedAutoTrim = false;

		// Undo. Made in Start (JUCE runs by then), released in Stop.
		std::unique_ptr<juce::UndoManager> undoManager;
		std::string undoKey;                                // the last recorded edit, for joining ("param:Delay:Mix")
		ULONGLONG undoAt = 0;
		std::vector<std::string> freshPedals;               // added by the last step: their settings join it
		std::map<std::string, std::map<std::string, std::string>> catalogDefaults;   // kind -> parameter -> default
		uint64_t catalogDefaultsVersion = 0;

		// Rescan.
		RescanStep rescanStep = RescanStep::None;
		std::unique_ptr<StompboxEngine> rescanEngine;
		std::string rescanProgram;
		std::vector<std::string> rescanProblems;
		int rescanBlocks = 0;
		int rescanPolls = 0;

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
