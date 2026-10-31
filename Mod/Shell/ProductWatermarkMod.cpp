#include "ProductWatermarkMod.hpp"

#include "HudDraw.hpp"
#include "GameState.hpp"
#include "../Shared/ProductVersion.hpp"

#include <algorithm>
#include <atomic>

namespace {
	// Drawn after the shell (order 0) on the background draw list, so it sits under every window anyway.
	constexpr int kOverlayOrder = 100;
	constexpr std::uint32_t kWatermarkColor = 0xFF28D7F2;
	constexpr std::uint32_t kShadowColor = 0xCC000000;

	std::atomic<bool> onMainMenu{ false };   // written on MainThread, read on the render thread

	// 4.x's GameOverlay::DisplayProductVersion: right-aligned in the bottom-right corner (a box from 70% to 98.5% of
	// the width, its top at 89.5% of the height), one height/720 shadow under it, at least 16 px tall.
	void Draw() {
		if (!GameState::GameLoaded || !onMainMenu.load(std::memory_order_acquire)) return;
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		if (display.x <= 0.0f || display.y <= 0.0f) return;

		const float right = display.x * 0.985f;
		const float top = display.y * 0.895f;
		const float shadowOffset = (std::max)(1.0f, static_cast<float>(static_cast<int>(display.y / 720.0f)));
		const float fontSize = static_cast<float>((std::max)(16, static_cast<int>(display.y) / 60));
		const char* text = ProductVersion::DISPLAY_NAME;
		const float left = right - AudioBridge::HudDraw::Measure(fontSize, text).x;

		ImDrawList* list = ImGui::GetBackgroundDrawList();
		AudioBridge::HudDraw::Text(list, fontSize, ImVec2(left + shadowOffset, top + shadowOffset),
			AudioBridge::HudDraw::Argb(kShadowColor), text);
		AudioBridge::HudDraw::Text(list, fontSize, ImVec2(left, top), AudioBridge::HudDraw::Argb(kWatermarkColor), text);
	}
}

void ProductWatermarkMod::OnInitialize(Framework::ModContext& c) {
	c.Draw().RegisterFrame([](IDirect3DDevice9*) { AudioBridge::HudDraw::PrepareFonts(); });
	c.Menu().RegisterOverlay("audio-bridge-watermark", kOverlayOrder, Draw);
}

// currentMenu is written on MainThread, which is where menu ticks run.
void ProductWatermarkMod::OnMenuTick(Framework::ModContext&) {
	onMainMenu.store(GameState::currentMenu == "MainMenu", std::memory_order_release);
}

void ProductWatermarkMod::OnSongEnter(Framework::ModContext&) {
	onMainMenu.store(false, std::memory_order_release);
}

static Framework::ModRegistrar<ProductWatermarkMod> _productWatermarkReg;
