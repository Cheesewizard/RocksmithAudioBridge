#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace AudioBridge::Input
{
	constexpr std::uint32_t PLAYER_COUNT = 2;

	// Which players RS_ASIO.ini gives an input, read the way RS_ASIO itself picks them.
	struct RsAsioInputs
	{
		std::array<bool, PLAYER_COUNT> configured{ false, false };
		std::array<int, PLAYER_COUNT> channels{ 0, -1 };
		std::array<std::string, PLAYER_COUNT> sources;
		std::array<bool, PLAYER_COUNT> inferred{ false, false };
	};

	// RS_ASIO.dll is next to the game and RS_ASIO.ini enables ASIO. Cable mode leaves RS_ASIO installed with ASIO off.
	bool IsRsAsioEnabled();
	RsAsioInputs ReadRsAsioInputs();
	// RS_ASIO.dll is loaded but its ini doesn't enable ASIO: logs exactly why (misnamed, UTF-16, no [Config], off).
	void ReportUnreadableRsAsioConfiguration();
}
