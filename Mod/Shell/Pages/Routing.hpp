#pragma once

#include <string>
#include <vector>

// The Input and Output pages' shared routing (ported from 4.x's OverlayShell.cpp): which ASIO interfaces there are,
// switching the bridge's interface live, and each part of the game's audio picking an interface and a channel the way
// RS_ASIO.ini can, but live: the output pair on the output interface (Output page; Switch output picks that
// interface), and each input (Player 1, Player 2, the mic) on any ASIO interface, the output's by default (Input page,
// one tab per player). RS_ASIO.ini keeps naming the bridge at fixed channels; the bridge maps them. An input on another
// interface runs on that interface's clock and is kept in time with the output (SplitInput); its pill shows how well.
// Changes apply at once with a short gap, off the render thread. Render thread unless noted.
namespace AudioBridge::Pages::Routing
{
	// The ASIO drivers the bridge can open: every 32-bit one (what RS_ASIO itself could open) plus, with `with64`, the
	// ones registered only as 64-bit, which the bridge runs in its 64-bit driver host. Cable mode's sound-only output
	// (AsioOutputDevice) loads drivers inside the game, so it asks without them. Never the bridge itself. Re-read every
	// 3 s.
	std::vector<std::wstring> InstalledAsioDrivers(bool with64 = true);
	// True when the bridge runs this driver's 64-bit build in its driver host; false: its 32-bit build in the game.
	bool RunsIn64BitHost(const std::wstring& name);

	// ---- Interface switching
	// Which ASIO interface the bridge opens, switched live: RS_ASIO.ini keeps naming the bridge, so the game never
	// notices. The switch parks the game on the bridge's virtual clock, releases the old driver and opens the new one
	// (engine op 25). A driver that will not open costs nothing: the previous interface is opened again and the reason
	// shown. Only a switch that worked is saved (HKCU Target), so the next launch starts there.
	// 0 idle, 1 switching, 2 switched, 3 refused and back on the previous one, 4 refused and nothing open.
	int InterfaceState();
	void SwitchInterface(const std::wstring& driver, const std::wstring& previous);
	// SwitchInterface's last result, for Switch output.
	std::string OutputSwitchMessage();
	// One input role's own interface (empty: the output's).
	void SwitchInputInterface(int role, const std::wstring& driver);
	// One role's channel on its interface (role 0 = the output pair).
	void ApplyRoute(int role, long channel);

	// ---- Routes
	struct RouteView
	{
		bool viaBridge[4]{};
		long host[4]{};
		long device[4]{};
		std::vector<std::string> names[4];     // per role: its interface's channel names (role 0 = outputs)
		std::wstring interfaceOf[4];           // per input role: its own input interface, empty = the output's
	};
	extern const char* const ROLE_NAMES[4];   // "Output", "Player 1", "Player 2", "Mic"

	// The route reads open RS_ASIO.ini and the registry: once a second, and right after a change lands.
	const RouteView& PolledRouteView();
	bool RouteBusy();
	// The bridge's reason an input interface would not open, then the result of the last route or interface change.
	void RouteStatusMessage();

	// ---- Input clocks
	// The pill's state for a role, shared by the clock row and the role's tab. `shown` false: the input shares the
	// output's clock, so there is nothing to drift.
	struct ClockView
	{
		bool shown = false;
		bool settled = false;
		char detail[256]{};
	};
	ClockView InputClockState(int role);
	// "In sync" / "Catching up" for a role's input on another clock, with the numbers on hover.
	void InputClockPill(int role, const char* label);
}
