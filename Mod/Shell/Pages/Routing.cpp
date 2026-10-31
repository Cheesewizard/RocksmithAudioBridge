#include "Routing.hpp"

#include "PageCommon.hpp"
#include "../../AudioBridge/Engine/AudioBridgeEngine.hpp"
#include "../../AudioBridge/Engine/OutputTap.hpp"

#include "Log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <mutex>
#include <set>
#include <thread>

using namespace Overlay;

namespace AudioBridge::Pages::Routing
{
	const char* const ROLE_NAMES[4] = { "Output", "Player 1", "Player 2", "Mic" };

	namespace
	{
		// The drivers the bridge would run in the 64-bit host (lower case), by the same rule as its resolver
		// (AsioProxyDriver ResolveAsioDriver): 64-bit only. Every other one is loaded in the game as its 32-bit build.
		std::set<std::wstring> host64Drivers;

		std::atomic<int> interfaceState{ 0 };
		std::mutex interfaceMessageMutex;
		std::string outputSwitchMessage;   // SwitchInterface, shown on Switch output
		std::string inputSwitchMessage;    // SwitchInputInterface, shown on the Input page's source rows

		// 0 idle, 1 applying, 2 applied, 3 saved for next launch, 4 failed.
		std::atomic<int> routeState{ 0 };

		// Every ASIO driver installed in one registry view, except the bridge itself.
		std::vector<std::wstring> AsioDriversIn(REGSAM view)
		{
			std::vector<std::wstring> names;
			HKEY asio{};
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | view, &asio) != ERROR_SUCCESS) return names;
			wchar_t name[256];
			for (DWORD index = 0;; ++index)
			{
				DWORD length = 256;
				if (RegEnumKeyExW(asio, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
				if (_wcsicmp(name, L"Rocksmith Audio Bridge ASIO") != 0 && _wcsicmp(name, L"Rocksmith Audio Bridge") != 0) names.push_back(name);
			}
			RegCloseKey(asio);
			return names;
		}

		std::wstring Lower(std::wstring text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
			return text;
		}

		void SaveInterfaceTarget(const std::wstring& driver)
		{
			HKEY key{};
			if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_32KEY, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
			RegSetValueExW(key, L"Target", 0, REG_SZ, reinterpret_cast<const BYTE*>(driver.c_str()), static_cast<DWORD>((driver.size() + 1) * sizeof(wchar_t)));
			const DWORD one = 1;
			RegSetValueExW(key, L"PreferReal", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
			RegCloseKey(key);
		}
	}

	std::vector<std::wstring> InstalledAsioDrivers(bool with64)
	{
		static std::vector<std::wstring> cached32, cachedAll;
		static ULONGLONG readAt = 0;
		if (readAt == 0 || GetTickCount64() - readAt >= 3000)
		{
			readAt = GetTickCount64();
			cached32 = AsioDriversIn(KEY_WOW64_32KEY);
			cachedAll = cached32;
			host64Drivers.clear();
			for (const std::wstring& name : AsioDriversIn(KEY_WOW64_64KEY))
			{
				const bool has32 = std::any_of(cached32.begin(), cached32.end(), [&](const std::wstring& n) { return _wcsicmp(n.c_str(), name.c_str()) == 0; });
				if (has32) continue;
				cachedAll.push_back(name);
				host64Drivers.insert(Lower(name));
			}
			const auto byName = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; };
			std::sort(cached32.begin(), cached32.end(), byName);
			std::sort(cachedAll.begin(), cachedAll.end(), byName);
		}
		return with64 ? cachedAll : cached32;
	}

	bool RunsIn64BitHost(const std::wstring& name)
	{
		return host64Drivers.count(Lower(name)) != 0;
	}

	int InterfaceState()
	{
		return interfaceState.load();
	}

	void SwitchInterface(const std::wstring& driver, const std::wstring& previous)
	{
		interfaceState.store(1);
		std::thread([driver, previous]
		{
			int state = 2;
			std::string message;
			if (SUCCEEDED(BridgeControl(25, driver.c_str()).result))
			{
				SaveInterfaceTarget(driver);
				message = "Now on " + Utf8(driver) + ". It is remembered for the next launch.";
			}
			else
			{
				// The proxy's own reason (busy, refused the buffer size...), read before the fallback clears it.
				const std::wstring why = Audio::OutputTap::ProxyBindError();
				const bool back = !previous.empty() && SUCCEEDED(BridgeControl(25, previous.c_str()).result);
				state = back ? 3 : 4;
				message = (why.empty() ? Utf8(driver) + " would not open." : Utf8(why))
					+ (back ? " Back on " + Utf8(previous) + "." : " Pick an interface to try again.");
			}
			LOG_INFO("(AUDIO BRIDGE) Interface switch to " << Utf8(driver) << ": " << message << std::endl);
			{
				std::lock_guard<std::mutex> lock(interfaceMessageMutex);
				outputSwitchMessage = message;
			}
			interfaceState.store(state);
		}).detach();
	}

	std::string OutputSwitchMessage()
	{
		std::lock_guard<std::mutex> lock(interfaceMessageMutex);
		return outputSwitchMessage;
	}

	void SwitchInputInterface(int role, const std::wstring& driver)
	{
		interfaceState.store(1);
		std::thread([role, driver]
		{
			const int result = Audio::OutputTap::SetProxyInputTarget(role, driver);
			const std::string error = Audio::OutputTap::ProxyInputError();
			static const char* const names[4] = { "the output", "Player 1", "Player 2", "the mic" };
			const std::string who = role >= 1 && role <= 3 ? names[role] : "every input";
			std::string message;
			if (result == 0) message = "Could not change the interface for " + who + ".";
			else if (!error.empty()) message.clear();   // the card shows the input error itself
			else if (driver.empty()) message = who + " comes from the output interface again.";
			else message = who + " now comes from " + Utf8(driver) + (result == 2 ? " once the output interface opens." : ".");
			if (!message.empty()) message[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(message[0])));
			LOG_INFO("(AUDIO BRIDGE) Input interface for " << who << ": " << (driver.empty() ? std::string("same as output") : Utf8(driver))
				<< ", result " << result << (error.empty() ? "" : ", " + error) << std::endl);
			{
				std::lock_guard<std::mutex> lock(interfaceMessageMutex);
				inputSwitchMessage = message;
			}
			interfaceState.store(result == 0 ? 4 : 2);
		}).detach();
	}

	void ApplyRoute(int role, long channel)
	{
		routeState.store(1);
		std::thread([role, channel]
		{
			const int result = Audio::OutputTap::SetProxyRoute(role, channel);
			routeState.store(result == 1 ? 2 : result == 2 ? 3 : 4);
		}).detach();
	}

	const RouteView& PolledRouteView()
	{
		static RouteView view;
		static ULONGLONG polled = 0;
		static int lastRouteState = 0, lastInterfaceState = 0;
		const int state = routeState.load();
		const int iface = interfaceState.load();
		if (state != 1 && iface != 1 && (GetTickCount64() - polled > 1000 || state != lastRouteState || iface != lastInterfaceState))
		{
			RouteView fresh;
			for (int role = 0; role < 4; ++role)
			{
				fresh.viaBridge[role] = Audio::OutputTap::ProxyRoute(role, fresh.host[role], fresh.device[role]);
				fresh.names[role] = Audio::OutputTap::ProxyRoleChannelNames(role);
				if (role > 0) fresh.interfaceOf[role] = Audio::OutputTap::ProxyInputTarget(role);
			}
			view = std::move(fresh);
			polled = GetTickCount64();
		}
		lastRouteState = state;
		lastInterfaceState = iface;
		return view;
	}

	bool RouteBusy()
	{
		return routeState.load() == 1 || interfaceState.load() == 1;
	}

	void RouteStatusMessage()
	{
		const std::string inputError = Audio::OutputTap::ProxyInputError();
		if (!inputError.empty()) WarnCaption(inputError.c_str());
		const int state = routeState.load();
		std::string message;
		ImU32 color = Color::Muted;
		if (RouteBusy()) message = "Applying...";
		else
		{
			{
				std::lock_guard<std::mutex> lock(interfaceMessageMutex);
				message = inputSwitchMessage;
			}
			if (state == 2) message = "Applied.";
			else if (state == 3) message = "Saved. It applies when the interface opens.";
			else if (state == 4)
			{
				message = "The interface would not take that channel. Check Playing to on the Output page.";
				color = Color::Bad;
			}
		}
		if (!message.empty()) ColorText(message.c_str(), color);
	}

	ClockView InputClockState(int role)
	{
		ClockView view;
		const Audio::OutputTap::InputClock clock = Audio::OutputTap::ProxyInputClock(role);
		if (clock.source == 0) return view;
		// "Catching up": not delivering yet, or a dropout in the last 5 s, or the correction near its limit.
		static unsigned lastUnderruns[4]{};
		static ULONGLONG lastDropoutAt[4]{};
		const size_t r = static_cast<size_t>(role & 3);
		if (clock.underruns != lastUnderruns[r])
		{
			lastUnderruns[r] = clock.underruns;
			lastDropoutAt[r] = GetTickCount64();
		}
		const bool recentDropout = lastDropoutAt[r] != 0 && GetTickCount64() - lastDropoutAt[r] < 5000;
		view.shown = true;
		view.settled = clock.primed && !recentDropout && std::abs(clock.ppm) < 2500;
		sprintf_s(view.detail, "%s runs on its own clock. Correcting by %+d ppm (%.3f%%); %d frames buffered (margin %d); %u dropout%s since it opened.",
			clock.source == 1 ? "Its interface" : "The Real Tone Cable", clock.ppm, clock.ppm / 10000.0, clock.fill, clock.margin,
			clock.underruns, clock.underruns == 1 ? "" : "s");
		return view;
	}

	void InputClockPill(int role, const char* label)
	{
		const ClockView clock = InputClockState(role);
		if (!clock.shown) return;
		PillRow(label, clock.settled ? "In sync" : "Catching up", clock.settled ? Color::Good : Color::Warn, clock.detail);
	}
}
