#pragma once

// Stands in for the game DLL's precompiled header when the proxy compiles a shared game source
// (../Audio/DelayLinePitchShifter.cpp, for the external amp link's Drop Pedal). Only this
// file's directory is added to that one source's include path, so nothing else in the proxy sees it.

#include <cmath>
#include <cstdint>
#include <string>
