#include "stdafx.h"
#include "OverlayToggles.hpp"
#include "Settings.hpp"

#include <map>
#include <sstream>

namespace
{
	// The registry. Add a row here to give a new overlay feature a console/.ini toggle.
	std::map<std::string, bool>& Registry()
	{
		static std::map<std::string, bool> toggles = {
			{ "bend_meter", true },   // NBN bend visualizer
			{ "ml_fret",    true },   // string/fret ML companion read
			{ "nbn_missed_label", true }, // RR HUD "MISSED" -> "NOTE BY NOTE" while NBN on (native font)
			{ "audio_diag", true },   // bottom-left audio latency / signal lines (RSModsPlus)
			{ "menu_renumber", true }, // RR Advanced Settings builder hook: renumber our row SortOrder
			{ "menu_preset",   true }, // RR Advanced Settings builder hook: preset InitialValue/Value to On
		};
		return toggles;
	}
}

bool OverlayToggles::Get(const std::string& name)
{
	auto& t = Registry();
	auto it = t.find(name);
	return it == t.end() ? true : it->second;   // fail-visible: an unknown tag still draws
}

bool OverlayToggles::Set(const std::string& name, bool on)
{
	auto& t = Registry();
	auto it = t.find(name);
	if (it == t.end()) return false;
	it->second = on;
	return true;
}

void OverlayToggles::ApplyIniDefaults()
{
	for (auto& [name, value] : Registry())
	{
		const std::string v = Settings::ReturnSettingValue("Overlay_" + name);
		if (v == "on") value = true;
		else if (v == "off") value = false;
		// anything else (missing / blank) keeps the code default (ships ON)
	}
}

std::string OverlayToggles::List()
{
	std::ostringstream os;
	for (auto& [name, value] : Registry())
		os << name << "=" << (value ? "on" : "off") << "\n";
	return os.str();
}
