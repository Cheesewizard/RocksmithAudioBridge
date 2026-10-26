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
		InterlockedExchange(&shared->hostPid, static_cast<LONG>(GetCurrentProcessId()));
		InterlockedExchange(&shared->playing, 0);
		SetSongState(StompboxControl::SongState::None);
		shared->playingTone[0] = '\0';
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

	bool StompboxService::IsWanted() const
	{
		return shared && StompboxControl::Load(&shared->wanted) != 0 && engine.IsLoaded();
	}

	void StompboxService::Poll()
	{
		if (!shared) return;

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

		if (isDevelopmentRemote) return;   // StompboxRemote drives the rig: no song tones
		PollGame();
		PollScenes();
	}

	void StompboxService::PrepareStream(int frames)
	{
		if (!engine.IsLoaded()) return;
		const bool changed = frames != streamFrames;
		streamFrames = frames;
		engine.Prepare(frames);
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
		return scenes ? static_cast<AudioSink*>(scenes.get()) : &engine;
	}

	float StompboxService::TakeInputPeak() { return scenes ? scenes->TakeInputPeak() : engine.TakeInputPeak(); }
	float StompboxService::TakeOutputPeak() { return scenes ? scenes->TakeOutputPeak() : engine.TakeOutputPeak(); }

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
		if (StartsWith(command, StompboxControl::SONG_TONE_COMMAND))
		{
			RunSongToneCommand(command);
			return;
		}
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
		// A saved tone the song uses changed on disk: load the song's tones again.
		if (StartsWith(command, "SavePreset") || StartsWith(command, "DeletePreset")) RebuildScenes();
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
		scene->SendCommand(std::string("SetGlobalChain MasterChain MasterIn Chain ") + StompboxControl::CHAIN_NAME + " MasterChain MasterOut");
		scene->Prepare(streamFrames);
		return scene;
	}

	void StompboxService::PollScenes()
	{
		// Tones being prepared are not in the stream yet: run them here until they have loaded and settled.
		if (!preparing.empty())
		{
			bool loading = false;
			for (auto& scene : preparing)
			{
				scene->Pump(PUMP_BLOCKS_PER_POLL);
				loading = loading || scene->IsPresetLoading();
			}
			if (!loading) preparedBlocks += PUMP_BLOCKS_PER_POLL;
			if (!loading && preparedBlocks * streamFrames >= WARM_FRAMES) FinishScenes();
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
		scenes.reset();   // the stream has stopped (the host is closing)
		retired.clear();
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
