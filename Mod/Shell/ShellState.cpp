#include "ShellState.hpp"

#include <atomic>
#include <mutex>
#include <utility>

namespace AudioBridge::Shell
{
	namespace
	{
		std::atomic<bool> overlayOpen{ false };
		std::atomic<unsigned> overlayKeyCode{ 0 };
		std::mutex requestedPageMutex;
		std::string requestedPage;
	}

	bool IsOverlayOpen() { return overlayOpen.load(std::memory_order_acquire); }
	unsigned OverlayKeyCode() { return overlayKeyCode.load(std::memory_order_acquire); }
	void VoteMouseCursor(bool wanted)
	{
		// Render thread only.
		static int votingFrame = -1;
		static bool anyWanted = false;
		const int frame = ImGui::GetFrameCount();
		if (frame != votingFrame)
		{
			votingFrame = frame;
			anyWanted = false;
		}
		anyWanted = anyWanted || wanted;
		ImGui::GetIO().MouseDrawCursor = anyWanted;
	}

	void OpenOverlayAt(const char* pageId)
	{
		{
			std::lock_guard<std::mutex> lock(requestedPageMutex);
			requestedPage = pageId ? pageId : "";
		}
		SetOverlayOpen(true);
	}

	std::string TakeRequestedPage()
	{
		std::lock_guard<std::mutex> lock(requestedPageMutex);
		return std::exchange(requestedPage, {});
	}

	void SetOverlayOpen(bool open) { overlayOpen.store(open, std::memory_order_release); }
	void SetOverlayKeyCode(unsigned code) { overlayKeyCode.store(code, std::memory_order_release); }
}
