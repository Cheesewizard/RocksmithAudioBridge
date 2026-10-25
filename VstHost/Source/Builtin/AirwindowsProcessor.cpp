#include "AirwindowsProcessor.hpp"
#include "BuiltinId.hpp"

#include <cmath>
#include <stdexcept>

namespace VstHost::Builtin
{
	AirwindowsProcessor::AirwindowsProcessor(const AirwinRegistry::awReg& registration)
		: BuiltinProcessor("builtin:airwindows/" + registration.name, registration.name),
		  effectName(registration.name), isTimeBased(registration.category == "Reverb" || registration.category == "Delay"),
		  effect(registration.generator())
	{
		if (!effect) throw std::runtime_error("Airwindows factory failed: " + effectName);
		if (registration.nParams < 0 || registration.nParams > 16) throw std::runtime_error("Unsupported Airwindows parameter count.");
		effect->setSampleRate(static_cast<float>(SAMPLE_RATE));
		for (int index = 0; index < registration.nParams; ++index)
		{
			char name[256]{};
			effect->getParameterName(index, name);
			parameters.push_back(AddFloat("parameter" + juce::String(index), name, 0.0f, 1.0f, effect->getParameter(index)));
		}
	}

	juce::var AirwindowsProcessor::SaveParameters() const
	{
		auto state = NewState();
		state.getDynamicObject()->setProperty("name", juce::String(effectName));
		juce::Array<juce::var> values;
		for (const auto* parameter : parameters) values.add(parameter->get());
		state.getDynamicObject()->setProperty("params", values);
		return state;
	}

	void AirwindowsProcessor::LoadParameters(const juce::var& state)
	{
		RequireVersion(state);
		auto& object = RequireObject(state);
		if (object.getProperty("name").toString().toStdString() != effectName) throw std::invalid_argument("Airwindows effect name does not match its block id.");
		const auto* values = object.getProperty("params").getArray();
		if (!values || values->size() != parameters.size()) throw std::invalid_argument("Airwindows parameter count does not match the pinned effect.");
		for (const auto& value : *values)
		{
			if ((!value.isDouble() && !value.isInt()) || !std::isfinite(static_cast<float>(value))
				|| static_cast<float>(value) < 0.0f || static_cast<float>(value) > 1.0f)
				throw std::invalid_argument("Airwindows parameters must be finite numbers between 0 and 1.");
		}
		for (int index = 0; index < values->size(); ++index) *parameters[index] = static_cast<float>((*values)[index]);
	}

	bool AirwindowsProcessor::IsTimeBased() const
	{
		return isTimeBased;
	}

	void AirwindowsProcessor::PrepareBlock(int frames)
	{
		scratch.setSize(2, frames);
		effect->setSampleRate(static_cast<float>(SAMPLE_RATE));
	}

	void AirwindowsProcessor::ProcessBlock(juce::AudioBuffer<float>& buffer)
	{
		for (int index = 0; index < parameters.size(); ++index) effect->setParameter(index, parameters[index]->get());
		const int frames = buffer.getNumSamples();
		scratch.copyFrom(0, 0, buffer, 0, 0, frames);
		scratch.copyFrom(1, 0, buffer, 1, 0, frames);
		float* inputs[] = { scratch.getWritePointer(0), scratch.getWritePointer(1) };
		float* outputs[] = { buffer.getWritePointer(0), buffer.getWritePointer(1) };
		effect->processReplacing(inputs, outputs, frames);
	}
}
