#pragma once

#include "../AudioFormat.hpp"
#include <string>

namespace VstHost::Builtin
{
	inline bool IsBuiltinId(const std::wstring& id)
	{
		return id.compare(0, 8, L"builtin:") == 0;
	}

	constexpr int ALGORITHM_VERSION = 1;
}
