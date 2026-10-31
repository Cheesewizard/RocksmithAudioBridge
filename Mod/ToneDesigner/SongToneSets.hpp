#pragma once

#include <map>
#include <string>
#include <vector>

// Song tone sets (.rabsong): a CDLC's own Tone designer tones, in a file beside its .psarc
// (`Song_p.psarc` -> `Song_p.rabsong`). RocksmithAudioBridge.Helper.exe reads and writes them (Helper/ToneDesigner/ToneDesignerTools.cs:
// --songtones-plan, --songtones-export, --songtones-import); this runs it in the background, one job at a time, and applies an
// imported set to the song's tone assignments:
//   - a song is chosen (in the game, or browsed on the page) and its sidecar is new or changed: import it, Rescan the
//     host's library, then give every tone key the player has not assigned themselves the set's tone;
//   - the keys given that way are kept in the import record as "fromSong", so the page can say which tones are the
//     song's and undo them ("Use mine instead", "useMine").
// Render thread only, except where a comment says otherwise.
namespace SongToneSets
{
	enum class SetState { None, Ready, Refused, Failed };

	// What the page shows for one song.
	struct View
	{
		bool isKnown = false;           // the song's .psarc and record have been looked at
		bool isDisc = false;            // in songs.psarc: Rocksmith's own songs carry no set beside them
		bool isImporting = false;
		bool isBusy = false;            // a job for this song is queued or running (export, import, plan)
		SetState state = SetState::None;
		std::string fileName;           // the .rabsong the record came from
		std::string author, reason;     // the charter (the credit); why it was refused (or did not read)
		bool useMine = false;
		bool isExportedHere = false;    // the player exported it from this PC: the set holds their own tones
		std::map<std::string, std::string> toneKeys;   // tone key -> the preset the import wrote
		std::vector<std::string> fromSong;

		// From --songtones-plan: what an export would carry.
		bool hasPlan = false;
		int tones = 0, captures = 0, cabinets = 0, embeddedFiles = 0;
		long long bytes = 0;            // embedded files only
		std::vector<std::string> problems;
	};

	// Every frame: the game's chosen song gets its sidecar imported even while the page is closed; finished jobs
	// are applied.
	void OnFrame(const std::string& gameSongKey);

	// The page shows `songKey` (the browsed song): look at its set, keep its export plan current.
	void Watch(const std::string& songKey);
	const View& Get(const std::string& songKey);

	// The song's tone assignments: the host's published ones for the game's song, else SongTones\<key>.txt as read
	// when the page started showing it, plus the page's own changes since.
	const std::map<std::string, std::string>& Assignments(const std::string& songKey);
	void Assign(const std::string& songKey, const std::string& toneKey, const std::string& preset);

	// "From song" (CONTRACTS 2): the key came from the set and still plays the set's tone.
	bool IsFromSong(const View& view, const std::string& toneKey, const std::string& assigned);

	void UseMine(const std::string& songKey);
	void UseSongTones(const std::string& songKey);
	void Export(const std::string& songKey);

	// A job's result for the page's notice, once; empty when there is none.
	std::string TakeNotice();
}
