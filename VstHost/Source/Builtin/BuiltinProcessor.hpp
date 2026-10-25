#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <string>
#include <vector>

namespace VstHost::Builtin
{
	class BuiltinProcessor : public juce::AudioPluginInstance
	{
	public:
		BuiltinProcessor(const juce::String& id, const juce::String& name);

		const juce::String getName() const override { return name; }
		bool acceptsMidi() const override { return false; }
		bool producesMidi() const override { return false; }
		bool isMidiEffect() const override { return false; }
		double getTailLengthSeconds() const override { return 0.0; }
		int getNumPrograms() override { return 1; }
		int getCurrentProgram() override { return 0; }
		void setCurrentProgram(int) override { }
		const juce::String getProgramName(int) override { return {}; }
		void changeProgramName(int, const juce::String&) override { }
		bool hasEditor() const override { return false; }
		juce::AudioProcessorEditor* createEditor() override { return nullptr; }
		void fillInPluginDescription(juce::PluginDescription& description) const override;
		bool isBusesLayoutSupported(const BusesLayout& layout) const override;
		void prepareToPlay(double sampleRate, int maxFrames) final;
		void releaseResources() override;
		void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) final;
		void getStateInformation(juce::MemoryBlock& destination) final;
		void setStateInformation(const void* data, int size) final;

		virtual juce::var SaveParameters() const = 0;
		// Main thread, while the owner excludes audio processing (PluginHost::Suspend or an unpublished scene).
		virtual void LoadParameters(const juce::var& state) = 0;
		virtual bool IsTimeBased() const { return false; }

	protected:
		virtual void PrepareBlock(int maxFrames) = 0;
		virtual void ProcessBlock(juce::AudioBuffer<float>& buffer) = 0;
		juce::AudioParameterFloat* AddFloat(const juce::String& key, const juce::String& label,
			float minimum, float maximum, float defaultValue);

		int maxFrames = 0;

	private:
		juce::String id;
		juce::String name;
	};

	juce::DynamicObject& RequireObject(const juce::var& value);
	void RequireVersion(const juce::var& state);
	float ReadNumber(const juce::var& state, const char* key, float minimum, float maximum);
	juce::var NewState();
}
