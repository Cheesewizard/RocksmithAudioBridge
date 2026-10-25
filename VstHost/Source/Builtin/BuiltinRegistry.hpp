#pragma once

#include "BuiltinProcessor.hpp"
#include "AssetLibrary.hpp"

namespace VstHost::Builtin
{
	class BuiltinRegistry
	{
	public:
		static std::unique_ptr<BuiltinProcessor> Create(const std::wstring& id, std::shared_ptr<AssetLibrary> library = {});
		static std::vector<std::wstring> List();
	};
}
