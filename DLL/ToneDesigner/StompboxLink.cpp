#include "stdafx.h"
#include "StompboxLink.hpp"
#include "VstHostLink.hpp"
#include "Shared/StompboxControl.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>

namespace StompboxLink
{
	namespace
	{
		constexpr wchar_t SETTINGS_KEY[] = L"Software\\RSMods\\VstHost";
		constexpr wchar_t WANTED_VALUE[] = L"ToneDesignerEngine";

		struct Token
		{
			std::string text;
			bool isQuoted = false;
		};

		// A queued command; `key` names what it sets ("SetParam NAM Model"), so a newer value replaces one not sent yet.
		struct PendingCommand
		{
			std::string key;
			std::string line;
		};

		HANDLE g_mapping = nullptr;
		StompboxControl::Shared* g_shared = nullptr;
		bool g_openFailed = false;
		bool g_wantedRead = false;
		bool g_wanted = false;
		LONG g_catalogSeen = 0, g_programSeen = 0, g_presetSeen = 0, g_loadSeen = 0;
		LONG g_sentSequence = 0;
		std::deque<PendingCommand> g_commands;
		// The newest program the host published, applied once no edit of ours is outstanding: an older program arriving
		// mid-drag would otherwise snap a knob back for a moment.
		std::string g_latestProgram;
		bool g_isProgramPending = false;
		Snapshot g_snapshot;

		// Splits a Stompbox line the way Stompbox does (std::quoted): words, or "quoted text" with \" and \\.
		std::vector<Token> Tokenize(const std::string& line)
		{
			std::vector<Token> tokens;
			size_t i = 0;
			while (i < line.size())
			{
				while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
				if (i >= line.size()) break;
				Token token;
				if (line[i] == '"')
				{
					token.isQuoted = true;
					for (++i; i < line.size() && line[i] != '"'; ++i)
					{
						if (line[i] == '\\' && i + 1 < line.size()) ++i;
						token.text += line[i];
					}
					++i;   // the closing quote
				}
				else
				{
					while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') token.text += line[i++];
				}
				tokens.push_back(std::move(token));
			}
			return tokens;
		}

		std::vector<std::string> Lines(const std::string& text)
		{
			std::vector<std::string> lines;
			size_t start = 0;
			while (start <= text.size())
			{
				const size_t end = text.find('\n', start);
				std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (!line.empty()) lines.push_back(std::move(line));
				if (end == std::string::npos) break;
				start = end + 1;
			}
			return lines;
		}

		std::string Join(const std::vector<Token>& tokens, size_t from)
		{
			std::string text;
			for (size_t i = from; i < tokens.size(); ++i) text += (i > from ? " " : "") + tokens[i].text;
			return text;
		}

		float ToFloat(const std::string& text) { return static_cast<float>(std::atof(text.c_str())); }

		std::string KindOf(const std::string& pedalId)
		{
			const size_t underscore = pedalId.find('_');
			return underscore == std::string::npos ? pedalId : pedalId.substr(0, underscore);
		}

		PedalDefinition* MutableDefinition(std::vector<PedalDefinition>& definitions, const std::string& kind)
		{
			for (PedalDefinition& definition : definitions)
				if (definition.name == kind) return &definition;
			return nullptr;
		}

		ParameterDefinition* MutableParameter(std::vector<PedalDefinition>& definitions, const std::string& kind, const std::string& name)
		{
			PedalDefinition* definition = MutableDefinition(definitions, kind);
			if (!definition) return nullptr;
			for (ParameterDefinition& parameter : definition->parameters)
				if (parameter.name == name) return &parameter;
			return nullptr;
		}

		// Stompbox's DumpConfig (PluginProcessor::DumpConfig, AppendParamDef).
		std::vector<PedalDefinition> ParseCatalog(const std::string& text)
		{
			std::vector<PedalDefinition> definitions;
			PedalDefinition* current = nullptr;
			for (std::string line : Lines(text))
			{
				if (line.rfind("ParameterConfig ", 0) == 0)
				{
					if (!current)
					{
						LOG_WARNING("(TONE DESIGNER) a parameter outside a pedal in Stompbox's catalogue: " << line << std::endl);
						continue;
					}
					// ValueFormat's value may be empty or hold spaces: cut it out before splitting the rest.
					ParameterDefinition parameter;
					const size_t formatAt = line.find(" ValueFormat ");
					const size_t syncAt = line.find(" CanSyncToHostBPM ", formatAt == std::string::npos ? 0 : formatAt);
					if (formatAt != std::string::npos && syncAt != std::string::npos)
					{
						parameter.displayFormat = line.substr(formatAt + 13, syncAt - formatAt - 13);
						line.erase(formatAt, syncAt - formatAt);
					}
					const std::vector<Token> tokens = Tokenize(line);
					if (tokens.size() < 3) continue;
					parameter.name = tokens[2].text;
					for (size_t i = 3; i + 1 < tokens.size(); i += 2)
					{
						const std::string& key = tokens[i].text;
						const std::string& value = tokens[i + 1].text;
						if (key == "Type")
						{
							static const char* const types[] = { "Knob", "Bool", "Int", "VSlider", "Enum", "File", "Power" };
							const auto found = std::find(std::begin(types), std::end(types), value);
							if (found == std::end(types))
							{
								LOG_WARNING("(TONE DESIGNER) unknown Stompbox parameter type " << value << " (" << current->name << " " << parameter.name << ")" << std::endl);
								continue;
							}
							parameter.type = static_cast<ParameterType>(found - std::begin(types));
						}
						else if (key == "MinValue") parameter.minimum = ToFloat(value);
						else if (key == "MaxValue") parameter.maximum = ToFloat(value);
						else if (key == "DefaultValue") parameter.defaultValue = ToFloat(value);
						else if (key == "RangePower") parameter.rangePower = ToFloat(value);
						else if (key == "IsAdvanced") parameter.isAdvanced = value == "1";
						else if (key == "IsOutput") parameter.isOutput = value == "1";
						else if (key == "Description") parameter.description = value;
					}
					current->parameters.push_back(std::move(parameter));
					continue;
				}
				const std::vector<Token> tokens = Tokenize(line);
				if (tokens.empty()) continue;
				const std::string& word = tokens[0].text;
				if (word == "PluginConfig" && tokens.size() > 1)
				{
					definitions.push_back({});
					current = &definitions.back();
					current->name = tokens[1].text;
					for (size_t i = 2; i + 1 < tokens.size(); i += 2)
					{
						const std::string& key = tokens[i].text;
						const std::string& value = tokens[i + 1].text;
						if (key == "BackgroundColor") current->backgroundColor = value;
						else if (key == "ForegroundColor") current->foregroundColor = value;
						else if (key == "IsUserSelectable") current->isUserSelectable = value == "1";
						else if (key == "Description") current->description = value;
					}
				}
				else if (word == "EndConfig") current = nullptr;
				else if ((word == "ParameterEnumValues" || word == "ParameterFileTree") && tokens.size() > 2)
				{
					// Pointers into `definitions` stay valid here: nothing is added while choices are filled in.
					ParameterDefinition* parameter = MutableParameter(definitions, tokens[1].text, tokens[2].text);
					if (!parameter) continue;
					size_t first = 3;
					if (word == "ParameterFileTree" && tokens.size() > 3 && !tokens[3].isQuoted)
					{
						parameter->fileFolder = tokens[3].text;
						first = 4;
					}
					parameter->choices.clear();
					for (size_t i = first; i < tokens.size(); ++i) parameter->choices.push_back(tokens[i].text);
				}
			}
			return definitions;
		}

		// Stompbox's DumpProgram: SetPreset, SetChain and SetParam lines.
		void ParseProgram(const std::string& text, Snapshot& snapshot)
		{
			std::vector<Pedal> chain;
			std::map<std::string, size_t> byId;
			std::vector<std::vector<Token>> parameters;
			for (const std::string& line : Lines(text))
			{
				std::vector<Token> tokens = Tokenize(line);
				if (tokens.empty()) continue;
				if (tokens[0].text == "SetChain" && tokens.size() > 1 && tokens[1].text == StompboxControl::CHAIN_NAME)
				{
					for (size_t i = 2; i < tokens.size(); ++i)
					{
						byId[tokens[i].text] = chain.size();
						chain.push_back({ tokens[i].text, KindOf(tokens[i].text), true, {} });
					}
				}
				else if (tokens[0].text == "SetParam" && tokens.size() > 3) parameters.push_back(std::move(tokens));
			}
			for (const std::vector<Token>& tokens : parameters)
			{
				const auto found = byId.find(tokens[1].text);
				if (found == byId.end()) continue;
				Pedal& pedal = chain[found->second];
				if (tokens[2].text == "Enabled") pedal.isEnabled = tokens[3].text == "1";
				else pedal.values[tokens[2].text] = Join(tokens, 3);
			}
			snapshot.chain = std::move(chain);
		}

		bool ReadWanted()
		{
			DWORD value = 0, size = sizeof(value);
			return RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, WANTED_VALUE, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value != 0;
		}

		void Queue(const std::string& key, const std::string& line)
		{
			if (line.size() + 1 > StompboxControl::COMMAND_CHARS)
			{
				LOG_ERROR("(TONE DESIGNER) a command is too long for the engine (" << line.size() << " bytes)" << std::endl);
				g_snapshot.error = "That change is too long to send to the Tone designer engine.";
				return;
			}
			if (!key.empty())
			{
				// Not sent yet: replace it with the newer value (keeps knob drags from queueing every step).
				for (PendingCommand& pending : g_commands)
					if (pending.key == key) { pending.line = line; return; }
			}
			g_commands.push_back({ key, line });
		}

		void SendNext()
		{
			if (g_commands.empty()) return;
			if (StompboxControl::Load(&g_shared->commandDone) != StompboxControl::Load(&g_shared->commandSeq)) return;   // the last one still runs
			const std::string& line = g_commands.front().line;
			std::memcpy(g_shared->command, line.c_str(), line.size() + 1);
			MemoryBarrier();
			g_sentSequence = InterlockedIncrement(&g_shared->commandSeq);
			g_commands.pop_front();
		}
	}

	void OnFrame()
	{
		if (!g_shared && !g_openFailed)
		{
			g_shared = StompboxControl::Open(g_mapping);
			if (!g_shared || g_shared->version != StompboxControl::VERSION)
			{
				LOG_ERROR("(TONE DESIGNER) the engine's shared block could not be opened" << std::endl);
				g_openFailed = true;
				g_shared = nullptr;
				return;
			}
		}
		if (!g_shared) return;
		if (!g_wantedRead)
		{
			g_wantedRead = true;
			g_wanted = ReadWanted();
		}
		InterlockedExchange(&g_shared->wanted, g_wanted ? 1 : 0);

		Snapshot& s = g_snapshot;
		s.isConnected = StompboxControl::Load(&g_shared->hostPid) != 0 && VstHostLink::HostProcessRunning();
		s.engineState = static_cast<EngineState>(std::clamp<LONG>(StompboxControl::Load(&g_shared->engineState), 0, 3));
		s.isWanted = g_wanted;
		s.isPlaying = s.isConnected && StompboxControl::Load(&g_shared->playing) != 0;
		s.isPresetLoading = s.isConnected && StompboxControl::Load(&g_shared->presetLoading) != 0;
		if (!s.isConnected) g_commands.clear();   // a host that starts again publishes everything afresh
		if (s.isConnected) SendNext();
		s.isBusy = !g_commands.empty() || StompboxControl::Load(&g_shared->commandDone) != StompboxControl::Load(&g_shared->commandSeq);
		if (s.isConnected && !s.isBusy) s.error.assign(g_shared->error, strnlen(g_shared->error, StompboxControl::ERROR_CHARS));

		const LONG loadSeq = StompboxControl::Load(&g_shared->loadSeq);
		if (loadSeq != g_loadSeen)
		{
			g_loadSeen = loadSeq;
			s.hasLoad = true;
			s.blockFrames = StompboxControl::Load(&g_shared->blockFrames);
			s.medianLoad = StompboxControl::Load(&g_shared->medianPermille) / 1000.0f;
			s.slowLoad = StompboxControl::Load(&g_shared->loadPermille) / 1000.0f;
			s.peakLoad = StompboxControl::Load(&g_shared->peakPermille) / 1000.0f;
		}
		if (!s.isPlaying) s.hasLoad = false;

		std::string text;
		if (StompboxControl::ReadText(&g_shared->catalogSeq, g_shared->catalog, StompboxControl::CATALOG_CHARS, g_catalogSeen, text))
			s.definitions = ParseCatalog(text);
		if (StompboxControl::ReadText(&g_shared->programSeq, g_shared->program, StompboxControl::PROGRAM_CHARS, g_programSeen, text))
		{
			g_latestProgram = std::move(text);
			g_isProgramPending = true;
		}
		if (g_isProgramPending && !s.isBusy)
		{
			ParseProgram(g_latestProgram, s);
			g_isProgramPending = false;
		}
		if (StompboxControl::ReadText(&g_shared->presetSeq, g_shared->presets, StompboxControl::PRESET_CHARS, g_presetSeen, text))
		{
			std::vector<std::string> lines;
			size_t start = 0;
			for (size_t end; (end = text.find('\n', start)) != std::string::npos; start = end + 1) lines.push_back(text.substr(start, end - start));
			s.currentPreset = lines.empty() ? std::string() : lines.front();
			s.presets.assign(lines.empty() ? lines.end() : lines.begin() + 1, lines.end());
		}
	}

	const Snapshot& GetSnapshot() { return g_snapshot; }

	const PedalDefinition* FindDefinition(const std::string& kind)
	{
		for (const PedalDefinition& definition : g_snapshot.definitions)
			if (definition.name == kind) return &definition;
		return nullptr;
	}

	void SetWanted(bool wanted)
	{
		g_wanted = wanted;
		g_wantedRead = true;
		const DWORD value = wanted ? 1 : 0;
		RegSetKeyValueW(HKEY_CURRENT_USER, SETTINGS_KEY, WANTED_VALUE, REG_DWORD, &value, sizeof(value));
		if (g_shared) InterlockedExchange(&g_shared->wanted, value);
	}

	void SetChain(const std::vector<std::string>& pedalIds)
	{
		std::string line = std::string("SetChain ") + StompboxControl::CHAIN_NAME;
		for (const std::string& id : pedalIds) line += " " + id;
		Queue("SetChain", line);
		// Shown at once; the host's program confirms it.
		std::vector<Pedal> chain;
		for (const std::string& id : pedalIds)
		{
			const auto existing = std::find_if(g_snapshot.chain.begin(), g_snapshot.chain.end(), [&](const Pedal& p) { return p.id == id; });
			chain.push_back(existing != g_snapshot.chain.end() ? *existing : Pedal{ id, KindOf(id), true, {} });
		}
		g_snapshot.chain = std::move(chain);
	}

	void SetParameter(const std::string& pedalId, const std::string& parameter, const std::string& value)
	{
		const std::string key = "SetParam " + pedalId + " " + parameter;
		Queue(key, key + " " + StompboxControl::Quote(value));
		for (Pedal& pedal : g_snapshot.chain)
			if (pedal.id == pedalId) pedal.values[parameter] = value;
	}

	void SetEnabled(const std::string& pedalId, bool enabled)
	{
		const std::string key = "SetParam " + pedalId + " Enabled";
		Queue(key, key + (enabled ? " 1" : " 0"));
		for (Pedal& pedal : g_snapshot.chain)
			if (pedal.id == pedalId) pedal.isEnabled = enabled;
	}

	void LoadPreset(const std::string& name) { Queue({}, "LoadPreset " + StompboxControl::Quote(name)); }
	void SavePreset(const std::string& name) { Queue({}, "SavePreset " + StompboxControl::Quote(name)); }
	void DeletePreset(const std::string& name) { Queue({}, "DeletePreset " + StompboxControl::Quote(name)); }

	std::string NewPedalId(const std::string& kind)
	{
		const auto taken = [&](const std::string& id)
		{
			return std::any_of(g_snapshot.chain.begin(), g_snapshot.chain.end(), [&](const Pedal& p) { return p.id == id; });
		};
		if (!taken(kind)) return kind;
		for (int n = 2;; ++n)
		{
			const std::string id = kind + "_" + std::to_string(n);
			if (!taken(id)) return id;
		}
	}

	std::string FormatValue(const ParameterDefinition& parameter, float value)
	{
		// "{0:0.0}dB": text before and after the braces, and as many decimals as the pattern has after its point.
		const std::string& format = parameter.displayFormat;
		const size_t open = format.find("{0");
		const size_t close = format.find('}', open == std::string::npos ? 0 : open);
		if (open == std::string::npos || close == std::string::npos)
		{
			char text[32];
			sprintf_s(text, "%.2f", value);
			return text;
		}
		const std::string pattern = format.substr(open, close - open);
		const size_t point = pattern.find('.');
		const int decimals = point == std::string::npos ? 0 : static_cast<int>(pattern.size() - point - 1);
		char number[32];
		sprintf_s(number, "%.*f", decimals, value);
		return format.substr(0, open) + number + format.substr(close + 1);
	}
}
