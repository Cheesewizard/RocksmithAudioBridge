#pragma once

#include <array>
#include <string_view>

// Menu lists 4.x keeps in its GameState that upstream's doesn't have. Compare with upstream's Contains(value, range).
namespace AudioBridge::GameMenus
{
	inline constexpr std::array<std::string_view, 4> riffRepeaterMenus = {
		"LearnASong_RiffRepeater",
		"RiffRepeater",
		"RiffRepeater_AdvancedSettings",
		"RiffRepeater_Pause",
	};
}
