#include "Text.hpp"

#include <Windows.h>

namespace AudioBridge::Text
{
	std::string Utf8(const std::wstring& text)
	{
		if (text.empty()) return {};
		const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		if (size <= 0) return {};
		std::string out(static_cast<size_t>(size), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
		return out;
	}
}
