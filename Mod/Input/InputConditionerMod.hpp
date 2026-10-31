#pragma once

#include "Framework/Framework.hpp"

#include "InputConditioner.hpp"

// Player 1's guitar input conditioner (noise gate pedal, compressor, make-up gain) at the front of the framework's
// input chain. The global gate goes through ToneGates, so a Tone designer tone's own gate takes over while it plays.
class InputConditionerMod : public Framework::IMod {
public:
	MOD_ID(InputConditionerMod)

	Framework::SettingDefs Settings() const override;

	void OnInitialize(Framework::ModContext& c) override;
	void OnEnabled(Framework::ModContext& c) override;
	void OnSettingsChanged(Framework::ModContext& c) override;

private:
	void Apply(const Framework::ModContext& c);
};
