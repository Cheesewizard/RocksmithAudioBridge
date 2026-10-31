#include "PlaybackMixer.hpp"

#include "Log.hpp"
#include "Wwise/SoundEngine.hpp"

#include <cmath>

namespace AudioBridge::PlaybackMixer
{
	namespace
	{
		constexpr unsigned int CHANNEL_COUNT = 7;
		// The game object the game sets its mixer levels on, besides the global one.
		constexpr AkGameObjectID MIXER_OBJECT = 0x1234;
		const char* const MIXER_NAMES[CHANNEL_COUNT] = {
			"Mixer_Music", "Mixer_Player1", "Master_Volume", "Mixer_Player2", "Mixer_Mic", "Mixer_VO", "Mixer_SFX" };
	}

	bool GetVolume(unsigned int channel, float& volume)
	{
		if (channel >= CHANNEL_COUNT || !Wwise::SoundEngine::IsInitialized()) return false;
		RTPCValue_type type = RTPCValue_GameObject;
		return Wwise::SoundEngine::Query::GetRTPCValue(MIXER_NAMES[channel], MIXER_OBJECT, &volume, &type) == AK_Success
			&& std::isfinite(volume) && volume >= 0.f && volume <= 100.f;
	}

	bool SetVolume(unsigned int channel, float volume)
	{
		return SetVolumeWithTransition(channel, volume, 0);
	}

	bool SetVolumeWithTransition(unsigned int channel, float volume, unsigned int transitionMilliseconds)
	{
		if (!std::isfinite(volume) || volume < 0.f || volume > 100.f) return false;
		if (channel >= CHANNEL_COUNT || !Wwise::SoundEngine::IsInitialized()) return false;

		const char* mixer = MIXER_NAMES[channel];
		const AkTimeMs duration = static_cast<AkTimeMs>(transitionMilliseconds);
		const auto global = Wwise::SoundEngine::SetRTPCValue(mixer, volume, AK_INVALID_GAME_OBJECT, duration, AkCurveInterpolation_Linear);
		const auto player = Wwise::SoundEngine::SetRTPCValue(mixer, volume, MIXER_OBJECT, duration, AkCurveInterpolation_Linear);
		if (global != AK_Success || player != AK_Success)
		{
			LOG_ERROR("(PLAYBACK MIXER) Could not set " << mixer << ": " << global << ", " << player << std::endl);
			return false;
		}
		return true;
	}
}
