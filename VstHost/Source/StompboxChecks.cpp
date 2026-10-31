#include "StompboxChecks.hpp"

#include "HostLog.hpp"

#include "../../Mod/Shared/StompboxControl.hpp"

#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace VstHost
{
	namespace
	{
		constexpr int PROBE_FRAMES = 256;
		constexpr int PROBE_BLOCKS = 4;
		constexpr uintmax_t MAX_PRESET_BYTES = 256 * 1024;   // a saved tone is a few kB; the page shows 64 kB at most
		constexpr char PROBE_CHAIN[] = "Probe";

		// What a saved tone may contain: the lines Stompbox's DumpProgram writes, and its controller mappings.
		// Anything else (SavePreset, DeletePreset, SetGlobalChain, Shutdown, ...) has no business in a tone file.
		const std::set<std::string> PRESET_VERBS = { "SetPreset", "SetChain", "SetPluginSlot", "SetParam",
			"MapController", "ClearControllerMap", "MapStompController", "MapModeController" };

		// The pedal each folder's files are tried in: the plain loaders, whose output with no file is the input.
		const std::map<std::string, std::string> PROBE_KINDS = { { "NAM", "NAM" }, { "Cabinets", "Cabinet" }, { "Reverb", "ConvoReverb" } };

		struct Noun
		{
			const char* name;
			const char* kind;
		};

		Noun NounFor(const std::string& folder)
		{
			if (folder == "NAM") return { "capture", "a NAM model" };
			if (folder == "Cabinets") return { "cabinet", "a WAV impulse response" };
			if (folder == "Reverb") return { "reverb impulse response", "a WAV impulse response" };
			return { "file", "a file this pedal reads" };
		}

		std::string Join(const std::vector<std::string>& words, size_t from)
		{
			std::string joined;
			for (size_t i = from; i < words.size(); ++i) joined += (i == from ? "" : " ") + words[i];
			return joined;
		}

		std::string Capitalised(std::string text)
		{
			if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
			return text;
		}

		// Finds `name` (no extension, as Stompbox lists it) in `folder`. Narrow strings are the ANSI code page, as
		// Stompbox's own std::filesystem calls use. `key` identifies this version of the file.
		bool FindFile(const std::filesystem::path& folder, const std::string& name, std::string& key)
		{
			std::error_code error;
			std::filesystem::directory_iterator entries(folder, error);
			if (error) return false;
			bool found = false;
			for (const auto& entry : entries)
			{
				try
				{
					if (!entry.is_regular_file(error) || entry.path().stem().string() != name) continue;
					const uintmax_t size = entry.file_size(error);
					const auto written = entry.last_write_time(error).time_since_epoch().count();
					key += "|" + entry.path().filename().string() + "|" + std::to_string(size) + "|" + std::to_string(written);
					found = true;
				}
				catch (const std::exception&) {}   // a name the code page cannot hold: Stompbox cannot list it either
			}
			return found;
		}
	}

	std::vector<std::string> StompboxChecks::Words(const std::string& line)
	{
		// As Stompbox's HandleCommand splits a line.
		std::vector<std::string> words;
		std::istringstream stream(line);
		std::string word;
		while (stream >> std::quoted(word)) words.push_back(word);
		return words;
	}

	void StompboxChecks::Reset(const std::wstring& path, const std::string& catalogue)
	{
		dataPath = path;
		fileParameters.clear();
		probePedals.clear();
		results.clear();
		probe.reset();   // a fresh probe lists the folders as they are now
		isProbeUnavailable = false;
		std::istringstream lines(catalogue);
		for (std::string line; std::getline(lines, line);)
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			// ParameterFileTree <pedal kind> <parameter> <folder> "<file>" "<file>" ...
			const std::vector<std::string> words = Words(line);
			if (words.size() < 4 || words[0] != "ParameterFileTree") continue;
			FileParameter& file = fileParameters[words[1]][words[2]];
			file.folder = words[3];
			file.names = std::set<std::string>(words.begin() + 4, words.end());
			const auto probeKind = PROBE_KINDS.find(file.folder);
			if (probeKind != PROBE_KINDS.end() && probeKind->second == words[1]) probePedals[file.folder] = { words[1], words[2] };
		}
	}

	std::string StompboxChecks::CheckCommand(const std::string& command)
	{
		const std::vector<std::string> words = Words(command);
		if (words.size() > 3 && words[0] == "SetParam") return CheckParameter(words[1], words[2], Join(words, 3));
		if (words.size() > 1 && words[0] == "LoadPreset") return CheckPreset(words[1]);
		return {};
	}

	std::string StompboxChecks::CheckParameter(const std::string& pedalId, const std::string& parameter, const std::string& value)
	{
		const std::string clause = ParameterClause(pedalId, parameter, value);
		return clause.empty() ? clause : Capitalised(clause) + ". The pedal keeps what it had.";
	}

	std::string StompboxChecks::ParameterClause(const std::string& pedalId, const std::string& parameter, const std::string& value)
	{
		const auto kind = fileParameters.find(KindOf(pedalId));
		if (kind == fileParameters.end()) return {};
		const auto file = kind->second.find(parameter);
		if (file == kind->second.end() || value.empty()) return {};   // not a file, or no file
		return FileClause(file->second, value);
	}

	std::string StompboxChecks::FileClause(const FileParameter& file, const std::string& name)
	{
		const Noun noun = NounFor(file.folder);
		const std::string quoted = std::string("the ") + noun.name + " '" + name + "'";
		if (file.names.count(name) == 0) return quoted + " is not in the list yet (press Rescan after adding files)";
		std::string key = file.folder + "|" + name;
		if (!FindFile(std::filesystem::path(dataPath) / file.folder, name, key)) return quoted + " is missing from the " + file.folder + " folder";
		auto known = results.find(key);
		if (known == results.end()) known = results.emplace(key, Probe(file.folder, name)).first;
		switch (known->second)
		{
		case Outcome::Ok: return {};
		case Outcome::Missing: return quoted + " is missing from the " + file.folder + " folder";
		case Outcome::Unreadable: return quoted + " is damaged or is not " + noun.kind;
		case Outcome::Dry: return quoted + " could not be read: it is damaged, or a kind the engine cannot play";
		case Outcome::Invalid: return quoted + " is damaged: it produces invalid sound";
		}
		return {};
	}

	std::string StompboxChecks::CheckPreset(const std::string& name, const char* who)
	{
		const std::string tone = std::string(who) + " '" + name + "'";
		if (name.empty() || name == "." || name == ".." || name.find_first_of("\\/:*?\"<>|") != std::string::npos)
			return tone + " has a name that is not a file name.";
		std::filesystem::path file;
		try
		{
			file = std::filesystem::path(dataPath) / "Presets" / std::filesystem::path(name);
		}
		catch (const std::exception&)
		{
			return tone + " has a name that is not a file name.";
		}
		std::error_code error;
		const uintmax_t size = std::filesystem::file_size(file, error);
		if (error) return tone + " is not saved any more.";
		if (size > MAX_PRESET_BYTES) return tone + " was not loaded: the file is too large to be a saved tone.";
		std::ifstream in(file, std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		if (!in && !in.eof()) return tone + " was not loaded: the file could not be read.";
		if (text.find('\0') != std::string::npos) return tone + " was not loaded: the file is damaged.";
		std::istringstream lines(text);
		for (std::string line; std::getline(lines, line);)
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const std::vector<std::string> words = Words(line);
			if (words.empty()) continue;
			if (PRESET_VERBS.count(words[0]) == 0) return tone + " was not loaded: it has a line the Tone designer will not run (" + words[0] + ").";
			if (words[0] != "SetParam" || words.size() < 4) continue;
			const std::string clause = ParameterClause(words[1], words[2], Join(words, 3));
			if (!clause.empty()) return tone + " was not loaded: " + clause + ".";
		}
		return {};
	}

	std::vector<std::pair<std::string, std::string>> StompboxChecks::RefusedLines(const std::string& program)
	{
		std::vector<std::pair<std::string, std::string>> refused;
		std::istringstream lines(program);
		for (std::string line; std::getline(lines, line);)
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const std::vector<std::string> words = Words(line);
			if (words.size() < 4 || words[0] != "SetParam") continue;
			const std::string clause = ParameterClause(words[1], words[2], Join(words, 3));
			if (!clause.empty()) refused.emplace_back(line, clause);
		}
		return refused;
	}

	bool StompboxChecks::EnsureProbe()
	{
		if (probe && !probe->HasFaulted()) return true;
		if (isProbeUnavailable) return false;
		probe.reset();   // a crashed one is kept alive inside StompboxEngine, never deleted
		auto engine = std::make_unique<StompboxEngine>();
		std::string error;
		if (!engine->Load(dataPath, error) || !engine->SendCommand(std::string("SetGlobalChain Chain ") + PROBE_CHAIN))
		{
			isProbeUnavailable = true;
			Log("stompbox: the file checks have no probe (%s); files are not tried before loading", error.c_str());
			return false;
		}
		engine->Prepare(PROBE_FRAMES);
		probe = std::move(engine);
		return true;
	}

	StompboxChecks::Outcome StompboxChecks::Probe(const std::string& folder, const std::string& name)
	{
		const auto pedal = probePedals.find(folder);
		if (pedal == probePedals.end() || !EnsureProbe()) return Outcome::Ok;   // nothing to try it in
		const std::string& kind = pedal->second.first;
		const std::string set = "SetParam " + kind + " " + pedal->second.second + " ";
		StompboxEngine& engine = *probe;
		std::vector<float> tone(PROBE_FRAMES), input(PROBE_FRAMES), output(PROBE_FRAMES);

		// The pedal alone, its previous file dropped (a name Stompbox does not list clears it), then this file.
		engine.SendCommand(std::string("SetChain ") + PROBE_CHAIN + " " + kind);
		engine.SendCommand(set + "-");
		engine.Render(input.data(), output.data());
		if (!engine.SendCommand(set + StompboxControl::Quote(name)))
		{
			Log("stompbox: %s\\%s refused: %s", folder.c_str(), name.c_str(), engine.LastError().c_str());
			return Outcome::Unreadable;
		}

		bool isChanged = false, isFinite = true;
		double phase = 0.0;
		for (int block = 0; block < PROBE_BLOCKS; ++block)
		{
			for (float& sample : tone)
			{
				sample = static_cast<float>(0.25 * std::sin(phase));
				phase += 2.0 * 3.14159265358979 * 220.0 / 48000.0;
			}
			input = tone;   // Stompbox may write to its input
			if (!engine.Render(input.data(), output.data()))
			{
				Log("stompbox: %s\\%s crashed the engine; refused", folder.c_str(), name.c_str());
				return Outcome::Unreadable;
			}
			for (int i = 0; i < PROBE_FRAMES; ++i)
			{
				isFinite = isFinite && std::isfinite(output[i]);
				isChanged = isChanged || output[i] != tone[i];
			}
		}
		if (!isFinite)
		{
			Log("stompbox: %s\\%s produces NaN or infinite samples; refused", folder.c_str(), name.c_str());
			return Outcome::Invalid;
		}
		// A convolution reverb mixes its dry signal in and may start late: only the amp and cabinet pedals, whose
		// output with no file is exactly their input, show a file that silently failed to load.
		if (!isChanged && folder != "Reverb")
		{
			Log("stompbox: %s\\%s did not load (the pedal passes the signal through); refused", folder.c_str(), name.c_str());
			return Outcome::Dry;
		}
		return Outcome::Ok;
	}
}
