#pragma once

#include <string>
#include <vector>

// Backups of the Tone designer's setup: tones, song tones, plugin settings and its settings, one zip each, in
// Documents\Rocksmith Audio Bridge\Tone designer backups. RocksmithAudioBridge.Helper.exe makes and restores them
// (Helper/ToneDesigner/ToneDesignerTools.cs). One automatic backup a day (the newest 7 kept), and Back up now / Restore on the
// page. A restore closes Tone designer first (it autosaves its chain), backs up what is there now, then starts
// Tone designer again if it was running.
// Export my setup / Import a setup move the whole setup to another PC: the same zip, saved or opened anywhere. An import
// adds and overwrites but deletes nothing (a restore replaces the VST rig's files), then says which plugins are missing.
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
	void ExportSetup();                    // asks where (a save dialog), then backs up there
	void ImportSetup();                    // asks for the file (an open dialog), then imports it as Restore does its steps

	// The outcome of an export or import, for the page to show once.
	struct Result
	{
		bool ok = false;
		bool isImport = false;
		std::string title;
		std::string text;
		std::vector<std::string> missing;  // "AmpliTube 5: used by 80's Marshall, Lead"
		std::vector<std::string> moved;    // "Helix Native (x64): found in D:\VST3\Line 6"
	};
	bool TakeResult(Result& out);          // true once per result

	void OnFrame(bool gameLoaded);         // render thread: the daily backup, and the restore's steps
	bool Busy();
	std::string Status();                  // the last result, for a few seconds after it
}
