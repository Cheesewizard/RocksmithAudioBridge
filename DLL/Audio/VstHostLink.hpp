#pragma once

#include <string>
#include <vector>

// Game side of the VST host (VstHost/, RocksmithAudioBridgeVstHost.exe): a 64-bit program that plays the guitar
// through a VST3 amp sim over the external amp link and shows the plugin's own window as a popout over the game.
// This tells the host when the overlay is on screen (the popout only shows with it) and carries the External amp
// page's buttons. State: VstHostControl.hpp.
namespace VstHostLink
{
	struct Snapshot
	{
		bool hostRunning = false;
		std::string pluginName;
		bool popoutWanted = false;
		bool popoutShown = false;
		bool hostInstalled = false;   // the exe is in the game folder
		bool switching = false;       // closing the host to start it with another plugin
		std::wstring lastPlugin;      // the plugin the host opens by default (HKCU\Software\RSMods\VstHost)
	};

	struct Plugin
	{
		std::string name;
		std::wstring path;
	};

	void OnFrame(bool overlayVisible);   // every rendered frame
	Snapshot GetSnapshot();
	const std::vector<Plugin>& Plugins();   // 64-bit VST3 plugins in Common Files\VST3, scanned on first use
	void Rescan();
	void SetPopout(bool wanted);
	void StartHost();                    // reopens the last plugin
	void LoadPlugin(const std::wstring& path);   // start the host with this plugin, closing a running one first
	void StopHost();
}
