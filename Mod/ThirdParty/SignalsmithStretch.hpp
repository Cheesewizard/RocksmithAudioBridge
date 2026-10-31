#pragma once

// Signalsmith Stretch (MIT, SignalsmithStretch/LICENSE.txt), vendored unmodified. RSMods builds at /W4 /WX, and the
// library's own float/double conversions and shadowed locals would fail that build, so its warnings are silenced
// here, for the library only.
#pragma warning(push)
#pragma warning(disable: 4127 4244 4305 4456 4458)
#include "SignalsmithStretch/signalsmith-stretch.h"
#pragma warning(pop)
