#pragma once

// The overlay's side of the plugin picture (Shared/PluginPicture.hpp): the VST host captures the inspected plugin's
// own window a few times a second; this uploads the newest frame to a texture the Inspector draws, so a plugin that
// names no controls for hosts (AmpliTube 5 lists "Param 1".."Param 16") still shows what it looks like, and a click
// opens the real window.
//
// Threads: everything runs on the render thread (OnFrame from ToneDesignerMod's frame callback, Get from the page).

#include "Lib/ImGUI/imgui.h"

#include <d3d9.h>
#include <string>

namespace PluginPictureView
{
	struct View
	{
		ImTextureID texture = nullptr;   // nullptr until a frame of the wanted slot has arrived
		ImVec2 uv1{ 1.0f, 1.0f };        // the picture's corner in the texture (it is uploaded top-left)
		float width = 0.0f;              // the picture's size in pixels
		float height = 0.0f;
		int sourceWidth = 0;             // the plugin window's own size
		int sourceHeight = 0;
		bool isFailed = false;           // the host could not capture it (`why`)
		bool hasNoEditor = false;        // the plugin has no window of its own
		std::string why;
	};

	// The Inspector shows `slot` this frame: keeps the mapping open and the frames coming. A slot not asked for
	// in the last half second is let go.
	void Want(int slot);

	// The newest picture of `slot` (empty while it has none).
	View Get(int slot);

	// Each rendered frame: uploads a new frame when one has arrived for the wanted slot.
	void OnFrame(IDirect3DDevice9* device);
}
