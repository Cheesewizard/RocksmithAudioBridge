#include "RecordingIndicator.hpp"

#include "../AudioBridge/Engine/AudioBridgeEngine.hpp"
#include "../Shell/HudDraw.hpp"

#include "Log.hpp"

#include <algorithm>

namespace AudioBridge::RecordingIndicator
{
	void Draw()
	{
		// Authoritative in-process take state. Audio::SharedOutput::IsRecording() unifies both recording
		// backends (the managed output session AND the passive proxy output-tap used for wet takes), so
		// this is true for every recording the audio bridge can start, including wet proxy takes (the
		// common ASIO path).
		const bool isRecording = Audio::SharedOutput::IsRecording();

		// Log the ON/off transition once, so the debug log shows whether the overlay knew a take was
		// running independently of whether the badge drew.
		static bool loggedRecording = false;
		if (isRecording != loggedRecording)
		{
			LOG_INFO("(REC OVERLAY) recording indicator " << (isRecording ? "ON" : "off") << std::endl);
			loggedRecording = isRecording;
		}

		if (!isRecording) return;

		// The badge sits at the right edge of the presented image. 4.x read the backbuffer's size and widened the game's
		// viewport for this draw, because the game often left a sub-region viewport that clipped D3DX text; ImGui's
		// display size is the backbuffer's, and its renderer sets its own full-screen viewport.
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const int surfaceW = static_cast<int>(display.x);
		const int surfaceH = static_cast<int>(display.y);
		if (surfaceW <= 0 || surfaceH <= 0) return;

		// One-shot geometry log so a take's log shows the size the badge laid out against.
		static bool loggedGeometry = false;
		if (!loggedGeometry)
		{
			LOG_INFO("(REC OVERLAY) display=" << surfaceW << "x" << surfaceH << std::endl);
			loggedGeometry = true;
		}

		// Single line, right-anchored. Shadow first, then the red fill. 4.x's text was U+25CF and "  REC" in bold; the dot
		// isn't in the baked HUD font, so it is a filled circle the size of the glyph.
		const int margin = (std::max)(8, surfaceW / 96);
		const int fontSize = (std::max)(18, surfaceH / 44);
		const int top = (std::max)(8, surfaceH / 54);
		const int right = surfaceW - margin;
		const float size = static_cast<float>(fontSize);
		const char* label = "  REC";
		const float labelWidth = HudDraw::Measure(size, label).x;
		const float dotRadius = size * 0.3f;
		const float left = static_cast<float>(right) - labelWidth - dotRadius * 2.0f;
		ImDrawList* list = ImGui::GetBackgroundDrawList();
		for (const int pass : { 0, 1 })
		{
			const float offset = pass == 0 ? 2.0f : 0.0f;
			const ImU32 color = HudDraw::Argb(pass == 0 ? 0xFF000000 : 0xFFFF2020);
			const ImVec2 at(left + offset, static_cast<float>(top) + offset);
			list->AddCircleFilled(ImVec2(at.x + dotRadius, at.y + size * 0.55f), dotRadius, color, 20);
			HudDraw::Text(list, size, ImVec2(at.x + dotRadius * 2.0f, at.y), color, label, true);
		}
	}
}
