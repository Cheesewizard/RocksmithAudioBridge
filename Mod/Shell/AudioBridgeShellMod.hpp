#pragma once

#include "Framework/Framework.hpp"

#include <mutex>
#include <string>

// The Audio Bridge overlay: one window on the overlay key ([Keybinds] OverlayKey) that takes the mouse and keys while
// it's open, through the overlay input claim we added to the framework (ask A). Upstream's F9 Mod Settings window gets a
// row of ours under Audio (MenuRegistry::RegisterSettingsRow, our framework patch) with buttons that open it, or open it
// on the General page to change the key. Pages come from the other mods (ShellPages.hpp); the shell owns the window, theme, fonts
// and logo.
class AudioBridgeShellMod : public Framework::IMod {
public:
	MOD_ID(AudioBridgeShellMod)

	Framework::SettingDefs Settings() const override;

	void OnInitialize(Framework::ModContext& c) override;
	void OnSettingsChanged(Framework::ModContext& c) override;

private:
	void LoadKeyName(const Framework::ModContext& c);
	std::string KeyName();
	std::string KeyLabel();

	std::mutex keyNameMutex;
	std::string keyName;
};
