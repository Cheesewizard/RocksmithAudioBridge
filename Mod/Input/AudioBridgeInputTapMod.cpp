#include "AudioBridgeInputTapMod.hpp"

#include "CaptureTap.hpp"
#include "InputStageMeter.hpp"

using Framework::ModContext;

// Two taps would feed the chain twice.
std::vector<std::string_view> AudioBridgeInputTapMod::ClaimsExclusive() const {
	return { "audio-input-tap" };
}

// OnInitialize runs where 4.x installed its capture hook (after the startup waits), before the game opens its
// capture stream. Streams are only prepared in the chain from OnTick, after every mod has added its processors.
// The stage meters (InputStageMeter.hpp) belong to the tap: they feed the Input page's level meter and the HUD's SIGNAL
// rows from the chain, as 4.x's capture hook did, around the conditioner.
void AudioBridgeInputTapMod::OnInitialize(ModContext& c) {
	c.Audio().AddInputProcessor(AudioBridge::Input::StageMeter::CaptureRawObserver(), AudioBridge::Input::StageMeter::CAPTURE_RAW_ORDER);
	c.Audio().AddInputProcessor(AudioBridge::Input::StageMeter::AfterConditionerObserver(), AudioBridge::Input::StageMeter::AFTER_CONDITIONER_ORDER);
	AudioBridge::Input::CaptureTap::Install();
}

void AudioBridgeInputTapMod::OnShutdown(ModContext&) {
	AudioBridge::Input::CaptureTap::Shutdown();
}

void AudioBridgeInputTapMod::OnEnabled(ModContext&) {
	AudioBridge::Input::CaptureTap::SetActive(true);
}

void AudioBridgeInputTapMod::OnDisabled(ModContext&) {
	AudioBridge::Input::CaptureTap::SetActive(false);
}

void AudioBridgeInputTapMod::OnTick(ModContext&) {
	AudioBridge::Input::CaptureTap::Tick();
	AudioBridge::Input::StageMeter::Tick();
}

static Framework::ModRegistrar<AudioBridgeInputTapMod> _audioBridgeInputTapReg;
