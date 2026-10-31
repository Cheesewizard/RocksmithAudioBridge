#include "OutputPage.hpp"

#include "PageCommon.hpp"
#include "Routing.hpp"
#include "../ShellPages.hpp"
#include "../../AudioBridge/Devices/DeviceEnumeratorHook.hpp"
#include "../../AudioBridge/Engine/AppAudioMix.hpp"
#include "../../AudioBridge/Engine/AsioOutputDevice.hpp"
#include "../../AudioBridge/Engine/AudioBridgeEngine.hpp"
#include "../../AudioBridge/Engine/CableGamePeriod.hpp"
#include "../../AudioBridge/Engine/DeviceVolume.hpp"
#include "../../AudioBridge/Engine/ExclusiveOutputDevice.hpp"
#include "../../AudioBridge/Engine/OutputDevices.hpp"
#include "../../AudioBridge/Engine/OutputTap.hpp"

#include "Log.hpp"
#include "Settings.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

using namespace Overlay;
using namespace AudioBridge::Pages::Routing;
using Audio::SharedOutput::AsioOutputDevice;
using Audio::SharedOutput::ExclusiveOutputDevice;

namespace AudioBridge::Pages
{
	namespace
	{
		// AudioBridgeRoutingMod's output guard settings.
		namespace Keys
		{
			constexpr char LIMITER[] = "AudioBridgeLimiter";
			constexpr char LIMITER_LEVEL[] = "AudioBridgeLimiterLevel";
			constexpr char LOUDNESS_MATCH[] = "AudioBridgeLoudnessMatch";
			constexpr char LOUDNESS_TARGET[] = "AudioBridgeLoudnessTarget";
		}

		// ---- Output device switching ---------------------------------------------------------------------
		// The device list is enumerated on a worker thread (COM + property reads take a few ms), refreshed when the page
		// opens and every few seconds while it stays open. A switch is remembered: the next launch starts on that device
		// if it is plugged in.

		struct DeviceCache
		{
			std::mutex mutex;
			Audio::OutputDevices::Snapshot snapshot;
			bool busy = false;
			ULONGLONG refreshed = 0;
		};

		DeviceCache& Devices()
		{
			static DeviceCache cache;
			return cache;
		}

		void RefreshDevices()
		{
			DeviceCache& cache = Devices();
			{
				std::lock_guard<std::mutex> lock(cache.mutex);
				static ULONGLONG busySince = 0;
				static bool stuckLogged = false;
				if (cache.busy)
				{
					// A hung scan would freeze the list on an old snapshot; log it once.
					if (!stuckLogged && GetTickCount64() - busySince > 5000)
					{
						stuckLogged = true;
						LOG_WARNING("(OUTPUT DEVICES) device scan has not finished in 5 s; the Output list is stale" << std::endl);
					}
					return;
				}
				cache.busy = true;
				busySince = GetTickCount64();
			}
			std::thread([]
			{
				Audio::OutputDevices::Snapshot snapshot = Audio::OutputDevices::Enumerate();
				DeviceCache& cache = Devices();
				std::lock_guard<std::mutex> lock(cache.mutex);
				cache.snapshot = std::move(snapshot);
				cache.busy = false;
				cache.refreshed = GetTickCount64();
			}).detach();
		}

		// A switch that is checked for a few seconds and undone if the new device does not play.
		struct PendingSwitch
		{
			bool active = false;
			bool route = false;            // op 21 route (proxy) vs op 4 session switch
			bool promote = false;          // op 25: bind the ASIO interface itself (see PromoteToAsio)
			std::wstring target, previous; // previous: session device to go back to (op 4 only)
			std::string name;
			ULONGLONG started = 0;
		};

		std::string outputMessage;
		bool outputMessageIsError = false;
		std::wstring outputMessageEndpoint;   // the output a "Now playing through" message is about (empty: any other message)

		void OutputSay(const std::string& text, bool error)
		{
			outputMessage = text;
			outputMessageIsError = error;
			outputMessageEndpoint.clear();
		}

		// Picking the ASIO entry while the proxy runs on its virtual clock (the game booted with the interface
		// unplugged, so input is the Real Tone Cable and the mix is routed to a Windows device). Stopping the route
		// (op 22) alone is not enough: the host re-creates its managed route on the next refresh and the proxy never
		// touches the interface, so input stays on the cable. Op 25 binds the interface for input AND output in one
		// step; then op 22 drops any route and forgets the remembered output, and PreferReal=1 makes the next launch
		// start on the interface. Runs off the render thread because loading and initialising a real ASIO driver can
		// take a second. 0 idle, 1 running, 2 bound, 3 failed.
		std::atomic<int> promoteState{ 0 };

		void PromoteToAsio(const std::wstring& driver)
		{
			promoteState.store(1);
			std::thread([driver]
			{
				const bool ok = SUCCEEDED(BridgeControl(25, driver.c_str()).result);
				if (ok)
				{
					BridgeControl(22);
					HKEY key{};
					if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_32KEY, nullptr, &key, nullptr) == ERROR_SUCCESS)
					{
						const DWORD one = 1;
						RegSetValueExW(key, L"PreferReal", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
						RegCloseKey(key);
					}
				}
				promoteState.store(ok ? 2 : 3);
			}).detach();
		}

		void VerifySwitch(PendingSwitch& pending, const LiveState& live, const std::string& asioName)
		{
			if (!pending.active) return;
			const ULONGLONG elapsed = GetTickCount64() - pending.started;
			if (pending.promote)
			{
				const int state = promoteState.load();
				if (state == 1) return;
				if (state == 2) OutputSay("Now playing through " + asioName + ". Guitar input moved to it too.", false);
				else OutputSay("Could not open " + asioName + ". Check it is plugged in and not used by another program. Sound stays on a Windows device and input on the cable.", true);
				promoteState.store(0);
				pending.active = false;
				return;
			}
			if (pending.route)
			{
				const HRESULT health = Audio::SharedOutput::RouteHealth();
				if (health == S_OK)
				{
					OutputSay("Now playing through " + pending.name + ". Lasts until you close Rocksmith.", false);
					pending.active = false;
					return;
				}
				if (health == E_PENDING && elapsed < 5000) return;
				// Could not open, lost, or never started: hand the mix straight back to the ASIO device.
				BridgeControl(22);
				OutputSay("Could not play through " + pending.name + ". Back on " + asioName + ".", true);
				pending.active = false;
				return;
			}
			if (std::wstring(live.status.endpoint) == pending.target)
			{
				OutputSay("Now playing through " + pending.name + ". Lasts until you close Rocksmith.", false);
				outputMessageEndpoint = pending.target;
				pending.active = false;
				return;
			}
			if (elapsed < 4000) return;
			if (!pending.previous.empty()) BridgeControl(4, pending.previous.c_str());
			if (ExclusiveOutputDevice::IsExclusiveEndpoint(pending.target))
				OutputSay("Could not open " + pending.name + ": another program may be using it in exclusive mode, or Windows does not allow exclusive mode on it (Sound settings, the device's Properties, Advanced). Switched back.", true);
			else if (AsioOutputDevice::IsAsioEndpoint(pending.target))
				OutputSay("Could not open " + pending.name + "'s ASIO driver: close other programs using it (a DAW, its control app), and check it can run at 48 kHz. Switched back.", true);
			else OutputSay("Could not play through " + pending.name + ". Switched back.", true);
			pending.active = false;
		}

		void OutputDeviceCard(const LiveState& live)
		{
			static PendingSwitch pending;
			static ULONGLONG lastVisible = 0;
			const ULONGLONG now = GetTickCount64();
			DeviceCache& cache = Devices();
			Audio::OutputDevices::Snapshot snapshot;
			ULONGLONG refreshed = 0;
			{
				std::lock_guard<std::mutex> lock(cache.mutex);
				snapshot = cache.snapshot;
				refreshed = cache.refreshed;
			}
			// Refresh on (re)opening the page, then every 4 s while it is shown.
			if (now - lastVisible > 500 || now - refreshed > 4000) RefreshDevices();
			lastVisible = now;

			const std::wstring endpoint = live.status.endpoint;
			const bool proxy = Audio::OutputTap::ProxyAvailable();
			const bool routing = endpoint.rfind(L"(route", 0) == 0;
			const bool passthrough = endpoint == L"(passthrough)" || endpoint == L"(silent)";
			const bool switching = endpoint == L"(downgrading)";
			const bool session = !endpoint.empty() && endpoint[0] != L'(';
			const unsigned outputMode = OutputStatusNumber(live, L"outputMode=");
			std::wstring routedId;
			if (routing)
			{
				const size_t close = endpoint.find(L')');
				if (close != std::wstring::npos) routedId = endpoint.substr(close + 1);
			}
			const std::wstring asioDriver = ReadPassthroughOutputName();
			std::string asioName = Utf8(asioDriver);
			if (asioName.empty()) asioName = "your ASIO device";

			VerifySwitch(pending, live, asioName);

			// Where the game is playing now, in words.
			std::string current = "Waiting for the audio engine";
			const char* state = "Unknown";
			ImU32 stateColor = Color::Muted;
			if (routing || session)
			{
				const std::wstring id = routing ? routedId : endpoint;
				const bool exclusiveId = ExclusiveOutputDevice::IsExclusiveEndpoint(id);
				const std::wstring windowsId = ExclusiveOutputDevice::WindowsId(id);
				current = "A Windows device";
				for (const auto& device : snapshot.devices)
					if (device.id == windowsId) current = device.name;
				state = routing ? "Switched" : exclusiveId ? "Exclusive" : "Windows";
				stateColor = Color::AccentHi;
				if (AsioOutputDevice::IsAsioEndpoint(id))
				{
					current = Utf8(AsioOutputDevice::DriverName(id));
					state = "ASIO";
					stateColor = Color::Good;
				}
				if (endpoint.rfind(L"(route-stalled)", 0) == 0)
				{
					state = "Stalled";
					stateColor = Color::Warn;
				}
			}
			else if (endpoint == L"(passthrough)") { current = asioName; state = proxy ? "ASIO" : "Direct"; stateColor = Color::Good; }
			else if (endpoint == L"(silent)") { current = "No output"; state = "Silent"; stateColor = Color::Bad; }
			else if (switching) { current = "Switching"; state = "Switching"; stateColor = Color::Warn; }
			else if (endpoint == L"(starting)") { current = "Waiting for audio to start"; state = "Starting"; }
			else if (endpoint == L"(failed)") { current = "Audio could not start"; state = "Failed"; stateColor = Color::Bad; }
			if (session)
			{
				switch (outputMode)
				{
				case 1: state = "Shared"; break;
				case 2: state = "Exclusive (event-driven)"; break;
				case 3: state = "Exclusive (polling)"; stateColor = Color::Warn; break;
				case 4: state = "ASIO"; break;
				default: state = "Opening"; stateColor = Color::Warn; break;
				}
			}

			// The real ASIO device refused to open this launch, so the proxy runs on its virtual clock and the guitar is
			// silent. Without this the card reads "ASIO" in green and the player only notices "no input". The proxy clears
			// the error once the device binds, so it is only shown while it still applies.
			const std::wstring bindError = proxy ? Audio::OutputTap::ProxyBindError() : std::wstring();
			static std::wstring loggedBindError;
			if (!bindError.empty() && bindError != loggedBindError)
			{
				loggedBindError = bindError;
				LOG_ERROR("(AUDIO BRIDGE) " << Utf8(bindError) << std::endl);
			}
			if (!bindError.empty() && !routing && !session && !switching) { current = asioName; state = "Not open"; stateColor = Color::Bad; }

			BeginCard("Playing to");
			ImGui::PushFont(GetFonts().title);
			ImGui::TextUnformatted(current.c_str());
			ImGui::PopFont();
			StatusPill(state, stateColor);
			if (session && outputMode != 0)
			{
				const unsigned deviceFrames = OutputStatusNumber(live, L"outputBufferFrames=");
				const unsigned periodFrames = OutputStatusNumber(live, L"outputPeriodFrames=");
				char bufferText[64];
				if (deviceFrames)
				{
					sprintf_s(bufferText, "%u samples (%.1f ms)", deviceFrames, deviceFrames / 48.0);
					InfoRow("Device buffer", bufferText);
				}
				if (periodFrames && periodFrames != deviceFrames)
				{
					sprintf_s(bufferText, "%u samples (%.1f ms)", periodFrames, periodFrames / 48.0);
					InfoRow("Output period", bufferText);
				}
			}
			// A setup that crashed the game is not reopened at boot (the driver's last-known-good guard); say what happened.
			static std::string safeBootNotice;
			static ULONGLONG safeBootPolled = 0;
			if (proxy && GetTickCount64() - safeBootPolled > 1000)
			{
				safeBootPolled = GetTickCount64();
				safeBootNotice = Audio::OutputTap::ProxySafeBootNotice();
			}
			if (!safeBootNotice.empty()) WarnCaption(safeBootNotice.c_str());
			// No restart needed: picking it again in Switch output binds it live (PromoteToAsio / SwitchInterface).
			if (!bindError.empty())
				ColorText((Utf8(bindError) + " Plug it in, or close other apps using it (its control app, a DAW, OBS), then pick it in Switch output below. No restart needed.").c_str(), Color::Bad);
			Meter("Left", MeterFraction(live.status.outputPeak[0]), "The game's left-channel output level, after the volume cap.");
			Meter("Right", MeterFraction(live.status.outputPeak[1]), "The game's right-channel output level, after the volume cap.");
			// The device's own Windows volume, here so nobody has to leave the game to find it (a player's speakers sat at
			// 0; in exclusive mode Windows will not even switch to the device). DeviceVolume reads and sets it off this
			// thread.
			if (session && !AsioOutputDevice::IsAsioEndpoint(endpoint))
			{
				Audio::DeviceVolume::Watch(endpoint, ExclusiveOutputDevice::IsExclusiveEndpoint(endpoint));
				const Audio::DeviceVolume::State volume = Audio::DeviceVolume::Get();
				if (volume.known)
				{
					static float percent = 100.0f;
					static bool dragging = false;
					if (!dragging) percent = volume.level * 100.0f;
					bool committed = false;
					const bool moved = SliderRow("Device volume", &percent, 0.0f, 100.0f, "%.0f%%", &committed,
						"This device's volume in Windows, the same as its slider in Windows' volume settings.");
					dragging = moved && !committed;
					if (moved) Audio::DeviceVolume::Set(percent / 100.0f);
					if (volume.muted || volume.level < 0.01f)
					{
						WarnCaption(volume.muted ? "Windows has this device muted, so nothing is heard." : "This device's Windows volume is at 0, so nothing is heard.");
						if (volume.muted && ImGui::Button("Unmute")) Audio::DeviceVolume::SetMute(false);
					}
				}
			}
			EndCard();

			BeginCard("Switch output");
			const bool canRoute = proxy && (passthrough || routing);
			if (!canRoute && !session)
			{
				Caption(endpoint == L"(failed)" ? "Audio initialization failed. Close Rocksmith and include a bug report when reporting this error."
					: endpoint == L"(starting)" ? "Waiting for Rocksmith to open its audio output."
					: switching ? "The audio engine is switching devices. Try again in a moment."
					: "Switching needs the Rocksmith Audio Bridge in the ASIO chain (RS_ASIO.ini Driver set to Rocksmith Audio Bridge), or Cable mode.");
				EndCard();
				return;
			}
			if (canRoute) Caption("Moves the game's sound to another device now, without restarting. An ASIO interface also takes the guitar, unless Guitar input below names another one. It is remembered: next launch starts there if it is plugged in.");
			else Caption("Moves the game's sound to another device now, without restarting. An ASIO interface plays the sound only: the guitar stays on the Real Tone Cable. It is remembered: next launch starts there if it is plugged in.");
			if (!snapshot.ok) Caption("Looking for devices...");

			const bool unidentified = snapshot.asioUnidentified;
			// The list is grouped (ASIO interfaces, then Windows devices) under small subheaders, with rows packed closer
			// than the page's default spacing so a long device list stays short.
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0f));
			ImGui::BeginDisabled(pending.active);
			if (canRoute)
			{
				ImGui::Dummy(ImVec2(0, 4));
				SectionLabel("ASIO interfaces");
				ImGui::Dummy(ImVec2(0, 2));
				// Every ASIO interface (what RS_ASIO can open). The current one: stop any route so the game plays straight
				// out of it again, or, when the bridge is virtual (booted without it), bind it (PromoteToAsio). Another
				// one: switch the bridge to it live (SwitchInterface), falling back to the current one if it will not open.
				const bool virtualProxy = Audio::OutputTap::ProxyOutputMode() == 1;
				const bool interfaceBusy = InterfaceState() == 1;
				for (const std::wstring& driver : InstalledAsioDrivers())
				{
					const bool isCurrent = _wcsicmp(driver.c_str(), asioDriver.c_str()) == 0;
					const std::string name = Utf8(driver);
					const std::string label = name + "##asio-" + name;
					// Which build the bridge uses: 32-bit in the game, or 64-bit in its driver host (one buffer later).
					const bool host64 = RunsIn64BitHost(driver);
					const char* detail = isCurrent ? (host64 ? "ASIO 64-bit, low latency" : "ASIO 32-bit, lowest latency") : (host64 ? "ASIO 64-bit" : "ASIO 32-bit");
					const bool clicked = ChoiceRow(("asio-" + name).c_str(), label.c_str(), detail, isCurrent && !routing && !virtualProxy && !interfaceBusy);
					if (!clicked || interfaceBusy) continue;
					if (isCurrent && virtualProxy)
					{
						pending = PendingSwitch{};
						pending.active = true;
						pending.promote = true;
						pending.name = name;
						pending.started = GetTickCount64();
						OutputSay("Opening " + name + " for input and output...", false);
						PromoteToAsio(driver);
					}
					else if (isCurrent && routing)
					{
						BridgeControl(22);
						OutputSay("Back on " + name + ".", false);
					}
					else if (!isCurrent)
					{
						if (routing) BridgeControl(22);
						OutputSay(std::string(), false);
						SwitchInterface(driver, asioDriver);
					}
				}
			}
			// Cable mode: an ASIO interface's own driver for the sound only (AsioOutputDevice), at the buffer size set in its
			// control panel. Windows' shared audio adds its period and the USB audio driver's buffering on top; through ASIO
			// an interface plays with the delay its owner knows from other programs. The guitar stays on the cable.
			if (!canRoute && session)
			{
				const std::vector<std::wstring> drivers = InstalledAsioDrivers(false);   // loaded in the game: 32-bit only
				if (!drivers.empty())
				{
					ImGui::Dummy(ImVec2(0, 4));
					SectionLabel("ASIO interfaces (sound only)");
					ImGui::Dummy(ImVec2(0, 2));
				}
				for (const std::wstring& driver : drivers)
				{
					const std::wstring id = AsioOutputDevice::EndpointOf(driver);
					const bool selected = endpoint == id;
					const std::string name = Utf8(driver);
					const bool clicked = ChoiceRow(("asio-out-" + name).c_str(), (name + "##asio-out-" + name).c_str(), selected ? "ASIO 32-bit, lowest latency" : "ASIO 32-bit", selected);
					Hint("Plays the game (and Tone designer) through this interface's own ASIO driver, at the buffer size set in its control panel. The guitar stays on the Real Tone Cable. While the game runs, other programs cannot use this ASIO driver.");
					if (!clicked || selected) continue;
					pending = PendingSwitch{};
					pending.active = true;
					pending.target = id;
					pending.name = name;
					pending.started = GetTickCount64();
					pending.previous = endpoint;
					const Audio::ControlResponse result = BridgeControl(4, id.c_str());
					if (FAILED(result.result))
					{
						OutputSay("Could not play through " + name + ".", true);
						pending.active = false;
					}
					else OutputSay("Opening " + name + "...", false);
				}
			}
			ImGui::BeginDisabled(unidentified && !snapshot.asioDrivers.empty());
			bool windowsLabelled = false;
			for (const auto& device : snapshot.devices)
			{
				// The ASIO interface is listed once, as the ASIO entry above; its Windows endpoint is the same box and is
				// hidden, bound or not. While bound it would silence the interface; while unbound the ASIO entry is the
				// right pick because it moves the input too.
				if (canRoute && device.isAsioTwin) continue;
				if (!windowsLabelled)
				{
					ImGui::Dummy(ImVec2(0, 6));
					SectionLabel("Windows devices");
					ImGui::Dummy(ImVec2(0, 2));
					windowsLabelled = true;
				}
				const bool selected = routing ? device.id == routedId
					: (session && device.id == ExclusiveOutputDevice::WindowsId(endpoint));
				std::string caption = device.isDefault ? "Windows default" : "";
				// Device-reported periods describe capability, not total latency or the buffer granted after alignment.
				if (device.sharedPeriod || device.exclusivePeriod)
				{
					char estimate[64] = "";
					const double shared = device.sharedPeriod / 48.0, exclusive = device.exclusivePeriod / 48.0;
					// A device whose driver stalls in event-driven exclusive mode runs it polling, three periods deep
					// (OutputBackend remembers it): say what it really gets, not the minimum it reports.
					DWORD polling = 0, pollingSize = sizeof(polling);
					const bool needsPolling = RegGetValueW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy\\ExclusivePolling", device.id.c_str(),
						RRF_RT_REG_DWORD, nullptr, &polling, &pollingSize) == ERROR_SUCCESS && polling;
					if (device.sharedPeriod && device.exclusivePeriod && needsPolling)
						sprintf_s(estimate, "Periods: shared %.1f ms, exclusive %.1f ms (polling)", shared, exclusive * 3.0);
					else if (device.sharedPeriod && device.exclusivePeriod) sprintf_s(estimate, "Periods: shared %.1f ms, exclusive min %.1f ms", shared, exclusive);
					else if (device.sharedPeriod) sprintf_s(estimate, "Shared period %.1f ms", shared);
					caption = caption.empty() ? estimate : caption + "  " + estimate;
				}
				ImGui::BeginDisabled(device.isProtected);
				if (device.isProtected) caption = "Used by ASIO";
				const std::string label = device.name + "##" + Utf8(device.id);
				const bool clicked = ChoiceRow(Utf8(device.id).c_str(), label.c_str(), caption.c_str(), selected);
				if (device.isProtected) Hint("This is the same interface RS_ASIO is using. Playing to its Windows device would silence it, so pick the ASIO entry above instead.");
				ImGui::EndDisabled();
				if (!clicked || selected || device.isProtected) continue;
				pending = PendingSwitch{};
				pending.active = true;
				pending.target = device.id;
				pending.name = device.name;
				pending.started = GetTickCount64();
				if (canRoute)
				{
					pending.route = true;
					const Audio::ControlResponse result = BridgeControl(21, device.id.c_str());
					if (FAILED(result.result))
					{
						OutputSay("Could not play through " + device.name + ".", true);
						pending.active = false;
					}
					else OutputSay("Switching to " + device.name + "...", false);
				}
				else
				{
					pending.previous = endpoint;
					const Audio::ControlResponse result = BridgeControl(4, device.id.c_str());
					if (FAILED(result.result))
					{
						OutputSay("Could not play through " + device.name + ".", true);
						pending.active = false;
					}
					else OutputSay("Switching to " + device.name + "...", false);
				}
			}
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			ImGui::PopStyleVar();
			ImGui::Dummy(ImVec2(0, 2));

			// Exclusive and ASIO outputs: other programs cannot play on the device, so the bridge mixes them in
			// (AppAudioMix, Windows' app capture of everything but Rocksmith). Not offered in shared mode, where they play
			// on the device already and mixing them in too would double them.
			if (session && (ExclusiveOutputDevice::IsExclusiveEndpoint(endpoint) || AsioOutputDevice::IsAsioEndpoint(endpoint)))
			{
				bool mixApps = Audio::AppAudioMix::Enabled();
				if (ToggleRow("Hear other apps here", "YouTube, Discord, Spotify and Windows sounds play through this device too. Set Windows' default output to a spare device (such as the laptop speakers) and mute it there: the bridge picks the sound up before the mute. The guitar's delay is not affected.", &mixApps))
					Audio::AppAudioMix::SetEnabled(mixApps);
				if (mixApps) Caption(Audio::AppAudioMix::Hearing() ? "Other apps: playing through here now." : "Other apps: nothing playing right now.");
			}

			// Cable mode on a Windows device: exclusive mode (ExclusiveOutputDevice) takes Windows' mixer out of the way,
			// the device's own ~3 ms period instead of its shared 10 ms. Switching it is a device switch ("excl:<id>"), so
			// it is remembered and verified like one.
			const bool onWindowsDevice = session && !AsioOutputDevice::IsAsioEndpoint(endpoint);
			if (onWindowsDevice && !pending.active)
			{
				const bool isExclusive = outputMode == 2 || outputMode == 3;
				bool wantExclusive = isExclusive;
				// Taking Windows' default output away breaks what plays on it (YouTube shows "Audio renderer error"): say
				// so before, while there is still time to move Windows' default.
				bool windowsDefault = false;
				for (const auto& device : snapshot.devices)
					if (device.id == ExclusiveOutputDevice::WindowsId(endpoint)) windowsDefault = device.isDefault;
				if (!isExclusive && windowsDefault)
					WarnCaption("This is Windows' default output: programs playing on it (YouTube, Discord) stop when exclusive mode starts. Make another device (such as the laptop speakers) the Windows default first and mute it; \"Hear other apps here\" then plays them through this one.");
				// Exclusive mode and direct USB (UsbCableCapture) are two separate latency features: direct USB is the
				// cable's input and is on by default; exclusive mode is this device's output and is opt-in. The text
				// describes exclusive mode on its own and does not mention the input path.
				ImGui::BeginDisabled(outputMode == 0);
				if (ToggleRow("Exclusive mode", "Lowers the delay of the sound you hear by playing straight to this device, past Windows' mixer. Only for devices that support exclusive mode.", &wantExclusive,
					"Other apps cannot play on this device while exclusive mode is on. A device that does not support it stays shared and this switch turns itself off."))
				{
					const std::wstring windowsId = ExclusiveOutputDevice::WindowsId(endpoint);
					const std::wstring target = wantExclusive ? ExclusiveOutputDevice::EndpointOf(windowsId) : windowsId;
					pending = PendingSwitch{};
					pending.active = true;
					pending.target = target;
					pending.name = current + (wantExclusive ? " (exclusive)" : " (shared)");
					pending.started = GetTickCount64();
					pending.previous = endpoint;
					const Audio::ControlResponse result = BridgeControl(4, target.c_str());
					if (FAILED(result.result))
					{
						OutputSay("Could not switch exclusive mode.", true);
						pending.active = false;
					}
					else OutputSay(wantExclusive ? "Opening exclusive mode..." : "Back to shared mode...", false);
				}
				ImGui::EndDisabled();
			}

			if (!snapshot.asioWrapper.empty() && canRoute)
				Caption((snapshot.asioWrapper + " wraps your Windows devices rather than being one. A device it is using itself will not open here; switch that device off in "
					+ snapshot.asioWrapper + "'s own panel, or pick another.").c_str());
			if (unidentified && !snapshot.asioDrivers.empty())
				WarnCaption(("Switching is off: the overlay cannot tell which Windows device is your ASIO interface (" + snapshot.asioDrivers.front()
					+ "), and playing to that device would silence it.").c_str());
			{
				std::string switchMessage = OutputSwitchMessage();
				const int iface = InterfaceState();
				if (iface == 1) switchMessage = "Switching interface...";
				if (!switchMessage.empty()) ColorText(switchMessage.c_str(), iface >= 3 ? Color::Bad : Color::Muted);
			}
			// The output moved without the user (unplugged, fell back, came back): an old "Now playing through" named the
			// wrong device next to the one shown playing.
			if (!pending.active && !outputMessageEndpoint.empty() && std::wstring(live.status.endpoint) != outputMessageEndpoint) OutputSay(std::string(), false);
			if (!outputMessage.empty()) ColorText(outputMessage.c_str(), outputMessageIsError ? Color::Bad : Color::Muted);
			EndCard();
		}

		// Output page: the pair of outputs the game plays on. Most interfaces have one stereo pair, where a dropdown that
		// can't change anything only confuses, so the card only shows when there is a real choice (4+ outputs, or an odd
		// pair saved earlier to move off). Each player's input is picked on the Input page.
		void OutputPairCard()
		{
			if (!Audio::OutputTap::ProxyAvailable()) return;
			const RouteView& view = PolledRouteView();
			if (!view.viaBridge[0] || view.names[0].size() < 2) return;
			std::vector<std::string> labels;
			std::vector<long> channels;
			int index = -1;
			for (long channel = 0; channel + 1 < static_cast<long>(view.names[0].size()); channel += 2)
			{
				if (channel == view.device[0]) index = static_cast<int>(labels.size());
				labels.push_back(view.names[0][channel] + " / " + view.names[0][channel + 1]);
				channels.push_back(channel);
			}
			if (index < 0 && view.device[0] + 1 < static_cast<long>(view.names[0].size()))   // an odd pair saved earlier
			{
				index = static_cast<int>(labels.size());
				labels.push_back(view.names[0][view.device[0]] + " / " + view.names[0][view.device[0] + 1]);
				channels.push_back(view.device[0]);
			}
			if (labels.size() <= 1) return;
			const std::wstring outputInterface = ReadPassthroughOutputName();
			BeginCard("Output pair");
			ImGui::BeginDisabled(RouteBusy());
			if (DropdownRow("Output pair", &index, labels, ("The pair of outputs of " + Utf8(outputInterface) + " the game's sound plays on. The interface itself is picked in Switch output above.").c_str()))
				ApplyRoute(0, channels[static_cast<size_t>(index)]);
			ImGui::EndDisabled();
			RouteStatusMessage();
			EndCard();
		}

		// Output buffer size, only when the game plays through a Windows device (Cable mode's managed session). Under
		// ASIO the buffer belongs to RS_ASIO and the interface. Op 5 reports the engine's limits in response.file
		// ("engineMinimum=.. engineFundamental=.. engineMaximum=.. enginePeriod=.."), read from the live poll (4.x sent
		// its own blocking op 5 once a second); op 6 sets it ("<frames>" newline "<endpoint>", 0 = automatic). Saved per
		// device in RSMods.ini [Audio Bridge Output Buffer <endpoint>], the same keys the engine reads at launch
		// (ReadSavedPeriod).
		void OutputBufferCard(const LiveState& live)
		{
			const std::wstring endpoint = live.status.endpoint;
			if (endpoint.empty() || endpoint[0] == L'(') return;

			const unsigned minimum = OutputStatusNumber(live, L"engineMinimum=");
			const unsigned fundamental = OutputStatusNumber(live, L"engineFundamental=");
			const unsigned maximum = OutputStatusNumber(live, L"engineMaximum=");
			const unsigned period = OutputStatusNumber(live, L"enginePeriod=");
			// Hide the card when there is nothing to pick. A driver without Windows' low-latency engine reports one period
			// (minimum == maximum, usually 480), and the converted-stream fallback reports none; a slider pinned to a
			// single value only made the setting look broken.
			if (!fundamental || maximum <= minimum) return;

			const std::filesystem::path ini = GameFolder() / L"RSMods.ini";
			const std::wstring section = L"Audio Bridge Output Buffer " + endpoint;
			// Read once a second (the page draws every frame), and again right after this page changes it below.
			static std::wstring modeFor;
			static ULONGLONG modeReadAt = 0;
			static bool modeAutomatic = true;
			const ULONGLONG modeNow = GetTickCount64();
			if (modeFor != section || modeReadAt == 0 || modeNow - modeReadAt >= 1000)
			{
				wchar_t mode[32]{};
				GetPrivateProfileStringW(section.c_str(), L"Mode", L"Automatic", mode, 32, ini.c_str());
				modeAutomatic = _wcsicmp(mode, L"Custom") != 0;
				modeFor = section;
				modeReadAt = modeNow;
			}
			bool automatic = modeAutomatic;

			BeginCard("Device buffer");
			char current[64];
			sprintf_s(current, "Now %u frames (%.2f ms).", period, period / 48.0);
			// At the device's smallest, say so, or players wonder why it cannot go lower.
			const std::string about = period && period <= minimum
				? std::string(current) + " The smallest this device allows in shared mode; for less, turn on Exclusive mode above."
				: std::string(current) + " How much sound Windows hands this device at a time. Smaller is lower latency; raise it if you hear crackles. Applies at once.";
			const bool toggled = ToggleRow("Automatic", about.c_str(), &automatic);
			static float frames = 0.0f;
			if (frames <= 0.0f || toggled) frames = static_cast<float>(period ? period : minimum);
			ImGui::BeginDisabled(automatic);
			bool committed = false;
			SliderRow("Size", &frames, static_cast<float>(minimum), static_cast<float>(maximum), "%.0f frames", &committed,
				"Output buffer in frames at 48 kHz, in steps the device allows.");
			ImGui::EndDisabled();
			const unsigned snapped = std::clamp(minimum + static_cast<unsigned>((frames - minimum) / fundamental + 0.5f) * fundamental, minimum, maximum);
			frames = static_cast<float>(snapped);
			if (toggled || (committed && !automatic))
			{
				const unsigned request = automatic ? 0u : snapped;
				const std::wstring value = std::to_wstring(request) + L"\n" + endpoint;
				const Audio::ControlResponse result = BridgeControl(6, value.c_str());
				if (SUCCEEDED(result.result))
				{
					WritePrivateProfileStringW(section.c_str(), L"PeriodFrames", automatic ? nullptr : std::to_wstring(snapped).c_str(), ini.c_str());
					WritePrivateProfileStringW(section.c_str(), L"Mode", automatic ? L"Automatic" : L"Custom", ini.c_str());
					modeAutomatic = automatic;
					modeFor = section;
					modeReadAt = GetTickCount64();
				}
			}
			EndCard();
		}

		// The block size the game's own engine runs at in Cable mode (CableGamePeriod.hpp). RS_ASIO players get it from
		// their interface's buffer; Automatic does the same here from the output the game plays to. Fixed for the session
		// when the game opens its devices, so a change applies with Apply now (the output reopens) or at the next start.
		void GameBlockCard(const LiveState& live)
		{
			const std::wstring endpoint = live.status.endpoint;
			if (endpoint.empty() || endpoint[0] == L'(') return;
			static const char* const labels[] = { "Automatic", "64", "128", "256", "512" };
			static const DWORD values[] = { 0, 64, 128, 256, 512 };
			static ULONGLONG readAt = 0;
			static DWORD setting = 0;
			static UINT32 next = 0;
			if (!readAt || GetTickCount64() - readAt >= 1000)
			{
				setting = Audio::CableGamePeriod::Setting();
				next = Audio::CableGamePeriod::NextStart();
				readAt = GetTickCount64();
			}
			int index = 0;
			for (int i = 0; i < 5; ++i)
				if (values[i] == setting) index = i;
			BeginCard("Game engine");
			// What the game's output really runs at; the requested size only takes over when the game reopens its output.
			const UINT32 running = Audio::SharedOutput::GameOutputBlockFrames();
			const UINT32 now = running ? running : Audio::CableGamePeriod::Frames();
			char text[96];
			sprintf_s(text, "%u samples (%.1f ms)", now, now / 48.0);
			InfoRow("Processing block", text);
			if (SegmentedRow("Game block", &index, labels, 5, "How many samples the game's own engine (its amps, pedals and note detection) processes in one go: the ASIO buffer size under RS_ASIO. Smaller is less delay in the game's tones but more work for the PC; raise it if game tones crackle. Automatic follows the output: an ASIO interface's own buffer size, or the device buffer up to 144. A change applies with Apply now, or at the next game start."))
			{
				Audio::CableGamePeriod::SetSetting(values[index]);
				readAt = 0;
			}
			// A different size waiting: one action, Apply now. The bridge tells the game its output changed and it reopens
			// it at the new size (the input keeps its start-up size); off the render thread, the game may work inside the
			// call. "Restart the game" only shows once Apply now was tried and the game did not take it within 3 s (a
			// warning plus a button was two answers to one question).
			static ULONGLONG appliedAt = 0;
			static UINT32 appliedSize = 0;
			if (next != now)
			{
				const bool refused = appliedAt && appliedSize == next && GetTickCount64() - appliedAt > 3000;
				if (refused)
				{
					sprintf_s(text, "%u samples after you restart the game (the game did not take it now)", next);
					InfoRow("Restart needed", text, Color::Warn);
				}
				else
				{
					sprintf_s(text, "Apply %u samples now (game sound restarts for a moment)", next);
					const bool applying = appliedAt && appliedSize == next && GetTickCount64() - appliedAt <= 3000;
					ImGui::BeginDisabled(applying);
					if (ImGui::Button(applying ? "Applying..." : text))
					{
						appliedAt = GetTickCount64();
						appliedSize = next;
						std::thread([]
						{
							Audio::CableGamePeriod::ApplyNextStart();
							Audio::PersistentInput::ResetGameOutput();
						}).detach();
					}
					ImGui::EndDisabled();
					if (setting == 0 && !applying) Caption("Automatic follows your output; it also applies at the next game start.");
				}
			}
			EndCard();
		}

		// Output protection (op 16; ceiling sent linear = pow(10, tenths/200)). Drags preview the op; a switch or a release
		// saves AudioBridgeRoutingMod's settings, which reconfigures the bridge driver's guard from OnSettingsChanged. The
		// loudness match is not on this page, so its saved values ride along unchanged (4.x's op turned it off).
		void VolumeCapCard()
		{
			static bool seeded = false;
			static bool limOn = false;
			static int limTenths = -60;
			if (!seeded)
			{
				limOn = Settings::GetModSetting(Keys::LIMITER) != 0;
				limTenths = Settings::GetModSetting(Keys::LIMITER_LEVEL);
				if (limTenths == 0) limTenths = -60;
				seeded = true;
			}

			BeginCard("Volume cap");
			const bool limChanged = ToggleRow("Cap the maximum volume",
				"A look-ahead limiter eases the level down before anything louder than the ceiling arrives. Adds a few milliseconds of latency.",
				&limOn);
			ImGui::BeginDisabled(!limOn);
			float ceiling = limTenths / 10.0f;
			bool ceilingSaved = false;
			const bool slid = SliderRow("Ceiling", &ceiling, -24.0f, 0.0f, "%.1f dBFS", &ceilingSaved,
				"Nothing in the game's output rises above this. Your interface volume still sets how loud that actually is.");
			if (slid) limTenths = static_cast<int>(ceiling * 10 + (ceiling < 0 ? -0.5f : 0.5f));
			ImGui::EndDisabled();
			if (limChanged || slid || ceilingSaved)
			{
				const int loudnessOn = Settings::GetModSetting(Keys::LOUDNESS_MATCH) != 0 ? 1 : 0;
				const int loudnessTenths = Settings::GetModSetting(Keys::LOUDNESS_TARGET);
				wchar_t value[64];
				swprintf_s(value, L"%d,%.6f,%d,%.6f", limOn ? 1 : 0, std::pow(10.0, limTenths / 200.0), loudnessOn, std::pow(10.0, loudnessTenths / 200.0));
				PreviewControl(16, value);
				if (limChanged || ceilingSaved)
				{
					SaveModInt(Keys::LIMITER, Keys::LIMITER, limOn ? 1 : 0, "Output limiter");
					SaveModInt(Keys::LIMITER_LEVEL, Keys::LIMITER_LEVEL, limTenths, "Output limiter ceiling");
				}
			}
			EndCard();
		}

		void Draw()
		{
			const LiveState& live = Poll();
			PageHeader("Output", "Where the game's sound goes and how loud it can get.");
			OutputDeviceCard(live);
			OutputPairCard();
			OutputBufferCard(live);
			GameBlockCard(live);
			VolumeCapCard();
		}
	}

	void RegisterOutputPage()
	{
		Shell::RegisterPage({ "output", "Audio", Glyph::Speaker, "Output", 40, Draw, {} });
	}
}
