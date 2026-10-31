#pragma once

#include <string>

// What routing and External amp ask of the Tone designer (its VST host), registered by the Tone designer's mod. Not
// registered (the Tone designer mod not loaded, or faulted before OnInitialize): there is no VST host, so its guest is never the amp link's, it adds
// no plugin latency and never loads.
namespace AudioBridge
{
	class IToneDesignerHost
	{
	public:
		virtual ~IToneDesignerHost() = default;

		// The amp link's guest is the Tone designer's VST host.
		virtual bool IsHostGuest(const std::string& guestName) const = 0;
		// The plugin chain's latency in samples.
		virtual int ChainLatencySamples() const = 0;
		// A plugin or a tone is loading.
		virtual bool IsLoading() const = 0;
	};

	// MainThread, from the Tone designer mod's OnInitialize; the host outlives the process's audio.
	void RegisterToneDesignerHost(IToneDesignerHost& host);
	// Any thread. nullptr until registered.
	IToneDesignerHost* ToneDesignerHost();
}
