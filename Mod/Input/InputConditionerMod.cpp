#include "InputConditionerMod.hpp"

#include "../Core/BespokeSettings.hpp"
#include "ToneGates.hpp"

#include "Log.hpp"

using Framework::ModContext;
using Framework::SettingDef;
using Framework::SettingDefs;

namespace {
	// Every mod shares one key space, so the keys carry the product's name; the RSMods.ini names are 4.x's, so
	// existing settings carry over.
	constexpr char kInputGain[] = "AudioBridgeInputGain";
	constexpr char kGateThreshold[] = "AudioBridgeNoiseGateThreshold";
	constexpr char kGateDecay[] = "AudioBridgeNoiseGateDecay";
	constexpr char kGateReduction[] = "AudioBridgeNoiseGateReduction";
	constexpr char kGateAttack[] = "AudioBridgeNoiseGateAttack";
	constexpr char kCompressorStrength[] = "AudioBridgeCompressorStrength";
	// Where the gate returns to when it's switched back on; the overlay's gate switch writes it.
	constexpr char kGateLastThreshold[] = "AudioBridgeNoiseGateLastThreshold";

	// Runs ahead of every other input processor (Drop Pedal's shifter is at 0), so a pitch shift always gets the
	// cleaned, lifted signal.
	constexpr int kChainOrder = -100;
}

SettingDefs InputConditionerMod::Settings() const {
	return AudioBridge::EditedByAudioBridge({
		SettingDef::Numeric(kInputGain, "Guitar input gain (dB)")
			.Ini("Mod Settings", "AsioInputGain")
			.Range(-240, 240)
			.Scale(0.1)
			.Hint("Lifts a quiet interface input towards a Real Tone Cable's level, so the game's note gate doesn't cut\n"
				"sustains and bends early. 0 = off.")
			.Heading(Framework::SettingHeading::Audio),
		SettingDef::Numeric(kGateThreshold, "Noise gate threshold (dBFS)")
			.Ini("Mod Settings", "NoiseGateThreshold")
			.Range(-900, 0)
			.Scale(0.1)
			.Hint("The level the gate opens at; it closes 6 dB below. -90 to -20 dBFS, 0 = off."),
		SettingDef::Numeric(kGateDecay, "Noise gate decay (ms)")
			.Ini("Mod Settings", "NoiseGateDecay")
			.Range(50, 2000)
			.Default("200")
			.Hint("How long the gate takes to fade down once it closes."),
		SettingDef::Numeric(kGateReduction, "Noise gate reduction (dB)")
			.Ini("Mod Settings", "NoiseGateReduction")
			.Range(-80, -10)
			.Default("-60")
			.Hint("How far down the gate turns the signal while closed."),
		SettingDef::Numeric(kGateAttack, "Noise gate attack (ms)")
			.Ini("Mod Settings", "NoiseGateAttack")
			.Range(1, 200)
			.Scale(0.1)
			.Default("5")
			.Hint("How long the gate takes to fade back up when it opens."),
		SettingDef::Numeric(kCompressorStrength, "Guitar input compressor (%)")
			.Ini("Mod Settings", "CompressorStrength")
			.Range(0, 100)
			.Hint("Flattens the natural string-beat wobble before the game's amp, so a quiet interface input doesn't\n"
				"warble. 0 = off."),
		SettingDef::Numeric(kGateLastThreshold, "Noise gate threshold to switch back on at")
			.Ini("Mod Settings", "NoiseGateLastThreshold")
			.Range(-900, 0)
			.Default("-620")
			.EditedBy("AudioBridgeOverlay"),
	});
}

// Always active: a Tone designer tone can switch its own gate on while the global one is off. With every stage off the
// conditioner leaves the input untouched.
// The chain is fixed when the tap first prepares a stream, long after OnInitialize, so the processor is always in it.
void InputConditionerMod::OnInitialize(ModContext& c) {
	c.Audio().AddInputProcessor(AudioBridge::Input::SharedConditioner(), kChainOrder);
}

void InputConditionerMod::OnEnabled(ModContext& c) {
	Apply(c);
}

void InputConditionerMod::OnSettingsChanged(ModContext& c) {
	Apply(c);
}

// The global gate goes through ToneGates, which applies it unless the playing tone has its own.
void InputConditionerMod::Apply(const ModContext& c) {
	ToneGates::Gate gate;
	const int thresholdTenths = c.Int(kGateThreshold);
	const int lastThresholdTenths = c.Int(kGateLastThreshold);
	gate.on = thresholdTenths < 0;
	gate.thresholdTenths = gate.on ? thresholdTenths : (lastThresholdTenths < 0 ? lastThresholdTenths : -620);
	gate.decayMs = c.Int(kGateDecay);
	gate.reductionDb = c.Int(kGateReduction);
	gate.attackTenths = c.Int(kGateAttack);

	AudioBridge::Input::InputConditioner& conditioner = AudioBridge::Input::SharedConditioner();
	conditioner.SetInputGainDb(c.Int(kInputGain) / 10.0f);
	conditioner.SetCompressorStrength(c.Int(kCompressorStrength) / 100.0f);
	ToneGates::InitGlobal(gate);

	LOG_INFO("[InputConditioner] gain " << c.Int(kInputGain) / 10.0f << " dB, global gate "
		<< (gate.on ? std::to_string(thresholdTenths / 10.0f) + " dBFS" : std::string("off"))
		<< ", compressor " << c.Int(kCompressorStrength) << "%." << std::endl);
}

static Framework::ModRegistrar<InputConditionerMod> _inputConditionerReg;
