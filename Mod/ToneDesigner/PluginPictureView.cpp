#include "PluginPictureView.hpp"

#include "../Shared/PluginPicture.hpp"

#include <cstring>

namespace PluginPictureView
{
	namespace
	{
		HANDLE g_mapping = nullptr;
		PluginPicture::Shared* g_shared = nullptr;
		IDirect3DTexture9* g_texture = nullptr;
		int g_wantedSlot = -1;
		ULONGLONG g_wantedAt = 0;
		LONG g_uploadedSeq = -1;      // the frame in the texture
		int g_uploadedSlot = -1;
		float g_width = 0.0f, g_height = 0.0f;
		int g_sourceWidth = 0, g_sourceHeight = 0;
		constexpr ULONGLONG WANT_MILLISECONDS = 500;

		bool IsWanted() { return g_wantedSlot >= 0 && GetTickCount64() - g_wantedAt < WANT_MILLISECONDS; }
	}

	void Want(int slot)
	{
		g_wantedSlot = slot;
		g_wantedAt = GetTickCount64();
	}

	View Get(int slot)
	{
		View view;
		if (!g_shared) return view;
		const auto state = static_cast<PluginPicture::State>(g_shared->state);
		if (g_shared->slot == slot)
		{
			view.isFailed = state == PluginPicture::State::Failed;
			view.hasNoEditor = state == PluginPicture::State::NoEditor;
			if (view.isFailed)
			{
				char why[sizeof(g_shared->why) + 1] = {};
				memcpy(why, g_shared->why, sizeof(g_shared->why));
				view.why = why;
			}
		}
		if (g_texture && g_uploadedSlot == slot && g_width > 0.0f)
		{
			view.texture = g_texture;
			view.uv1 = ImVec2(g_width / PluginPicture::MAX_WIDTH, g_height / PluginPicture::MAX_HEIGHT);
			view.width = g_width;
			view.height = g_height;
			view.sourceWidth = g_sourceWidth;
			view.sourceHeight = g_sourceHeight;
		}
		return view;
	}

	void OnFrame(IDirect3DDevice9* device)
	{
		if (!IsWanted() || !device) return;
		if (!g_shared)
		{
			g_shared = PluginPicture::Open(g_mapping);
			if (!g_shared) return;
		}
		// A managed-pool texture as large as the biggest picture, made once: it survives device resets, and each
		// frame only rewrites the rows the picture covers.
		if (!g_texture && FAILED(device->CreateTexture(PluginPicture::MAX_WIDTH, PluginPicture::MAX_HEIGHT, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_texture, nullptr)))
		{
			g_texture = nullptr;
			return;
		}
		const LONG seq = g_shared->frameSeq;
		if ((seq & 1) != 0) return;   // the host is writing
		if (static_cast<PluginPicture::State>(g_shared->state) != PluginPicture::State::Ready || g_shared->slot != g_wantedSlot) return;
		if (seq == g_uploadedSeq && g_uploadedSlot == g_wantedSlot) return;
		const int width = static_cast<int>(g_shared->width), height = static_cast<int>(g_shared->height);
		if (width <= 0 || height <= 0 || width > PluginPicture::MAX_WIDTH || height > PluginPicture::MAX_HEIGHT) return;
		D3DLOCKED_RECT locked{};
		const RECT area{ 0, 0, width, height };
		if (FAILED(g_texture->LockRect(0, &locked, &area, 0))) return;
		for (int y = 0; y < height; ++y)
			memcpy(static_cast<unsigned char*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch, &g_shared->pixels[static_cast<size_t>(y) * width], static_cast<size_t>(width) * 4);
		g_texture->UnlockRect(0);
		// The host started another frame during the copy: this one may be torn. Keep it on screen (it is replaced on
		// the next frame) but do not count it as uploaded.
		MemoryBarrier();
		if (g_shared->frameSeq != seq) return;
		g_uploadedSeq = seq;
		g_uploadedSlot = g_wantedSlot;
		g_width = static_cast<float>(width);
		g_height = static_cast<float>(height);
		g_sourceWidth = static_cast<int>(g_shared->sourceWidth);
		g_sourceHeight = static_cast<int>(g_shared->sourceHeight);
	}
}
