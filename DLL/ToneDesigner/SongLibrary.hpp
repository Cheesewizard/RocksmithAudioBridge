#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The player's songs (disc and DLC) for the Tone designer's song browser. The game DLL cannot read .psarc files,
// so RSMods.exe --song-index (GUI/Features/ToneDesigner/ToneDesignerTools.cs) writes an index file, reading only files that are new
// or changed since its last run; this loads that file. The index from the last session shows at once and is
// refreshed in the background the first time the Tone designer page is shown in a session.
namespace SongLibrary
{
	// One of an arrangement's tones (its manifest's Tone_Base / Tone_A..D): what the Tone designer's song tones assign.
	struct SongTone
	{
		std::string arrangement;    // "Lead", "Rhythm", "Bass"
		std::string slot;           // "Base", "A".."D"
		std::string key;            // the tone key the game reports while it plays (GameTones::CurrentToneKey)
		std::string name;           // what players see; the key when the manifest names none
	};

	struct Song
	{
		std::string key;            // the game's song key (what song tones are stored under)
		std::string title, artist, album;
		std::string arrangements;   // "Lead,Rhythm,Bass"
		std::string file;           // the .psarc it is in
		int year = 0;
		std::vector<SongTone> tones;   // every arrangement's, in arrangement then slot order
	};

	struct Library
	{
		std::vector<Song> songs;                          // by artist, then title
		std::unordered_map<std::string, size_t> byKey;    // lower-case key -> index
		const Song* Find(const std::string& key) const;
	};

	enum class State { NotRead, Reading, Ready, Failed };

	void Refresh();                                  // reads the index; the first call each session also updates it
	State GetState();
	std::shared_ptr<const Library> Get();            // never null (empty until read)
}
