#include "Tone3000Engine.hpp"

#include "HostLog.hpp"
#include "StompboxEngine.hpp"
#include "Processor.h"

#include <algorithm>
#include <cmath>

namespace VstHost
{
	namespace
	{
		constexpr double SAMPLE_RATE = 48000.0;   // the bridge's stream

		float Peak(const float* samples, int count)
		{
			float peak = 0.0f;
			for (int i = 0; i < count; ++i) peak = std::max(peak, std::fabs(samples[i]));
			return peak;
		}

		void RaisePeak(std::atomic<float>& slot, float value)
		{
			float seen = slot.load(std::memory_order_relaxed);
			while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
		}

		void SetParameter(TONE3000Processor& processor, const char* id, float normalised)
		{
			if (auto* parameter = processor.getParameters().getParameter(id)) parameter->setValueNotifyingHost(normalised);
			else Log("tone3000: no parameter %s", id);
		}
	}

	Tone3000Engine::Tone3000Engine()
		: processor(std::make_unique<TONE3000Processor>())
	{
		// The guitar arrives on one channel: fold it onto both, so the chain is mono and Spread builds the stereo.
		processor->setInputMode(TONE3000Processor::InputMode::Left);
		SetSpread(true);
		Log("tone3000: engine created (TONE3000 %s, local files only)", processor->getName().toRawUTF8());
	}

	Tone3000Engine::~Tone3000Engine()
	{
		if (processor && preparedFrames > 0) processor->releaseResources();
	}

	void Tone3000Engine::Prepare(int frames)
	{
		if (frames <= 0 || frames == preparedFrames) return;
		if (preparedFrames > 0) processor->releaseResources();
		processor->setPlayConfigDetails(2, 2, SAMPLE_RATE, frames);
		processor->prepareToPlay(SAMPLE_RATE, frames);
		preparedFrames = frames;
		Log("tone3000: prepared for %d frames at 48 kHz (latency %d samples)", frames, processor->getLatencySamples());
	}

	void Tone3000Engine::SetSpread(bool on) { SetParameter(*processor, "spreadEnabled", on ? 1.0f : 0.0f); }

	void Tone3000Engine::RemoveBlocks()
	{
		const juce::var state = processor->getChainState(-1);
		for (const char* lane : { "chain", "chainRight" })
		{
			if (const auto* items = state[lane].getArray())
				for (const auto& item : *items)
					if (item["kind"].toString() == "tone") processor->removeChainBlock(item["blockId"].toString().toStdString());
		}
	}

	void Tone3000Engine::Apply(const std::wstring& capture, const std::wstring& cabinet)
	{
		if (capture == capturePath && cabinet == cabinetPath) return;
		capturePath = capture;
		cabinetPath = cabinet;
		RemoveBlocks();
		// In signal order: each local load is added at the end of the chain.
		for (const std::wstring& path : { capture, cabinet })
		{
			if (path.empty()) continue;
			const juce::var result = processor->loadLocalTonePath(juce::File(juce::String(path.c_str())));
			if (!result["error"].isVoid())
				Log("tone3000: %ls not loaded: %s", path.c_str(), result["error"].toString().toRawUTF8());
		}
		Log("tone3000: rig %ls + %ls", capture.empty() ? L"(no capture)" : capture.c_str(), cabinet.empty() ? L"(no cabinet)" : cabinet.c_str());
	}

	void Tone3000Engine::Process(const float* guitar, float* left, float* right, int frames)
	{
		const float gain = StompboxEngine::InputGain();   // the player's measured trim, as for Stompbox
		for (int i = 0; i < frames; ++i) left[i] = guitar[i] * gain;
		std::copy(left, left + frames, right);
		RaisePeak(inputPeak, Peak(left, frames));
		if (frames != preparedFrames || faulted.load(std::memory_order_relaxed)) return;   // dry, never silence

		float* channels[] = { left, right };
		juce::AudioBuffer<float> buffer(channels, 2, frames);
		juce::MidiBuffer midi;
		try
		{
			processor->processBlock(buffer, midi);
		}
		catch (...)
		{
			faulted.store(true, std::memory_order_release);
			for (int i = 0; i < frames; ++i) left[i] = guitar[i] * gain;
			std::copy(left, left + frames, right);
			return;
		}
		RaisePeak(outputPeak, std::max(Peak(left, frames), Peak(right, frames)));
	}
}
