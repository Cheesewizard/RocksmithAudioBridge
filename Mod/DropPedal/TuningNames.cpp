#include "TuningNames.hpp"

#include "DropPedalState.hpp"

#include "Lib/Json/json.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <mutex>

namespace AudioBridge::DropPedal::TuningNames
{
	namespace
	{
		std::once_flag loadOnce;
		nlohmann::json definitions;
		bool loaded = false;

		void Load()
		{
			wchar_t executable[MAX_PATH]{};
			GetModuleFileNameW(nullptr, executable, MAX_PATH);
			const auto path = std::filesystem::path(executable).parent_path() / L"RSMods" / L"CustomMods" / L"tuning.database.json";
			std::error_code error;
			if (!std::filesystem::exists(path, error)) return;
			std::ifstream file(path);
			const nlohmann::json parsed = nlohmann::json::parse(file, nullptr, false);
			if (parsed.is_discarded()) return;
			const auto statics = parsed.find("Static");
			if (statics == parsed.end() || !statics->is_object()) return;
			const auto tunings = statics->find("TuningDefinitions");
			if (tunings == statics->end() || !tunings->is_object()) return;
			definitions = *tunings;
			loaded = true;
		}
	}

	// The display name comes from the entry's UIName, which carries a "$[id]" prefix ahead of the text; the key is the
	// space-less fallback.
	bool TryNameForOffsets(const int (&offsets)[6], std::string& name)
	{
		std::call_once(loadOnce, Load);
		if (!loaded) return false;

		for (const auto& tuning : definitions.items())
		{
			if (!tuning.value().is_object()) continue;
			const auto strings = tuning.value().find("Strings");
			if (strings == tuning.value().end() || !strings->is_object()) continue;

			bool matches = true;
			for (int string = 0; string < 6 && matches; string++)
			{
				const auto entry = strings->find("string" + std::to_string(string));
				matches = entry != strings->end() && entry->is_number_integer() && entry->get<int>() == offsets[string];
			}
			if (!matches) continue;

			const auto uiName = tuning.value().find("UIName");
			if (uiName != tuning.value().end() && uiName->is_string())
			{
				std::string displayName = uiName->get<std::string>();
				const size_t prefixEnd = displayName.find(']');
				if (displayName.rfind("$[", 0) == 0 && prefixEnd != std::string::npos) displayName = displayName.substr(prefixEnd + 1);
				if (!displayName.empty())
				{
					name = displayName;
					return true;
				}
			}
			name = tuning.key();
			return true;
		}
		return false;
	}

	std::string NameShape(Shape shape, const int (&offsets)[6], int reference)
	{
		if (shape == Shape::Uniform) return DropPedalState::GetAbsoluteTuningName(reference);
		std::string name;
		if (TryNameForOffsets(offsets, name)) return name;
		if (shape == Shape::Drop) return "Drop " + DropPedalState::GetAbsoluteTuningName(reference - 2);
		return DropPedalState::GetAbsoluteTuningName(reference) + " custom";
	}
}
