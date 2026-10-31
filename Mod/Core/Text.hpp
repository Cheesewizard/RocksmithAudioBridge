#pragma once

#include <string>

namespace AudioBridge::Text
{
	// UTF-16 (Windows) to UTF-8, e.g. a driver or device name for the log.
	std::string Utf8(const std::wstring& text);
}
