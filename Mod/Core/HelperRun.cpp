#include "HelperRun.hpp"
#include "HelperExe.hpp"

#include "Log.hpp"

#include <vector>

namespace AudioBridge::HelperRun
{
	std::wstring GameFolder()
	{
		wchar_t exe[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring path = exe;
		const size_t slash = path.find_last_of(L"\\/");
		return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
	}

	std::wstring Quote(const std::wstring& text) { return L"\"" + text + L"\""; }

	int Run(const std::wstring& arguments, DWORD timeoutMs)
	{
		const std::filesystem::path helper = HelperExePath();
		const std::wstring exe = helper.wstring();
		if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			LOG_WARNING("(HELPER) RocksmithAudioBridge.Helper.exe is not in the game's RSMods\\AudioBridge folder" << std::endl);
			return -1;
		}
		std::wstring command = Quote(exe) + L" " + arguments;
		std::vector<wchar_t> buffer(command.begin(), command.end());
		buffer.push_back(0);
		STARTUPINFOW startup{ sizeof(startup) };
		startup.dwFlags = STARTF_USESHOWWINDOW;
		startup.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION process{};
		const std::wstring folder = helper.parent_path().wstring();
		if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
			nullptr, folder.c_str(), &startup, &process))
		{
			LOG_WARNING("(HELPER) could not start RocksmithAudioBridge.Helper.exe (error " << GetLastError() << ")" << std::endl);
			return -1;
		}
		CloseHandle(process.hThread);
		int result = -1;
		if (WaitForSingleObject(process.hProcess, timeoutMs) == WAIT_OBJECT_0)
		{
			DWORD code = 0;
			GetExitCodeProcess(process.hProcess, &code);
			result = static_cast<int>(code);
		}
		else
		{
			LOG_WARNING("(HELPER) RocksmithAudioBridge.Helper.exe ran too long; ended" << std::endl);
			TerminateProcess(process.hProcess, 1);
		}
		CloseHandle(process.hProcess);
		return result;
	}
}
