#pragma once

#include <string>

// Per-feature overlay visibility toggles, so each overlay can be screenshotted or tested
// in isolation.
//
// Each named toggle:
//   * ships ON (code default), so features are visible out of the box;
//   * is overridden at startup by an RSMods .ini setting "Overlay_<name>" = on|off
//     (ApplyIniDefaults), giving persistent per-feature gating in every build incl. Release;
//   * is flippable live via the research bridge 'set_overlay' command (Debug), so a feature
//     can be isolated for a screenshot without a restart.
//
// Feature overlays (bend_meter, ml_fret) draw in ALL build configs; research-only
// indicators stay additionally _DEBUG-gated at their call site.
namespace OverlayToggles
{
	bool Get(const std::string& name);            // unknown name -> true (fail-visible)
	bool Set(const std::string& name, bool on);   // false if the name is not registered
	void ApplyIniDefaults();                       // read RSMods .ini "Overlay_<name>" at startup
	std::string List();                            // "name=on\nname2=off\n..." for the bridge
}
