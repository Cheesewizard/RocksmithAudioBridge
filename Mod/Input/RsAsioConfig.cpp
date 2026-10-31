#include "RsAsioConfig.hpp"

#include "Log.hpp"

#include <cstdio>

namespace AudioBridge::Input
{
	namespace
	{
		std::string GameDirectory()
		{
			char path[MAX_PATH];
			const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
			if (length == 0 || length >= MAX_PATH) return "";
			const std::string modulePath(path, length);
			const std::size_t slash = modulePath.find_last_of("\\/");
			return slash == std::string::npos ? "" : modulePath.substr(0, slash + 1);
		}

		bool FileExists(const std::string& path)
		{
			return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
		}
	}

	bool IsRsAsioEnabled()
	{
		const std::string directory = GameDirectory();
		if (!FileExists(directory + "RS_ASIO.dll")) return false;

		CSimpleIniA reader;
		if (reader.LoadFile((directory + "RS_ASIO.ini").c_str()) < 0) return false;
		return reader.GetBoolValue("Config", "EnableAsio", false);
	}

	RsAsioInputs ReadRsAsioInputs()
	{
		CSimpleIniA reader;
		if (reader.LoadFile((GameDirectory() + "RS_ASIO.ini").c_str()) < 0) return {};

		RsAsioInputs inputs;
		const char* inputZeroDriver = reader.GetValue("Asio.Input.0", "Driver", "");
		const char* inputOneDriver = reader.GetValue("Asio.Input.1", "Driver", "");
		const bool hasInputZero = inputZeroDriver && *inputZeroDriver;
		const bool hasInputOne = inputOneDriver && *inputOneDriver;

		if (hasInputZero)
		{
			inputs.configured[0] = true;
			inputs.channels[0] = static_cast<int>(reader.GetLongValue("Asio.Input.0", "Channel", 0));
			inputs.sources[0] = "[Asio.Input.0]";
			if (hasInputOne)
			{
				inputs.configured[1] = true;
				inputs.channels[1] = static_cast<int>(reader.GetLongValue("Asio.Input.1", "Channel", -1));
				inputs.sources[1] = "[Asio.Input.1]";
			}
		}
		else if (hasInputOne)
		{
			inputs.configured[0] = true;
			inputs.channels[0] = static_cast<int>(reader.GetLongValue("Asio.Input.1", "Channel", 0));
			inputs.sources[0] = "[Asio.Input.1], the only configured input section";
			inputs.inferred[0] = true;
		}
		else if (reader.GetBoolValue("Config", "EnableWasapiInputs", false))
		{
			inputs.configured[0] = true;
			inputs.channels[0] = 0;
			inputs.sources[0] = "the Windows Cable input";
		}
		return inputs;
	}

	void ReportUnreadableRsAsioConfiguration()
	{
		if (GetModuleHandleA("RS_ASIO.dll") == nullptr) return;   // Cable mode renames RS_ASIO away: expected

		const std::string directory = GameDirectory();
		const std::string ini = directory + "RS_ASIO.ini";
		std::string reason;
		if (!FileExists(ini))
		{
			reason = FileExists(ini + ".txt")
				? "the file is named RS_ASIO.ini.txt; rename it to RS_ASIO.ini (turn on file name extensions in Explorer to see this)"
				: "RS_ASIO.ini is missing from " + directory;
		}
		else
		{
			unsigned char bom[2]{};
			FILE* file = nullptr;
			if (fopen_s(&file, ini.c_str(), "rb") == 0 && file)
			{
				const std::size_t read = std::fread(bom, 1, sizeof(bom), file);
				std::fclose(file);
				if (read == 2 && ((bom[0] == 0xFF && bom[1] == 0xFE) || (bom[0] == 0xFE && bom[1] == 0xFF)))
					reason = "RS_ASIO.ini is saved as UTF-16 (\"Unicode\"); save it as UTF-8 or ANSI";
			}
			CSimpleIniA reader;
			if (reason.empty() && reader.LoadFile(ini.c_str()) < 0)
				reason = "RS_ASIO.ini could not be read";
			else if (reason.empty() && reader.GetSection("Config") == nullptr)
				reason = "RS_ASIO.ini has no [Config] section; its first line must be [Config], above EnableWasapiOutputs, EnableWasapiInputs and EnableAsio";
			else if (reason.empty())
				reason = "RS_ASIO.ini [Config] does not set EnableAsio=1 (Cable mode is chosen in the Audio Bridge setup, which turns RS_ASIO off)";
		}
		LOG_ERROR("[InputTap] RS_ASIO.dll is loaded but its settings do not enable ASIO: " << reason
			<< ". The game will see no ASIO devices and the Cable input cannot install over RS_ASIO." << std::endl);
	}
}
