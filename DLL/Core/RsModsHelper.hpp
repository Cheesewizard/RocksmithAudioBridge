#pragma once

#include <Windows.h>
#include <string>

// RSMods.exe (the game's RSMods folder) run as a hidden helper: no window, the overlay waits for its exit code.
// The Tone designer's song index and backups use it (GUI/Features/ToneDesigner/ToneDesignerTools.cs); it can read the game's .psarc
// files and write zips, which the game DLL cannot.
namespace RsModsHelper
{
	// Blocking: call from a worker thread. The exit code, or -1 when it did not start or ran past `timeoutMs`
	// (then it is ended).
	int Run(const std::wstring& arguments, DWORD timeoutMs);
	std::wstring Quote(const std::wstring& text);
	std::wstring GameFolder();   // with a trailing backslash
}
