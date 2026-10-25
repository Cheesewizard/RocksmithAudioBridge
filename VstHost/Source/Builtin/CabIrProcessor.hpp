#pragma once

#include "AssetLibrary.hpp"
#include "BuiltinProcessor.hpp"

#include <NAM/dsp.h>

namespace VstHost::Builtin
{
	class CabIrProcessor final : public BuiltinProcessor
	{
	public:
		explicit CabIrProcessor(std::shared_ptr<AssetLibrary> library);

		juce::var SaveParameters() const override;
		void LoadParameters(const juce::var& state) override;

	private:
		void PrepareBlock(int frames) override;
		void ProcessBlock(juce::AudioBuffer<float>& buffer) override;

		std::shared_ptr<AssetLibrary> library;
		std::unique_ptr<nam::DSP> model;
		std::string assetHash;
		juce::AudioParameterFloat* mix;
		std::vector<float> input;
	};
}
