#pragma once

#include <string>
#include <vector>

// Overlay side of the VST host (VstHost/, RocksmithAudioBridgeVstHost.exe): a 64-bit program that plays the guitar
// through a chain of VST3 plugins over the external amp link and shows the chosen plugin's window as a popout over
// the game. This tells the host when the overlay is on screen (the popout only shows with it), carries the External
// amp page's commands, starts the host with the game, and loads each song's tone when the song starts.
// State: VstHostControl.hpp. Settings: HKCU\Software\RSMods\VstHost. Song tones: SongTones.ini next to the tones.
namespace VstHostLink
{
	struct SlotView
	{
		std::string name;
		bool bypass = false;
	};

	struct Snapshot
	{
		bool hostInstalled = false;   // the exe is in the game folder
		bool hostRunning = false;
		bool hostHung = false;        // the process is there but has stopped answering (not while loading)
		bool busy = false;            // plugins loading, or commands not yet handled
		std::vector<SlotView> slots;
		int shownSlot = -1;
		std::string toneName;         // the tone last loaded or saved; empty = unsaved chain
		bool toneModified = false;
		int inputTenths = 0, outputTenths = 0;
		float inputPeak = 0.0f, outputPeak = 0.0f;   // linear, 1.0 = full scale
		std::string loadError;        // the last command's problem
		bool popoutWanted = false;
		bool popoutShown = false;
		bool autoStart = true;
		bool muteWhileTuning = false;
		bool exclusiveFullscreen = false;   // Rocksmith.ini: no window can show over the game
		std::string songKey;          // the song being played, empty outside songs
		std::string songTone;         // the tone assigned to that song
	};

	struct Plugin
	{
		std::string name;
		std::wstring path;
	};

	void OnFrame(bool overlayVisible);   // every rendered frame
	Snapshot GetSnapshot();              // cheap: refreshed a few times a second

	// Plugins: 64-bit VST3 in Common Files\VST3 and any added folders, scanned on first use.
	const std::vector<Plugin>& Plugins();
	void Rescan();
	void AddScanFolder();                // asks for a folder (dialog on its own thread), then rescans
	std::vector<std::wstring> ScanFolders();

	// Tones: the saved chains.
	const std::vector<std::string>& Tones();
	void OpenTonesFolder();

	// Host.
	void StartHost();
	void StopHost();
	void ForceCloseHost();               // only for a host that has stopped answering
	void SetAutoStart(bool on);
	void SetMuteWhileTuning(bool on);   // the chain goes quiet in the tuner screens
	void SetPopout(bool wanted);

	// Chain commands (queued; the host handles them in order). AddPlugin starts the host when it is not running.
	void AddPlugin(const std::wstring& path);
	void ReplaceSlot(int slot, const std::wstring& path);
	void RemoveSlot(int slot);
	void MoveSlot(int slot, int delta);
	void SetBypass(int slot, bool bypass);
	void ShowSlot(int slot);
	void ClearChain();
	void SetLevels(int inputTenths, int outputTenths);
	void LoadTone(const std::string& name);
	void SaveTone(const std::string& name);
	void DeleteTone(const std::string& name);

	// Per-song tones: the current song (Snapshot::songKey) gets this tone when it starts.
	void SetSongTone(const std::string& tone);
	void ClearSongTone();
}
