#pragma once

#include <Windows.h>
#include <string>

// Rocksmith Audio Bridge's helper exe (HelperExe.hpp) run as a hidden job: no window, below-normal priority, working
// directory = the helper's folder; the caller waits for its exit code. Replaces 4.x's RsModsHelper, which ran RSMods.exe
// with the same hidden arguments (--song-index, --tone-backup, --tone-backup-auto, --tone-restore, --tone-import,
// --songtones-plan/export/import), now in Helper/ToneDesigner/ToneDesignerTools.cs. It can read the game's .psarc files
// and write zips, which the game DLL cannot.
namespace AudioBridge::HelperRun
{
	// Blocking: call from a worker thread. The exit code, or -1 when it did not start or ran past `timeoutMs`
	// (then it is ended).
	int Run(const std::wstring& arguments, DWORD timeoutMs);
	std::wstring Quote(const std::wstring& text);
	std::wstring GameFolder();   // the game's folder, with a trailing backslash
}
