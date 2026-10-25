#include "CabIrProcessor.hpp"
#include "BuiltinId.hpp"

#include <stdexcept>

namespace VstHost::Builtin
{
	CabIrProcessor::CabIrProcessor(std::shared_ptr<AssetLibrary> library)
		: BuiltinProcessor("builtin:ir", "Cabinet IR"), library(std::move(library))
	{
		if (!this->library) throw std::invalid_argument("Cabinet requires an asset library.");
		mix = AddFloat("mix", "Mix", 0.0f, 1.0f, 1.0f);
	}

	juce::var CabIrProcessor::SaveParameters() const
	{
		auto state = NewState();
		state.getDynamicObject()->setProperty("asset", juce::String(assetHash));
		state.getDynamicObject()->setProperty("mix", mix->get());
		return state;
	}

	void CabIrProcessor::LoadParameters(const juce::var& state)
	{
		RequireVersion(state);
		const auto hash = RequireObject(state).getProperty("asset").toString().toStdString();
		const auto amount = ReadNumber(state, "mix", 0.0f, 1.0f);
		library->Verify(hash);
		if (library->Find(hash)->kind != AssetKind::ImpulseResponse) throw std::invalid_argument("Cabinet requires a .wav IR asset.");
		auto loaded = AssetLibrary::LoadModel(library->PathOf(hash), AssetKind::ImpulseResponse);
		if (loaded->GetExpectedSampleRate() != SAMPLE_RATE) throw std::invalid_argument("Managed cabinet IR must be 48000 Hz.");
		if (maxFrames > 0) loaded->Reset(SAMPLE_RATE, maxFrames);
		model = std::move(loaded);
		assetHash = hash;
		*mix = amount;
	}

	void CabIrProcessor::PrepareBlock(int frames)
	{
		if (!model) throw std::invalid_argument("Select an imported cabinet IR before playing the cabinet.");
		input.resize(frames);
		model->Reset(SAMPLE_RATE, frames);
	}

	void CabIrProcessor::ProcessBlock(juce::AudioBuffer<float>& buffer)
	{
		const int frames = buffer.getNumSamples();
		std::copy_n(buffer.getReadPointer(0), frames, input.data());
		float* inputs[] = { input.data() };
		float* outputs[] = { buffer.getWritePointer(0), buffer.getWritePointer(1) };
		model->process(inputs, outputs, frames);
		if (model->NumOutputChannels() == 1) buffer.copyFrom(1, 0, buffer, 0, 0, frames);
		const float wet = mix->get();
		for (int channel = 0; channel < 2; ++channel)
		{
			auto* output = buffer.getWritePointer(channel);
			for (int frame = 0; frame < frames; ++frame) output[frame] = output[frame] * wet + input[frame] * (1.0f - wet);
		}
	}
}
