#include "stdafx.h"
#include "AsioProxyRegistration.hpp"
#include "../Log.hpp"

#include <windows.h>
#include <filesystem>
#include <string>

namespace
{
	// Must match kAsioName in DLL/AsioProxy/AsioProxyDriver.cpp and AsioProxySetup.ProxyName in the GUI.
	constexpr const wchar_t* kAsioName = L"Rocksmith Audio Bridge ASIO";
	constexpr const wchar_t* kProxyFile = L"RocksmithAudioBridgeAsio.dll";

	std::wstring ReadString(HKEY root, const std::wstring& subKey, const wchar_t* value)
	{
		wchar_t buffer[1024]{};
		DWORD size = sizeof(buffer);
		if (RegGetValueW(root, subKey.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
			return std::wstring();
		return buffer;
	}

	bool SameFile(const std::wstring& a, const std::filesystem::path& b)
	{
		if (a.empty()) return false;
		std::error_code error;
		if (!std::filesystem::is_regular_file(a, error)) return false;
		return std::filesystem::equivalent(a, b, error) && !error;
	}

	std::string Narrow(const std::wstring& text)
	{
		if (text.empty()) return std::string();
		const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		std::string result(length, '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
		return result;
	}
}

void AsioProxyRegistration::Heal()
{
	// Only a driver already in the ASIO list can be healed; adding it there needs HKLM (the GUI's Install).
	const std::wstring clsid = ReadString(HKEY_LOCAL_MACHINE, std::wstring(L"Software\\ASIO\\") + kAsioName, L"CLSID");
	if (clsid.empty()) return;

	wchar_t executable[MAX_PATH]{};
	GetModuleFileNameW(nullptr, executable, MAX_PATH);
	const std::filesystem::path proxy = std::filesystem::path(executable).parent_path() / kProxyFile;
	std::error_code error;
	if (!std::filesystem::is_regular_file(proxy, error))
	{
		LOG_ERROR("(ASIO PROXY) " << Narrow(kProxyFile) << " is missing from the game folder; RS_ASIO cannot load the bridge driver. Reinstall Rocksmith Audio Bridge." << std::endl);
		return;
	}

	// Resolve exactly as RS_ASIO does (this is the same 32-bit process, so the same registry view).
	const std::wstring inprocKey = L"CLSID\\" + clsid + L"\\InprocServer32";
	const std::wstring resolved = ReadString(HKEY_CLASSES_ROOT, inprocKey, nullptr);
	if (SameFile(resolved, proxy)) return;

	LOG_ERROR("(ASIO PROXY) Driver entry resolves to '" << Narrow(resolved.empty() ? L"(nothing)" : resolved)
		<< "', not the bridge driver; RS_ASIO would find no audio device. Repairing for this user." << std::endl);
	const std::wstring userKey = L"Software\\Classes\\CLSID\\" + clsid + L"\\InprocServer32";
	const std::wstring path = proxy.wstring();
	HKEY key = nullptr;
	LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, userKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
	if (status == ERROR_SUCCESS)
	{
		status = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path.c_str()), static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
		if (status == ERROR_SUCCESS)
		{
			static const wchar_t apartment[] = L"Apartment";
			status = RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(apartment), sizeof(apartment));
		}
		RegCloseKey(key);
	}
	const std::wstring after = ReadString(HKEY_CLASSES_ROOT, inprocKey, nullptr);
	if (status == ERROR_SUCCESS && SameFile(after, proxy))
		LOG_INFO("(ASIO PROXY) Repaired: driver entry now resolves to " << Narrow(after) << std::endl);
	else
		LOG_ERROR("(ASIO PROXY) Repair failed (status " << status << ", resolves to '" << Narrow(after) << "'). Use Repair on the RSMods Rocksmith Audio Bridge tab." << std::endl);
}
