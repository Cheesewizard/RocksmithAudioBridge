#pragma once

#include <Windows.h>
#include <audioclient.h>
#include <string>

#include "AudioControl.hpp"

namespace Audio::SharedOutput
{
	enum class ProxyRouteAction
	{
		None,
		DemoteProxy,
		StopManagedRoute,
		StartManagedRoute,
		ReplaceLostManagedRoute,
	};

	struct Configuration
	{
		bool enabled = false;
		std::wstring inputDeviceId;
		std::wstring outputDeviceId;
	};

	Configuration ReadConfiguration();
	HRESULT CreateClient(const std::wstring& endpointId, IAudioClient** client);
	// Authoritative live recording state used by the in-game overlay. These queries
	// unify the managed output session and passive output-tap backends.
	bool IsRecording();
	// A take is open, including one that failed partway (IsRecording is false for those). The Record toggle must
	// use this, otherwise it would start a new take instead of stopping the failed one.
	bool HasTake();
	HRESULT RecordingError();
	ProxyRouteAction PlanProxyRoute(int proxyMode, bool routeExists, bool routeManaged,
		bool routedEndpointAvailable, bool resolvedOutputAvailable);

	// Host the audio control pipe for the whole process life, independent of whether routing is
	// enabled, so the GUI can always connect to a running game. Idempotent; safe to call once at init.
	void StartControlServer();
	// Run one control request against the live engine directly (same dispatch the pipe uses), for the
	// in-game overlay to drive the audio backend in-process without a pipe round trip. Thread-safe.
	ControlResponse DispatchControl(const ControlRequest& request);
	// Same, but returns false at once instead of waiting while another control op runs. For pollers on the
	// game's render thread (the overlay's status poll), which must never block on a slow op such as op 25.
	bool TryDispatchControl(const ControlRequest& request, ControlResponse& response);
	// Same as DispatchControl but the op writes nothing to RSMods.ini: for the overlay's sliders while they are being
	// dragged, so a drag does not rewrite the ini every frame; the release sends the normal op, which saves.
	ControlResponse DispatchControlPreview(const ControlRequest& request);
	// State of the live alternate-device route (op 21): S_FALSE none, S_OK playing, E_PENDING opening,
	// failure = could not open / lost / its ASIO source stalled. For the overlay's switch-and-verify.
	HRESULT RouteHealth();
	// Reconciles the proxy's current transport with the saved output. In virtual mode this automatically
	// opens a shared Windows route (or stays silently alive when no endpoint exists). A returning ASIO
	// device is never selected here; promotion requires an explicit Apply output command, and input is
	// always paired with output: losing the interface demotes to the cable, and plugging it back in does
	// nothing until the interface is applied as output, which makes it input and output again.
	void RefreshProxyOutput();
}
