#pragma once

// The picture of a plugin's own window for the overlay's Inspector (Mod/Shared/PluginPicture.hpp). Main.cpp decides
// which window to capture (the plugin's open window, or a hidden editor made only for this) and how often; this
// prints it, scales it down to fit PluginPicture::MAX_WIDTH x MAX_HEIGHT and writes it under the block's sequence
// lock. Main thread only: printing a window waits on its owner thread, which is this one, and the stream thread
// never comes near it.
//
// Capture is PrintWindow with PW_RENDERFULLCONTENT: Windows renders the window through the compositor, so a plugin
// that draws with the GPU (Direct3D, OpenGL, a flip-model swap chain in a child window) comes out too, not only one
// that draws with GDI. A window the compositor has no content for (hidden, or a plugin drawing in a way it cannot
// see) prints black; that is reported as Failed with why, never shown as a black picture.

#include "../../Mod/Shared/PluginPicture.hpp"

#include <Windows.h>
#include <cstdint>
#include <string>

namespace VstHost
{
	class PicturePublisher
	{
	public:
		enum class Result
		{
			Ready,    // a frame was written (or the same frame is already there)
			Blank,    // the window printed black and `quietIfBlank` asked not to report it
			Failed,   // reported as Failed
		};

		~PicturePublisher() { Close(); }

		bool Open();
		void Close();

		// Prints `top` (a top-level window) and keeps the part `content` covers (the editor inside it; may be `top`).
		// `quietIfBlank`: a black print is left unreported, for an editor that has only just opened and may not have
		// drawn yet.
		Result Capture(int slot, HWND top, HWND content, bool quietIfBlank);
		// No picture: None (nothing inspected), NoEditor, or Failed with why.
		void Report(PluginPicture::State state, int slot, const char* why = "");

	private:
		// A 32-bit top-down DIB section selected into its own memory DC, kept between captures and remade only when
		// the size changes (a plugin window is printed four or five times a second).
		struct Bitmap
		{
			HDC dc = nullptr;
			HBITMAP bitmap = nullptr;
			HGDIOBJ previous = nullptr;
			uint32_t* pixels = nullptr;
			int width = 0;
			int height = 0;

			bool Fit(int newWidth, int newHeight);
			void Release();
		};

		Result Fail(int slot, const char* why);
		void Write(PluginPicture::State state, int slot, const char* why, const Bitmap* picture, int sourceWidth, int sourceHeight);

		HANDLE m_mapping = nullptr;
		PluginPicture::Shared* m_shared = nullptr;
		Bitmap m_print;     // the whole top-level window, as printed
		Bitmap m_picture;   // the editor's part of it, scaled to fit
		// What the log last said, so a picture failing four times a second is logged once.
		PluginPicture::State m_loggedState = PluginPicture::State::None;
		int m_loggedSlot = -1;
		std::string m_loggedWhy;
	};
}
