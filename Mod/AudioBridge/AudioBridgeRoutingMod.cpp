#include "AudioBridgeRoutingMod.hpp"

#include "../Core/BespokeSettings.hpp"
#include "Cable/CableCaptureSession.hpp"
#include "Cable/CableInput.hpp"
#include "Driver/BridgeDriverRegistration.hpp"
#include "Engine/OutputTap.hpp"
#include "Input/RocksmithGate.hpp"
#include "Ui/AudioDiagnosticsHud.hpp"
#include "../Core/GameView.hpp"
#include "../Input/LatencyCapture.hpp"
#include "../Shell/Pages/GeneralPage.hpp"
#include "../Shell/Pages/InputPage.hpp"
#include "../Shell/Pages/MixerPage.hpp"
#include "../Shell/Pages/OutputPage.hpp"
#include "../Shell/HudDraw.hpp"

#include "GameState.hpp"

#include "Log.hpp"

#include <cmath>

using Framework::ModContext;
using Framework::SettingDef;
using Framework::SettingDefs;

namespace {
	// The RSMods.ini names are 4.x's [Mod Settings] ones, so existing settings carry over.
	constexpr char kGateOverride[] = "AudioBridgeRocksmithGateOverride";
	constexpr char kGateThreshold[] = "AudioBridgeRocksmithGateThreshold";
	constexpr char kLimiter[] = "AudioBridgeLimiter";
	constexpr char kLimiterLevel[] = "AudioBridgeLimiterLevel";
	constexpr char kLoudnessMatch[] = "AudioBridgeLoudnessMatch";
	constexpr char kLoudnessTarget[] = "AudioBridgeLoudnessTarget";

	// After the input conditioner (-100), before Drop Pedal (0): the loopback measurement hears the conditioned input,
	// unshifted, as 4.x's capture hook did.
	constexpr int kLatencyCaptureOrder = -50;

	// The diagnostics rows draw on the background draw list, so their order among the overlays doesn't show.
	constexpr int kDiagnosticsOverlayOrder = 100;
	// Under the Drop Pedal rows (TopLeft 0 and 1) in the HUD's top-left stack.
	constexpr char kUsbRestartHudId[] = "audio-bridge-usbdk-restart";
	constexpr int kUsbRestartHudOrder = 10;
	constexpr int kNoticeColor = static_cast<int>(0xFFFFC033);

	float DbToLinear(float decibels) {
		return std::pow(10.0f, decibels / 20.0f);
	}
}

SettingDefs AudioBridgeRoutingMod::Settings() const {
	return AudioBridge::EditedByAudioBridge({
		// 4.x stores these three switches as 0/1, so they stay numbers to read existing RSMods.ini files.
		SettingDef::Numeric(kGateOverride, "Override Rocksmith's noise gate (0 / 1)")
			.Ini("Mod Settings", "RocksmithGateOverride")
			.Range(0, 1)
			.Hint(
			"Takes over the game's own amp noise gate (P1_NoiseFloor) with the threshold below, instead of the level\n"
			"Rocksmith calibrated. Lower opens the gate for longer sustain.")
			.Heading(Framework::SettingHeading::Audio),
		SettingDef::Numeric(kGateThreshold, "Rocksmith noise gate (dB)")
			.Ini("Mod Settings", "RocksmithGateThreshold")
			.Range(-1000, 100)
			.Scale(0.1)
			.Default("-593")
			.Hint("The forced P1_NoiseFloor while the override is on. -59.3 dB is the game's default.")
			.WithVisibleWhen(kGateOverride, "1"),
		SettingDef::Numeric(kLimiter, "Output limiter (0 / 1)")
			.Range(0, 1)
			.Hint("A look-ahead brickwall limiter on the Audio Bridge's output: nothing leaves above the ceiling below."),
		SettingDef::Numeric(kLimiterLevel, "Output limiter ceiling (dBFS)")
			.Ini("Mod Settings", "AudioBridgeLimiterLevel")
			.Range(-300, 0)
			.Scale(0.1)
			.Default("-60")
			.WithVisibleWhen(kLimiter, "1"),
		SettingDef::Numeric(kLoudnessMatch, "Match song loudness (0 / 1)")
			.Range(0, 1)
			.Hint("A slow loudness leveller on the Audio Bridge's output, so songs play at a similar level."),
		SettingDef::Numeric(kLoudnessTarget, "Loudness target (dBFS RMS)")
			.Ini("Mod Settings", "AudioBridgeLoudnessTarget")
			.Range(-400, 0)
			.Scale(0.1)
			.Default("-200")
			.WithVisibleWhen(kLoudnessMatch, "1"),
	});
}

// Installs once and stays (Retry-faulted runs OnInitialize again). The cable's Pa_OpenStream detour has to be in before
// the game opens its input, which is why 4.x installed it with the boot-time bug fixes; OnInitialize lands at the same
// point. The bridge driver's registration is healed first: RS_ASIO enumerates drivers ~30 s into boot.
// The one-off framework budget warning (~170 ms) is this install. Most of it was PersistentInput::Install scanning the
// exe's ~30 MB of code byte by byte, twice, for the device-scan call sites (now one memchr pass, a few times faster). It
// stays here, on MainThread: those call sites and the Pa_OpenStream detour must be patched before the game's device
// scan and stream opens, and a background install would race them. The install's own log line gives its timing.
void AudioBridgeRoutingMod::OnInitialize(ModContext& c) {
	c.Audio().AddInputProcessor(AudioBridge::Input::SharedLatencyCapture(), kLatencyCaptureOrder);

	// The overlay's routing pages; registering again (a retried OnInitialize) replaces them.
	AudioBridge::Pages::RegisterInputPage();
	AudioBridge::Pages::RegisterOutputPage();
	AudioBridge::Pages::RegisterMixerPage();
	AudioBridge::Pages::RegisterGeneralPage();

	// The game rewrites P1_NoiseFloor on calibration and song changes, so a forced gate is re-asserted every frame.
	c.Draw().RegisterFrame([](IDirect3DDevice9*) {
		RocksmithGate::ApplyPerFrame();
		AudioBridge::HudDraw::PrepareFonts();
	});

	// 4.x's D3DX audio diagnostics HUD (signal, Player 2, latency readout), top right.
	c.Menu().RegisterOverlay("audio-bridge-diagnostics", kDiagnosticsOverlayOrder, [] { AudioBridge::DiagnosticsHud::Draw(); });

	if (installed) return;
	installed = true;
	AsioProxyRegistration::Heal();
	Audio::CableInput::Install();
	ApplyRocksmithGate(c);
}

void AudioBridgeRoutingMod::OnSettingsChanged(ModContext& c) {
	ApplyRocksmithGate(c);
	outputGuardApplied = false;
}

void AudioBridgeRoutingMod::OnTick(ModContext& c) {
	Audio::CableInput::Poll();
	if (!outputGuardApplied) TryApplyOutputGuard(c);
	// The game's memory isn't safe to read while it boots.
	if (c.phase != Framework::GamePhase::Loading) {
		AudioBridge::GameView::Publish();
		AudioBridge::DiagnosticsHud::PublishGameState();
		PublishUsbRestartNotice(c);
	}
}

// 4.x drew this amber line from its Note by Note status block (with any build extensions' lines, which the framework
// build has none of), at the left a sixth of the way down. Menus only, so it never covers a song: the restart that
// finishes the UsbDk install is easy to skip. A plain line, so it goes in the HUD's top-left stack.
void AudioBridgeRoutingMod::PublishUsbRestartNotice(ModContext& c) {
	Framework::HudText text;
	text.visible = Audio::PersistentInput::IsDirectUsbWaitingForRestart() && !GameState::IsInSong();
	if (text.visible) {
		text.text = "Restart Windows to finish installing the Real Tone Cable's low-latency driver (UsbDk).";
		text.colorHex = kNoticeColor;
	}
	c.Hud().Set(kUsbRestartHudId, { Framework::HudAnchor::TopLeft, kUsbRestartHudOrder }, std::move(text));
}

void AudioBridgeRoutingMod::ApplyRocksmithGate(const ModContext& c) {
	RocksmithGate::SetOverride(c.Int(kGateOverride) != 0, c.Int(kGateThreshold) / 10.0f);
}

// The output guard lives in the bridge driver, so it can only be configured once the game has loaded the driver.
void AudioBridgeRoutingMod::TryApplyOutputGuard(const ModContext& c) {
	if (!Audio::OutputTap::ProxyAvailable()) return;
	outputGuardApplied = Audio::OutputTap::ConfigureOutputGuard(c.Int(kLimiter) != 0, DbToLinear(c.Int(kLimiterLevel) / 10.0f),
		c.Int(kLoudnessMatch) != 0, DbToLinear(c.Int(kLoudnessTarget) / 10.0f));
	if (outputGuardApplied) LOG_INFO("(AUDIO ROUTING) Saved output limiter applied to the bridge driver" << std::endl);
}

static Framework::ModRegistrar<AudioBridgeRoutingMod> _audioBridgeRoutingReg;
