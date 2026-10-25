#include "BuiltinTone.hpp"
#include "AssetLibrary.hpp"
#include "BuiltinId.hpp"

#include <stdexcept>

namespace VstHost::Builtin
{
	BuiltinTone BuiltinTone::Read(const juce::File& file)
	{
		return FromJson(ReadJson(file));
	}

	BuiltinTone BuiltinTone::FromJson(const juce::var& json)
	{
		auto& object = RequireObject(json);
		if (object.getProperty("format").toString() != "rab-tone"
			|| !object.getProperty("formatVersion").isInt() || static_cast<int>(object.getProperty("formatVersion")) != 1)
			throw std::invalid_argument("Unsupported portable tone format or version.");
		BuiltinTone tone;
		tone.outputDb = ReadNumber(json, "outputDb", -40.0f, 12.0f);
		const auto* blocks = object.getProperty("blocks").getArray();
		if (!blocks || blocks->size() > 8) throw std::invalid_argument("A built-in tone must contain at most eight blocks.");
		for (const auto& block : *blocks)
		{
			auto& data = RequireObject(block);
			ToneBlock entry;
			entry.id = std::wstring(data.getProperty("id").toString().toWideCharPointer());
			if (!IsBuiltinId(entry.id)) throw std::invalid_argument("Portable tones cannot contain a VST block.");
			if (!data.getProperty("bypass").isBool()) throw std::invalid_argument("Tone block bypass must be a boolean.");
			entry.isBypassed = static_cast<bool>(data.getProperty("bypass"));
			entry.state = data.getProperty("state");
			RequireVersion(entry.state);
			tone.blocks.push_back(std::move(entry));
		}
		return tone;
	}

	juce::var BuiltinTone::ToJson() const
	{
		juce::var json(new juce::DynamicObject());
		auto& object = *json.getDynamicObject();
		object.setProperty("format", "rab-tone");
		object.setProperty("formatVersion", 1);
		object.setProperty("outputDb", outputDb);
		juce::Array<juce::var> entries;
		for (const auto& block : blocks)
		{
			juce::var entry(new juce::DynamicObject());
			entry.getDynamicObject()->setProperty("id", juce::String(block.id.c_str()));
			entry.getDynamicObject()->setProperty("bypass", block.isBypassed);
			entry.getDynamicObject()->setProperty("state", block.state);
			entries.add(entry);
		}
		object.setProperty("blocks", entries);
		FromJson(json);
		return json;
	}

	void BuiltinTone::Write(const juce::File& file) const
	{
		WriteJson(file, ToJson());
	}
}
