#pragma once

#include "SpeakerModeController.hpp"

#include <filesystem>

// Speaker Mode's song shift: hooks Wwise's Vorbis decoder (through RegisterCodec, so it must install before Wwise
// registers its codecs) and pitch-shifts the song's music, from a pre-rendered cache once it's ready and live until
// then. Shared by the 4.x DLL and the framework mod.
namespace Audio::SongShift::WwiseMusicHook
{
	// Once. owner outlives the process's audio; extractor is PreRenderedPitchCache's song decoder exe.
	void Install(ISpeakerModeController& owner, DetourFunction_t detour, const std::filesystem::path& extractor);
	// The game loop / MainThread.
	void Poll();
}
