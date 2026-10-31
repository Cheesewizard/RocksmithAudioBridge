#pragma once

#include "../Shared/VstHostControl.hpp"

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

	// One parameter of the inspected plugin (InspectSlot), as the host publishes it.
	struct ParamView
	{
		int index = 0;                // the plugin's own parameter index: what SetParam takes
		float value = 0.0f;           // normalized 0..1
		int steps = 0;                // 0 = continuous; else how many values it takes (2 = a switch)
		bool isDiscrete = false;
		bool isBoolean = false;
		std::string name, text, label;   // text: the value as the plugin shows it; label: its unit ("dB"), may be empty
	};

	struct Snapshot
	{
		bool hostInstalled = false;   // the exe is in the game folder
		bool hostRunning = false;
		bool standby = false;         // running but off: the game's amp plays, the plugins stay loaded (TurnOff)
		bool hostHung = false;        // the process is there but has stopped answering (not while loading)
		bool hostRecovering = false;
		bool hostDisconnected = false; // the host executable is present but has no usable shared protocol connection
		bool scanningPlugins = false;
		std::string pluginScanError;
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
		// Fast tone changes: plugins the host keeps loaded outside the chain (a tone using them switches at once).
		std::vector<std::wstring> spares;
		std::string preloadingName;   // a plugin loading in the background now (starred, or the highlighted song's)
		// The host is busy only with that background load: tone file actions (save, open, rename) can still be
		// asked for; they queue and run once it has loaded. waitingOnBackground: something is queued behind it.
		bool backgroundOnly = false;
		bool waitingOnBackground = false;
		std::vector<std::wstring> alwaysLoaded;    // "Always keep loaded": loaded in the background each session
		std::vector<std::wstring> preloadFailed;   // plugins that went down while loading in the background
		std::vector<std::pair<std::wstring, std::string>> preloadFailedWhy;   // (path, why) for those, when the host said
		// The Edit menu: what Undo and Redo would do ("remove AmpliTube 5"); empty when there is nothing.
		std::string undoLabel, redoLabel;
		// The Inspector (read every frame): the parameters of paramSlot (-1 = none yet, or the inspected slot is
		// empty), the first VstHostControl::kMaxParams of the paramTotal it has.
		std::vector<ParamView> params;
		int paramSlot = -1;
		int paramTotal = 0;
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
	// Any thread: the host is loading plugins or a tone, or has commands not yet handled. A raw read of the shared
	// state (no refresh, never opens the mapping); for the cable amp link, which must not treat a load as a slow PC.
	bool IsLoading();
	bool IsHostGuest(const std::string& guestName);
	// Any thread, raw read like IsLoading: the delay the chain's playing plugins report, in samples; -1 when no host
	// is running (or the overlay has not opened the shared block yet).
	int ChainLatencySamples();   // the amp link's guest is the VST host (Tone designer)

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
	std::wstring DataFolder();           // %APPDATA%\RocksmithAudioBridge\VstHost: SongTones.ini, ToneGates.ini
	std::wstring TonesFolder();          // %APPDATA%\RocksmithAudioBridge\VstHost\Tones (not touched by install or uninstall)
	std::vector<std::string> SongsUsingTone(const std::string& tone);   // song keys from SongTones.ini
	size_t SongToneCount();              // songs with a tone of their own
	bool IsFavouriteTone(const std::string& tone);
	void SetFavouriteTone(const std::string& tone, bool favourite);

	// Host.
	void StartHost(bool standby = false);   // turns a host in standby on; standby = true starts a new one off
	void TurnOff();                      // standby (Game amp, External app tabs): plugins kept loaded so turning on is instant
	void StopHost();                     // closes the host completely (its plugins unload)
	void ForceCloseHost();               // only for a host that has stopped answering
	void RestartHost();                  // closes a verified stuck or disconnected host, then starts it again
	void SetAutoStart(bool on);
	void SetMuteWhileTuning(bool on);   // the chain goes quiet in the tuner screens
	void SetPopout(bool wanted);
	// Plugins kept loaded outside the chain, so tones using them switch at once (the host's spares).
	void LoadInBackground(const std::wstring& path);              // now, until the game closes (host running)
	void SetAlwaysLoaded(const std::wstring& path, bool on);      // every session, loaded in the background
	void Unload(const std::wstring& path);                        // its spares go, and Always is cleared
	// Every loaded copy (chain and spares) unloaded and loaded again from scratch, settings kept: for a plugin that
	// only sees new content (imported tones, a changed library) when it starts. Sound stops while it loads.
	void Reload(const std::wstring& path);
	void DismissCrashNote();

	// Chain commands (queued; the host handles them in order). AddPlugin starts the host when it is not running.
	void AddPlugin(const std::wstring& path, int position = -1);   // -1 appends; otherwise it goes in at that slot
	void ReplaceSlot(int slot, const std::wstring& path);
	void RemoveSlot(int slot);
	void MoveSlot(int slot, int delta);
	void SetBypass(int slot, bool bypass);
	void ShowSlot(int slot);             // opens that pedal's plugin window (or brings it to the front)
	void HideSlot(int slot);             // closes it
	void DuplicateSlot(int slot);
	void SetChainBypass(bool bypass);
	void ClearChain();                   // one undo step ("clear board")
	void NewTone();                      // clears the chain like ClearChain, and the undo history with it
	// Chain edits above (Add, Replace, Remove, Move, SetBypass, Duplicate, SetChainBypass, ClearChain, SetLevels)
	// and SetParam can be taken back; Snapshot::undoLabel / redoLabel name the steps. Loading a tone, or NewTone,
	// clears the history. Edits made in a plugin's own window are not recorded.
	void Undo();
	void Redo();
	// The Inspector: the host publishes this slot's parameters (Snapshot::params), -1 = none.
	void InspectSlot(int slot);
	// A parameter edit from the Inspector: a knob drag is one Begin, any number of Change, one End (one undo step,
	// one automation gesture); Set is a single edit (a click, a typed value). `value` is normalized 0..1.
	void SetParam(int slot, int index, float value, VstHostControl::ParamPhase phase);
	// The player clicked or typed on the Tone designer page: background loads of "Always keep loaded" plugins hold
	// off until the page has been quiet for a while, so they do not hold up what the player asks for.
	void NotePageInUse();
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
	// The divider between the board and the pane under it: pixels added to (or, negative, taken from) that pane.
	int BottomPaneExtra();
	void SetBottomPaneExtra(int pixels);

	// Per-song tones: the current song (Snapshot::songKey) gets this tone when it starts.
	void SetSongTone(const std::string& tone);
	void ClearSongTone();
	// Any song (the song browser): `tone` empty takes its tone away. The song playing now gets it at once.
	void SetToneForSong(const std::string& song, const std::string& tone);
	std::vector<std::pair<std::string, std::string>> SongToneList();   // song key, tone (SongTones.ini)

	// How long each song's tone took to load the last time the song started (SongLoads.ini): an audit, so a
	// player can see which songs wait for plugins that are not kept loaded.
	struct SongLoad
	{
		float seconds = 0.0f;
		std::string tone;
		std::string plugins;          // the ones loaded from scratch, "AmpliTube 5, Helix Native"; empty = all kept
	};
	std::vector<std::pair<std::string, SongLoad>> SongLoads();


	bool HostProcessRunning();           // thread safe
	void ReloadSettings();               // after a restore: favourites, lists and tones read again
}
