#pragma once

#include "Framework/Framework.hpp"

#include "Host/InputObserver.hpp"

// Note by Note: in Riff Repeater the song waits on each note until it is played (flow mode lets it run until a miss).
// This is the engine (4.x DLL/NoteByNote/{Controller,Detection,Host,Ml,Protocol}), the Riff Repeater "NOTE BY NOTE"
// switch that turns it on (Ui/NoteByNoteMenu), the HUD (Ui/NoteByNoteHud: detection readout, target, bend meter,
// finger numerals), the MISSED -> NOTE BY NOTE label (Ui/NoteByNoteHudLabel), the overlay page with the HUD layout
// editor (Ui/NoteByNotePage) and the Look ahead tab (Ui/TablaturePreviewWindow). The highway renderer ports later
// behind the seams in Ui/HighwaySeam.hpp.
//
// Always enabled: the game's scoring and menu detours install once in OnInitialize (as 4.x did at startup) and pass
// straight through while Note by Note is off. Note by Note itself is switched on in the Riff Repeater menu, per session,
// never saved, as in 4.x.
class NoteByNoteMod : public Framework::IMod {
public:
	MOD_ID(NoteByNoteMod)

	Framework::SettingDefs Settings() const override;

	void OnInitialize(Framework::ModContext& c) override;
	void OnShutdown(Framework::ModContext& c) override;
	void OnSettingsChanged(Framework::ModContext& c) override;
	void OnTick(Framework::ModContext& c) override;

private:
	NoteByNote::InputObserver observer;
	bool installed = false;
	bool mlServiceWanted = false;
};
