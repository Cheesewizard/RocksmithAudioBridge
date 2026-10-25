#include "BuiltinRegistry.hpp"
#include "AirwindowsProcessor.hpp"
#include "NamAmpProcessor.hpp"
#include "CabIrProcessor.hpp"

#include <stdexcept>

namespace VstHost::Builtin
{
	std::unique_ptr<BuiltinProcessor> BuiltinRegistry::Create(const std::wstring& id, std::shared_ptr<AssetLibrary> library)
	{
		if (id == L"builtin:nam" || id == L"builtin:ir")
		{
			if (!library) library = std::make_shared<AssetLibrary>();
			if (id == L"builtin:nam") return std::make_unique<NamAmpProcessor>(std::move(library));
			return std::make_unique<CabIrProcessor>(std::move(library));
		}
		const std::wstring prefix = L"builtin:airwindows/";
		if (id.compare(0, prefix.size(), prefix) == 0)
		{
			const auto name = juce::String(id.substr(prefix.size()).c_str()).toStdString();
			const auto found = AirwinRegistry::nameToIndex.find(name);
			if (found == AirwinRegistry::nameToIndex.end()) throw std::invalid_argument("Unknown built-in Airwindows effect: " + name);
			return std::make_unique<AirwindowsProcessor>(AirwinRegistry::registry.at(found->second));
		}
		throw std::invalid_argument("Unknown built-in processor id: " + juce::String(id.c_str()).toStdString());
	}

	std::vector<std::wstring> BuiltinRegistry::List()
	{
		std::vector<std::wstring> result = { L"builtin:nam", L"builtin:ir" };
		for (const auto& effect : AirwinRegistry::registry) result.push_back(L"builtin:airwindows/" + std::wstring(juce::String(effect.name).toWideCharPointer()));
		return result;
	}
}
