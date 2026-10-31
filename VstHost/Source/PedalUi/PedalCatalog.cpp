#include "PedalCatalog.hpp"

namespace PedalUi
{
	namespace
	{
		juce::Colour ParseColour(const juce::String& hex, juce::Colour fallback)
		{
			if (!hex.startsWithChar('#') || hex.length() != 7) return fallback;
			return juce::Colour(static_cast<juce::uint32>(0xff000000u | hex.substring(1).getHexValue32()));
		}

		ParamType ParseType(const juce::String& type)
		{
			if (type == "Int") return ParamType::Int;
			if (type == "VSlider") return ParamType::VSlider;
			if (type == "Bool") return ParamType::Bool;
			if (type == "Enum") return ParamType::Enum;
			if (type == "File") return ParamType::File;
			if (type == "Power") return ParamType::Meter;
			return ParamType::Knob;
		}

		// ValueFormat is a .NET format string: "{0:0.0}dB" is one decimal and a dB suffix.
		void ParseFormat(const juce::String& format, ParamDesc& param)
		{
			const auto inner = format.fromFirstOccurrenceOf(":", false, false).upToFirstOccurrenceOf("}", false, false);
			param.decimals = inner.containsChar('.') ? inner.fromFirstOccurrenceOf(".", false, false).length() : 0;
			param.suffix = format.fromFirstOccurrenceOf("}", false, false).trim();
			if (param.suffix.equalsIgnoreCase("hz")) param.suffix = "Hz";
		}
	}

	std::vector<Word> Tokenize(const juce::String& line)
	{
		std::vector<Word> words;
		Word current;
		bool quoted = false, any = false;
		for (auto it = line.getCharPointer(); !it.isEmpty(); ++it)
		{
			const auto c = *it;
			if (quoted && c == '\\' && !(it + 1).isEmpty()) { ++it; current.text += *it; continue; }
			if (c == '"') { quoted = !quoted; current.quoted = true; any = true; continue; }
			if (!quoted && (c == ' ' || c == '\t'))
			{
				if (any) words.push_back(current);
				current = {};
				any = false;
				continue;
			}
			current.text += c;
			any = true;
		}
		if (any) words.push_back(current);
		return words;
	}

	juce::String KindOf(const juce::String& pedalId)
	{
		return pedalId.containsChar('_') ? pedalId.upToFirstOccurrenceOf("_", false, false) : pedalId;
	}

	std::vector<ProgramPedal> ParseProgram(const juce::String& program, const juce::String& chain)
	{
		std::vector<ProgramPedal> pedals;
		std::vector<std::vector<Word>> params;
		for (auto& line : juce::StringArray::fromLines(program))
		{
			auto words = Tokenize(line.trim());
			if (words.size() > 1 && words[0].text == "SetChain" && words[1].text == chain)
			{
				for (size_t i = 2; i < words.size(); ++i)
				{
					ProgramPedal pedal;
					pedal.id = words[i].text;
					pedal.kind = KindOf(pedal.id);
					pedals.push_back(pedal);
				}
			}
			else if (words.size() > 3 && words[0].text == "SetParam") params.push_back(std::move(words));
		}
		for (auto& words : params)
			for (auto& pedal : pedals)
				if (pedal.id == words[1].text)
				{
					if (words[2].text == "Enabled") pedal.enabled = words[3].text != "0";
					else pedal.values[words[2].text] = words[3].text;
				}
		return pedals;
	}

	juce::String ParamDesc::Format(double value) const
	{
		if (type == ParamType::Bool) return value >= 0.5 ? "On" : "Off";
		if (type == ParamType::Int) return juce::String(juce::roundToInt(value)) + (suffix.isNotEmpty() ? " " + suffix : juce::String());
		juce::String text = decimals > 0 ? juce::String(value, decimals) : juce::String(juce::roundToInt(value));
		if (suffix.isNotEmpty()) text << " " << suffix;
		return text;
	}

	const ParamDesc* PluginDesc::Find(const juce::String& paramName) const
	{
		for (auto& param : params)
			if (param.name == paramName) return &param;
		return nullptr;
	}

	const PluginDesc* Catalog::Find(const juce::String& name) const
	{
		for (auto& plugin : plugins)
			if (plugin.name == name) return &plugin;
		return nullptr;
	}

	PluginDesc* Catalog::FindMutable(const juce::String& name)
	{
		for (auto& plugin : plugins)
			if (plugin.name == name) return &plugin;
		plugins.push_back({});
		plugins.back().name = name;
		return &plugins.back();
	}

	bool Catalog::LoadFile(const juce::File& file)
	{
		if (!file.existsAsFile()) return false;
		Parse(file.loadFileAsString());
		return !plugins.empty();
	}

	void Catalog::Parse(const juce::String& configText)
	{
		plugins.clear();
		for (auto rawLine : juce::StringArray::fromLines(configText))
		{
			// ValueFormat's value may be empty or hold spaces: cut it out before splitting the rest (as the overlay's
			// StompboxLink does).
			juce::String format;
			const int formatAt = rawLine.indexOf(" ValueFormat ");
			const int syncAt = formatAt >= 0 ? rawLine.indexOf(formatAt, " CanSyncToHostBPM ") : -1;
			if (formatAt >= 0 && syncAt > formatAt)
			{
				format = rawLine.substring(formatAt + 13, syncAt);
				rawLine = rawLine.substring(0, formatAt) + rawLine.substring(syncAt);
			}

			const auto words = Tokenize(rawLine.trim());
			if (words.size() < 2) continue;
			const auto& kind = words[0].text;

			if (kind == "PluginConfig")
			{
				auto* plugin = FindMutable(words[1].text);
				for (size_t i = 2; i + 1 < words.size(); i += 2)
				{
					const auto& key = words[i].text;
					const auto& value = words[i + 1].text;
					if (key == "BackgroundColor") plugin->background = ParseColour(value, plugin->background);
					else if (key == "ForegroundColor") plugin->foreground = ParseColour(value, plugin->foreground);
					else if (key == "IsUserSelectable") plugin->userSelectable = value.getIntValue() != 0;
					else if (key == "Description") plugin->description = value;
				}
			}
			else if (kind == "ParameterConfig" && words.size() >= 3)
			{
				auto* plugin = FindMutable(words[1].text);
				ParamDesc param;
				param.name = words[2].text;
				ParseFormat(format, param);
				for (size_t i = 3; i + 1 < words.size(); i += 2)
				{
					const auto& key = words[i].text;
					const auto& value = words[i + 1].text;
					if (key == "Type") param.type = ParseType(value);
					else if (key == "MinValue") param.min = value.getDoubleValue();
					else if (key == "MaxValue") param.max = value.getDoubleValue();
					else if (key == "DefaultValue") param.def = value.getDoubleValue();
					else if (key == "RangePower") param.power = value.getDoubleValue();
					else if (key == "IsAdvanced") param.advanced = value.getIntValue() != 0;
					else if (key == "IsOutput") param.output = value.getIntValue() != 0;
					else if (key == "Description") param.description = value;
				}
				if (param.output && (param.type == ParamType::Knob || param.type == ParamType::Int)) param.type = ParamType::Meter;
				if (param.power <= 0.0) param.power = 1.0;
				// Stompbox leaves some defaults uninitialised (Compressor's Comp meter): keep them in range.
				if (param.max > param.min) param.def = juce::jlimit(param.min, param.max, param.def);
				if (param.output) param.def = param.min;   // a meter rests empty until the pedal reports
				plugin->params.push_back(std::move(param));
			}
			else if ((kind == "ParameterEnumValues" || kind == "ParameterFileTree") && words.size() >= 3)
			{
				// ParameterEnumValues <pedal> <param> <value>...; ParameterFileTree <pedal> <param> <folder> "<file>"...
				auto* plugin = FindMutable(words[1].text);
				for (auto& param : plugin->params)
				{
					if (param.name != words[2].text) continue;
					size_t first = 3;
					if (kind == "ParameterFileTree" && words.size() > 3 && !words[3].quoted)
					{
						param.fileTree = words[3].text;
						first = 4;
					}
					param.choices.clear();
					for (size_t i = first; i < words.size(); ++i) param.choices.add(words[i].text);
				}
			}
		}
	}
}
