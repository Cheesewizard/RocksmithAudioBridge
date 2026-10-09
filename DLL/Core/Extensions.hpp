#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Extension points for optional code that is compiled into some builds but is not part of this repository
// (a developer's own tools, added by an optional MSBuild import; see DLL.vcxproj). Each piece registers its
// callbacks from a static initializer. A build without extensions registers nothing, so every call below is
// a no-op and costs a vector check.
namespace Extensions
{
	struct Callbacks
	{
		const char* name = nullptr;
		void (*onAudioHooksInstalled)() = nullptr;   // after the input tap is installed, before capture streams attach
		void (*onTick)() = nullptr;                  // every mod-loop tick
		bool (*onKeyUp)(uint32_t key) = nullptr;     // WM_KEYUP in the game window; true = handled, not passed on
		void (*drawOverlayDebugPage)() = nullptr;    // extra cards on the overlay's Debug page (ImGui)
		void (*collectHudLines)(std::vector<std::string>& lines) = nullptr;   // status lines drawn on the game screen
	};

	void Register(const Callbacks& callbacks);
	bool Any();
	std::string Names();   // "a, b" for the build label

	void AudioHooksInstalled();
	void Tick();
	bool KeyUp(uint32_t key);
	void DrawOverlayDebugPage();
	void CollectHudLines(std::vector<std::string>& lines);
}
