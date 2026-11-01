#pragma once
#include <windows.h>
#include <string>

// On-disk preferences for the NEXT launch. Overlay edits deliberately do not
// update the live auto-load settings or apply/uninstall native hooks mid-game.
namespace StartupPreferences {
    enum class UbisoftMode { Normal, SkipSignIn, Offline };
    enum class ProfileMode { ChooseInGame, SelectedProfile, NamedProfile };
    struct Values {
        UbisoftMode ubisoft = UbisoftMode::Normal;
        ProfileMode profile = ProfileMode::ChooseInGame;
        std::string profileName;
    };

    inline std::wstring IniPath() {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return {};
        std::wstring result(path);
        const auto slash = result.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring{} : result.substr(0, slash + 1) + L"RSMods.ini";
    }
    inline std::string Utf8(const std::wstring& value) {
        if (value.empty()) return {};
        const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        std::string result(count, '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
        return result;
    }
    inline std::wstring Wide(const std::string& value) {
        if (value.empty()) return {};
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (!count) return {};
        std::wstring result(count, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
        return result;
    }
    inline std::wstring ReadKey(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
        wchar_t value[1024]{};
        GetPrivateProfileStringW(section, key, fallback, value, 1024, path.c_str());
        return value;
    }
    inline Values Read(const std::wstring& path) {
        Values values;
        const auto enabled = [&](const wchar_t* section, const wchar_t* key) {
            return _wcsicmp(ReadKey(path, section, key, L"off").c_str(), L"on") == 0;
        };
        values.ubisoft = enabled(L"Startup", L"OfflineStartup") ? UbisoftMode::Offline
            : enabled(L"Startup", L"SkipOptionalUbisoftLogin") ? UbisoftMode::SkipSignIn : UbisoftMode::Normal;
        values.profileName = Utf8(ReadKey(path, L"Toggle Switches", L"ProfileToLoad", L""));
        values.profile = !enabled(L"Toggle Switches", L"ForceProfileLoad") ? ProfileMode::ChooseInGame
            : values.profileName.empty() ? ProfileMode::SelectedProfile : ProfileMode::NamedProfile;
        return values;
    }
    inline bool SaveUbisoft(const std::wstring& path, UbisoftMode mode) {
        if (path.empty()) return false;
        const auto old = Read(path);
        const bool skip = mode != UbisoftMode::Normal;
        const bool offline = mode == UbisoftMode::Offline;
        if (WritePrivateProfileStringW(L"Startup", L"SkipOptionalUbisoftLogin", skip ? L"on" : L"off", path.c_str()) &&
            WritePrivateProfileStringW(L"Startup", L"OfflineStartup", offline ? L"on" : L"off", path.c_str())) return true;
        WritePrivateProfileStringW(L"Startup", L"SkipOptionalUbisoftLogin", old.ubisoft == UbisoftMode::Normal ? L"off" : L"on", path.c_str());
        WritePrivateProfileStringW(L"Startup", L"OfflineStartup", old.ubisoft == UbisoftMode::Offline ? L"on" : L"off", path.c_str());
        return false;
    }
    inline bool SaveProfile(const std::wstring& path, ProfileMode mode, const std::string& name) {
        if (path.empty()) return false;
        const auto old = Read(path);
        const auto wideName = Wide(name);
        if (mode == ProfileMode::NamedProfile && (wideName.empty() || wideName.find_first_not_of(L" \t") == std::wstring::npos ||
            wideName.find_first_of(L"\r\n") != std::wstring::npos)) return false;
        const bool automatic = mode != ProfileMode::ChooseInGame;
        // Manual mode keeps a previously chosen name for the next time it is enabled.
        const auto savedName = mode == ProfileMode::ChooseInGame ? Wide(old.profileName)
            : mode == ProfileMode::NamedProfile ? wideName : std::wstring{};
        if (WritePrivateProfileStringW(L"Toggle Switches", L"ProfileToLoad", savedName.c_str(), path.c_str()) &&
            WritePrivateProfileStringW(L"Toggle Switches", L"ForceProfileLoad", automatic ? L"on" : L"off", path.c_str())) return true;
        const auto oldName = Wide(old.profileName);
        WritePrivateProfileStringW(L"Toggle Switches", L"ProfileToLoad", oldName.c_str(), path.c_str());
        WritePrivateProfileStringW(L"Toggle Switches", L"ForceProfileLoad", old.profile == ProfileMode::ChooseInGame ? L"off" : L"on", path.c_str());
        return false;
    }
}
