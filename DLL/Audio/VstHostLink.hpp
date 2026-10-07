#pragma once

#include <string>
#include <vector>

// Overlay side of the VST host (VstHost/, RocksmithAudioBridgeVstHost.exe): a 64-bit program that plays the guitar
// through a chain of VST3 plugins over the external amp link and shows the chosen plugin's window as a popout over
// the game. In the overlay it is the Tone designer page. This tells the host when the overlay is on screen (the
// popout only shows with it), carries the page's commands, starts the host with the game, and loads each song's
// tone when the song starts.
// State: VstHostControl.hpp. Settings: HKCU\Software\RSMods\VstHost. Song tones: SongTones.ini next to the tones.
namespace VstHostLink
{
	struct SlotView
	{
		std::string name;
		std::wstring path;
		bool bypass = false;
		bool windowOpen = false;      // its plugin window is open (shown while popoutWanted and the overlay is open)
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
		bool chainBypassed = false;   // every plugin skipped for now (clean guitar)
		// Plugins loading now (read every frame): the plugin's file name, the step of a tone, and for how long.
		bool loading = false;
		std::string loadingName;
		int loadingStep = 0, loadingTotal = 0;
		float loadingSeconds = 0.0f;
		std::string crashNote;        // a plugin crashed the host this session (what, and whether it was restarted)
		bool popoutWanted = false;
		bool popoutShown = false;
		bool autoStart = true;
		bool muteWhileTuning = false;
		bool exclusiveFullscreen = false;   // Rocksmith.ini: no window can show over the game
		std::string songKey;          // the song being played, empty outside songs
		std::string songTone;         // the tone assigned to that song
		std::string defaultTone;      // loads for songs without a tone of their own (empty: none)
	};

	// What a plugin is, from its VST3 sub categories (moduleinfo.json); Unknown when the bundle does not say.
	enum class Kind { Unknown, Amp, Effect, Dynamics, Instrument };

	struct Plugin
	{
		std::string name;
		std::wstring path;
		std::string vendor;           // moduleinfo.json, else the folder it sits in under VST3; may be empty
		std::string version;
		Kind kind = Kind::Unknown;
	};

	void OnFrame(bool overlayVisible);   // every rendered frame
	Snapshot GetSnapshot();              // cheap: refreshed a few times a second
	bool IsHostGuest(const std::string& guestName);   // the amp link's guest is the VST host (Tone designer)

	// Plugins: 64-bit VST3 in Common Files\VST3 and any added folders, scanned on first use.
	const std::vector<Plugin>& Plugins();
	void Rescan();                       // also forgets which plugins failed to load
	void AddScanFolder();                // asks for a folder (dialog on its own thread), then rescans
	std::vector<std::wstring> ScanFolders();
	void ShowPluginFile(const std::wstring& path);   // Explorer, with the plugin selected

	// Plugin browser marks, remembered in HKCU\Software\RSMods\VstHost.
	bool IsFavourite(const std::wstring& path);
	void SetFavourite(const std::wstring& path, bool favourite);
	std::vector<std::wstring> RecentPlugins();      // newest first, the last 8 added
	bool FailedToLoad(const std::wstring& path);

	// Tones: the saved chains.
	const std::vector<std::string>& Tones();
	void OpenTonesFolder();
	std::vector<std::string> SongsUsingTone(const std::string& tone);   // song keys from SongTones.ini
	size_t SongToneCount();              // songs with a tone of their own
	bool IsFavouriteTone(const std::string& tone);
	void SetFavouriteTone(const std::string& tone, bool favourite);

	// Host.
	void StartHost();
	void StopHost();
	void ForceCloseHost();               // only for a host that has stopped answering
	void SetAutoStart(bool on);
	void SetMuteWhileTuning(bool on);   // the chain goes quiet in the tuner screens
	void SetPopout(bool wanted);
	void DismissCrashNote();

	// Chain commands (queued; the host handles them in order). AddPlugin starts the host when it is not running.
	void AddPlugin(const std::wstring& path);
	void ReplaceSlot(int slot, const std::wstring& path);
	void RemoveSlot(int slot);
	void MoveSlot(int slot, int delta);
	void SetBypass(int slot, bool bypass);
	void ShowSlot(int slot);             // opens that pedal's plugin window (or brings it to the front)
	void HideSlot(int slot);             // closes it
	void DuplicateSlot(int slot);
	void SetChainBypass(bool bypass);
	void ClearChain();
	void SetLevels(int inputTenths, int outputTenths);
	void LoadTone(const std::string& name);
	void SaveTone(const std::string& name);
	void DeleteTone(const std::string& name);
	void RenameTone(const std::string& from, const std::string& to);   // song assignments follow the new name
	// Tone names are "Name" or "Folder/Name" (one folder level in the tones folder); RenameTone moves between folders.
	std::string DefaultTone();
	void SetDefaultTone(const std::string& tone);   // empty: none

	// Tone designer page layout: which sections are open (bit per section), remembered.
	unsigned SectionsOpen(unsigned fallback);
	void SetSectionsOpen(unsigned bits);

	// Per-song tones: the current song (Snapshot::songKey) gets this tone when it starts.
	void SetSongTone(const std::string& tone);
	void ClearSongTone();
}
