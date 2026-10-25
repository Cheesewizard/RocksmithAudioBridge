#pragma once

#include "BuiltinProcessor.hpp"
#include <AirwinRegistry.h>

namespace VstHost::Builtin
{
	class AirwindowsProcessor final : public BuiltinProcessor
	{
	public:
		explicit AirwindowsProcessor(const AirwinRegistry::awReg& registration);

		juce::var SaveParameters() const override;
		void LoadParameters(const juce::var& state) override;
		bool IsTimeBased() const override;
		double getTailLengthSeconds() const override { return IsTimeBased() ? 2.0 : 0.0; }

	private:
		void PrepareBlock(int frames) override;
		void ProcessBlock(juce::AudioBuffer<float>& buffer) override;

		std::string effectName;
		bool isTimeBased;
		std::unique_ptr<AirwinConsolidatedBase> effect;
		std::vector<juce::AudioParameterFloat*> parameters;
		juce::AudioBuffer<float> scratch;
	};
}
