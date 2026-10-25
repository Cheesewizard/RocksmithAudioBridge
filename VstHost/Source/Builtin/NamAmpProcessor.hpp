#pragma once

#include "AssetLibrary.hpp"
#include "BuiltinProcessor.hpp"

#include <NAM/dsp.h>

namespace VstHost::Builtin
{
	class NamAmpProcessor final : public BuiltinProcessor
	{
	public:
		explicit NamAmpProcessor(std::shared_ptr<AssetLibrary> library);

		juce::var SaveParameters() const override;
		void LoadParameters(const juce::var& state) override;

	private:
		void PrepareBlock(int frames) override;
		void ProcessBlock(juce::AudioBuffer<float>& buffer) override;

		std::shared_ptr<AssetLibrary> library;
		std::unique_ptr<nam::DSP> model;
		std::string assetHash;
		float slimmable = 1.0f;
		juce::AudioParameterFloat* inputDb;
		juce::AudioParameterFloat* outputDb;
		std::vector<float> input;
	};
}
