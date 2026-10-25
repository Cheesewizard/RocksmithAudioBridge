#include "NamAmpProcessor.hpp"
#include "BuiltinId.hpp"

#include <NAM/slimmable.h>
#include <stdexcept>

namespace VstHost::Builtin
{
	NamAmpProcessor::NamAmpProcessor(std::shared_ptr<AssetLibrary> library)
		: BuiltinProcessor("builtin:nam", "NAM amp"), library(std::move(library))
	{
		if (!this->library) throw std::invalid_argument("NAM amp requires an asset library.");
		inputDb = AddFloat("inputDb", "Input", -24.0f, 24.0f, 0.0f);
		outputDb = AddFloat("outputDb", "Output", -40.0f, 12.0f, 0.0f);
	}

	juce::var NamAmpProcessor::SaveParameters() const
	{
		auto state = NewState();
		auto& object = *state.getDynamicObject();
		object.setProperty("asset", juce::String(assetHash));
		object.setProperty("inputDb", inputDb->get());
		object.setProperty("outputDb", outputDb->get());
		object.setProperty("slimmable", slimmable);
		return state;
	}

	void NamAmpProcessor::LoadParameters(const juce::var& state)
	{
		RequireVersion(state);
		const auto hash = RequireObject(state).getProperty("asset").toString().toStdString();
		const auto input = ReadNumber(state, "inputDb", -24.0f, 24.0f);
		const auto output = ReadNumber(state, "outputDb", -40.0f, 12.0f);
		const auto quality = ReadNumber(state, "slimmable", 0.0f, 1.0f);
		library->Verify(hash);
		if (library->Find(hash)->kind != AssetKind::NamModel) throw std::invalid_argument("NAM amp requires a .nam asset.");
		auto loaded = AssetLibrary::LoadModel(library->PathOf(hash), AssetKind::NamModel);
		if (auto* sized = dynamic_cast<nam::SlimmableModel*>(loaded.get())) sized->SetSlimmableSize(quality);
		else if (quality != 1.0f) throw std::invalid_argument("This NAM model does not support a reduced quality setting.");
		if (maxFrames > 0) loaded->Reset(SAMPLE_RATE, maxFrames);
		model = std::move(loaded);
		assetHash = hash;
		slimmable = quality;
		*inputDb = input;
		*outputDb = output;
	}

	void NamAmpProcessor::PrepareBlock(int frames)
	{
		if (!model) throw std::invalid_argument("Select an imported NAM model before playing the amp.");
		input.resize(frames);
		model->Reset(SAMPLE_RATE, frames);
	}

	void NamAmpProcessor::ProcessBlock(juce::AudioBuffer<float>& buffer)
	{
		const int frames = buffer.getNumSamples();
		const float gain = juce::Decibels::decibelsToGain(inputDb->get());
		const auto* source = buffer.getReadPointer(0);
		for (int frame = 0; frame < frames; ++frame) input[frame] = source[frame] * gain;
		float* inputs[] = { input.data() };
		float* outputs[] = { buffer.getWritePointer(0) };
		model->process(inputs, outputs, frames);
		buffer.applyGain(0, 0, frames, juce::Decibels::decibelsToGain(outputDb->get()));
		buffer.copyFrom(1, 0, buffer, 0, 0, frames);
	}
}
