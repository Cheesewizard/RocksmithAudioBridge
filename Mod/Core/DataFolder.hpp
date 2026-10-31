#pragma once

#include <string>

namespace AudioBridge
{
	// %APPDATA%\RocksmithAudioBridge\VstHost: the Tone designer's data (tones, SongTones.ini, ToneGates.ini), shared
	// with the VST host. Empty if the folder can't be resolved.
	std::wstring VstHostDataFolder();
}
