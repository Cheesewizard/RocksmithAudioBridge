#include "stdafx.h"
#include "ToneCatalog.hpp"

#include <ShlObj.h>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace ToneCatalog
{
	namespace
	{
		std::wstring ManifestPath()
		{
			PWSTR appData = nullptr;
			std::wstring path;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) path = std::wstring(appData) + L"\\RocksmithAudioBridge\\Stompbox\\StarterTones\\manifest.json";
			CoTaskMemFree(appData);
			return path;
		}

		Character ParseCharacter(const std::string& text)
		{
			if (text == "clean") return Character::Clean;
			if (text == "crunch") return Character::Crunch;
			if (text == "high-gain") return Character::HighGain;
			if (text == "drive") return Character::Drive;
			if (text == "boost") return Character::Boost;
			return Character::Unknown;
		}

		// "NAM/5150 Block Letter.nam" -> key "NAM/5150 Block Letter" (Stompbox lists files without their extension).
		std::unordered_map<std::string, FileInfo> Load()
		{
			std::unordered_map<std::string, FileInfo> files;
			std::ifstream in(std::filesystem::path(ManifestPath()), std::ios::binary);
			if (!in) return files;   // no starter tones installed: the browser shows names only
			try
			{
				const nlohmann::json manifest = nlohmann::json::parse(in);
				for (const nlohmann::json& entry : manifest.at("files"))
				{
					const std::string path = entry.at("path").get<std::string>();
					const std::string kind = entry.at("kind").get<std::string>();
					if (kind != "amp" && kind != "pedal" && kind != "cabinet") continue;
					const size_t dot = path.find_last_of('.');
					FileInfo info;
					info.isPedal = kind == "pedal";
					info.isBass = entry.value("instrument", std::string()) == "bass";
					info.character = ParseCharacter(entry.value("character", std::string()));
					info.description = entry.value("description", std::string());
					info.author = entry.value("author", std::string());
					info.license = entry.value("license", std::string());
					files[dot == std::string::npos ? path : path.substr(0, dot)] = std::move(info);
				}
			}
			catch (const nlohmann::json::exception& error)
			{
				LOG_ERROR("(TONE DESIGNER) the starter tones' manifest.json could not be read: " << error.what() << std::endl);
				files.clear();
			}
			return files;
		}

		const std::unordered_map<std::string, FileInfo>& Files()
		{
			static const std::unordered_map<std::string, FileInfo> files = Load();
			return files;
		}
	}

	const FileInfo* Find(const std::string& folder, const std::string& name)
	{
		const auto& files = Files();
		const auto found = files.find(folder + "/" + name);
		return found == files.end() ? nullptr : &found->second;
	}

	const char* CharacterLabel(Character character)
	{
		switch (character)
		{
		case Character::Clean: return "Clean";
		case Character::Crunch: return "Crunch";
		case Character::HighGain: return "High gain";
		case Character::Drive: return "Drive";
		case Character::Boost: return "Boost";
		default: return "";
		}
	}
}
