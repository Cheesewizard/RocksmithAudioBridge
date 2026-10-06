#include "stdafx.h"
#include "OutputDevices.hpp"
#include "OutputTap.hpp"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <filesystem>
#include <set>

namespace Audio::OutputDevices
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		// Windows puts every built-in device in this one container, so it says nothing about hardware identity
		// and must not be used to widen a match (it would protect the laptop speakers along with everything else).
		const GUID kMachineContainer = { 0x00000000, 0x0000, 0x0000, { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
		// Our proxy's ASIO name, plus its former name (RS_ASIO.ini files may still use it).
		bool IsProxyDriver(const std::wstring& name)
		{
			return _wcsicmp(name.c_str(), L"Rocksmith Audio Bridge ASIO") == 0 || _wcsicmp(name.c_str(), L"Rocksmith Audio Bridge") == 0;
		}

		struct Endpoint
		{
			std::wstring id, friendlyName, interfaceName;
			GUID container{};
			bool hasContainer = false;
			bool render = false;
		};

		std::string Utf8(const std::wstring& text)
		{
			if (text.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
			std::string out(size > 0 ? size - 1 : 0, '\0');
			if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
			return out;
		}

		std::wstring Lower(std::wstring text)
		{
			for (wchar_t& ch : text) ch = static_cast<wchar_t>(towlower(ch));
			return text;
		}

		std::wstring Trim(const std::wstring& text)
		{
			const size_t first = text.find_first_not_of(L" \t");
			if (first == std::wstring::npos) return {};
			return text.substr(first, text.find_last_not_of(L" \t") - first + 1);
		}

		// The bare hardware description, same rule as the desktop bridge (AudioRoutingPanel.DeviceCore): the text
		// inside the last parentheses of a Windows name with any "N- " port prefix removed, or an ASIO driver name
		// without a trailing " ASIO".
		std::wstring DeviceCore(std::wstring name)
		{
			const size_t open = name.rfind(L'('), close = name.rfind(L')');
			if (open != std::wstring::npos && close != std::wstring::npos && close > open) name = name.substr(open + 1, close - open - 1);
			const size_t dash = name.find(L"- ");
			if (dash != std::wstring::npos && dash <= 3) name = name.substr(dash + 2);
			name = Trim(name);
			if (name.size() > 5 && Lower(name.substr(name.size() - 5)) == L" asio") name = name.substr(0, name.size() - 5);
			return Lower(Trim(name));
		}

		bool CoresMatch(const std::wstring& a, const std::wstring& b)
		{
			if (a.size() < 3 || b.size() < 3) return false;
			return a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos;
		}

		std::wstring ReadString(IPropertyStore* store, const PROPERTYKEY& key)
		{
			PROPVARIANT value{};
			std::wstring text;
			if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) text = value.pwszVal;
			PropVariantClear(&value);
			return text;
		}

		std::vector<Endpoint> ReadEndpoints(IMMDeviceEnumerator* enumerator)
		{
			std::vector<Endpoint> endpoints;
			ComPtr<IMMDeviceCollection> collection;
			if (FAILED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &collection))) return endpoints;
			UINT count = 0;
			collection->GetCount(&count);
			for (UINT index = 0; index < count; ++index) {
				ComPtr<IMMDevice> device;
				if (FAILED(collection->Item(index, &device))) continue;
				Endpoint endpoint;
				LPWSTR id = nullptr;
				if (FAILED(device->GetId(&id)) || !id) continue;
				endpoint.id = id;
				CoTaskMemFree(id);
				ComPtr<IMMEndpoint> direction;
				EDataFlow flow = eCapture;
				if (SUCCEEDED(device.As(&direction))) direction->GetDataFlow(&flow);
				endpoint.render = flow == eRender;
				ComPtr<IPropertyStore> store;
				if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
					endpoint.friendlyName = ReadString(store.Get(), PKEY_Device_FriendlyName);
					endpoint.interfaceName = ReadString(store.Get(), PKEY_DeviceInterface_FriendlyName);
					PROPVARIANT value{};
					if (SUCCEEDED(store->GetValue(PKEY_Device_ContainerId, &value)) && value.vt == VT_CLSID && value.puuid
						&& *value.puuid != GUID_NULL && *value.puuid != kMachineContainer) {
						endpoint.container = *value.puuid;
						endpoint.hasContainer = true;
					}
					PropVariantClear(&value);
				}
				endpoints.push_back(std::move(endpoint));
			}
			return endpoints;
		}

		std::wstring ReadProxyTarget()
		{
			HKEY key{};
			std::wstring target;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, KEY_READ | KEY_WOW64_32KEY, &key) == ERROR_SUCCESS) {
				wchar_t value[256]{}; DWORD size = sizeof(value); DWORD type = 0;
				if (RegQueryValueExW(key, L"Target", nullptr, &type, reinterpret_cast<LPBYTE>(value), &size) == ERROR_SUCCESS && type == REG_SZ)
					target = value;
				RegCloseKey(key);
			}
			return target;
		}

		// The real ASIO drivers RS_ASIO is holding right now. Read from RS_ASIO.ini (never written by RSMods), with
		// our proxy resolved to the hardware it wraps. The proxy only holds real hardware while it runs on the real
		// ASIO clock (mode 2); on its virtual clock (interface absent at boot) the hardware is free. Empty when
		// RS_ASIO is not loaded, i.e. Cable mode.
		std::vector<std::wstring> BoundAsioDrivers(bool requireBound = true)
		{
			std::vector<std::wstring> drivers;
			if (!GetModuleHandleW(L"RS_ASIO.dll")) return drivers;
			wchar_t executable[MAX_PATH]{};
			GetModuleFileNameW(nullptr, executable, MAX_PATH);
			const auto ini = std::filesystem::path(executable).parent_path() / L"RS_ASIO.ini";
			const wchar_t* sections[] = { L"Asio.Output", L"Asio.Input.0", L"Asio.Input.1", L"Asio.Input.2", L"Asio.Input.3" };
			std::set<std::wstring> seen;
			for (const wchar_t* section : sections) {
				wchar_t value[256]{};
				GetPrivateProfileStringW(section, L"Driver", L"", value, 256, ini.c_str());
				std::wstring driver = Trim(value);
				if (driver.empty()) continue;
				if (IsProxyDriver(driver)) {
					if (requireBound && OutputTap::ProxyOutputMode() != 2) continue;
					driver = Trim(ReadProxyTarget());
					if (driver.empty()) continue;
				}
				if (seen.insert(Lower(driver)).second) drivers.push_back(driver);
			}
			return drivers;
		}

		// Marks protected endpoints. Returns false when some bound driver matched nothing (unidentified).
		bool Protect(const std::vector<std::wstring>& drivers, const std::vector<Endpoint>& endpoints, std::set<std::wstring>& protectedIds)
		{
			bool allIdentified = true;
			std::vector<GUID> containers;
			for (const std::wstring& driver : drivers) {
				const std::wstring core = DeviceCore(driver);
				bool matched = false;
				for (const Endpoint& endpoint : endpoints) {
					if (!CoresMatch(core, DeviceCore(endpoint.friendlyName)) && !CoresMatch(core, Lower(Trim(endpoint.interfaceName)))) continue;
					matched = true;
					protectedIds.insert(endpoint.id);
					if (endpoint.hasContainer) containers.push_back(endpoint.container);
				}
				if (!matched) allIdentified = false;
			}
			// Widen to the whole physical box: every endpoint sharing a matched endpoint's container.
			for (const Endpoint& endpoint : endpoints)
				for (const GUID& container : containers)
					if (endpoint.hasContainer && endpoint.container == container) protectedIds.insert(endpoint.id);
			return allIdentified;
		}

		struct ComScope
		{
			bool uninitialize = false;
			bool ok = false;
			ComScope()
			{
				const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				uninitialize = SUCCEEDED(result);
				ok = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
			}
			~ComScope() { if (uninitialize) CoUninitialize(); }
		};
	}

	Snapshot Enumerate()
	{
		Snapshot snapshot;
		ComScope com;
		if (!com.ok) return snapshot;
		ComPtr<IMMDeviceEnumerator> enumerator;
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return snapshot;

		const std::vector<Endpoint> endpoints = ReadEndpoints(enumerator.Get());
		const std::vector<std::wstring> drivers = BoundAsioDrivers();
		std::set<std::wstring> protectedIds;
		snapshot.asioUnidentified = !Protect(drivers, endpoints, protectedIds);
		for (const std::wstring& driver : drivers) snapshot.asioDrivers.push_back(Utf8(driver));
		// The same hardware's Windows endpoints, bound or not: the overlay lists the interface once, as its ASIO entry.
		std::set<std::wstring> twinIds;
		Protect(BoundAsioDrivers(false), endpoints, twinIds);

		std::wstring defaultId;
		ComPtr<IMMDevice> defaultDevice;
		if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice))) {
			LPWSTR id = nullptr;
			if (SUCCEEDED(defaultDevice->GetId(&id)) && id) { defaultId = id; CoTaskMemFree(id); }
		}
		for (const Endpoint& endpoint : endpoints) {
			if (!endpoint.render) continue;
			Device device;
			device.id = endpoint.id;
			device.name = Utf8(endpoint.friendlyName.empty() ? endpoint.id : endpoint.friendlyName);
			device.isDefault = endpoint.id == defaultId;
			device.isProtected = protectedIds.count(endpoint.id) != 0;
			device.isAsioTwin = twinIds.count(endpoint.id) != 0;
			snapshot.devices.push_back(std::move(device));
		}
		std::stable_partition(snapshot.devices.begin(), snapshot.devices.end(), [](const Device& d) { return d.isDefault; });
		snapshot.ok = true;

		// Log the inputs to the protection decision whenever they change.
		{
			std::ostringstream line;
			line << "rsasio=" << (GetModuleHandleW(L"RS_ASIO.dll") ? 1 : 0) << " proxyMode=" << OutputTap::ProxyOutputMode()
				<< " drivers=" << snapshot.asioDrivers.size();
			for (const std::string& driver : snapshot.asioDrivers) line << " [" << driver << "]";
			line << " endpoints=" << endpoints.size() << " protected=" << protectedIds.size() << " unidentified=" << snapshot.asioUnidentified;
			static std::mutex logMutex;
			static std::string lastLine;
			std::lock_guard<std::mutex> guard(logMutex);
			if (line.str() != lastLine) { lastLine = line.str(); LOG_INFO("(OUTPUT DEVICES) " << lastLine << std::endl); }
		}
		return snapshot;
	}

	bool IsProtected(const std::wstring& endpointId)
	{
		const std::vector<std::wstring> drivers = BoundAsioDrivers();
		if (drivers.empty()) return false;
		ComScope com;
		if (!com.ok) return false;
		ComPtr<IMMDeviceEnumerator> enumerator;
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return false;
		std::set<std::wstring> protectedIds;
		Protect(drivers, ReadEndpoints(enumerator.Get()), protectedIds);
		return protectedIds.count(endpointId) != 0;
	}
}
