#pragma once

#include <string>
#include <vector>

// Backups of the Tone designer's setup: tones, song tones, plugin settings and its settings, one zip each, in
// Documents\Rocksmith Audio Bridge\Tone designer backups. RSMods.exe makes and restores them
// (GUI/ToneDesignerTools.cs). One automatic backup a day (the newest 7 kept), and Back up now / Restore on the
// page. A restore closes Tone designer first (it autosaves its chain), backs up what is there now, then starts
// Tone designer again if it was running.
namespace ToneBackup
{
	struct Entry
	{
		std::wstring path;
		std::string name;   // the file name without .zip ("Auto 2026-10-07", "Backup 2026-10-07 2114")
	};

	std::wstring Folder();
	std::vector<Entry> List();             // newest first; reads the folder (call when a menu opens, not every frame)
	void BackupNow();
	void Restore(const std::wstring& zip);
	void OpenFolder();

	void OnFrame(bool gameLoaded);         // render thread: the daily backup, and the restore's steps
	bool Busy();
	std::string Status();                  // the last result, for a few seconds after it
}
