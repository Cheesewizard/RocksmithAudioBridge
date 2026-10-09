#include "stdafx.h"
#include "Extensions.hpp"

namespace Extensions
{
	namespace
	{
		// Function-local so registrations from other files' static initializers never run before it exists.
		std::vector<Callbacks>& Registry()
		{
			static std::vector<Callbacks> registry;
			return registry;
		}
	}

	void Register(const Callbacks& callbacks) { Registry().push_back(callbacks); }
	bool Any() { return !Registry().empty(); }

	std::string Names()
	{
		std::string names;
		for (const Callbacks& c : Registry())
		{
			if (!c.name) continue;
			if (!names.empty()) names += ", ";
			names += c.name;
		}
		return names;
	}

	void AudioHooksInstalled() { for (const Callbacks& c : Registry()) if (c.onAudioHooksInstalled) c.onAudioHooksInstalled(); }
	void Tick() { for (const Callbacks& c : Registry()) if (c.onTick) c.onTick(); }

	bool KeyUp(uint32_t key)
	{
		for (const Callbacks& c : Registry())
			if (c.onKeyUp && c.onKeyUp(key)) return true;
		return false;
	}

	void DrawOverlayDebugPage() { for (const Callbacks& c : Registry()) if (c.drawOverlayDebugPage) c.drawOverlayDebugPage(); }
	void CollectHudLines(std::vector<std::string>& lines) { for (const Callbacks& c : Registry()) if (c.collectHudLines) c.collectHudLines(lines); }
}
