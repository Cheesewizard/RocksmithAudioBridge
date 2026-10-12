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
	struct Song
	{
		std::string key;            // the game's song key (what song tones are stored under)
		std::string title, artist, album;
		std::string arrangements;   // "Lead,Rhythm,Bass"
		std::string file;           // the .psarc it is in
		int year = 0;
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
