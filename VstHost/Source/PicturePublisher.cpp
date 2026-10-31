#include "PicturePublisher.hpp"

#include "HostLog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace VstHost
{
	using PluginPicture::State;

	bool PicturePublisher::Bitmap::Fit(int newWidth, int newHeight)
	{
		if (bitmap && width == newWidth && height == newHeight) return true;
		Release();
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(info.bmiHeader);
		info.bmiHeader.biWidth = newWidth;
		info.bmiHeader.biHeight = -newHeight;   // top-down: row 0 first, as the shared block wants it
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		HDC screen = GetDC(nullptr);
		void* bits = nullptr;
		bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		dc = bitmap ? CreateCompatibleDC(screen) : nullptr;
		ReleaseDC(nullptr, screen);
		if (!bitmap || !dc) { Release(); return false; }
		previous = SelectObject(dc, bitmap);
		pixels = static_cast<uint32_t*>(bits);
		width = newWidth;
		height = newHeight;
		return true;
	}

	void PicturePublisher::Bitmap::Release()
	{
		if (dc && previous) SelectObject(dc, previous);
		if (dc) DeleteDC(dc);
		if (bitmap) DeleteObject(bitmap);
		*this = Bitmap{};
	}

	bool PicturePublisher::Open()
	{
		if (m_shared) return true;
		m_shared = PluginPicture::Open(m_mapping);
		if (!m_shared) { Log("picture: could not open the shared block (error %lu)", GetLastError()); return false; }
		// A host that went down halfway through a frame left the lock odd; the overlay would wait for it forever.
		if (m_shared->frameSeq & 1) InterlockedIncrement(&m_shared->frameSeq);
		Write(State::None, -1, "", nullptr, 0, 0);
		return true;
	}

	void PicturePublisher::Close()
	{
		if (m_shared)
		{
			Write(State::None, -1, "", nullptr, 0, 0);
			UnmapViewOfFile(m_shared);
			m_shared = nullptr;
		}
		if (m_mapping) { CloseHandle(m_mapping); m_mapping = nullptr; }
		m_print.Release();
		m_picture.Release();
	}

	PicturePublisher::Result PicturePublisher::Capture(int slot, HWND top, HWND content, bool quietIfBlank)
	{
		if (!m_shared) return Result::Failed;
		RECT topRect{}, contentRect{};
		if (!IsWindow(top) || !IsWindow(content) || !GetWindowRect(top, &topRect) || !GetWindowRect(content, &contentRect))
			return Fail(slot, "The plugin's window is gone.");
		const int topWidth = topRect.right - topRect.left, topHeight = topRect.bottom - topRect.top;
		const int sourceWidth = contentRect.right - contentRect.left, sourceHeight = contentRect.bottom - contentRect.top;
		const int offsetX = contentRect.left - topRect.left, offsetY = contentRect.top - topRect.top;
		if (topWidth <= 0 || topHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0)
			return Fail(slot, "The plugin's window has no size.");
		if (!m_print.Fit(topWidth, topHeight)) return Fail(slot, "Not enough memory to print the plugin's window.");

		// PrintWindow on a child window does not go through the compositor; the top-level window does, and the
		// editor's part is cut out of it below.
		if (!PrintWindow(top, m_print.dc, PW_RENDERFULLCONTENT))
		{
			char why[96];
			snprintf(why, sizeof(why), "Windows could not print the plugin's window (error %lu).", GetLastError());
			return Fail(slot, why);
		}

		// Fit inside the block, aspect kept, never enlarged.
		const double scale = std::min({ 1.0, static_cast<double>(PluginPicture::MAX_WIDTH) / sourceWidth,
			static_cast<double>(PluginPicture::MAX_HEIGHT) / sourceHeight });
		const int width = std::clamp(static_cast<int>(std::lround(sourceWidth * scale)), 1, PluginPicture::MAX_WIDTH);
		const int height = std::clamp(static_cast<int>(std::lround(sourceHeight * scale)), 1, PluginPicture::MAX_HEIGHT);
		if (!m_picture.Fit(width, height)) return Fail(slot, "Not enough memory for the picture.");
		if (width == sourceWidth && height == sourceHeight)
			BitBlt(m_picture.dc, 0, 0, width, height, m_print.dc, offsetX, offsetY, SRCCOPY);
		else
		{
			// HALFTONE averages every source pixel a target pixel covers, so small text and thin knob markings
			// soften instead of breaking up the way the default (dropping rows and columns) leaves them.
			SetStretchBltMode(m_picture.dc, HALFTONE);
			SetBrushOrgEx(m_picture.dc, 0, 0, nullptr);
			StretchBlt(m_picture.dc, 0, 0, width, height, m_print.dc, offsetX, offsetY, sourceWidth, sourceHeight, SRCCOPY);
		}
		GdiFlush();   // the DIB's memory is read directly next

		// GDI leaves the fourth byte 0; the overlay draws the picture opaque. All black means nothing was printed.
		const size_t count = static_cast<size_t>(width) * height;
		bool drawn = false;
		for (size_t i = 0; i < count; ++i)
		{
			drawn = drawn || (m_picture.pixels[i] & 0x00FFFFFFu) != 0;
			m_picture.pixels[i] |= 0xFF000000u;
		}
		if (!drawn)
		{
			if (quietIfBlank) return Result::Blank;
			return Fail(slot, "The plugin's window printed black: it draws in a way Windows cannot capture.");
		}

		// The same frame as last time (a plugin at rest) is not written again, so the overlay uploads nothing new.
		if (m_shared->state == static_cast<LONG>(State::Ready) && m_shared->slot == slot && m_shared->width == width &&
			m_shared->height == height && m_shared->sourceWidth == sourceWidth && m_shared->sourceHeight == sourceHeight &&
			memcmp(const_cast<const uint32_t*>(m_shared->pixels), m_picture.pixels, count * sizeof(uint32_t)) == 0)
			return Result::Ready;
		Write(State::Ready, slot, "", &m_picture, sourceWidth, sourceHeight);
		return Result::Ready;
	}

	void PicturePublisher::Report(State state, int slot, const char* why)
	{
		if (!m_shared) return;
		if (m_shared->state == static_cast<LONG>(state) && m_shared->slot == slot && strcmp(m_shared->why, why) == 0) return;
		Write(state, slot, why, nullptr, 0, 0);
	}

	PicturePublisher::Result PicturePublisher::Fail(int slot, const char* why)
	{
		Report(State::Failed, slot, why);
		return Result::Failed;
	}

	void PicturePublisher::Write(State state, int slot, const char* why, const Bitmap* picture, int sourceWidth, int sourceHeight)
	{
		InterlockedIncrement(&m_shared->frameSeq);   // odd: being written
		m_shared->state = static_cast<LONG>(state);
		m_shared->slot = slot;
		strncpy_s(m_shared->why, why, _TRUNCATE);
		m_shared->width = picture ? picture->width : 0;
		m_shared->height = picture ? picture->height : 0;
		m_shared->sourceWidth = sourceWidth;
		m_shared->sourceHeight = sourceHeight;
		if (picture) memcpy(m_shared->pixels, picture->pixels, static_cast<size_t>(picture->width) * picture->height * sizeof(uint32_t));
		InterlockedIncrement(&m_shared->frameSeq);   // even: complete

		if (state == m_loggedState && slot == m_loggedSlot && m_loggedWhy == why) return;
		m_loggedState = state;
		m_loggedSlot = slot;
		m_loggedWhy = why;
		switch (state)
		{
		case State::None: Log("picture: none"); break;
		case State::Ready: Log("picture: slot %d, %dx%d from %dx%d", slot + 1, picture->width, picture->height, sourceWidth, sourceHeight); break;
		case State::NoEditor: Log("picture: slot %d has no window of its own", slot + 1); break;
		case State::Failed: Log("picture: slot %d failed: %s", slot + 1, why); break;
		}
	}
}
