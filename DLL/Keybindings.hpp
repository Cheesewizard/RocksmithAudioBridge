#pragma once

#include <functional>
#include <string>
#include <vector>
#include <map>
#include <Windows.h>
#include "Mods/VolumeControl.hpp"
#include "Mods/Loft.hpp"
#include "Mods/Enumeration.hpp"
#include "Mods/VoiceOverControl.hpp"
#include "Twitch.hpp"
#include "CC/ControlServer.hpp"
#include "Menu.hpp"

struct ModCommand {
    std::function<bool()> condition = [] { return true; };
    std::function<void()> action;
    std::string logMessage;
};

namespace Keybindings {
    // Start or stop a recording take in game (Audio::Takes): the hotkey uses the remembered format, the overlay
    // passes its own. Video takes run a hidden RSMods.exe; no desktop window is involved.
    void ToggleAudioBridgeRecording();
    void ToggleAudioBridgeRecording(bool video);

    // Overlay key picker (Record page): while capturing, the next key released is handed to the overlay instead
    // of acting as a hotkey. Esc and the overlay key cancel. The overlay polls TakeCapturedKey once per frame.
    void BeginKeyCapture();
    void CancelKeyCapture();
    bool IsCapturingKey();
    void CaptureKey(WPARAM keyPressed);
    bool TakeCapturedKey(unsigned int& vk);

    void DispatchCommand(WPARAM keyPressed, const std::map<std::string, ModCommand, std::less<>>& commands);

    void HandleKeyUp(WPARAM keyPressed);
    void HandleKeyDown(WPARAM keyPressed);

    void InitializeCommands();
    void UpdateSettingsOnGUIChange(LPARAM lParam);

    // Looping functionality.
    inline float loopStart = NULL; // The start of the loop, as specified by the user.
    inline float roughLoopStart = NULL; // Just like loopStart, except we account for the lead-in time.
    inline float loopEnd = NULL; // The end of the loop, as specified by the user.
}
