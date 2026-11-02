#pragma once
#include "Mods/StartupPreferences.hpp"
#include "OverlayUi.hpp"
#include <cstring>

namespace Overlay {
namespace StartupPanel {
    struct State {
        StartupPreferences::Values saved;
        int profileMode = 0;
        char profileName[1024]{};
        bool loaded = false;
        std::string message;
        bool error = false;
    };
    inline State& Current() { static State state; return state; }
    inline void Draw(bool refresh) {
        using namespace StartupPreferences;
        auto& state = Current();
        static const auto path = IniPath();
        if (!state.loaded || refresh) {
            state.saved = Read(path);
            state.profileMode = static_cast<int>(state.saved.profile);
            strncpy_s(state.profileName, state.saved.profileName.c_str(), _TRUNCATE);
            state.loaded = true;
            state.message.clear();
        }
        BeginCard("Game startup");
        Caption("Changes apply the next time Rocksmith starts.");
        static const std::vector<std::string> modes { "Normal sign-in", "Skip sign-in", "Start offline" };
        int mode = static_cast<int>(state.saved.ubisoft);
        if (DropdownRow("Ubisoft at startup", &mode, modes)) {
            const bool saved = SaveUbisoft(path, static_cast<UbisoftMode>(mode));
            state.saved = Read(path);
            state.error = !saved;
            state.message = saved ? "Saved. Applies next launch." : "Could not save RSMods.ini. Check that the file is writable.";
        }
        if (state.saved.ubisoft == UbisoftMode::Offline)
            Caption("Skips Ubisoft sign-in and automatic connection waits and failure dialogs.");
        else if (state.saved.ubisoft == UbisoftMode::SkipSignIn)
            Caption("Skips the optional sign-in screen. Automatic connection attempts stay enabled.");
        else Caption("Uses the game's original Ubisoft sign-in and connection flow.");

        static const std::vector<std::string> profiles { "Choose in game", "Continue with selected profile", "Use a named profile" };
        if (DropdownRow("Profile at startup", &state.profileMode, profiles)) {
            const auto selected = static_cast<ProfileMode>(state.profileMode);
            if (selected != ProfileMode::NamedProfile || state.profileName[0]) {
                const bool saved = SaveProfile(path, selected, state.profileName);
                state.saved = Read(path);
                state.error = !saved;
                state.message = saved ? "Saved. Applies next launch." : "Could not save the startup profile.";
            } else { state.message = "Enter the exact profile name, then choose Save profile."; state.error = false; }
        }
        if (state.profileMode == static_cast<int>(ProfileMode::NamedProfile)) {
            ImGui::SetNextItemWidth(RowWidth());
            const bool enter = ImGui::InputTextWithHint("##startupProfileName", "Exact saved profile name", state.profileName,
                sizeof(state.profileName), ImGuiInputTextFlags_EnterReturnsTrue);
            Caption("Match the name shown on Rocksmith's profile screen.");
            if (Button("Save profile") || enter) {
                const bool saved = SaveProfile(path, ProfileMode::NamedProfile, state.profileName);
                state.saved = Read(path);
                state.error = !saved;
                state.message = saved ? "Saved. Applies next launch." : "Enter a valid profile name and check that RSMods.ini is writable.";
            }
        }
        Caption("Native startup skips support the December 2024 Learn & Play build.");
        if (!state.message.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(state.error ? Color::Bad : Color::Muted));
            ImGui::TextWrapped("%s", state.message.c_str());
            ImGui::PopStyleColor();
        }
        EndCard();
    }
}
}
