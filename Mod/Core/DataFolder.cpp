#include "DataFolder.hpp"

#include <Windows.h>
#include <ShlObj.h>

namespace AudioBridge
{
	std::wstring VstHostDataFolder()
	{
		static const std::wstring folder = []
		{
			PWSTR appData = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData))) return std::wstring();
			std::wstring path = std::wstring(appData) + L"\\RocksmithAudioBridge\\VstHost";
			CoTaskMemFree(appData);
			return path;
		}();
		return folder;
	}
}
