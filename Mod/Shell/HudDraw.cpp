#include "HudDraw.hpp"

#include "ImGuiFontManager.hpp"
#include "Settings.hpp"

#include <cfloat>
#include <string>

namespace AudioBridge::HudDraw
{
	namespace
	{
		// The baked sizes. Four or so are in use at once (readout 16, target 24, diagnostics 32, numerals and REC 48 at
		// 1080p), which leaves upstream's cap of 8 room for its own HUD size and a face change.
		constexpr int BAKED_SIZES[] = { 16, 24, 32, 48, 64, 96 };
		constexpr int BAKED_COUNT = static_cast<int>(sizeof(BAKED_SIZES) / sizeof(BAKED_SIZES[0]));

		// Render thread only: PrepareFonts and the overlays both run there.
		ImFont* baked[BAKED_COUNT] = {};
		bool attempted[BAKED_COUNT] = {};
		bool wanted[BAKED_COUNT] = {};
		std::string face;

		int BucketFor(float pixelHeight)
		{
			for (int index = 0; index < BAKED_COUNT; ++index)
				if (pixelHeight <= static_cast<float>(BAKED_SIZES[index])) return index;
			return BAKED_COUNT - 1;
		}
	}

	void PrepareFonts()
	{
		// The frame callbacks run before upstream creates the ImGui context on the first frame.
		if (ImGui::GetCurrentContext() == nullptr) return;
		const std::string currentFace = Settings::ReturnSettingValue(Settings::Setting::OnScreenFont);
		if (currentFace != face)
		{
			// A new face: the old one's fonts stay in the atlas (ImGui 1.85 can't drop one), the new one is baked as asked.
			face = currentFace;
			for (int index = 0; index < BAKED_COUNT; ++index)
			{
				baked[index] = nullptr;
				attempted[index] = false;
			}
		}
		for (int index = 0; index < BAKED_COUNT; ++index)
		{
			if (!wanted[index] || attempted[index]) continue;
			attempted[index] = true;
			// Null when GDI has no bytes for the face; the default font then stands in for good.
			baked[index] = ImGuiHud::AcquireFont(face, BAKED_SIZES[index]);
		}
		// Built here rather than left to upstream's PrepareImGuiHud, which skips the rebuild until the game has loaded:
		// the REC badge draws before that, and an unbuilt font can't be drawn with. Same steps as upstream's.
		if (ImGuiHud::ConsumeAtlasDirty())
		{
			ImGui::GetIO().Fonts->Build();
			ImGui_ImplDX9_InvalidateDeviceObjects();   // NewFrame recreates the font texture
		}
	}

	ImFont* Font(float pixelHeight)
	{
		const int bucket = BucketFor(pixelHeight);
		wanted[bucket] = true;
		return baked[bucket] != nullptr ? baked[bucket] : ImGui::GetFont();
	}

	ImVec2 Measure(float pixelHeight, const char* text)
	{
		return Font(pixelHeight)->CalcTextSizeA(pixelHeight, FLT_MAX, 0.0f, text);
	}

	void Text(ImDrawList* list, float pixelHeight, ImVec2 position, ImU32 color, const char* text, bool bold)
	{
		ImFont* font = Font(pixelHeight);
		list->AddText(font, pixelHeight, position, color, text);
		if (bold) list->AddText(font, pixelHeight, ImVec2(position.x + 1.0f, position.y), color, text);
	}

	ImU32 Argb(std::uint32_t argb)
	{
		std::uint32_t alpha = (argb >> 24) & 0xFF;
		if (alpha == 0) alpha = 0xFF;
		return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, alpha);
	}
}
