#include "HelperExe.hpp"

#include <Windows.h>

namespace AudioBridge
{
	std::filesystem::path HelperExePath()
	{
		wchar_t path[MAX_PATH];
		const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
		if (length == 0 || length >= MAX_PATH) return {};
		return std::filesystem::path(std::wstring(path, length)).parent_path()
			/ L"RSMods" / L"AudioBridge" / L"RocksmithAudioBridge.Helper.exe";
	}
}
