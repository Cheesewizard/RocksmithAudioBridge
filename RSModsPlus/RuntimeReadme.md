# Rocksmith Audio Bridge runtime package

This package contains exactly four runtime binaries:

- `xinput1_3.dll` in the Rocksmith installation directory
- `RocksmithAudioBridge.dll` in the Rocksmith installation directory
- `RocksmithAudioBridgeAsio.dll` in the Rocksmith installation directory
- `RSMods.exe` in the Rocksmith installation directory

The FretNet model and its CQT filters are embedded inside `RocksmithAudioBridge.dll` in the main game folder.
The settings application's background mode runs inference automatically while
the game is open. There is no Python dependency, loose model, model download,
or manual service-start step. Supporting native/managed runtime DLLs and tools are embedded in RSMods.exe and
automatically extracted to a verified LocalAppData cache.

This package contains no loose dependency DLLs, capture DLL, installer helper, or
readme file. RSMods.exe embeds and verifies its supporting runtime payload.

## Installation

Close Rocksmith and the settings application, back up the files being replaced,
then extract the complete package into the Rocksmith 2014 installation directory.
Keep all four binaries from the same package. `RocksmithAudioBridgeAsio.dll` is used
only for optional ASIO output routing. Public builds need no research probe or
developer DLL.

The existing .NET Framework requirement remains. This package includes the x64
Visual C++ runtime files needed by the bundled inference/audio libraries.

If the Note by Note menu has not been installed, install it using the existing
Note by Note menu setup workflow before enabling it from Riff Repeater. No model
file needs selecting or downloading.

## Diagnostics

The game log reports startup of the bundled C# FretNet service. Its own bounded
log is `%LOCALAPPDATA%\RSModsPlus\Logs\ml-service.log`, with at most one rotated
previous file. Missing dependencies and inference failures are explicit errors.
The service stops with the game. Library notices are embedded and extracted under the runtime cache ThirdParty folder.

The runtime package does not contain development injectors, Python training tools,
test audio, model sidecar files, or a separate model updater.
