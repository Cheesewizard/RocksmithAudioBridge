#include "../../stdafx.h"
#include "../../Audio/AsioHook.hpp"
#include "../../Audio/DelayLinePitchShifter.hpp"
#include "DropPedal.hpp"
#include "DropPedalHooks.hpp"
#include "DropPedalInput.hpp"
#include "DropPedalOverlay.hpp"
#include "DropPedalState.hpp"
#include "../../GameState.hpp"
#include "../../SongTuning.hpp"

namespace
{
	constexpr int MIN_CHART_TUNING_SEMITONES = -24;
	constexpr int MAX_CHART_TUNING_SEMITONES = 24;
	constexpr int SEMITONES_PER_OCTAVE = 12;

	// Any chart shape is supported: the player physically tunes each string to
	// chart + shift -- the pre-song tuner guides that, because its per-string
	// targets carry the shift -- and the uniform song shift plus the global
	// detection transpose then match every string by the same interval. A drop
	// chart needs one string retuned, an open tuning a few more; that trade is
	// the player's to make.
	enum class ChartTuningShape
	{
		Uniform,
		Drop,
		Custom
	};

	std::string speakerTargetSongKey;
	bool hasSynchronizedSpeakerTarget = false;
	ChartTuningShape synchronizedChartShape = ChartTuningShape::Uniform;
	int synchronizedChartOffsets[6] = {};
	int synchronizedSpeakerTargetSemitones = 0;
	int synchronizedSpeakerOctaveAdjustment = 0;
	int pendingChartOffsets[6] = {};
	bool hasPendingChartTuning = false;
	unsigned long long speakerTargetReadAfterTick = 0;
	unsigned long long synchronizedSpeakerModeTick = 0;

	// The reference is the offset the most strings share, so the player's base
	// maps onto it and the fewest strings need physical retuning. Ties resolve
	// in favour of the upper strings, which keeps drop shapes anchored to the
	// uniform five.
	int GetChartReference(const int (&stringTunings)[6])
	{
		const int candidateOrder[6] = { 1, 2, 3, 4, 5, 0 };
		int reference = stringTunings[1];
		int referenceCount = 0;
		for (const int candidate : candidateOrder)
		{
			int count = 0;
			for (const int stringTuning : stringTunings)
			{
				if (stringTuning == stringTunings[candidate]) count++;
			}
			if (count > referenceCount)
			{
				referenceCount = count;
				reference = stringTunings[candidate];
			}
		}
		return reference;
	}

	ChartTuningShape ClassifyChartTuning(const int (&stringTunings)[6], int reference)
	{
		for (int string = 1; string < 6; string++)
		{
			if (stringTunings[string] != reference) return ChartTuningShape::Custom;
		}

		if (stringTunings[0] == reference) return ChartTuningShape::Uniform;
		if (stringTunings[0] == reference - 2) return ChartTuningShape::Drop;
		return ChartTuningShape::Custom;
	}

	bool IsSpeakerChartShapeActive()
	{
		return hasSynchronizedSpeakerTarget
			&& DropPedalState::GetPitchMode() == DropPedal::PitchMode::SpeakerMode;
	}

	// Names a shape. Uniform names are computed; drop and custom shapes prefer the
	// game's tuning list so conventional names appear (Drop D, Eb Drop Db, Open G),
	// falling back to a computed drop name or a "custom" marker for unlisted shapes.
	std::string NameChartShape(
		ChartTuningShape shape,
		const int (&offsets)[6],
		int reference)
	{
		if (shape == ChartTuningShape::Uniform)
		{
			return DropPedalState::GetAbsoluteTuningName(reference);
		}

		std::array<int, 6> shapeOffsets;
		for (int string = 0; string < 6; string++) shapeOffsets[string] = offsets[string];
		std::string name;
		if (SongTuning::TryGetTuningNameForOffsets(shapeOffsets, name)) return name;

		if (shape == ChartTuningShape::Drop)
		{
			return "Drop " + DropPedalState::GetAbsoluteTuningName(reference - 2);
		}

		return DropPedalState::GetAbsoluteTuningName(reference) + " custom";
	}

	// The tuner UI's tuning text is authoritative while the pre-song tuner is up.
	// Once it is gone (a guitar already in tune passes it in under a second), the
	// loaded arrangement tuning stands in so a quick tuner cannot starve the sync.
	// Failed reads are detectable either way: GetTuningAtTuner's failure value (69
	// per string) falls outside the chart range, and TryGetCurrentTuning reports
	// an unresolved pointer explicitly.
	bool TryReadChartTuning(int (&stringTunings)[6])
	{
		if (GameState::Menus::IsInPreSongTuner())
		{
			const auto tuning = SongTuning::GetTuningAtTuner();
			stringTunings[0] = static_cast<int>(static_cast<signed char>(tuning.lowE));
			stringTunings[1] = static_cast<int>(static_cast<signed char>(tuning.strA));
			stringTunings[2] = static_cast<int>(static_cast<signed char>(tuning.strD));
			stringTunings[3] = static_cast<int>(static_cast<signed char>(tuning.strG));
			stringTunings[4] = static_cast<int>(static_cast<signed char>(tuning.strB));
			stringTunings[5] = static_cast<int>(static_cast<signed char>(tuning.highE));
			return true;
		}

		std::array<byte, 6> arrangementTuning{};
		if (!SongTuning::TryGetCurrentTuning(arrangementTuning)) return false;

		for (int i = 0; i < 6; i++)
		{
			stringTunings[i] = static_cast<int>(static_cast<signed char>(arrangementTuning[i]));
		}
		return true;
	}

	int GetClosestOctaveAdjustment(int selectedShiftSemitones, int chartShiftSemitones)
	{
		const int difference = selectedShiftSemitones - chartShiftSemitones;
		const int roundingAdjustment = difference >= 0
			? SEMITONES_PER_OCTAVE / 2
			: -(SEMITONES_PER_OCTAVE / 2);
		return ((difference + roundingAdjustment) / SEMITONES_PER_OCTAVE)
			* SEMITONES_PER_OCTAVE;
	}

	Audio::DelayLinePitchShifter playerOneInputPitchShifter{ 0 };
	Audio::DelayLinePitchShifter playerTwoInputPitchShifter{ 0 };

	Audio::DelayLinePitchShifter& GetInputPitchShifter(DropPedal::Player player)
	{
		return player == DropPedal::Player::One
			? playerOneInputPitchShifter
			: playerTwoInputPitchShifter;
	}
}

void DropPedal::LoadSettings()
{
	DropPedalState::Configure(Settings::ReturnSettingValue("EnableDropPedal"));
	Overlay::LoadSettings();
}

bool DropPedal::IsConfiguredEnabled()
{
	return DropPedalState::IsConfiguredEnabled();
}

bool DropPedal::ShouldInstallInputHooks()
{
	return DropPedalState::IsConfiguredEnabled();
}

void DropPedal::ReportInputShifterUnavailable()
{
	DropPedalHooks::ReportInputShifterUnavailable();
}

void DropPedal::InstallInputHooks()
{
	if (!ShouldInstallInputHooks()) return;

	Audio::AsioHook::Install();
	Audio::AsioHook::SetProcessor(GetPlayerIndex(Player::One), &playerOneInputPitchShifter);
	Audio::AsioHook::SetProcessor(GetPlayerIndex(Player::Two), &playerTwoInputPitchShifter);
	UpdateInputShifterPitch(Player::One);
	UpdateInputShifterPitch(Player::Two);
}

void DropPedal::UpdateInputShifterPitch(Player player)
{
	if (!ShouldInstallInputHooks()) return;

	const int targetSemitones = IsEnabled() ? GetTargetSemitones(player) : 0;
	GetInputPitchShifter(player).SetSemitones(targetSemitones);
}

int DropPedal::GetAppliedInputShiftSemitones()
{
	// The semitones the input shifter is CURRENTLY applying to the Player 1 route's
	// samples: the divergence between the player's physical pitch and the audio every
	// post-processing consumer observes (the raw pitch verifier must measure in the
	// shifted frame or it vetoes correct notes). 0 whenever the shifter is inactive or
	// the pedal is off.
	if (!ShouldInstallInputHooks() || !IsEnabled()) return 0;
	if (!DropPedalHooks::IsInputShifterActive()) return 0;
	return GetTargetSemitones(Player::One);
}

bool DropPedal::SetInputPitchDetectionEnabled(bool enabled)
{
	if (enabled && (!ShouldInstallInputHooks()
		|| !Audio::AsioHook::IsInputConfigured(GetPlayerIndex(Player::One))
		|| !Audio::AsioHook::IsInputReady(GetPlayerIndex(Player::One))))
	{
		LOG_ERROR("Note by Note requires an active Player 1 input route for live pitch detection." << std::endl);
		return false;
	}

	GetInputPitchShifter(Player::One).SetPitchDetectionEnabled(enabled);
	return true;
}

bool DropPedal::TryGetDetectedInputMidi(Player player, int& midi)
{
	if (!GetInputPitchShifter(player).TryGetDetectedMidi(midi)) return false;

	if (GetPitchMode() != PitchMode::Off)
	{
		midi += GetTargetSemitones(player);
	}
	return midi >= 0 && midi < 128;
}

bool DropPedal::IsEnabled()
{
	return DropPedalState::IsConfiguredEnabled() && DropPedalState::IsEnabled();
}

bool DropPedal::IsSpeakerModeEnabled()
{
	return DropPedalState::IsConfiguredEnabled() && DropPedalState::IsSpeakerModeEnabled();
}

void DropPedal::DisableSpeakerMode()
{
	if (!DropPedalState::DisableSpeakerMode()) return;

	// Runs on Wwise decoder threads as well as the game loop, so only queue the
	// restore here: taking trueTuningMutex or logging would stall audio decode.
	// The in-song arrangement pass reapplies the detection reference within a frame.
	DropPedalHooks::QueuePitchRestore();
}

bool DropPedal::TrySynchronizeSpeakerTarget(const std::string& songKey)
{
	if (songKey.empty() || !IsSpeakerModeEnabled()) return false;

	bool didSynchronize = false;
	const auto modeTick = DropPedalState::GetModeNoticeTick();
	if (speakerTargetSongKey != songKey || synchronizedSpeakerModeTick != modeTick)
	{
		speakerTargetSongKey = songKey;
		synchronizedSpeakerModeTick = modeTick;
		hasSynchronizedSpeakerTarget = false;
		hasPendingChartTuning = false;
		DropPedalState::SetSpeakerTargetSynchronized(false);
		speakerTargetReadAfterTick = 0;
	}

	if (!hasSynchronizedSpeakerTarget)
	{
		// The tuner populates its tuning data while the screen builds, so no single
		// reading is trusted; the shape is accepted once two consecutive reads agree.
		// Reads start immediately -- a fixed initial delay loses the race against a
		// guitar that is already in tune and passes the tuner in under a second.
		const auto now = GetTickCount64();
		if (now < speakerTargetReadAfterTick) return false;
		speakerTargetReadAfterTick = now + 250;

		int stringTunings[6];
		if (!TryReadChartTuning(stringTunings)) return false;

		for (const int stringTuning : stringTunings)
		{
			if (stringTuning < MIN_CHART_TUNING_SEMITONES
				|| stringTuning > MAX_CHART_TUNING_SEMITONES)
			{
				hasPendingChartTuning = false;
				return false;
			}
		}

		bool confirmsPending = hasPendingChartTuning;
		for (int string = 0; string < 6; string++)
		{
			confirmsPending = confirmsPending
				&& pendingChartOffsets[string] == stringTunings[string];
		}

		if (!confirmsPending)
		{
			hasPendingChartTuning = true;
			for (int string = 0; string < 6; string++)
			{
				pendingChartOffsets[string] = stringTunings[string];
			}
			return false;
		}

		const int reference = GetChartReference(stringTunings);
		synchronizedSpeakerTargetSemitones = reference;
		synchronizedChartShape = ClassifyChartTuning(stringTunings, reference);
		for (int string = 0; string < 6; string++)
		{
			synchronizedChartOffsets[string] = stringTunings[string];
		}
		DropPedalState::SetSpeakerTargetSynchronized(true);
		const int chartShiftSemitones = synchronizedSpeakerTargetSemitones
			- DropPedalState::GetBaseTuningSemitones(Player::One);
		synchronizedSpeakerOctaveAdjustment = GetClosestOctaveAdjustment(
			DropPedalState::GetTargetSemitones(Player::One),
			chartShiftSemitones);
		hasSynchronizedSpeakerTarget = true;
		didSynchronize = true;
	}

	const int shiftSemitones = synchronizedSpeakerTargetSemitones
		- DropPedalState::GetBaseTuningSemitones(Player::One)
		+ synchronizedSpeakerOctaveAdjustment;
	if (!DropPedalState::SetTargetSemitones(Player::One, shiftSemitones))
	{
		LOG_ERROR("Speaker Mode chart tuning is outside the supported pitch range for "
			<< songKey << std::endl);
		DisableSpeakerMode();
		return false;
	}
	if (didSynchronize)
	{
		LOG_INFO("Speaker Mode chart target synchronized to " << GetTargetTuningName()
			<< " (chart " << synchronizedSpeakerTargetSemitones
			<< (synchronizedChartShape == ChartTuningShape::Drop ? ", drop shape" : "")
			<< (synchronizedChartShape == ChartTuningShape::Custom ? ", custom shape" : "")
			<< ", octave adjustment " << synchronizedSpeakerOctaveAdjustment
			<< ") for " << songKey << std::endl);
	}
	return true;
}

DropPedal::PitchMode DropPedal::GetPitchMode()
{
	return DropPedalState::GetPitchMode();
}

std::string DropPedal::GetPitchModeName()
{
	switch (GetPitchMode())
	{
		case PitchMode::DropPedal:
			return "Drop Pedal";
		case PitchMode::SpeakerMode:
			return "Speaker Mode";
		case PitchMode::Off:
		default:
			return "Off";
	}
}

void DropPedal::HandleArrangementTuning(bool isGameplay)
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	DropPedalState::SetGameplayInProgress(isGameplay);
	DropPedalHooks::HandleArrangementTuning();
}

void DropPedal::ResetSongState()
{
	DropPedalState::SetGameplayInProgress(false);
	DropPedalState::SetSpeakerTargetSynchronized(false);
	speakerTargetSongKey.clear();
	hasSynchronizedSpeakerTarget = false;
	hasPendingChartTuning = false;
	synchronizedChartShape = ChartTuningShape::Uniform;
	synchronizedSpeakerTargetSemitones = 0;
	synchronizedSpeakerOctaveAdjustment = 0;
	speakerTargetReadAfterTick = 0;
	synchronizedSpeakerModeTick = 0;
	if (!DropPedalState::IsConfiguredEnabled()) return;

	DropPedalHooks::ResetSongState();
}

int DropPedal::GetTargetSemitones(Player player)
{
	return DropPedalState::GetTargetSemitones(player);
}

// Speaker Mode shifts the one song mix; its route rides on Player One's state.
int DropPedal::GetShiftSemitones()
{
	return DropPedalState::GetTargetSemitones(Player::One);
}

// The shift that would bring the player's PHYSICAL guitar exactly into the chart's
// authored tuning: chart reference minus the player's base. This is the only shift at
// which the retuned input is in tune WITH THE SONG. It is computed live from the chart
// tuning (tuner or in-song arrangement) and the base, so it follows the song and the
// pedal dynamically without touching any dialed setting. Returns false when the chart
// tuning cannot be read (menus, tuner still populating) so callers can fall back rather
// than force a spurious miss. Used to grade Note by Note against the song rather than
// against wherever the dialed shift has detuned the input to (dialed == this when the
// pedal is set correctly; they diverge only when the player is genuinely out of tune).
bool DropPedal::TryGetChartMatchShiftSemitones(int& shiftSemitones)
{
	int stringTunings[6];
	if (!TryReadChartTuning(stringTunings)) return false;

	for (const int stringTuning : stringTunings)
	{
		if (stringTuning < MIN_CHART_TUNING_SEMITONES
			|| stringTuning > MAX_CHART_TUNING_SEMITONES)
		{
			return false;
		}
	}

	const int reference = GetChartReference(stringTunings);
	shiftSemitones = reference - DropPedalState::GetBaseTuningSemitones(Player::One);
	return true;
}

bool DropPedal::TryGetPhysicalOpenStringMidi(int stringIndex, int& midi)
{
	static constexpr int OPEN_MIDI[] = { 40, 45, 50, 55, 59, 64 };
	midi = -1;
	if (stringIndex < 0 || stringIndex >= 6) return false;
	int stringTunings[6];
	if (!TryReadChartTuning(stringTunings)) return false;
	for (const int tuning : stringTunings)
	{
		if (tuning < MIN_CHART_TUNING_SEMITONES || tuning > MAX_CHART_TUNING_SEMITONES) return false;
	}
	midi = OPEN_MIDI[stringIndex] + stringTunings[stringIndex];
	if (GetPitchMode() != PitchMode::Off)
	{
		midi += DropPedalState::GetBaseTuningSemitones(Player::One) - GetChartReference(stringTunings);
	}
	return true;
}

std::string DropPedal::GetTuningName(Player player)
{
	return DropPedalState::GetTuningName(player);
}

std::string DropPedal::GetPitchRouteName()
{
	const auto physicalTuning = GetPhysicalTuningName();
	const auto targetTuning = GetTargetTuningName();
	const int shiftSemitones = GetShiftSemitones();
	if (shiftSemitones == 0) return physicalTuning;

	std::ostringstream route;
	if (GetPitchMode() == PitchMode::SpeakerMode)
	{
		route << targetTuning << " -> " << physicalTuning;
		const int speakerShiftSemitones = -shiftSemitones;
		route << " (" << (speakerShiftSemitones > 0 ? "+" : "")
			<< speakerShiftSemitones << ")";
		return route.str();
	}

	route << physicalTuning << " -> " << targetTuning;
	route << " (" << (shiftSemitones > 0 ? "+" : "") << shiftSemitones << ")";
	return route.str();
}

std::string DropPedal::GetPhysicalTuningName()
{
	const int baseSemitones = DropPedalState::GetBaseTuningSemitones(Player::One);
	if (IsSpeakerChartShapeActive())
	{
		// The guitar's physical shape is the chart moved onto the player's base.
		int physicalOffsets[6];
		const int shapeShift = baseSemitones - synchronizedSpeakerTargetSemitones;
		for (int string = 0; string < 6; string++)
		{
			physicalOffsets[string] = synchronizedChartOffsets[string] + shapeShift;
		}
		return NameChartShape(synchronizedChartShape, physicalOffsets, baseSemitones);
	}

	return DropPedalState::GetAbsoluteTuningName(baseSemitones);
}

std::string DropPedal::GetTargetTuningName()
{
	if (IsSpeakerChartShapeActive())
	{
		return NameChartShape(
			synchronizedChartShape,
			synchronizedChartOffsets,
			synchronizedSpeakerTargetSemitones);
	}

	return DropPedalState::GetAbsoluteTuningName(
		DropPedalState::GetBaseTuningSemitones(Player::One)
		+ DropPedalState::GetTargetSemitones(Player::One));
}

bool DropPedal::TryGetAuthoredTrueTuning(float& trueTuning)
{
	if (!DropPedalState::IsConfiguredEnabled()) return false;

	return DropPedalHooks::TryGetAuthoredTrueTuning(trueTuning);
}

std::string DropPedal::GetBaseTuningName(Player player)
{
	return DropPedalState::GetBaseTuningName(player);
}

int DropPedal::GetBaseTuningSemitones(Player player)
{
	return DropPedalState::GetBaseTuningSemitones(player);
}

int DropPedal::GetShiftDirection(Player player)
{
	const int direction = DropPedalState::GetShiftDirection(player);
	return IsSpeakerModeEnabled() && player == Player::One ? -direction : direction;
}

void DropPedal::InstallHooks()
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	DropPedalHooks::Install();
}

void DropPedal::SetInputShifterActive(bool active)
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	DropPedalHooks::SetInputShifterActive(active);
}

bool DropPedal::IsInputShifterActive()
{
	return DropPedalState::IsConfiguredEnabled() && DropPedalHooks::IsInputShifterActive();
}

uint32_t DropPedal::GetInputShifterLatencyFrames()
{
	if (!IsInputShifterActive() || !IsEnabled()) return 0;
	// The live tap distance, not the fixed nominal figure: it grows with the shift amount
	// and the note's period, which is what a player feels.
	return static_cast<uint32_t>(GetInputPitchShifter(Player::One).GetLiveDelayFrames() + 0.5f);
}

bool DropPedal::IsPlayerShiftAvailable(Player player)
{
	if (player == Player::One) return true;

	const size_t routeIndex = GetPlayerIndex(player);
	return Audio::AsioHook::IsInputConfigured(routeIndex)
		&& Audio::AsioHook::IsInputReady(routeIndex);
}

void DropPedal::Poll()
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	DropPedalHooks::Poll();
}
