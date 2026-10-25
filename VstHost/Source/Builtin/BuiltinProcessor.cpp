#include "BuiltinProcessor.hpp"
#include "BuiltinId.hpp"

#include <cmath>
#include <stdexcept>

namespace VstHost::Builtin
{
	BuiltinProcessor::BuiltinProcessor(const juce::String& id, const juce::String& name)
		: AudioPluginInstance(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
			.withOutput("Output", juce::AudioChannelSet::stereo(), true)), id(id), name(name)
	{
	}

	void BuiltinProcessor::fillInPluginDescription(juce::PluginDescription& description) const
	{
		description.name = name;
		description.descriptiveName = name;
		description.pluginFormatName = "Builtin";
		description.fileOrIdentifier = id;
		description.manufacturerName = "Rocksmith Audio Bridge";
		description.version = juce::String(ALGORITHM_VERSION);
		description.numInputChannels = 2;
		description.numOutputChannels = 2;
		description.uniqueId = id.hashCode();
	}

	bool BuiltinProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
	{
		return layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
			&& layout.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
	}

	void BuiltinProcessor::prepareToPlay(double sampleRate, int frames)
	{
		if (sampleRate != SAMPLE_RATE) throw std::invalid_argument("Built-in tones require 48000 Hz.");
		if (frames <= 0 || frames > 4096) throw std::invalid_argument("Built-in block size must be 1 to 4096 frames.");
		maxFrames = 0;
		PrepareBlock(frames);
		maxFrames = frames;
	}

	void BuiltinProcessor::releaseResources()
	{
		maxFrames = 0;
	}

	void BuiltinProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
	{
		juce::ScopedNoDenormals noDenormals;
		if (maxFrames == 0 || buffer.getNumSamples() > maxFrames || buffer.getNumChannels() < 2)
		{
			buffer.clear();
			return;
		}
		ProcessBlock(buffer);
	}

	void BuiltinProcessor::getStateInformation(juce::MemoryBlock& destination)
	{
		const auto json = juce::JSON::toString(SaveParameters(), true);
		destination.replaceAll(json.toRawUTF8(), json.getNumBytesAsUTF8());
	}

	void BuiltinProcessor::setStateInformation(const void* data, int size)
	{
		if (!data || size <= 0) throw std::invalid_argument("Built-in state is empty.");
		juce::var state;
		const auto result = juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(data), size), state);
		if (result.failed()) throw std::invalid_argument(result.getErrorMessage().toStdString());
		RequireVersion(state);
		LoadParameters(state);
	}

	juce::AudioParameterFloat* BuiltinProcessor::AddFloat(const juce::String& key, const juce::String& label,
		float minimum, float maximum, float defaultValue)
	{
		auto* parameter = new juce::AudioParameterFloat(juce::ParameterID(key, 1), label,
			juce::NormalisableRange<float>(minimum, maximum), defaultValue);
		addHostedParameter(std::unique_ptr<juce::HostedAudioProcessorParameter>(parameter));
		return parameter;
	}

	juce::DynamicObject& RequireObject(const juce::var& value)
	{
		auto* object = value.getDynamicObject();
		if (!object) throw std::invalid_argument("Expected a built-in state object.");
		return *object;
	}

	void RequireVersion(const juce::var& state)
	{
		auto& object = RequireObject(state);
		const auto version = object.getProperty("version");
		if (!version.isInt() || static_cast<int>(version) != ALGORITHM_VERSION)
			throw std::invalid_argument("Unsupported built-in algorithm version.");
	}

	float ReadNumber(const juce::var& state, const char* key, float minimum, float maximum)
	{
		const auto& value = RequireObject(state).getProperty(key);
		if (!value.isInt() && !value.isDouble()) throw std::invalid_argument(std::string("Missing numeric ") + key + ".");
		const auto number = static_cast<float>(value);
		if (!std::isfinite(number) || number < minimum || number > maximum)
			throw std::invalid_argument(std::string("Out-of-range ") + key + ".");
		return number;
	}

	juce::var NewState()
	{
		juce::var state(new juce::DynamicObject());
		state.getDynamicObject()->setProperty("version", ALGORITHM_VERSION);
		return state;
	}
}
