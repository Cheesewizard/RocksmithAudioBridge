#include "ShellPages.hpp"

#include <algorithm>
#include <mutex>

namespace AudioBridge::Shell
{
	namespace
	{
		std::mutex pagesMutex;
		std::vector<Page> registeredPages;   // kept sorted by order, then label
	}

	void RegisterPage(Page page)
	{
		std::lock_guard<std::mutex> lock(pagesMutex);
		const auto existing = std::find_if(registeredPages.begin(), registeredPages.end(),
			[&](const Page& registered) { return registered.id == page.id; });
		if (existing != registeredPages.end()) *existing = std::move(page);
		else registeredPages.push_back(std::move(page));
		std::stable_sort(registeredPages.begin(), registeredPages.end(), [](const Page& a, const Page& b) {
			return a.order != b.order ? a.order < b.order : a.label < b.label;
		});
	}

	std::vector<Page> Pages()
	{
		std::lock_guard<std::mutex> lock(pagesMutex);
		return registeredPages;
	}
}
