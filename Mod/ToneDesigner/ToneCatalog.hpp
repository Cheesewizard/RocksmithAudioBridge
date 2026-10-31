#pragma once

#include <string>

// What the installed starter tones say about their files: each amp, pedal and cabinet's sound and instrument, so the
// Tone designer's browser can sort captures by what they sound like instead of by gear names. Read from the tones
// release's manifest (%APPDATA%\RocksmithAudioBridge\Stompbox\StarterTones\manifest.json, installed with the mod from
// Cheesewizard/RocksmithAudioBridge-Tones) the first time it is asked. A file the player added has no entry.
// Render thread only.
namespace ToneCatalog
{
	enum class Character { Unknown, Clean, Crunch, HighGain, Drive, Boost };

	struct FileInfo
	{
		bool isPedal = false;           // a pedal capture (drive, boost) rather than an amp; cabinets are neither
		bool isBass = false;
		Character character = Character::Unknown;
		std::string description;        // the gear and settings captured, or the cabinet, speaker and mic
		std::string author;
		std::string license;            // SPDX identifier
	};

	// `folder` is "NAM" or "Cabinets", `name` the file name without its extension (as Stompbox lists it).
	const FileInfo* Find(const std::string& folder, const std::string& name);

	const char* CharacterLabel(Character character);
}
