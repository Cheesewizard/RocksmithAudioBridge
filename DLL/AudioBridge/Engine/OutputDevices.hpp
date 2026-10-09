#pragma once
// Windows playback devices the game mix can safely be switched to at runtime, for the in-game overlay's
// Output page and as the engine-side guard on the live route command (op 21).
//
// RS_ASIO holds the ASIO interface for the whole session. Opening the same hardware's Windows (WASAPI)
// endpoint while RS_ASIO has it can silence the interface, and RS_ASIO input does not recover without a
// game restart. So any endpoint that belongs to an interface RS_ASIO is bound to is
// "protected": listed but never selectable, and refused by the engine even if something asks for it.
//
// Matching ASIO drivers to Windows endpoints: the two are separate namespaces for the same box
// ("M-Audio M-Track Solo and Duo ASIO" vs "Speakers (3- M-Audio M-Track Solo and Duo)"), so a driver matches an
// endpoint on the bare device description (same rule as the desktop bridge's DeviceCore). The match is then
// widened to every endpoint in the same Windows device container, so a box whose playback endpoint is named
// differently from its driver is still caught through its other endpoints. A bound driver that matches no
// endpoint at all (generic wrappers like ASIO4ALL, or unusual names) is reported as unidentified, and the
// overlay then refuses to switch rather than guess.

#include <string>
#include <vector>

namespace Audio::OutputDevices
{
	struct Device
	{
		std::wstring id;
		std::string name;          // UTF-8 friendly name for display
		bool isDefault = false;    // Windows default playback device
		bool isProtected = false;  // belongs to the interface RS_ASIO is bound to; never switch to it
		bool isAsioTwin = false;   // Windows endpoint of the ASIO interface in RS_ASIO.ini, bound or not; the overlay hides it
	};

	struct Snapshot
	{
		std::vector<Device> devices;           // active playback endpoints, default first
		std::vector<std::string> asioDrivers;  // real ASIO drivers RS_ASIO is bound to (UTF-8), empty in Cable mode
		bool asioUnidentified = false;         // a bound driver matched no Windows endpoint: switching is unsafe
		bool ok = false;                       // enumeration succeeded
	};

	// Enumerates on the calling thread (initialises COM itself). A few milliseconds; do not call per frame.
	Snapshot Enumerate();

	// True when the endpoint positively belongs to a bound ASIO interface. Used by the op-21 guard.
	bool IsProtected(const std::wstring& endpointId);
}
