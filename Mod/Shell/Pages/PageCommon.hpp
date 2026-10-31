#pragma once

#include "../OverlayUi.hpp"
#include "../../AudioBridge/Cable/CableInput.hpp"
#include "../../AudioBridge/Engine/AudioControl.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

// What the Audio Bridge pages share (ported from 4.x's OverlayShell.cpp): engine access, the live status poll, the
// passthrough device's name, key binding pickers and a few text rows. Everything here runs on the render thread unless
// noted.
namespace AudioBridge::Pages
{
	// ---- Engine access
	// Same operation codes the desktop bridge sends over the pipe; the overlay calls the engine directly.
	Audio::ControlResponse BridgeControl(std::uint32_t op, const wchar_t* value = L"");
	void SendInt(std::uint32_t op, int value);
	// Slider drags apply live but save nothing (the engine's preview dispatch); the release saves, once.
	void PreviewControl(std::uint32_t op, const wchar_t* value);
	void PreviewInt(std::uint32_t op, int value);

	// A [Mod Settings] number a framework mod owns (InputConditionerMod, AudioBridgeRoutingMod), saved through upstream's
	// setting edits: written to RSMods.ini (or handed to the RSMods GUI while it owns the file) and applied on MainThread
	// through the owner's OnSettingsChanged. Pair it with a Preview* op to hear the change at once.
	void SaveModInt(const char* key, const char* iniName, int value, const char* label);

	// ---- Live status
	// Op 1 (status) and the guitar input's diagnostics ~10 Hz, op 5 (output details) twice a second; never waits on a
	// slow op: the last answer stays until the engine is free.
	struct LiveState
	{
		Audio::ControlResponse status{};
		Audio::ControlResponse output{};
		Audio::CableInput::Diagnostics input{};
	};
	const LiveState& Poll();
	// A "name=<number>" field of op 5's text, 0 when missing.
	unsigned OutputStatusNumber(const LiveState& live, const wchar_t* field);

	// ---- Files and names
	std::filesystem::path GameFolder();
	// The device the game plays straight out of in passthrough: RS_ASIO.ini [Asio.Output] Driver, resolved through the
	// proxy's HKCU Target when it is the bridge. Read at most once a second. RSMods never writes RS_ASIO.ini.
	std::wstring ReadPassthroughOutputName();
	std::string Utf8(const std::wstring& text);
	// Linear peak to a -60..0 dBFS meter fraction.
	float MeterFraction(float peak);
	std::string FormatTime(double seconds);
	// ShellExecuteW "open" on a thread of its own (it can take 100 ms or more on the render thread).
	void OpenAsync(std::wstring file);

	// ---- Rows
	// Label on the left, value on the right of the card row.
	void InfoRow(const char* label, const std::string& value, ImU32 valueColor = Overlay::Color::Text);
	void WarnCaption(const char* text);
	// Wrapped text across the card in `color`.
	void ColorText(const char* text, ImU32 color);

	// ---- Key bindings
	// A key's name as the keyboard prints it, or "not set".
	std::string KeyName(unsigned int vk);
	// MainThread (a page's Register function): the declared key bindings, for the pickers' clash check. The settings
	// schema is MainThread-only, so the pickers read this copy.
	void SnapshotKeyBinds();
	// A key binding's picker: the label and key on one line, Change key under it. A key the game, Windows, Steam or
	// another binding already uses is refused, and so is any `extraReason` gives. Saved through upstream's setting
	// edits, so the owner rebinds it at once.
	void KeyBindPicker(const char* bindName, const char* label, const char* (*extraReason)(unsigned int) = nullptr);
}
