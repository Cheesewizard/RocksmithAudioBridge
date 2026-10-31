#pragma once

#include "Framework/Framework.hpp"

#include "DropPedalHud.hpp"
#include "DropPedalTypes.hpp"
#include "InputShifter.hpp"
#include "PedalControls.hpp"
#include "SpeakerMode/SpeakerModeSession.hpp"

// Drop Pedal: retunes each player's guitar input before the game hears it, so a guitar in one tuning plays charts in
// another, or (Speaker Mode) shifts the song's music to the player's guitar and moves note detection with it. F7 cycles
// Off, Drop Pedal and Speaker Mode, "," and "." step the shift (Ctrl for Player 2), and each player's physical tuning
// names what they hear.
class DropPedalMod : public Framework::IMod {
public:
	MOD_ID(DropPedalMod)

	DropPedalMod();

	bool IsEnabled(const Framework::ModContext& c) const override;
	Framework::SettingDefs Settings() const override;
	std::vector<std::string_view> ClaimsExclusive() const override;
	int Priority() const override;

	void OnInitialize(Framework::ModContext& c) override;
	void OnSettingsChanged(Framework::ModContext& c) override;
	void OnEnabled(Framework::ModContext& c) override;
	void OnDisabled(Framework::ModContext& c) override;
	void OnTick(Framework::ModContext& c) override;
	void OnMenuTick(Framework::ModContext& c) override;
	void OnSongTick(Framework::ModContext& c) override;

private:
	void LoadBaseTunings(const Framework::ModContext& c);
	void StepShift(const Framework::ModContext& c, const Framework::KeyEvent& event, int semitoneDelta);
	void ResetSong();

	AudioBridge::DropPedal::InputShifter inputShifter;
	AudioBridge::DropPedal::PedalControls controls;
	AudioBridge::DropPedal::DropPedalHud hud;
	AudioBridge::SpeakerMode::SpeakerModeSession speakerMode;
	bool hooksInstalled = false;
};
