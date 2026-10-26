#include "SceneSet.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace VstHost
{
	namespace
	{
		float Peak(const float* samples, int count)
		{
			float peak = 0.0f;
			for (int i = 0; i < count; ++i) peak = std::max(peak, std::fabs(samples[i]));
			return peak;
		}

		void RaisePeak(std::atomic<float>& peak, float value)
		{
			float current = peak.load(std::memory_order_relaxed);
			while (value > current && !peak.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
		}
	}

	SceneSet::SceneSet(StompboxEngine& editingEngine, std::vector<std::unique_ptr<StompboxEngine>> sceneEngines, int frameCount)
		: editing(editingEngine), scenes(std::move(sceneEngines)), frames(frameCount)
	{
		if (frames <= 0) throw std::invalid_argument("SceneSet needs the stream's block size.");
		for (const auto& scene : scenes)
			if (!scene || !scene->IsLoaded()) throw std::invalid_argument("SceneSet got an engine that is not loaded.");
		silence.assign(static_cast<size_t>(frames), 0.0f);
		tailLeftChannel.assign(static_cast<size_t>(frames), 0.0f);
		tailRightChannel.assign(static_cast<size_t>(frames), 0.0f);
	}

	StompboxEngine& SceneSet::Engine(int index)
	{
		if (index < 0 || index >= Count()) throw std::out_of_range("SceneSet::Engine index.");
		return index == 0 ? editing : *scenes[static_cast<size_t>(index - 1)];
	}

	void SceneSet::SetTarget(int index)
	{
		if (index < 0 || index >= Count()) throw std::out_of_range("SceneSet::SetTarget index.");
		target.store(index, std::memory_order_release);
	}

	void SceneSet::Process(const float* guitar, float* left, float* right, int frameCount)
	{
		if (frameCount != frames)
		{
			std::fill(left, left + frameCount, 0.0f);
			std::fill(right, right + frameCount, 0.0f);
			return;
		}
		RaisePeak(inputPeak, Peak(guitar, frameCount));

		const int wanted = target.load(std::memory_order_acquire);
		int current = active.load(std::memory_order_relaxed);
		if (wanted != current && phase != Phase::FadingOut) phase = Phase::FadingOut;   // also mid fade-in: from where it is

		Engine(current).Process(guitar, left, right, frameCount);

		// The fade: one linear ramp per block, down to silence or up to full level.
		const float step = static_cast<float>(frameCount) / FADE_FRAMES;
		const float from = gain;
		if (phase == Phase::FadingOut) gain = std::max(0.0f, gain - step);
		else if (phase == Phase::FadingIn) gain = std::min(1.0f, gain + step);
		if (from != 1.0f || gain != 1.0f)
		{
			for (int i = 0; i < frameCount; ++i)
			{
				const float g = from + (gain - from) * (static_cast<float>(i + 1) / frameCount);
				left[i] *= g;
				right[i] *= g;
			}
		}
		if (phase == Phase::FadingOut && gain <= 0.0f)
		{
			// Silent now: the old scene rings out on its own, the new one fades in from the next block.
			tail.store(current, std::memory_order_release);
			tailLeft = TAIL_FRAMES;
			active.store(wanted, std::memory_order_release);
			phase = Phase::FadingIn;
		}
		else if (phase == Phase::FadingIn && gain >= 1.0f) phase = Phase::Steady;

		const int ringing = tail.load(std::memory_order_relaxed);
		if (ringing >= 0)
		{
			if (ringing == active.load(std::memory_order_relaxed))
			{
				tail.store(-1, std::memory_order_release);   // switched straight back: it plays again, no tail
			}
			else
			{
				Engine(ringing).Process(silence.data(), tailLeftChannel.data(), tailRightChannel.data(), frameCount);
				for (int i = 0; i < frameCount; ++i)
				{
					left[i] += tailLeftChannel[i];
					right[i] += tailRightChannel[i];
				}
				tailLeft -= frameCount;
				if (tailLeft <= 0) tail.store(-1, std::memory_order_release);
			}
		}
		RaisePeak(outputPeak, std::max(Peak(left, frameCount), Peak(right, frameCount)));
	}
}
