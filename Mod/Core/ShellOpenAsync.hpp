#pragma once

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <string>
#include <thread>

// ShellExecuteW off the calling thread. The overlay's buttons and the Tone designer's auto-start run on the game's
// render thread, and one ShellExecuteW there can take 400 ms (starting the VST host at the main menu: shell
// COM start-up plus the anti-virus scan of the launched exe), a visible freeze. Nothing that calls this needs the
// result, so it runs on a short-lived thread of its own, with the single-threaded COM the shell expects.
namespace ShellOpenAsync
{
	inline void Open(std::wstring file, std::wstring parameters = {})
	{
		std::thread([file = std::move(file), parameters = std::move(parameters)]
		{
			const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			ShellExecuteW(nullptr, L"open", file.c_str(), parameters.empty() ? nullptr : parameters.c_str(), nullptr, SW_SHOWNORMAL);
			if (SUCCEEDED(com)) CoUninitialize();
		}).detach();
	}
}
