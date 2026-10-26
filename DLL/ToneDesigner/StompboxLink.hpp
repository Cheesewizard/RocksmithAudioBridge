#pragma once

#include <map>
#include <string>
#include <vector>

// Overlay side of the Tone designer's engine (DLL/Shared/StompboxControl.hpp; the host side is
// VstHost/Source/StompboxService.cpp). Reads the pedal catalogue, the current rig and the saved tones the host
// publishes, and sends the page's edits as Stompbox commands, one at a time (knob drags are coalesced to the latest
// value). Render thread only: OnFrame and every call below.
namespace StompboxLink
{
	enum class ParameterType { Knob, Bool, Int, VerticalSlider, Enum, File, Power };

	struct ParameterDefinition
	{
		std::string name;
		std::string description;
		std::string displayFormat;          // Stompbox's .NET-style format, "{0:0.0}dB" (FormatValue reads it)
		ParameterType type = ParameterType::Knob;
		float minimum = 0.0f;
		float maximum = 1.0f;
		float defaultValue = 0.0f;
		float rangePower = 1.0f;
		bool isAdvanced = false;
		bool isOutput = false;              // a meter the effect writes, not a control
		std::vector<std::string> choices;   // Enum values; File: the files in the pedal's data folder
		std::string fileFolder;             // File: the folder under the Stompbox data folder ("NAM", "Cabinets")
	};

	// One kind of pedal: an entry of Stompbox's factory.
	struct PedalDefinition
	{
		std::string name;                   // "Delay"; instances are "Delay", "Delay_2", ...
		std::string description;
		std::string backgroundColor;        // "#rrggbb", empty when Stompbox gives none
		std::string foregroundColor;
		bool isUserSelectable = true;
		std::vector<ParameterDefinition> parameters;
	};

	// One pedal on the board, in chain order.
	struct Pedal
	{
		std::string id;                     // the instance ("Delay_2")
		std::string kind;                   // its definition's name ("Delay")
		bool isEnabled = true;
		std::map<std::string, std::string> values;   // parameter name -> value as Stompbox writes it
	};

	enum class EngineState { Off, Loading, Ready, Failed };

	struct Snapshot
	{
		bool isConnected = false;           // the host has the shared block open
		EngineState engineState = EngineState::Off;
		bool isWanted = false;              // the page asked for the Tone designer engine
		bool isPlaying = false;             // the stream goes through it now
		bool isPresetLoading = false;       // Stompbox is loading a preset or a model
		bool isBusy = false;                // commands not yet run by the host
		std::string error;
		std::vector<PedalDefinition> definitions;
		std::vector<Pedal> chain;
		std::string currentPreset;
		std::vector<std::string> presets;
	};

	void OnFrame();
	const Snapshot& GetSnapshot();
	const PedalDefinition* FindDefinition(const std::string& kind);

	// Remembered (HKCU\Software\RSMods\VstHost ToneDesignerEngine), so the stream goes through the engine from the
	// host's start when the player last chose it.
	void SetWanted(bool wanted);

	void SetChain(const std::vector<std::string>& pedalIds);
	void SetParameter(const std::string& pedalId, const std::string& parameter, const std::string& value);
	void SetEnabled(const std::string& pedalId, bool enabled);
	void LoadPreset(const std::string& name);
	void SavePreset(const std::string& name);
	void DeletePreset(const std::string& name);

	// A free instance id for a new pedal of `kind` on this chain: "Delay", then "Delay_2", ...
	std::string NewPedalId(const std::string& kind);
	// A value as the page shows it, using the parameter's display format ("{0:0.0}dB" -> "3.5dB").
	std::string FormatValue(const ParameterDefinition& parameter, float value);
}
