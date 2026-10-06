#pragma once

#include <string>

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
	};

	void OnFrame(bool overlayVisible);   // every rendered frame
	Snapshot GetSnapshot();
	void SetPopout(bool wanted);
	void StartHost();                    // reopens the last plugin
	void StopHost();
}
