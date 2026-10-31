#include "MixerPage.hpp"

#include "PageCommon.hpp"
#include "../ShellPages.hpp"

using namespace Overlay;

namespace AudioBridge::Pages
{
	namespace
	{
		// Mixer buses in control-channel order (op 7 + index), matching ControlResponse::volumes[].
		const char* const CHANNELS[7] = { "Song", "Player 1", "Master", "Player 2", "Mic", "Voice", "Effects" };
		// Display order, same grouping as the desktop mixer: Master | Player 1, Player 2 | Song, Effects, Voice, Mic.
		const int CHANNEL_ORDER[7] = { 2, 1, 3, 0, 6, 5, 4 };

		void Draw()
		{
			const LiveState& live = Poll();
			PageHeader("Mixer", "What you hear. Recordings of the game mix and Note by Note are not affected.");
			static float volumes[7] = {};
			static bool seeded = false;
			if (!seeded)
			{
				for (int i = 0; i < 7; ++i) volumes[i] = live.status.volumes[i];
				seeded = true;
			}

			BeginCard();
			const float width = RowWidth();
			const float gap = 8.0f, groupGap = 22.0f;   // wider gaps after Master and after the players
			const float strip = (width - gap * 4.0f - groupGap * 2.0f) / 7.0f;
			const float height = 240.0f;
			for (int slot = 0; slot < 7; ++slot)
			{
				const int channel = CHANNEL_ORDER[slot];
				char id[8];
				sprintf_s(id, "ch%d", channel);
				if (FaderStrip(id, CHANNELS[channel], &volumes[channel], ImVec2(strip, height)))
					SendInt(7 + channel, static_cast<int>(volumes[channel] + 0.5f));
				Hint(channel == 2 ? "Master playback volume over everything. Drag, or scroll for 1% steps."
					: "Playback volume for this bus. Drag, or scroll for 1% steps.");
				if (slot == 6) break;
				const bool groupEnd = slot == 0 || slot == 2;
				ImGui::SameLine(0.0f, groupEnd ? groupGap : gap);
				if (groupEnd)
				{
					// Thin divider centred in the group gap.
					const ImVec2 at = ImGui::GetCursorScreenPos();
					ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x - groupGap * 0.5f, at.y + 6.0f),
						ImVec2(at.x - groupGap * 0.5f, at.y + height - 6.0f), Color::CardEdge, 1.0f);
				}
			}
			EndCard();
			Caption("Mixer levels last for this session.");
		}
	}

	void RegisterMixerPage()
	{
		Shell::RegisterPage({ "mixer", "Audio", Glyph::Mixer, "Mixer", 50, Draw, {} });
	}
}
