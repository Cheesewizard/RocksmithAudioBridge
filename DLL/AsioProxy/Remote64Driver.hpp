#pragma once

// The bridge's stand-in for a 64-bit-only ASIO driver (see Remote64.hpp). It is an IAsioDriver like any real driver,
// so LoadReal and SplitInput use it in place of one, and everything above them (routes, split input, Tone designer,
// External amp) works unchanged.

#include "AsioInterface.h"
#include <string>

namespace Remote64
{
	using LogFn = void(*)(const char* format, ...);

	// Where `name` is registered as a 64-bit ASIO driver: its DLL and CLSID from the 64-bit registry view.
	bool Resolve(const std::wstring& name, std::wstring& dllPath, CLSID& clsid);

	// True when `name` is registered only as a 64-bit ASIO driver (no 32-bit one the game could load).
	bool IsOnly64(const std::wstring& name);

	// When `driver` is a stand-in whose host is gone (a call that hung past its timeout, or the host process ended):
	// true, with `why` in the bind-failure wording ("its driver did not answer for 10 s while opening its buffers, ...").
	// Every call to a dead stand-in returns -999, which would otherwise read as the interface refusing.
	bool HostFailure(IAsioDriver* driver, std::string& why);

	// Starts the 64-bit host for this driver and returns the stand-in (refcount 1), or nullptr with `failure` set.
	IAsioDriver* Create(const std::wstring& name, const std::wstring& dllPath, const CLSID& clsid, LogFn log, std::string& failure);
}
