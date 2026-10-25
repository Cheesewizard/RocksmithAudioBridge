#pragma once

#include "VstHostLink.hpp"

#include <atomic>
#include <cstdint>
#include <utility>
#include <functional>
#include <mutex>
#include <stdexcept>

namespace VstHostLink
{
	// The worker owns enumeration; the render thread only requests work and takes a completed result.
	class PluginCatalog final
	{
	public:
		explicit PluginCatalog(std::function<std::vector<Plugin>()> scan) : scan(std::move(scan))
		{
			if (!this->scan) throw std::invalid_argument("Plugin catalog requires a scanner.");
		}

		bool WasRequested() const { return requested.load() != 0; }
		bool IsPending() const { return requested.load() != completed.load(); }

		void Request()
		{
			requested.fetch_add(1);
		}

		// Called by the existing background worker, never by the render thread.
		bool RunPending(std::string& failure)
		{
			const auto revision = requested.load();
			if (revision == attempted) return false;
			attempted = revision;
			std::vector<Plugin> fresh;
			failure.clear();
			try { fresh = scan(); }
			catch (const std::exception& exception) { failure = exception.what(); }
			{
				std::lock_guard<std::mutex> guard(resultLock);
				result = std::move(fresh);
				error = failure;
				resultRevision = revision;
			}
			completed.store(revision);
			return true;
		}

		// Take once, at the start of a frame, so references remain stable for the entire draw.
		bool TryTake(std::vector<Plugin>& plugins, std::string& failure, uint32_t& revision)
		{
			std::unique_lock<std::mutex> guard(resultLock, std::try_to_lock);
			if (!guard.owns_lock() || resultRevision == revision) return false;
			plugins.swap(result);
			failure = error;
			revision = resultRevision;
			return true;
		}

	private:
		std::function<std::vector<Plugin>()> scan;
		std::atomic<uint32_t> requested{ 0 };
		std::atomic<uint32_t> completed{ 0 };
		uint32_t attempted = 0;
		std::mutex resultLock;
		std::vector<Plugin> result;
		std::string error;
		uint32_t resultRevision = 0;
	};
}
