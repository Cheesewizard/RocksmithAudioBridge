#include "SongSpeed.hpp"

#include "../../Core/GameVersion.hpp"

#include "Mods/RiffRepeater.hpp"

#include <algorithm>
#include <atomic>

namespace NoteByNote::SongSpeed
{
	namespace
	{
		constexpr float DIVISOR = 10000.f;
		std::atomic<float> playerSliderPercent{ -1.f };
	}

	float GetRealPercent()
	{
		return RiffRepeater::GetSpeed(true);
	}

	// Slider -> RTPC is 400 - 3*slider at or below 100 (0x409400); the Linear RR hook makes it 10000/slider.
	float RealSpeedToSlider(float realPercent)
	{
		return RiffRepeater::currentlyEnabled_LinearRR ? realPercent : (400.f - DIVISOR / realPercent) / 3.f;
	}

	// The game curve 0x409400 (slider > 100: RTPC = 100 - (slider - 100) * 0.75), RTPC clamped 25..400,
	// real = 10000 / RTPC.
	float SliderToRealSpeed(float sliderPercent)
	{
		if (!(sliderPercent > 0.f)) return -1.f;
		if (RiffRepeater::currentlyEnabled_LinearRR) return sliderPercent;
		float rtpc = sliderPercent <= 100.f ? 400.f - 3.f * sliderPercent : 100.f - (sliderPercent - 100.f) * 0.75f;
		rtpc = (std::min)(400.f, (std::max)(25.f, rtpc));
		return DIVISOR / rtpc;
	}

	void RequestGameSpeed(float realPercent)
	{
		if (!(realPercent >= 25.f && realPercent <= 100.f)) return;
		const float slider = RealSpeedToSlider(realPercent);
		using SetRRSpeedFn = void(__cdecl*)(float);
		reinterpret_cast<SetRRSpeedFn>(AudioBridge::GameVersion::Address(0x405880, 0x000058A0))(slider);
	}

	float GetPlayerSliderPercent()
	{
		return playerSliderPercent.load();
	}

	void SetPlayerSliderPercent(float sliderPercent)
	{
		playerSliderPercent.store(sliderPercent);
	}

	float GetPlayerRealPercent()
	{
		return SliderToRealSpeed(playerSliderPercent.load());
	}
}
