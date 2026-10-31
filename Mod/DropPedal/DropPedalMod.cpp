#include "DropPedalMod.hpp"

#include "../Core/BespokeSettings.hpp"
#include "DropPedalHooks.hpp"
#include "DropPedalPage.hpp"
#include "DropPedalSettings.hpp"
#include "DropPedalState.hpp"
#include "GameTuning.hpp"
#include "SpeakerMode/WwiseMusicHook.hpp"
#include "../Core/HelperExe.hpp"
#include "../Input/CaptureTap.hpp"

#include "GameState.hpp"
#include "Log.hpp"

using DropPedal::Player;
using Framework::Availability;
using Framework::KeyEdge;
using Framework::KeyEvent;
using Framework::ModContext;
using Framework::SettingDef;
using Framework::SettingDefs;
namespace Keys = AudioBridge::DropPedal::SettingKeys;

namespace {
	// Above MidiMod (0), which claims tuning-controller only while its auto-tune is on: with both on, the pedal wins
	// and auto-tune stands down, since retuning the input and the guitar at once would shift twice.
	constexpr int kPriority = 10;
	// After the input conditioner (-100), so the shifter hears the cleaned, lifted signal.
	constexpr int kChainOrder = 0;
	// The tap's input indexes for Player 1 and Player 2.
	constexpr std::uint32_t kPlayerOneInput = 0;
	constexpr std::uint32_t kPlayerTwoInput = 1;

	// Downwards from E, as the overlay's picker lists them.
	const std::vector<std::string> kTuningNames = { "E", "Eb", "D", "C#", "C", "B", "Bb", "A", "Ab", "G", "F#", "F" };

	const char* PlayerName(Player player) {
		return player == Player::One ? "Player 1" : "Player 2";
	}
}

DropPedalMod::DropPedalMod() : controls(inputShifter) {
}

SettingDefs DropPedalMod::Settings() const {
	SettingDefs settings = {
		SettingDef::Toggle(Keys::ENABLED, "EnableDropPedal", "Drop Pedal", "Drop Pedal", "Drop Pedal", "on",
			"Retunes your guitar's input before the game hears it, so you can play a chart in another tuning without\n"
			"retuning the guitar, or (Speaker Mode) shifts the song to your guitar instead. Starts off each session;\n"
			"the toggle key cycles Drop Pedal, Speaker Mode and off.")
			.Heading(Framework::SettingHeading::Tuning),
		SettingDef::Enum(Keys::BASE_TUNING, "My guitar is in")
			.Ini("Drop Pedal", "BaseTuning")
			.Choices(kTuningNames, "E")
			.Hint("The standard tuning your guitar is physically in. Shifts are named from it.")
			.WithVisibleWhen(Keys::ENABLED),
		SettingDef::Enum(Keys::BASE_TUNING_PLAYER2, "Player 2's guitar is in")
			.Ini("Drop Pedal", "BaseTuningPlayer2")
			.Choices(kTuningNames, "E")
			.Hint("The standard tuning Player 2's guitar is physically in, when a second input is set up.")
			.WithVisibleWhen(Keys::ENABLED),
		Framework::KeyBind(Keys::TOGGLE_KEY, "Drop Pedal / Speaker Mode / off", "VK_F7").ListedUnder(Keys::ENABLED),
		Framework::KeyBind(Keys::PITCH_DOWN_KEY, "Drop Pedal down a semitone (Ctrl: Player 2)", "VK_OEM_COMMA").ListedUnder(Keys::ENABLED),
		Framework::KeyBind(Keys::PITCH_UP_KEY, "Drop Pedal up a semitone (Ctrl: Player 2)", "VK_OEM_PERIOD").ListedUnder(Keys::ENABLED),
	};
	for (SettingDef& setting : AudioBridge::DropPedal::DropPedalHud::Settings(Keys::ENABLED)) settings.push_back(std::move(setting));
	return AudioBridge::EditedByAudioBridge(std::move(settings));
}

// "off" is a revert switch: the pedal ships on and starts each session with no shift.
bool DropPedalMod::IsEnabled(const ModContext& c) const {
	return c.IsOn(Keys::ENABLED);
}

std::vector<std::string_view> DropPedalMod::ClaimsExclusive() const {
	return { "tuning-controller" };
}

int DropPedalMod::Priority() const {
	return kPriority;
}

// The hooks install once and stay (Retry-faulted runs OnInitialize again). Speaker Mode's Wwise hook must be in before
// Wwise registers its codecs; OnInitialize runs where 4.x installed it.
void DropPedalMod::OnInitialize(ModContext& c) {
	c.Audio().AddInputProcessor(inputShifter, kChainOrder);
	LoadBaseTunings(c);
	hud.LoadSettings(c);
	AudioBridge::DropPedal::RegisterPage(controls);

	if (!hooksInstalled) {
		hooksInstalled = true;
		DropPedalHooks::Install();
		Audio::SongShift::WwiseMusicHook::Install(speakerMode, &AudioBridge::GameTuning::DetourFunction,
			AudioBridge::HelperExePath());
	}

	// Each key republishes the readout at once: the HUD otherwise follows on the next maintenance tick (250 ms), where
	// 4.x redrew it every frame.
	c.Commands().BindSetting(Keys::TOGGLE_KEY, KeyEdge::Up, Availability::Active,
		[this](ModContext& ctx, const KeyEvent&) { controls.CycleMode(); hud.Publish(ctx); }, {}, "Drop Pedal / Speaker Mode / off");
	c.Commands().BindSetting(Keys::PITCH_DOWN_KEY, KeyEdge::Up, Availability::Active,
		[this](ModContext& ctx, const KeyEvent& event) { StepShift(ctx, event, -1); hud.Publish(ctx); }, {}, "Drop Pedal down");
	c.Commands().BindSetting(Keys::PITCH_UP_KEY, KeyEdge::Up, Availability::Active,
		[this](ModContext& ctx, const KeyEvent& event) { StepShift(ctx, event, 1); hud.Publish(ctx); }, {}, "Drop Pedal up");
}

void DropPedalMod::OnSettingsChanged(ModContext& c) {
	LoadBaseTunings(c);
	hud.LoadSettings(c);
}

// DropPedalState's "configured" flag gates every change; it follows this mod's Active state.
void DropPedalMod::OnEnabled(ModContext&) {
	DropPedalState::Configure("on");
	controls.ApplyShifts();
}

// Off also restores the arrangement's own detection reference, so nothing of Speaker Mode outlives the mod.
void DropPedalMod::OnDisabled(ModContext&) {
	DropPedalState::Configure("off");
	controls.ApplyShifts();
	ResetSong();
}

// 4.x's game loop set the "input shifter active" flag from its capture hook every tick (processing enabled: the capture
// route is set up) and reported a missing input once. Here: Player 1's tap route is configured, or the shifter has run in
// the last second. Note by Note's applied shift and the readout's "Input unavailable" read it. The game opens its input
// well after boot, so a missing route is only reported once the game has reached its menus.
void DropPedalMod::OnTick(ModContext& c) {
	// Speaker Mode's cache pass reads the song key through upstream's unguarded pointer walk, which isn't safe while the
	// game boots (the same pattern as the IsMultiplayer crash). Its Wwise hook needs nothing from the tick until then.
	if (c.phase != Framework::GamePhase::Loading) Audio::SongShift::WwiseMusicHook::Poll();
	DropPedalHooks::Poll();
	const bool applying = AudioBridge::Input::CaptureTap::IsPlayerConfigured(kPlayerOneInput) || inputShifter.IsApplying(Player::One);
	DropPedalHooks::SetInputShifterActive(applying);
	if (!applying && GameState::GameLoaded) DropPedalHooks::ReportInputShifterUnavailable();
	hud.Publish(c);
}

// The pre-song tuner already follows the song: the detection reference, and so the tuner's targets, move with the
// shift there. Any other menu ends the song.
void DropPedalMod::OnMenuTick(ModContext&) {
	const bool inTuner = GameState::Menus::IsInPreSongTuner();
	controls.UpdateChartMatch(inTuner);
	if (inTuner) {
		DropPedalState::SetGameplayInProgress(false);
		DropPedalHooks::HandleArrangementTuning();
		return;
	}
	ResetSong();
}

void DropPedalMod::OnSongTick(ModContext&) {
	controls.UpdateChartMatch(true);
	DropPedalState::SetGameplayInProgress(true);
	DropPedalHooks::HandleArrangementTuning();
}

void DropPedalMod::ResetSong() {
	DropPedalState::SetGameplayInProgress(false);
	speakerMode.ResetSong();
	DropPedalHooks::ResetSongState();
}

void DropPedalMod::LoadBaseTunings(const ModContext& c) {
	const std::pair<Player, const char*> keys[] = { { Player::One, Keys::BASE_TUNING }, { Player::Two, Keys::BASE_TUNING_PLAYER2 } };
	for (const auto& [player, key] : keys) {
		const std::string name = c.Value(key);
		int semitonesFromE = 0;
		if (!DropPedalState::TryParseTuningName(name, semitonesFromE)) {
			LOG_ERROR("[DropPedal] " << PlayerName(player) << "'s base tuning \"" << name << "\" is not a standard tuning name." << std::endl);
			continue;
		}
		if (!DropPedalState::SetBaseTuningSemitones(player, semitonesFromE))
			LOG_WARNING("[DropPedal] " << PlayerName(player) << "'s base tuning can't change right now; keeping "
				<< DropPedalState::GetBaseTuningName(player) << "." << std::endl);
	}
}

// Ctrl picks Player 2; refused in Speaker Mode (one shift for the song) and without a second input.
void DropPedalMod::StepShift(const ModContext& c, const KeyEvent& event, int semitoneDelta) {
	const Player player = event.control ? Player::Two : Player::One;
	if (player == Player::Two && DropPedalState::IsSpeakerModeEnabled()) {
		LOG_INFO("[DropPedal] Speaker Mode is one setting for the song; only Player 1 changes it." << std::endl);
		DropPedalState::NotifyModeLocked();
		return;
	}
	if (player == Player::Two && !c.Audio().InputTapLive(kPlayerTwoInput) && !AudioBridge::Input::CaptureTap::IsPlayerLive(kPlayerTwoInput)) {
		LOG_ERROR("[DropPedal] Player 2 controls need a second input route (RS_ASIO.ini [Asio.Input.1])." << std::endl);
		return;
	}
	controls.AdjustShift(player, semitoneDelta);
}

static Framework::ModRegistrar<DropPedalMod> _dropPedalReg;
