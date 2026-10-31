#pragma once

#include "Framework/Framework.hpp"

// The Audio Bridge's routing core: the Real Tone Cable's modern capture and the game's persistent devices
// (Cable/, Devices/), the output engine and its control pipe (Engine/), the bridge driver's registration (Driver/),
// the forced Rocksmith gate and the output limiter, and 4.x's audio diagnostics HUD (Ui/AudioDiagnosticsHud). Always on: it is what the rest of the Audio Bridge plays through.
class AudioBridgeRoutingMod : public Framework::IMod {
public:
	MOD_ID(AudioBridgeRoutingMod)

	Framework::SettingDefs Settings() const override;

	void OnInitialize(Framework::ModContext& c) override;
	void OnSettingsChanged(Framework::ModContext& c) override;
	void OnTick(Framework::ModContext& c) override;

private:
	void ApplyRocksmithGate(const Framework::ModContext& c);
	void TryApplyOutputGuard(const Framework::ModContext& c);
	void PublishUsbRestartNotice(Framework::ModContext& c);

	bool installed = false;
	bool outputGuardApplied = false;
};
