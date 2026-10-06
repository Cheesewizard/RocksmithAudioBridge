#pragma once

// One log file next to the exe (the game folder), like the bridge driver's logs.
namespace VstHost
{
	void LogOpen();
	void Log(const char* format, ...);
}
