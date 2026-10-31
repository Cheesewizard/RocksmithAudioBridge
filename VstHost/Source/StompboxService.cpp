#include "StompboxService.hpp"

#include "AudioFormat.hpp"
#include "HostLog.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace VstHost
{
	namespace
	{
		constexpr int PUMP_BLOCKS_PER_POLL = 16;            // a tone being prepared, per 15 ms main-thread tick
		constexpr int WARM_FRAMES = 48000 / 2;              // run on silence this long after loading: NAM settles
		constexpr int UNPREPARED_FRAMES = 480;              // a Rescan before the stream has ever run the engine
		constexpr int SWITCH_POLLS = 3;                     // ticks to wait for the stream to take an adopted engine
		constexpr char RESCAN_COMMAND[] = "Rescan";         // the page's own command (CONTRACTS section 3)

		std::string GlobalChainCommand()
		{
			return std::string("SetGlobalChain MasterChain MasterIn Chain ") + StompboxControl::CHAIN_NAME + " MasterChain MasterOut";
		}

		bool StartsWith(const std::string& text, const char* prefix)
		{
			const size_t length = std::strlen(prefix);
			return text.size() >= length && text.compare(0, length, prefix) == 0;
		}

		std::vector<std::string> Words(const std::string& line)
		{
			std::vector<std::string> words;
			std::istringstream stream(line);
			std::string word;
			while (stream >> std::quoted(word)) words.push_back(word);
			return words;
		}

		std::wstring Wide(const std::string& text)
		{
			if (text.empty()) return {};
			const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
			std::wstring wide(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
			return wide;
		}

		bool IsAmpPedal(const std::string& id)
		{
			const std::string kind = id.substr(0, id.find('_'));
			return kind == "NAM" || kind == "NAMMulti";
		}

		// The NAM pedals of a program (Stompbox's DumpProgram) that are switched on.
		std::vector<std::string> EnabledAmps(const std::string& program)
		{
			std::vector<std::string> amps;
			std::set<std::string> disabled;
			std::istringstream lines(program);
			for (std::string line; std::getline(lines, line);)
			{
				const std::vector<std::string> words = Words(line);
				if (words.size() > 2 && words[0] == "SetChain" && words[1] == StompboxControl::CHAIN_NAME)
				{
					for (size_t i = 2; i < words.size(); ++i)
						if (IsAmpPedal(words[i])) amps.push_back(words[i]);
				}
				else if (words.size() > 3 && words[0] == "SetParam" && words[2] == "Enabled" && words[3] == "0") disabled.insert(words[1]);
			}
			amps.erase(std::remove_if(amps.begin(), amps.end(), [&](const std::string& id) { return disabled.count(id) != 0; }), amps.end());
			return amps;
		}

		constexpr int UNDO_UNITS_KEPT = 4096;          // kilobytes of command lines: far more steps than anyone takes back
		constexpr int UNDO_STEPS_KEPT = 100;
		constexpr ULONGLONG PARAM_JOIN_MS = 1000;       // a knob turned again within this is the same step
		constexpr ULONGLONG FRESH_JOIN_MS = 2000;       // a new pedal's starting settings follow its SetChain at once

		std::string KindOf(const std::string& id) { return id.substr(0, id.find('_')); }

		// The board in a program (DumpProgram): the Main chain's pedals in order, and each pedal's values.
		struct Board
		{
			std::vector<std::string> chain;
			std::map<std::string, std::map<std::string, std::string>> values;   // pedal id -> parameter -> value
		};

		Board ParseBoard(const std::string& program)
		{
			Board board;
			std::istringstream lines(program);
			for (std::string line; std::getline(lines, line);)
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				const std::vector<std::string> words = Words(line);
				if (words.size() >= 2 && words[0] == "SetChain" && words[1] == StompboxControl::CHAIN_NAME)
					board.chain.assign(words.begin() + 2, words.end());
				else if (words.size() >= 4 && words[0] == "SetParam")
				{
					std::string value = words[3];
					for (size_t i = 4; i < words.size(); ++i) value += " " + words[i];
					board.values[words[1]][words[2]] = value;
				}
			}
			return board;
		}

		bool Contains(const std::vector<std::string>& list, const std::string& item) { return std::find(list.begin(), list.end(), item) != list.end(); }

		std::string SetParamLine(const std::string& id, const std::string& parameter, const std::string& value)
		{
			return "SetParam " + id + " " + parameter + " " + StompboxControl::Quote(value);
		}

		std::string ChainLine(const std::vector<std::string>& ids)
		{
			std::string line = std::string("SetChain ") + StompboxControl::CHAIN_NAME;
			for (const std::string& id : ids) line += " " + id;
			return line;
		}

		// The Edit menu's name for a chain change: what was added, removed, swapped or moved.
		std::string ChainChangeName(const std::vector<std::string>& before, const std::vector<std::string>& after)
		{
			std::vector<std::string> added, removed;
			for (const std::string& id : after) if (!Contains(before, id)) added.push_back(id);
			for (const std::string& id : before) if (!Contains(after, id)) removed.push_back(id);
			if (after.empty() && !before.empty()) return "clear board";
			if (!added.empty() && removed.empty()) return "add " + KindOf(added.front());
			if (added.empty() && removed.size() == 1) return "remove " + KindOf(removed.front());
			if (added.empty() && !removed.empty()) return "remove pedals";
			if (added.size() == 1 && removed.size() == 1) return "replace " + KindOf(removed.front());
			if (added.empty() && removed.empty())
			{
				// The same pedals in a new order: the one that moved furthest is the one the player dragged.
				std::string moved;
				ptrdiff_t furthest = 0;
				for (size_t i = 0; i < after.size(); ++i)
				{
					const ptrdiff_t was = std::find(before.begin(), before.end(), after[i]) - before.begin();
					const ptrdiff_t distance = std::abs(was - static_cast<ptrdiff_t>(i));
					if (distance > furthest) { furthest = distance; moved = after[i]; }
				}
				return moved.empty() ? "change board" : "move " + KindOf(moved);
			}
			return "change board";
		}

		// One board edit as an undo step: the command lines that do it and the ones that take it back, both run
		// through the service (checked, then sent to the board's engine). The first perform is the page's command
		// itself. `key` lets consecutive turns of one knob coalesce into one action (the first one's undo, the last
		// one's do).
		class BoardAction final : public juce::UndoableAction
		{
		public:
			using Apply = std::function<bool(const std::vector<std::string>&)>;

			BoardAction(Apply applyLines, std::vector<std::string> doLines, std::vector<std::string> undoLines, std::string key)
				: apply(std::move(applyLines)), forward(std::move(doLines)), backward(std::move(undoLines)), coalesceKey(std::move(key)) {}

			bool perform() override { return apply(forward); }
			bool undo() override { return apply(backward); }

			// Redo of an added pedal also sets every value it had right after the original edit: Stompbox may keep a
			// removed pedal's instance, so a pedal coming back must not keep values from an earlier life.
			void AddRedoLines(const std::vector<std::string>& lines) { forward.insert(forward.end(), lines.begin(), lines.end()); }

			int getSizeInUnits() override
			{
				size_t bytes = 0;
				for (const std::string& line : forward) bytes += line.size();
				for (const std::string& line : backward) bytes += line.size();
				return 1 + static_cast<int>(bytes / 1024);
			}

			juce::UndoableAction* createCoalescedAction(juce::UndoableAction* next) override
			{
				const auto* later = dynamic_cast<BoardAction*>(next);
				if (!later || coalesceKey.empty() || later->coalesceKey != coalesceKey) return nullptr;
				return new BoardAction(apply, later->forward, backward, coalesceKey);
			}

		private:
			Apply apply;
			std::vector<std::string> forward, backward;
			std::string coalesceKey;
		};
	}

	StompboxService::StompboxService(StompboxEngine& engineToUse, SourceRouter& routerToUse, std::function<bool(bool)> setPlaysCallback)
		: engine(engineToUse), router(routerToUse), setPlays(std::move(setPlaysCallback))
	{
		if (!setPlays) throw std::invalid_argument("StompboxService needs a setPlays callback.");
	}

	StompboxService::~StompboxService() { Stop(); }

	void StompboxService::Start(const std::wstring& path, bool developmentRemote)
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
		dataPath = path;
		isDevelopmentRemote = developmentRemote;
		undoManager = std::make_unique<juce::UndoManager>(UNDO_UNITS_KEPT, UNDO_STEPS_KEPT);
		PublishUndo();
		InterlockedExchange(&shared->hostPid, static_cast<LONG>(GetCurrentProcessId()));
		InterlockedExchange(&shared->playing, 0);
		SetSongState(StompboxControl::SongState::None);
		shared->playingTone[0] = '\0';
		try
		{
			LoadEngine();
		}
		catch (const std::exception& exception)
		{
			SetError("The Tone designer engine did not start. The VST rig still works.");
			SetState(StompboxControl::EngineState::Failed);
			Log("stompbox: start failed: %s", exception.what());
		}
	}

	// At start, and on Rescan after a failed start (the player may have put the DLL back). A failure leaves the
	// engine unloaded: IsWanted stays false, so the stream keeps the VST rig.
	bool StompboxService::LoadEngine()
	{
		SetState(StompboxControl::EngineState::Loading);
		std::string error;
		if (!engine.Load(dataPath, error))
		{
			SetError(error);
			SetState(StompboxControl::EngineState::Failed);
			return false;
		}
		if (isDevelopmentRemote) engine.StartRemoteServer();
		else engine.SendCommand(GlobalChainCommand());
		DWORD useTone3000 = 1, size = sizeof(useTone3000);
		RegGetValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\VstHost", L"Tone3000Engine", RRF_RT_REG_DWORD, nullptr, &useTone3000, &size);
		if (useTone3000 != 0 && !isDevelopmentRemote && !tone3000)
		{
			try
			{
				tone3000 = std::make_unique<Tone3000Engine>();
				if (streamFrames > 0) tone3000->Prepare(streamFrames);
			}
			catch (const std::exception& exception)
			{
				Log("tone3000: engine not started (%s); Stompbox plays", exception.what());
				tone3000.reset();
			}
		}
		PublishCatalog();
		PublishPresets();
		PublishProgram();
		SetError({});
		SetState(StompboxControl::EngineState::Ready);
		Log("stompbox: ready for the Tone designer page%s", isDevelopmentRemote ? " (StompboxDev: Stompbox's own layout and remote server)" : "");
		return true;
	}

	bool StompboxService::IsWanted() const
	{
		return shared && StompboxControl::Load(&shared->wanted) != 0 && engine.IsLoaded();
	}

	// Nothing may leave the host's key timer: a failure here is reported on the page and logged.
	void StompboxService::Poll()
	{
		if (!shared) return;
		try
		{
			PollEngine();
		}
		catch (const std::exception& exception)
		{
			SetError("The Tone designer hit a problem; see the host's log.");
			Log("stompbox: poll failed: %s", exception.what());
		}
	}

	void StompboxService::PollEngine()
	{
		const bool wanted = IsWanted();
		if (wanted != isPlaying)
		{
			const bool plays = setPlays(wanted);
			isPlaying = wanted && plays;
			InterlockedExchange(&shared->playing, isPlaying ? 1 : 0);
			if (wanted && !plays) SetError("The Tone designer could not switch the stream to its engine.");
			// Song tones are prepared only while the Tone designer plays: the VST rig never pays for them.
			if (isPlaying) RebuildScenes();
			else ClearScenes();
		}

		PublishAutoTrim();

		const LONG trim = StompboxControl::Load(&shared->inputTrimCentidB);
		if (trim != inputTrimCentidB)
		{
			inputTrimCentidB = trim;
			StompboxEngine::SetInputGain(std::pow(10.0f, static_cast<float>(trim) / 2000.0f));
			Log("stompbox: input trim %+.1f dB", trim / 100.0);
		}

		// One command at a time: the page writes the next only after commandDone has caught up. A Rescan holds the
		// next command until the fresh engine has taken over, so no edit lands on the processor being replaced.
		const LONG sequence = StompboxControl::Load(&shared->commandSeq);
		if (sequence != StompboxControl::Load(&shared->commandDone) && rescanStep == RescanStep::None)
		{
			const std::string command(shared->command, strnlen(shared->command, StompboxControl::COMMAND_CHARS));
			try
			{
				RunCommand(command);
			}
			catch (const std::exception& exception)
			{
				SetError("That change could not be made; see the host's log.");
				Log("stompbox: command failed (%s): %s", exception.what(), command.c_str());
			}
			PublishUndo();
			InterlockedExchange(&shared->commandDone, sequence);   // always: the page never waits on a failed one
		}

		if (!engine.IsLoaded()) return;
		if (rescanStep != RescanStep::None) PollRescan();
		ReportFaults();
		// The stream runs the TONE3000 engine, not Stompbox, and Stompbox applies chain changes and loads presets only
		// while it processes: run it on silence so the page's edits still land (and reach ApplyToTone3000).
		// Never while song tones play: SceneSet hands the board's engine to the stream then.
		if (tone3000 && !scenes && preparing.empty() && engine.PreparedFrames() > 0) engine.Pump(2);
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

		if (isDevelopmentRemote) return;   // StompboxRemote drives the rig: no song tones
		PollGame();
		PollScenes();
	}

	void StompboxService::PrepareStream(int frames)
	{
		if (!engine.IsLoaded()) return;
		const bool changed = frames != streamFrames;
		streamFrames = frames;
		engine.Prepare(frames);   // completes a Rescan's switch if one waits (the stream is stopped)
		if (tone3000) tone3000->Prepare(frames);
		if (rescanEngine) rescanEngine->Prepare(frames);
		retired.clear();   // the stream is stopped: nothing runs them
		if (changed && (scenes || !preparing.empty()))
		{
			// Prepared at the old block size: load them again at the new one.
			RestoreBypassedAmps();
			scenes.reset();
			RebuildScenes();
		}
	}

	AudioSink* StompboxService::Sink()
	{
		if (scenes) return scenes.get();
		return tone3000 ? static_cast<AudioSink*>(tone3000.get()) : &engine;
	}

	float StompboxService::TakeInputPeak() { return scenes ? scenes->TakeInputPeak() : tone3000 ? tone3000->TakeInputPeak() : engine.TakeInputPeak(); }
	float StompboxService::TakeOutputPeak() { return scenes ? scenes->TakeOutputPeak() : tone3000 ? tone3000->TakeOutputPeak() : engine.TakeOutputPeak(); }

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
		const std::vector<std::string> words = Words(command);
		if (!words.empty() && words[0] == RESCAN_COMMAND)
		{
			StartRescan();
			return;
		}
		if (!words.empty() && words[0] == StompboxControl::EDIT_PEDAL_COMMAND)
		{
			if (onEditPedal) onEditPedal(words.size() > 1 ? words[1] : std::string());
			return;
		}
		if (!engine.IsLoaded())
		{
			SetError("The Stompbox engine is not loaded.");
			return;
		}
		SetError({});
		if (StartsWith(command, StompboxControl::SONG_TONE_COMMAND))
		{
			RunSongToneCommand(command);
			return;
		}
		if (!words.empty() && (words[0] == StompboxControl::UNDO_COMMAND || words[0] == StompboxControl::REDO_COMMAND))
		{
			UndoOrRedo(words[0] == StompboxControl::UNDO_COMMAND);
			return;
		}
		if (!words.empty() && words[0] == StompboxControl::NEW_TONE_COMMAND)
		{
			engine.SendCommand(ChainLine({}));
			ClearUndo("a new tone");
			PublishProgram();
			return;
		}
		// A capture, cabinet or saved tone that would fail inside Stompbox is refused here: the rig keeps what it
		// had, and the program is published again so the page shows that.
		const std::string refused = checks.CheckCommand(command);
		if (!refused.empty())
		{
			SetError(refused);
			Log("stompbox: refused (%s): %s", refused.c_str(), command.c_str());
			PublishProgram();
			return;
		}
		const bool done = IsBoardEdit(words) ? RecordBoardEdit(command, words) : engine.SendCommand(command);
		if (!done)
		{
			SetError(engine.LastError().empty() ? std::string("That change could not be made.") : engine.LastError());
			Log("stompbox: command failed (%s): %s", engine.LastError().c_str(), command.c_str());
			PublishProgram();
			return;
		}
		PublishProgram();
		if (StartsWith(command, "LoadPreset")) ClearUndo("a tone was loaded");
		if (StartsWith(command, "SavePreset") || StartsWith(command, "DeletePreset") || StartsWith(command, "LoadPreset")) PublishPresets();
		// A saved tone the song uses changed on disk: load the song's tones again.
		if (StartsWith(command, "SavePreset") || StartsWith(command, "DeletePreset")) RebuildScenes();
	}

	// The board's edits that are undoable: the Main chain's order and its pedals' settings. Not presets, song tones
	// or a Rescan; nothing while StompboxDev's remote drives the rig (its layout is not the board).
	bool StompboxService::IsBoardEdit(const std::vector<std::string>& words) const
	{
		if (!undoManager || isDevelopmentRemote || words.size() < 2) return false;
		if (words[0] == "SetChain") return words[1] == StompboxControl::CHAIN_NAME;
		return words[0] == "SetParam" && words.size() >= 4;
	}

	// Runs the edit as a step whose inverse comes from the program before it (publishedProgram: it is republished
	// after every command and once a preset has loaded). A step is joined by what belongs to it: a new pedal's
	// starting settings and file (the page and the pedal window send them right after the SetChain), and further
	// turns of the same knob within PARAM_JOIN_MS, which coalesce into one action.
	bool StompboxService::RecordBoardEdit(const std::string& command, const std::vector<std::string>& words)
	{
		const Board before = ParseBoard(publishedProgram);
		const BoardAction::Apply apply = [this](const std::vector<std::string>& lines) { return ApplyBoardLines(lines); };
		const ULONGLONG now = GetTickCount64();
		std::vector<std::string> undoLines;
		std::string name, key;
		bool joins = false;
		std::vector<std::string> added;
		if (words[0] == "SetChain")
		{
			const std::vector<std::string> after(words.begin() + 2, words.end());
			if (after == before.chain) return engine.SendCommand(command);   // nothing changes: not a step
			undoLines.push_back(ChainLine(before.chain));
			// A pedal that leaves comes back with every value it had.
			for (const std::string& id : before.chain)
			{
				if (Contains(after, id)) continue;
				const auto values = before.values.find(id);
				if (values == before.values.end()) continue;
				for (const auto& [parameter, value] : values->second) undoLines.push_back(SetParamLine(id, parameter, value));
			}
			for (const std::string& id : after) if (!Contains(before.chain, id)) added.push_back(id);
			name = ChainChangeName(before.chain, after);
		}
		else
		{
			const std::string& id = words[1];
			const std::string& parameter = words[2];
			std::string value = words[3];
			for (size_t i = 4; i < words.size(); ++i) value += " " + words[i];
			const bool isFresh = Contains(freshPedals, id) && undoKey == "fresh" && now - undoAt < FRESH_JOIN_MS;
			if (!Contains(before.chain, id) && !isFresh) return engine.SendCommand(command);   // not on the board
			std::string old;
			const auto values = before.values.find(id);
			if (values != before.values.end() && values->second.count(parameter)) old = values->second.at(parameter);
			else old = DefaultValue(id, parameter);
			if (old.empty())
			{
				// Neither the program nor the catalogue says what it was (a file never chosen). On a new pedal the
				// step's SetChain takes it out again anyway; otherwise it is done but not recorded.
				if (isFresh) old = value;
				else
				{
					Log("stompbox: undo: no earlier value for %s %s; not recorded", id.c_str(), parameter.c_str());
					return engine.SendCommand(command);
				}
			}
			if (old == value && !isFresh) return engine.SendCommand(command);   // nothing changes: not a step
			undoLines.push_back(SetParamLine(id, parameter, old));
			if (parameter == "Enabled") name = (value == "0" ? "bypass " : "turn on ") + KindOf(id);
			else name = "change " + KindOf(id) + " " + parameter;
			if (isFresh) joins = true;
			else
			{
				key = "param:" + id + ":" + parameter;
				joins = key == undoKey && now - undoAt < PARAM_JOIN_MS;
			}
		}

		if (!joins) undoManager->beginNewTransaction(juce::String::fromUTF8(name.c_str()));
		auto* action = new BoardAction(apply, { command }, std::move(undoLines), key);
		if (!undoManager->perform(action)) { undoKey.clear(); return false; }   // the manager deleted it
		undoAt = now;
		if (words[0] == "SetChain")
		{
			freshPedals = added;
			undoKey = added.empty() ? std::string() : "fresh";
			// A pedal added comes back on redo with the values it has now (its starting point in Stompbox). The
			// action is the transaction's newest: a SetChain never coalesces, so the manager has not replaced it.
			if (!added.empty())
			{
				const Board after = ParseBoard(engine.DumpProgram());
				std::vector<std::string> lines;
				for (const std::string& id : added)
					if (const auto values = after.values.find(id); values != after.values.end())
						for (const auto& [parameter, value] : values->second) lines.push_back(SetParamLine(id, parameter, value));
				action->AddRedoLines(lines);
			}
		}
		else if (!joins || undoKey != "fresh")
		{
			freshPedals.clear();
			undoKey = key;
		}
		return true;
	}

	// Runs an undo step's lines on the board. A line refused by the checks (a capture or cabinet deleted since) is
	// left out and said; the rest still runs, so the board comes back as near as it can. False only when Stompbox
	// itself failed, which drops the history (JUCE does when a step cannot be done).
	bool StompboxService::ApplyBoardLines(const std::vector<std::string>& lines)
	{
		bool ok = true;
		for (const std::string& line : lines)
		{
			const std::string refused = checks.CheckCommand(line);
			if (!refused.empty())
			{
				SetError(refused);
				Log("stompbox: undo: refused (%s): %s", refused.c_str(), line.c_str());
				continue;
			}
			if (!engine.SendCommand(line))
			{
				Log("stompbox: undo: failed (%s): %s", engine.LastError().c_str(), line.c_str());
				ok = false;
			}
		}
		return ok;
	}

	// A parameter's default from the catalogue (DumpConfig), for an edit whose old value the program does not list.
	// An Enum's default is an index into its choices; Stompbox's SetParam takes the choice. Empty when unknown (a
	// File parameter). A pedal is switched on unless the program says otherwise.
	std::string StompboxService::DefaultValue(const std::string& pedalId, const std::string& parameter)
	{
		if (parameter == "Enabled") return "1";
		if (catalogDefaultsVersion != catalogVersion)
		{
			catalogDefaultsVersion = catalogVersion;
			catalogDefaults.clear();
			std::map<std::string, std::map<std::string, std::string>> enumDefaults;   // kind -> parameter -> index
			std::istringstream lines(publishedCatalog);
			for (std::string line; std::getline(lines, line);)
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				// ValueFormat's value may be empty or hold spaces: cut it out before splitting the rest (as the
				// page's StompboxLink does).
				const size_t formatAt = line.find(" ValueFormat ");
				const size_t syncAt = formatAt == std::string::npos ? std::string::npos : line.find(" CanSyncToHostBPM ", formatAt);
				if (formatAt != std::string::npos && syncAt != std::string::npos) line.erase(formatAt, syncAt - formatAt);
				const std::vector<std::string> words = Words(line);
				if (words.size() >= 3 && words[0] == "ParameterConfig")
				{
					std::string type, value;
					for (size_t i = 3; i + 1 < words.size(); i += 2)
					{
						if (words[i] == "Type") type = words[i + 1];
						else if (words[i] == "DefaultValue") value = words[i + 1];
					}
					if (type == "File" || value.empty()) continue;
					if (type == "Enum") enumDefaults[words[1]][words[2]] = value;
					else catalogDefaults[words[1]][words[2]] = value;
				}
				else if (words.size() >= 3 && words[0] == "ParameterEnumValues")
				{
					const auto kind = enumDefaults.find(words[1]);
					if (kind == enumDefaults.end()) continue;
					const auto index = kind->second.find(words[2]);
					if (index == kind->second.end()) continue;
					const size_t choice = static_cast<size_t>(std::max(0, std::atoi(index->second.c_str()))) + 3;
					if (choice < words.size()) catalogDefaults[words[1]][words[2]] = words[choice];
				}
			}
		}
		const auto kind = catalogDefaults.find(KindOf(pedalId));
		if (kind == catalogDefaults.end()) return {};
		const auto value = kind->second.find(parameter);
		return value == kind->second.end() ? std::string() : value->second;
	}

	void StompboxService::UndoOrRedo(bool isUndo)
	{
		if (!undoManager) return;
		if (isUndo ? !undoManager->canUndo() : !undoManager->canRedo()) return;
		const juce::String name = isUndo ? undoManager->getUndoDescription() : undoManager->getRedoDescription();
		if (isUndo) undoManager->undo();
		else undoManager->redo();
		undoKey.clear();
		freshPedals.clear();
		Log("stompbox: %s %s", isUndo ? "undo" : "redo", name.toRawUTF8());
		PublishProgram();
	}

	void StompboxService::ClearUndo(const char* why)
	{
		undoKey.clear();
		freshPedals.clear();
		if (!undoManager || (!undoManager->canUndo() && !undoManager->canRedo())) return;
		undoManager->clearUndoHistory();
		Log("stompbox: undo history cleared (%s)", why);
	}

	void StompboxService::PublishUndo()
	{
		if (!shared) return;
		const std::string undo = undoManager ? undoManager->getUndoDescription().toStdString() : std::string();
		const std::string redo = undoManager ? undoManager->getRedoDescription().toStdString() : std::string();
		if (undo == shared->undoLabel && redo == shared->redoLabel) return;
		StompboxControl::WriteUndo(*shared, undo, redo);
	}

	bool StompboxService::RunLocalCommand(const std::string& command)
	{
		if (!shared || rescanStep != RescanStep::None || !engine.IsLoaded()) return false;
		RunCommand(command);
		PublishUndo();
		return true;
	}

	// SongTone "<song key>" "<tone key>" "<saved tone>": an empty saved tone clears the assignment. Any song: the page
	// lists every tone of the song from its manifest, so nothing has to be reached in the game first.
	void StompboxService::RunSongToneCommand(const std::string& command)
	{
		const std::vector<std::string> words = Words(command);
		if (words.size() != 4 || words[1].empty() || words[2].empty())
		{
			SetError("The song tone assignment was malformed.");
			Log("stompbox: malformed song tone command: %s", command.c_str());
			return;
		}
		const std::string& song = words[1];
		const std::string& tone = words[2];
		const std::string& saved = words[3];
		std::map<std::string, std::string> tones = song == songKey ? assignments : ReadAssignments(song);
		if (saved.empty()) tones.erase(tone);
		else tones[tone] = saved;
		WriteAssignments(song, tones);
		Log("stompbox: song %s: game tone '%s' -> %s", song.c_str(), tone.c_str(), saved.empty() ? "(cleared)" : saved.c_str());
		if (song != songKey) return;
		assignments = std::move(tones);
		PublishSong();
		RebuildScenes();
	}

	void StompboxService::PollGame()
	{
		std::string song, tone;
		if (!StompboxControl::ReadGame(*shared, gameSeen, song, tone)) return;
		if (song != songKey) ChangeSong(song);
		if (tone != toneKey)
		{
			toneKey = tone;
			ApplyTarget();
		}
	}

	void StompboxService::ChangeSong(const std::string& key)
	{
		songKey = key;
		toneKey.clear();
		assignments = ReadAssignments(songKey);
		PublishSong();
		RebuildScenes();
	}

	// The song's tones, each in an engine of its own, loaded at the stream's block size. They are run on silence in
	// PollScenes until Stompbox has loaded them, then become the stream's sink (FinishScenes).
	void StompboxService::RebuildScenes()
	{
		preparing.clear();
		preparingTones.clear();
		preparedBlocks = 0;
		std::set<std::string> tones;
		for (const auto& assignment : assignments) tones.insert(assignment.second);
		if (!isPlaying && !IsWanted()) tones.clear();
		if (songKey.empty() || tones.empty() || streamFrames <= 0)
		{
			ClearScenes();
			return;
		}
		const std::vector<std::string> saved = engine.ListPresets();
		for (const std::string& tone : tones)
		{
			if (std::find(saved.begin(), saved.end(), tone) == saved.end())
			{
				SetError("The song's tone '" + tone + "' is not saved any more.");
				Log("stompbox: song %s: saved tone %s is missing; that game tone keeps the previous tone", songKey.c_str(), tone.c_str());
				continue;
			}
			// Stompbox loads it on a thread of its own, where a damaged file would end the host: try it first.
			const std::string refused = checks.CheckPreset(tone, "The song's tone");
			if (!refused.empty())
			{
				SetError(refused);
				Log("stompbox: song %s: %s; that game tone keeps the previous tone", songKey.c_str(), refused.c_str());
				continue;
			}
			std::string error;
			std::unique_ptr<StompboxEngine> scene = CreateEngine(error);
			if (!scene)
			{
				SetError(error);
				SetSongState(StompboxControl::SongState::Failed);
				preparing.clear();
				preparingTones.clear();
				return;
			}
			scene->SendCommand("LoadPreset " + StompboxControl::Quote(tone));
			preparing.push_back(std::move(scene));
			preparingTones.push_back(tone);
		}
		if (preparing.empty())
		{
			ClearScenes();
			return;
		}
		SetSongState(StompboxControl::SongState::Preparing);
		Log("stompbox: song %s: preparing %zu tone(s)", songKey.c_str(), preparing.size());
	}

	std::unique_ptr<StompboxEngine> StompboxService::CreateEngine(std::string& error)
	{
		auto scene = std::make_unique<StompboxEngine>();
		if (!scene->Load(dataPath, error)) return nullptr;
		scene->SendCommand(GlobalChainCommand());
		scene->Prepare(streamFrames);
		return scene;
	}

	void StompboxService::PollScenes()
	{
		// Tones being prepared are not in the stream yet: run them here until they have loaded and settled.
		if (!preparing.empty())
		{
			bool loading = false;
			for (size_t i = 0; i < preparing.size();)
			{
				preparing[i]->Pump(PUMP_BLOCKS_PER_POLL);
				if (preparing[i]->HasFaulted())
				{
					// Stompbox crashed on it (StompboxEngine keeps the processor): leave that tone out.
					SetError("The song's tone '" + preparingTones[i] + "' stopped the engine while loading, so it is left out.");
					Log("stompbox: song %s: tone %s crashed while loading; left out", songKey.c_str(), preparingTones[i].c_str());
					preparing.erase(preparing.begin() + static_cast<std::ptrdiff_t>(i));
					preparingTones.erase(preparingTones.begin() + static_cast<std::ptrdiff_t>(i));
					continue;
				}
				loading = loading || preparing[i]->IsPresetLoading();
				++i;
			}
			if (preparing.empty()) ClearScenes();
			else
			{
				if (!loading) preparedBlocks += PUMP_BLOCKS_PER_POLL;
				if (!loading && preparedBlocks * streamFrames >= WARM_FRAMES) FinishScenes();
			}
		}

		if (scenes)
		{
			// A scene ringing out plays silence through its amps: bypass them for the tail, as a NAM costs the same on
			// silence. Only the song's own engines: the board's engine is the player's to edit.
			const int ringing = scenes->Tail();
			if (ringing != bypassedScene)
			{
				RestoreBypassedAmps();
				if (ringing > 0)
				{
					bypassedScene = ringing;
					bypassedAmps = EnabledAmps(scenes->Engine(ringing).DumpProgram());
					for (const std::string& id : bypassedAmps) scenes->Engine(ringing).SendCommand("SetParam " + id + " Enabled 0");
				}
			}
		}

		const uint64_t played = router.BlockCounter();
		retired.erase(std::remove_if(retired.begin(), retired.end(), [&](const RetiredScenes& old) { return played >= old.freeAfterBlock; }), retired.end());
	}

	void StompboxService::FinishScenes()
	{
		RestoreBypassedAmps();
		auto ready = std::make_unique<SceneSet>(engine, std::move(preparing), streamFrames);
		preparing.clear();
		sceneTones = std::move(preparingTones);
		preparingTones.clear();
		std::unique_ptr<SceneSet> old = std::move(scenes);
		scenes = std::move(ready);
		if (isPlaying) router.SetSource(scenes.get(), true);
		Retire(std::move(old));
		SetSongState(StompboxControl::SongState::Ready);
		Log("stompbox: song %s: %zu tone(s) ready", songKey.c_str(), sceneTones.size());
		ApplyTarget();
	}

	void StompboxService::ClearScenes()
	{
		RestoreBypassedAmps();
		preparing.clear();
		preparingTones.clear();
		if (scenes)
		{
			if (isPlaying) router.SetSource(&engine, true);
			Retire(std::move(scenes));
		}
		sceneTones.clear();
		SetSongState(StompboxControl::SongState::None);
		shared->playingTone[0] = '\0';
	}

	// The game's current tone picks the scene. A game tone without an assignment keeps the tone playing, as the game
	// keeps its own amp when a tone has nothing new.
	void StompboxService::ApplyTarget()
	{
		if (!scenes) return;
		const auto assignment = assignments.find(toneKey);
		if (assignment == assignments.end()) return;
		const auto tone = std::find(sceneTones.begin(), sceneTones.end(), assignment->second);
		if (tone == sceneTones.end()) return;
		const int index = static_cast<int>(tone - sceneTones.begin()) + 1;
		if (index == bypassedScene) RestoreBypassedAmps();   // switching back into its tail: amps on before it fades in
		scenes->SetTarget(index);
		strncpy_s(shared->playingTone, assignment->second.c_str(), _TRUNCATE);
	}

	void StompboxService::RestoreBypassedAmps()
	{
		if (bypassedScene > 0 && scenes && bypassedScene < scenes->Count())
			for (const std::string& id : bypassedAmps) scenes->Engine(bypassedScene).SendCommand("SetParam " + id + " Enabled 1");
		bypassedScene = -1;
		bypassedAmps.clear();
	}

	// The stream may still be inside the old set for a block: freed once two more have played (or the stream stops).
	void StompboxService::Retire(std::unique_ptr<SceneSet> old)
	{
		if (old) retired.push_back({ std::move(old), router.BlockCounter() + 2 });
	}

	std::wstring StompboxService::SongTonesFile(const std::string& key) const
	{
		std::wstring name = Wide(key);
		for (wchar_t& c : name)
			if (wcschr(L"<>:\"/\\|?*", c) || c < 32) c = L'_';
		return dataPath + L"\\SongTones\\" + name + L".txt";
	}

	std::map<std::string, std::string> StompboxService::ReadAssignments(const std::string& song) const
	{
		std::map<std::string, std::string> tones;
		if (song.empty()) return tones;
		std::ifstream file(std::filesystem::path(SongTonesFile(song)));
		for (std::string line; std::getline(file, line);)
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const size_t tab = line.find('\t');
			if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size())
			{
				if (!line.empty()) Log("stompbox: song %s: ignoring a malformed song tone line", song.c_str());
				continue;
			}
			tones[line.substr(0, tab)] = line.substr(tab + 1);
		}
		return tones;
	}

	void StompboxService::WriteAssignments(const std::string& song, const std::map<std::string, std::string>& tones) const
	{
		const std::wstring file = SongTonesFile(song);
		CreateDirectoryW((dataPath + L"\\SongTones").c_str(), nullptr);
		if (tones.empty())
		{
			DeleteFileW(file.c_str());
			return;
		}
		const std::wstring temp = file + L".tmp";
		{
			std::ofstream out(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
			for (const auto& tone : tones) out << tone.first << '\t' << tone.second << "\r\n";
			if (!out)
			{
				Log("stompbox: song %s: the song tones could not be written", song.c_str());
				return;
			}
		}
		if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) Log("stompbox: song %s: the song tones could not be saved (%lu)", song.c_str(), GetLastError());
	}

	void StompboxService::PublishSong()
	{
		std::string text;
		for (const auto& assignment : assignments) text += assignment.first + "\t" + assignment.second + "\n";
		if (!StompboxControl::WriteText(&shared->songToneSeq, shared->songTones, StompboxControl::SONG_TONE_CHARS, text))
			Log("stompbox: song %s: its song tones do not fit the page (%zu bytes)", songKey.c_str(), text.size());
	}

	void StompboxService::PublishCatalog()
	{
		const std::string catalog = engine.DumpConfig();
		if (catalog.empty()) return;   // the engine crashed: the page keeps the last one
		publishedCatalog = catalog;
		++catalogVersion;
		checks.Reset(dataPath, catalog);
		WriteCatalogueFile(catalog);
		if (!StompboxControl::WriteText(&shared->catalogSeq, shared->catalog, StompboxControl::CATALOG_CHARS, catalog))
		{
			SetError("The pedal catalogue is too large for the Tone designer page.");
			Log("stompbox: catalogue of %zu bytes does not fit (%d)", catalog.size(), StompboxControl::CATALOG_CHARS);
		}
	}

	// Once a second: the trim the player's playing calls for, published once there are 15 s of playing behind it, then
	// again when it moves by a dB or more (InputLevel; the page applies it while its input is on Automatic).
	void StompboxService::PublishAutoTrim()
	{
		constexpr float MINIMUM_SECONDS = 15.0f;
		constexpr LONG STEP_CENTIDB = 100;
		const ULONGLONG now = GetTickCount64();
		if (now - autoTrimCheckedAt < 1000) return;
		autoTrimCheckedAt = now;
		const InputLevel::Reading reading = router.Input().Measure();
		InterlockedExchange(&shared->autoPlayingTenths, static_cast<LONG>(reading.playingSeconds * 10.0f));
		if (reading.playingSeconds < MINIMUM_SECONDS) return;
		const float trimDb = std::clamp(InputLevel::TARGET_PEAK_DB - reading.levelDb, -12.0f, 30.0f);
		const LONG trim = static_cast<LONG>(std::lround(trimDb * 2.0f) * 50);   // half dB steps, in hundredths
		if (hasPublishedAutoTrim && std::abs(trim - publishedAutoTrim) < STEP_CENTIDB) return;
		InterlockedExchange(&shared->autoTrimCentidB, trim);
		InterlockedIncrement(&shared->autoTrimSeq);
		Log("stompbox: input level %.1f dBFS from %.0f s of playing (background %.1f dBFS): automatic trim %+.1f dB",
			reading.levelDb, reading.playingSeconds, reading.noiseDb, trim / 100.0);
		publishedAutoTrim = trim;
		hasPublishedAutoTrim = true;
	}

	void StompboxService::PublishProgram()
	{
		if (engine.HasFaulted()) return;   // the page keeps showing the last tone; Rescan rebuilds it from that
		const std::string program = engine.DumpProgram();
		publishedProgram = program;
		++programVersion;
		if (!StompboxControl::WriteText(&shared->programSeq, shared->program, StompboxControl::PROGRAM_CHARS, program))
		{
			SetError("This tone is too large for the Tone designer page.");
			Log("stompbox: program of %zu bytes does not fit (%d)", program.size(), StompboxControl::PROGRAM_CHARS);
		}
		if (tone3000) ApplyToTone3000(program);
	}

	// Engine swap phase 1: the board's amp capture and cabinet, as the page set them in Stompbox, play through the
	// TONE3000 engine. The first switched-on NAM pedal and the first switched-on Cabinet in chain order; Stompbox's
	// other pedals (drives, EQ, modulation, delay, reverb) are not carried over yet.
	void StompboxService::ApplyToTone3000(const std::string& program)
	{
		std::vector<std::string> chain;
		std::map<std::string, std::string> files, enabled;
		std::istringstream lines(program);
		for (std::string line; std::getline(lines, line);)
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			std::istringstream words(line);
			std::string verb, first, pedal, parameter;
			words >> verb;
			if (verb == "SetChain")
			{
				words >> first;   // the chain's name
				for (std::string id; words >> id;) chain.push_back(id);
				continue;
			}
			if (verb != "SetParam" || !(words >> pedal >> parameter)) continue;
			std::string value;
			std::getline(words >> std::ws, value);
			if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
			if (parameter == "Model" || parameter == "Impulse") files[pedal] = value;
			else if (parameter == "Enabled") enabled[pedal] = value;
		}
		auto kindOf = [](const std::string& id) { return id.substr(0, id.find('_')); };
		auto isOn = [&](const std::string& id) { const auto it = enabled.find(id); return it == enabled.end() || it->second != "0"; };
		auto findFile = [&](const wchar_t* folder, const std::string& name, std::initializer_list<const wchar_t*> extensions) {
			const std::wstring base = dataPath + L"\\" + folder + L"\\" + std::filesystem::path(std::u8string(name.begin(), name.end())).wstring();
			for (const wchar_t* extension : extensions)
				if (GetFileAttributesW((base + extension).c_str()) != INVALID_FILE_ATTRIBUTES) return base + extension;
			return std::wstring();
		};
		std::wstring capture, cabinet;
		for (const std::string& id : chain)
		{
			const auto file = files.find(id);
			if (file == files.end() || file->second.empty() || !isOn(id)) continue;
			if (kindOf(id) == "NAM" && capture.empty()) capture = findFile(L"NAM", file->second, { L".nam", L".json", L".aidax" });
			if (kindOf(id) == "Cabinet" && cabinet.empty()) cabinet = findFile(L"Cabinets", file->second, { L".wav" });
		}
		tone3000->Apply(capture, cabinet);
	}

	void StompboxService::PublishPresets()
	{
		if (engine.HasFaulted()) return;
		publishedPreset = engine.CurrentPreset();
		std::string presets = publishedPreset + "\n";
		for (const std::string& name : engine.ListPresets()) presets += name + "\n";
		if (!StompboxControl::WriteText(&shared->presetSeq, shared->presets, StompboxControl::PRESET_CHARS, presets))
		{
			SetError("There are too many saved tones to list.");
			Log("stompbox: preset list of %zu bytes does not fit (%d)", presets.size(), StompboxControl::PRESET_CHARS);
		}
	}

	// Rescan: Stompbox lists its folders once, when a processor is made, so a fresh processor is made, given the
	// board's tone and run on silence until ready; then it replaces the board's processor (StompboxEngine::Adopt).
	// The page's commands wait meanwhile (PollEngine). After a failed start it simply tries the start again.
	void StompboxService::StartRescan()
	{
		if (rescanStep != RescanStep::None) return;
		if (!engine.IsLoaded())
		{
			if (LoadEngine()) Log("stompbox: rescan: the engine started");
			return;
		}
		if (isDevelopmentRemote)
		{
			SetError("Rescan is off while StompboxDev is on: restart the host to list new files.");
			return;
		}
		SetError({});
		auto fresh = std::make_unique<StompboxEngine>();
		std::string error;
		if (!fresh->Load(dataPath, error) || !fresh->SendCommand(GlobalChainCommand()))
		{
			SetError("Rescan did not work, so nothing changed: " + (error.empty() ? fresh->LastError() : error));
			Log("stompbox: rescan: no fresh engine (%s)", error.c_str());
			return;
		}
		fresh->Prepare(engine.PreparedFrames() > 0 ? engine.PreparedFrames() : UNPREPARED_FRAMES);
		const std::string catalog = fresh->DumpConfig();
		if (catalog.empty())
		{
			SetError("Rescan did not work, so nothing changed: the fresh engine did not start.");
			return;
		}
		checks.Reset(dataPath, catalog);   // the files the fresh engine lists: the board's tone is checked against them
		rescanProgram = engine.HasFaulted() ? publishedProgram : engine.DumpProgram();
		const std::string preset = engine.HasFaulted() ? publishedPreset : engine.CurrentPreset();
		rescanProblems.clear();
		rescanBlocks = 0;
		rescanPolls = 0;
		rescanEngine = std::move(fresh);
		// The saved tone first, for its name, then the board's own state over it, so unsaved edits stay.
		const std::vector<std::string> saved = rescanEngine->ListPresets();
		if (!preset.empty() && std::find(saved.begin(), saved.end(), preset) != saved.end() && checks.CheckPreset(preset).empty()
			&& rescanEngine->SendCommand("LoadPreset " + StompboxControl::Quote(preset)))
			rescanStep = RescanStep::LoadingPreset;
		else
			RestoreBoard();
		Log("stompbox: rescan: a fresh engine lists the folders again%s", preset.empty() ? "" : (" and loads " + preset).c_str());
	}

	void StompboxService::PollRescan()
	{
		if (rescanStep == RescanStep::Switching)
		{
			if (engine.FinishAdopt(++rescanPolls > SWITCH_POLLS)) FinishRescan();
			return;
		}
		rescanEngine->Pump(PUMP_BLOCKS_PER_POLL);
		if (rescanEngine->HasFaulted())
		{
			AbortRescan("Rescan did not work, so nothing changed: the engine crashed loading the tone.");
			return;
		}
		if (rescanEngine->IsPresetLoading()) return;
		if (rescanStep == RescanStep::LoadingPreset)
		{
			RestoreBoard();
			return;
		}
		rescanBlocks += PUMP_BLOCKS_PER_POLL;
		if (rescanBlocks * rescanEngine->PreparedFrames() < WARM_FRAMES) return;
		// Never prepared means the stream has never run the board, so it may be prepared here.
		if (engine.PreparedFrames() == 0) engine.Prepare(rescanEngine->PreparedFrames());
		if (!engine.Adopt(*rescanEngine))
		{
			AbortRescan("Rescan did not work, so nothing changed.");
			return;
		}
		rescanStep = RescanStep::Switching;
		rescanPolls = 0;
	}

	// The board's program on the fresh engine, except a file it can no longer load: that pedal is left without it.
	void StompboxService::RestoreBoard()
	{
		std::set<std::string> refused;
		for (const auto& line : checks.RefusedLines(rescanProgram))
		{
			refused.insert(line.first);
			rescanProblems.push_back(line.second);
			Log("stompbox: rescan: not restored (%s): %s", line.second.c_str(), line.first.c_str());
		}
		std::istringstream lines(rescanProgram);
		for (std::string line; std::getline(lines, line);)
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const std::vector<std::string> words = Words(line);
			if (words.empty() || words[0] == "SetPreset" || refused.count(line) != 0) continue;
			rescanEngine->SendCommand(line);
		}
		rescanStep = RescanStep::Warming;
		rescanBlocks = 0;
	}

	void StompboxService::FinishRescan()
	{
		rescanEngine.reset();   // its processor is the board's now
		rescanStep = RescanStep::None;
		isFaultReported = false;
		PublishCatalog();       // writes Catalogue.txt
		PublishPresets();
		PublishProgram();
		RebuildScenes();        // the song's tones, from the new lists
		if (!rescanProblems.empty())
		{
			std::string message = "Rescanned, but " + rescanProblems.front() + ", so that pedal plays without it";
			if (rescanProblems.size() > 1) message += " (and " + std::to_string(rescanProblems.size() - 1) + " more)";
			SetError(message + ".");
		}
		Log("stompbox: rescan done (%zu problem(s))", rescanProblems.size());
	}

	void StompboxService::AbortRescan(const std::string& error)
	{
		rescanEngine.reset();
		rescanStep = RescanStep::None;
		checks.Reset(dataPath, engine.DumpConfig());   // back to what the board lists
		SetError(error);
		Log("stompbox: rescan abandoned: %s", error.c_str());
	}

	// A crash inside Stompbox (StompboxEngine::Guard) leaves that engine playing the guitar dry: say so once.
	void StompboxService::ReportFaults()
	{
		bool faulted = engine.HasFaulted();
		if (scenes)
			for (int i = 1; i < scenes->Count(); ++i) faulted = faulted || scenes->Engine(i).HasFaulted();
		if (faulted && !isFaultReported) SetError("The Tone designer engine crashed, so the guitar plays without it. Press Rescan to start it again.");
		isFaultReported = faulted;
	}

	// For RSMods.exe's import (CONTRACTS section 3): the effects and parameters this engine accepts, and its files.
	void StompboxService::WriteCatalogueFile(const std::string& catalog) const
	{
		const std::wstring file = dataPath + L"\\Catalogue.txt";
		const std::wstring temp = file + L".tmp";
		{
			std::ofstream out(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
			out << catalog;
			if (!out)
			{
				Log("stompbox: Catalogue.txt could not be written");
				return;
			}
		}
		if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) Log("stompbox: Catalogue.txt could not be saved (%lu)", GetLastError());
	}

	void StompboxService::SetState(StompboxControl::EngineState state)
	{
		InterlockedExchange(&shared->engineState, static_cast<LONG>(state));
	}

	void StompboxService::SetSongState(StompboxControl::SongState state)
	{
		InterlockedExchange(&shared->songState, static_cast<LONG>(state));
	}

	void StompboxService::SetError(const std::string& error)
	{
		const size_t length = std::min(error.size(), static_cast<size_t>(StompboxControl::ERROR_CHARS - 1));
		std::memcpy(shared->error, error.data(), length);
		shared->error[length] = '\0';
	}

	void StompboxService::Stop()
	{
		RestoreBypassedAmps();
		preparing.clear();
		rescanEngine.reset();
		rescanStep = RescanStep::None;
		scenes.reset();   // the stream has stopped (the host is closing)
		retired.clear();
		undoManager.reset();   // a JUCE object: Stop runs before JUCE shuts down
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
