#include "AudioBridgeShellMod.hpp"

#include "HudDraw.hpp"
#include "OverlayUi.hpp"
#include "../Core/Text.hpp"
#include "ShellState.hpp"
#include "ShellWindow.hpp"

#include "Settings.hpp"

using Framework::Availability;
using Framework::KeyEdge;
using Framework::KeyEvent;
using Framework::ModContext;
using Framework::SettingDefs;

namespace {
	// 4.x's [Keybinds] name, so a key rebound there carries over.
	constexpr char kOverlayKey[] = "OverlayKey";
	// Insert: free in Rocksmith and in RSMods. 4.x's backslash is one of the game's own keys. A key a player rebound
	// in 4.x carries over; the key picker in the RSMods settings changes it.
	constexpr char kDefaultOverlayKey[] = "VK_INSERT";
	constexpr int kMenuOrder = 0;
	// The overlay page with the overlay key picker (Pages/GeneralPage.cpp).
	constexpr char kGeneralPage[] = "general";
}

SettingDefs AudioBridgeShellMod::Settings() const {
	return {
		// Its entry carries the row's title, so Mod Settings shows one "Rocksmith Audio Bridge" row: the key, then the
		// row's buttons (SettingsMenu draws a row inside the group with the same title and heading).
		Framework::KeyBind(kOverlayKey, "Audio Bridge overlay (open / close)", kDefaultOverlayKey)
			.Heading(Framework::SettingHeading::Audio)
			.EntryTitle("Rocksmith Audio Bridge"),
	};
}

void AudioBridgeShellMod::OnInitialize(ModContext& c) {
	LoadKeyName(c);

	c.Menu().RegisterFonts([] { Overlay::LoadFonts(ImGui::GetIO()); });
	// The logo is a managed-pool texture, so it survives device resets; CreateTextures returns at once once it exists.
	c.Draw().RegisterFrame([](IDirect3DDevice9* device) {
		Overlay::CreateTextures(device);
		AudioBridge::HudDraw::PrepareFonts();
	});

	// While the window is open it claims input each frame (Framework::Input().ClaimOverlayInput, our patch for
	// upstream's ask A): clicks, scrolls and keys go to the window and the game's DirectInput mouse reads come back
	// empty, as 4.x's OverlayInputCapture did. Rocksmith hides the system cursor, so ImGui draws one while it's open;
	// the Look ahead tab votes for it too (ShellState.hpp VoteMouseCursor), so neither switches off the other's.
	c.Menu().RegisterOverlay("audio-bridge-shell", kMenuOrder, [this] {
		const bool open = AudioBridge::Shell::IsOverlayOpen();
		AudioBridge::Shell::VoteMouseCursor(open);
		if (!open) return;
		Framework::Input().ClaimOverlayInput();
		bool stayOpen = true;
		AudioBridge::Shell::DrawWindow(&stayOpen, KeyName());
		if (!stayOpen) AudioBridge::Shell::SetOverlayOpen(false);
	});

	// Our settings are handed to our own editors (Core/BespokeSettings.hpp), so F9's generic Mod Settings form skips them;
	// our row there (RegisterSettingsRow, our framework patch) points players who look there to the overlay that edits them.
	c.Menu().RegisterSettingsRow("audio-bridge", "Rocksmith Audio Bridge", Framework::SettingHeading::Audio, [this] {
		ImGui::TextWrapped("Its settings are in the Audio Bridge overlay.");
		if (ImGui::Button("Open overlay")) AudioBridge::Shell::SetOverlayOpen(true);
		ImGui::SameLine();
		if (ImGui::Button("Change key")) AudioBridge::Shell::OpenOverlayAt(kGeneralPage);
		ImGui::TextDisabled("Overlay key: %s", KeyLabel().c_str());
	});

	c.Commands().BindSetting(kOverlayKey, KeyEdge::Up, Availability::Active,
		[](ModContext&, const KeyEvent&) { AudioBridge::Shell::SetOverlayOpen(!AudioBridge::Shell::IsOverlayOpen()); },
		{}, "Audio Bridge overlay");
}

void AudioBridgeShellMod::OnSettingsChanged(ModContext& c) {
	LoadKeyName(c);
}

// "VK_OEM_5" shows as "OEM_5"; a letter or F key reads as itself.
void AudioBridgeShellMod::LoadKeyName(const ModContext& c) {
	std::string name = c.Value(kOverlayKey);
	// The key's VK code for the Tone designer's VST host (ShellState.hpp); 0 for a name upstream doesn't know.
	const auto found = Settings::keyMap.find(name);
	AudioBridge::Shell::SetOverlayKeyCode(found != Settings::keyMap.end() ? found->second : 0u);
	if (name.rfind("VK_", 0) == 0) name = name.substr(3);
	std::lock_guard<std::mutex> lock(keyNameMutex);
	keyName = name;
}

// The overlay key as the keyboard prints it ("\\" for VK_OEM_5), else its setting name.
std::string AudioBridgeShellMod::KeyLabel() {
	const unsigned code = AudioBridge::Shell::OverlayKeyCode();
	const UINT scan = code != 0 ? MapVirtualKeyW(code, MAPVK_VK_TO_VSC) : 0;
	wchar_t name[64]{};
	if (scan != 0 && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) return AudioBridge::Text::Utf8(name);
	return KeyName();
}

std::string AudioBridgeShellMod::KeyName() {
	std::lock_guard<std::mutex> lock(keyNameMutex);
	return keyName;
}

static Framework::ModRegistrar<AudioBridgeShellMod> _audioBridgeShellReg;
