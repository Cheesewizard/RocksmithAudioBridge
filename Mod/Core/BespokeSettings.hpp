#pragma once

#include "Framework/Framework.hpp"

#include <utility>

// Rocksmith Audio Bridge edits its settings in its own UI: the overlay's pages in game and the Rocksmith Audio Bridge
// app on the desktop. The framework's generic forms (F9's Mod Settings and the RSMods app) skip a setting whose
// editedBy names a bespoke editor, so each mod hands its settings to ours here. Key binds stay in the generic forms, so
// they can still be rebound from RSMods. A setting already given to a narrower editor (the HUD layout, the gate's
// remembered threshold) keeps it: those stay out of the app's Settings page too.
namespace AudioBridge
{
	inline constexpr char BESPOKE_EDITOR[] = "RocksmithAudioBridge";

	inline Framework::SettingDefs EditedByAudioBridge(Framework::SettingDefs settings)
	{
		for (Framework::SettingDef& setting : settings)
			if (setting.type != Framework::SettingType::Key && setting.editedBy.empty()) setting.editedBy = BESPOKE_EDITOR;
		return settings;
	}
}
