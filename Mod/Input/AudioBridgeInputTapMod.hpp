#pragma once

#include "Framework/Framework.hpp"

// Feeds the game's guitar capture streams (one per player) through the framework's input chain (CaptureTap.hpp).
// Always on: it is what every Audio Bridge input processor runs on.
class AudioBridgeInputTapMod : public Framework::IMod {
public:
	MOD_ID(AudioBridgeInputTapMod)

	std::vector<std::string_view> ClaimsExclusive() const override;

	void OnInitialize(Framework::ModContext& c) override;
	void OnShutdown(Framework::ModContext& c) override;
	void OnEnabled(Framework::ModContext& c) override;
	void OnDisabled(Framework::ModContext& c) override;
	void OnTick(Framework::ModContext& c) override;
};
