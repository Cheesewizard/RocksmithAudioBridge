#pragma once

#include <Windows.h>
#include <Unknwn.h>

namespace AudioBridge::Input
{
	// Implemented by the Audio Bridge's own capture clients (the persistent input devices), and queried from the game's
	// capture client when the input tap binds to it. A packet that isn't physical is a placeholder the bridge hands
	// out while the real device is away; the generation changes whenever the device behind the client changes.
	struct __declspec(uuid("923E6216-6077-4EC6-8F46-60AE05237BE1")) ICaptureState : IUnknown
	{
		virtual BOOL STDMETHODCALLTYPE IsPhysicalPacket() = 0;
		virtual UINT64 STDMETHODCALLTYPE GetGeneration() = 0;
	};
}
