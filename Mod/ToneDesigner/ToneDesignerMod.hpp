#pragma once

#include "Framework/Framework.hpp"

// The Tone designer: its overlay page (Guitar Tone, Ui/GuitarTonePage.cpp) and its backend: the VST host link
// (VstHostLink, RocksmithAudioBridgeVstHost.exe), the Stompbox engine link, song tones and .rabsong sets, the daily tone
// backup, and the game's own tone readout naming the designer's tone (ToneSwitchPopup, GameTones). Always on like
// routing: routing and External amp ask it about the VST host through IToneDesignerHost (ToneDesignerLink.hpp).
//
// Settings stay where 4.x kept them (HKCU\Software\RSMods\VstHost and the Tone designer's own ini files), so the mod
// declares none.
class ToneDesignerMod : public Framework::IMod {
public:
	MOD_ID(ToneDesignerMod)

	void OnInitialize(Framework::ModContext& c) override;

private:
	bool installed = false;
};
