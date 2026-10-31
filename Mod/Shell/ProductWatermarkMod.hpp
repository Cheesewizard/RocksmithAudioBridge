#pragma once

#include "Framework/Framework.hpp"

// "Rocksmith Audio Bridge <version>" in the main menu's bottom-right corner, so a player can tell the custom build from
// the official one. 4.x's D3DX line, drawn with ImGui at its place (the HUD's anchors have no bottom-right one).
class ProductWatermarkMod : public Framework::IMod {
public:
	MOD_ID(ProductWatermarkMod)

	void OnInitialize(Framework::ModContext& c) override;
	void OnMenuTick(Framework::ModContext& c) override;
	void OnSongEnter(Framework::ModContext& c) override;
};
