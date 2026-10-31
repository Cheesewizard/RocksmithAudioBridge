#pragma once

// Drop Pedal's setting keys, shared by the mod (its schema) and the overlay page (its edits). The RSMods.ini names are
// 4.x's, so existing settings carry over; the keys carry the product's name (every mod shares one key space).
namespace AudioBridge::DropPedal::SettingKeys
{
	inline constexpr char ENABLED[] = "AudioBridgeDropPedalEnabled";
	inline constexpr char BASE_TUNING[] = "AudioBridgeDropPedalBaseTuning";
	inline constexpr char BASE_TUNING_PLAYER2[] = "AudioBridgeDropPedalBaseTuningPlayer2";
	inline constexpr char PITCH_DOWN_KEY[] = "DropPedalPitchDownKey";
	inline constexpr char PITCH_UP_KEY[] = "DropPedalPitchUpKey";
	inline constexpr char TOGGLE_KEY[] = "DropPedalToggleKey";
}
