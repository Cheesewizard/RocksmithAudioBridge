#pragma once

#include <filesystem>

namespace AudioBridge
{
	// Rocksmith Audio Bridge's own helper exe, for the jobs 4.x handed to its RSMods.exe with hidden arguments (Speaker
	// Mode's song decoding, video takes, the ML service, ...). RSMods.exe is upstream's configurator in the framework
	// build. <game>\RSMods\AudioBridge\RocksmithAudioBridge.Helper.exe.
	std::filesystem::path HelperExePath();
}
