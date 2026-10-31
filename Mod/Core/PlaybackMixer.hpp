#pragma once

// The game's own mixer levels (Wwise RTPCs, 0..100), which upstream's VolumeControl only steps by keys. Channels are
// 4.x's order: 0 music, 1 Player 1, 2 master, 3 Player 2, 4 mic, 5 voice-over, 6 sound effects. Every call refuses
// until Wwise is initialized: touching the mixer before that crashes the game.
namespace AudioBridge::PlaybackMixer
{
	constexpr unsigned int PLAYER1 = 1;

	bool GetVolume(unsigned int channel, float& volume);
	bool SetVolume(unsigned int channel, float volume);
	bool SetVolumeWithTransition(unsigned int channel, float volume, unsigned int transitionMilliseconds);
}
