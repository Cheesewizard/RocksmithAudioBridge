#include "ToneDesignerLink.hpp"

#include <atomic>

namespace AudioBridge
{
	namespace
	{
		std::atomic<IToneDesignerHost*> registeredHost{ nullptr };
	}

	void RegisterToneDesignerHost(IToneDesignerHost& host)
	{
		registeredHost.store(&host, std::memory_order_release);
	}

	IToneDesignerHost* ToneDesignerHost()
	{
		return registeredHost.load(std::memory_order_acquire);
	}
}
