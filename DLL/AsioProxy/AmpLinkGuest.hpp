#pragma once

// Guest side of the external amp link (see AmpLink.hpp): the driver any process other than Rocksmith gets
// when it loads "Rocksmith Audio Bridge ASIO" (AmpliTube 5 loads the 64-bit build). It never loads the real
// ASIO driver; it runs the amp sim on the game's clock through the shared block, and on its own silent clock
// while the game is not running.

#include "AsioInterface.h"

namespace AmpLink
{
	// True in every process that must not open the audio interface itself: anything that is not Rocksmith,
	// and always in the 64-bit build (Rocksmith is 32-bit).
	bool IsGuestProcess();

	// A new guest driver instance (refcount 1). `clsid` is the driver CLSID, which ASIO uses as the IID.
	IAsioDriver* CreateGuestDriver(const CLSID& clsid);
}
