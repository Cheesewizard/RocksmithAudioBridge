#pragma once

#include "Framework/Framework.hpp"

#include "DropPedalTypes.hpp"

#include <string>

namespace AudioBridge::DropPedal
{
	// The pedal's readout in the top left corner: "Pitch: Off", "Drop: E -> Eb (-1)" or "Speaker: Eb -> E (+1)",
	// green while shifted down and amber while shifted up, plus a Player 2 row in multiplayer. A pedal key refused
	// because Speaker Mode is locked for the song flashes the row orange. MainThread.
	class DropPedalHud final
	{
	public:
		static constexpr char SHOW_SETTING[] = "AudioBridgeDropPedalShowOverlay";
		static constexpr char CUSTOM_COLORS_SETTING[] = "AudioBridgeDropPedalCustomOverlayColors";
		static constexpr char DOWN_COLOR_SETTING[] = "AudioBridgeDropPedalOverlayDownColor";
		static constexpr char UP_COLOR_SETTING[] = "AudioBridgeDropPedalOverlayUpColor";
		static constexpr char STATUS_COLOR_SETTING[] = "AudioBridgeDropPedalOverlayStatusColor";

		static Framework::SettingDefs Settings(const char* parentSetting);

		void LoadSettings(const Framework::ModContext& c);
		// Every tick: publishes both rows (hidden when they shouldn't show).
		void Publish(Framework::ModContext& c);

		// The readout's text for a player, also shown on the overlay's Drop Pedal page. Any thread.
		static std::string Line(::DropPedal::Player player);

	private:
		int Color(::DropPedal::Player player) const;

		int downColor = 0;
		int upColor = 0;
		int statusColor = 0;
		bool customColors = false;
	};
}
