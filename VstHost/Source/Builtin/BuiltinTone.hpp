#pragma once

#include "BuiltinProcessor.hpp"

namespace VstHost::Builtin
{
	struct ToneBlock
	{
		std::wstring id;
		bool isBypassed = false;
		juce::var state;
	};

	struct BuiltinTone
	{
		std::vector<ToneBlock> blocks;
		float outputDb = 0.0f;

		static BuiltinTone Read(const juce::File& file);
		static BuiltinTone FromJson(const juce::var& json);
		juce::var ToJson() const;
		void Write(const juce::File& file) const;
	};
}
