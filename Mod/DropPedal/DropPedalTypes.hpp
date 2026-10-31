#pragma once

#include <cstddef>
#include <cstdint>

namespace DropPedal
{
	enum class Player : uint8_t
	{
		One,
		Two
	};

	constexpr size_t PLAYER_COUNT = 2;

	constexpr size_t GetPlayerIndex(Player player)
	{
		return static_cast<size_t>(player);
	}

	// Where the pitch shift happens: on the guitar input (Drop Pedal) or on the song's music (Speaker Mode).
	enum class PitchMode
	{
		Off,
		DropPedal,
		SpeakerMode
	};
}
