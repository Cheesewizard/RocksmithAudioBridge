#pragma once

// What Stompbox says about its pedals, parsed from its config protocol (StompboxEngine::DumpConfig, or a saved copy
// such as the tones repo's tools/stompbox-catalog.txt): PluginConfig, ParameterConfig, ParameterEnumValues,
// ParameterFileTree and EndConfig lines. The pedal editors (PedalView) are built from this alone, so a pedal Stompbox
// adds gets an editor without any code here. Program (DumpProgram) parsing is here too: the same words.

#include <juce_graphics/juce_graphics.h>

#include <map>
#include <string>
#include <vector>

namespace PedalUi
{
	enum class ParamType { Knob, Int, VSlider, Bool, Enum, File, Meter };

	struct ParamDesc
	{
		juce::String name;
		juce::String description;
		ParamType type = ParamType::Knob;
		double min = 0.0, max = 1.0, def = 0.0;
		double power = 1.0;          // Stompbox's RangePower: value = min + (max - min) * position^power
		int decimals = 2;            // from ValueFormat, "{0:0.0}dB" -> 1 decimal, "dB" suffix
		juce::String suffix;
		bool advanced = false;       // hidden until the pedal's "more" switch is on
		bool output = false;         // the pedal reports it (a level, gain reduction): shown, never edited
		juce::StringArray choices;   // Enum: its values; File: the files Stompbox lists (when the config has them)
		juce::String fileTree;       // a File parameter's folder in the data path (NAM, Cabinets, Music)

		bool IsChoice() const { return type == ParamType::Enum || type == ParamType::File; }
		juce::String Format(double value) const;
	};

	struct PluginDesc
	{
		juce::String name;
		juce::String description;
		juce::Colour background{ 0xff2a2d33 };
		juce::Colour foreground{ 0xffffffff };
		bool userSelectable = true;
		std::vector<ParamDesc> params;

		const ParamDesc* Find(const juce::String& paramName) const;
	};

	struct Word
	{
		juce::String text;
		bool quoted = false;
	};
	// Splits a protocol line on spaces; "quoted text" is one word (without its quotes, \" and \\ unescaped).
	std::vector<Word> Tokenize(const juce::String& line);

	// One pedal on the board, from a program: its instance id ("Delay_2"), its kind ("Delay") and its values as
	// Stompbox writes them (numbers, or a file or choice name).
	struct ProgramPedal
	{
		juce::String id;
		juce::String kind;
		bool enabled = true;
		std::map<juce::String, juce::String> values;
	};
	// The pedals of `chain` (SetChain <chain> ...) in order, with their SetParam values.
	std::vector<ProgramPedal> ParseProgram(const juce::String& program, const juce::String& chain);
	// "Delay_2" -> "Delay": instances of a kind are numbered after an underscore.
	juce::String KindOf(const juce::String& pedalId);

	class Catalog
	{
	public:
		void Parse(const juce::String& configText);
		bool LoadFile(const juce::File& file);

		const std::vector<PluginDesc>& Plugins() const { return plugins; }
		const PluginDesc* Find(const juce::String& name) const;

	private:
		std::vector<PluginDesc> plugins;
		PluginDesc* FindMutable(const juce::String& name);
	};
}
