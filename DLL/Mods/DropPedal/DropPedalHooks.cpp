#include "../../stdafx.h"
#include "DropPedalHooks.hpp"
#include "DropPedalState.hpp"

namespace
{
	constexpr float SEMITONES_PER_OCTAVE = 12.0f;
	constexpr float TRUE_TUNING_COMPARISON_EPSILON = 0.0001f;
	std::atomic<bool> isInputShifterActive{ false };
	std::atomic<bool> tuningRestorePending{ false };
	bool hasReportedInputShifterUnavailable = false;
	unsigned long long inputCaptureWaitStartTick = 0;

	// The reference builder converts an arrangement's cent offset into the frequency
	// note detection expects, 440 * 2^(cents / 1200), and stamps it into the
	// detection object at song load. It is detoured so every consumer sees the
	// shifted reference, including the pre-song tuner, which snapshots its expected
	// pitches from that stamp the instant it lands.
	void* referenceBuilderTrampoline = nullptr;

	// Written by the game loop, read by the naked detour on the game's loading
	// thread. Aligned 32-bit loads and stores are atomic on x86.
	volatile LONG referenceCentsAdjustment = 0;
	volatile LONG playerTwoReferenceCentsAdjustment = 0;
	volatile LONG authoredReferenceCents = 0;
	volatile LONG hasAuthoredReferenceCents = 0;

	std::mutex trueTuningMutex;
	uintptr_t trueTuningAddress = 0;
	float authoredTrueTuning = 0.0f;
	float appliedTrueTuning = 0.0f;
	bool hasCapturedTrueTuning = false;
	bool hasAppliedTrueTuning = false;
	bool hasReportedTrueTuningUnavailable = false;

	// (detection, cents) pairs captured by the builder detour, identified on the
	// game loop where locks and logging are allowed.
	constexpr LONG BUILDER_CAPTURE_SLOTS = 8; // Power of two; the detour masks with 7.
	PVOID volatile builderCapturedDetections[BUILDER_CAPTURE_SLOTS] = {};
	volatile LONG builderCapturedCents[BUILDER_CAPTURE_SLOTS] = {};
	volatile LONG builderCaptureReserveCount = 0;
	volatile LONG builderCapturePublishCount = 0;
	LONG builderCaptureConsumedCount = 0;               // Game loop, under trueTuningMutex.

	void* identifiedPlayerOneDetection = nullptr;       // Game loop, under trueTuningMutex.
	PVOID volatile identifiedPlayerTwoDetection = nullptr; // Compared, never dereferenced, by the detour.
	LONG playerTwoAuthoredCents = 0;                    // Under trueTuningMutex.
	bool hasPlayerTwoAuthoredCents = false;
	bool sawPlayerOneBuilderCapture = false;
	void* pendingUnmatchedDetection = nullptr;
	LONG pendingUnmatchedCents = 0;
	bool hasPendingUnmatchedCapture = false;

	int GetDetectionShiftSemitones(DropPedal::Player player)
	{
		if (DropPedalState::IsSpeakerModeEnabled())
		{
			// Speaker Mode rides on Player One's route; Player Two detection stays authored.
			return player == DropPedal::Player::One
				? DropPedalState::GetTargetSemitones(DropPedal::Player::One)
				: 0;
		}

		return 0;
	}

	// The builder takes its cent offset as a single stack argument and carries the
	// detection object it stamps in ESI. Adjusting the argument in place and
	// running the original means the game computes the shifted frequency with its
	// own math: non-A440 offsets and the -1200 emulated-bass case compose
	// naturally. Player Two's stamps take Player Two's adjustment once its
	// detection object is identified; unknown objects take Player One's, and the
	// guarded live writes correct such a stamp within a poll. Each (detection,
	// cents) pair is published for the game loop, which owns identification and
	// the authored-cents globals. Runs on the game's loading thread, so no
	// logging and no locks.
	__declspec(naked) void SpyReferenceBuilder()
	{
		__asm
		{
			push eax
			push ebx

			// Reserve a ring slot, fill it, then publish, so the game loop never
			// reads a slot that is still being written.
			mov eax, 1
			lock xadd dword ptr [builderCaptureReserveCount], eax
			and eax, 7 // BUILDER_CAPTURE_SLOTS - 1
			mov dword ptr [builderCapturedDetections + eax * 4], esi
			mov ebx, dword ptr [esp + 12]
			mov dword ptr [builderCapturedCents + eax * 4], ebx
			lock inc dword ptr [builderCapturePublishCount]

			mov eax, dword ptr [esp + 12]
			cmp esi, identifiedPlayerTwoDetection
			jne adjustAsPlayerOne
			add eax, dword ptr [playerTwoReferenceCentsAdjustment]
			jmp writeAdjustedCents

		adjustAsPlayerOne:
			add eax, dword ptr [referenceCentsAdjustment]

		writeAdjustedCents:
			mov dword ptr [esp + 12], eax
			pop ebx
			pop eax
			jmp referenceBuilderTrampoline
		}
	}

	/// <summary>
	/// Keep the cents adjustments in step with the pedal. Raising the reference makes
	/// detection expect the player's physical pitch: at a -2 target, 0 cents becomes
	/// +200 and A440 becomes ~A494. When the shared input shifter owns pitch the input
	/// itself is already retuned, so the reference stays authored.
	/// </summary>
	void UpdateReferenceCentsAdjustment()
	{
		const int playerOneSemitones = GetDetectionShiftSemitones(DropPedal::Player::One);
		const int playerTwoSemitones = GetDetectionShiftSemitones(DropPedal::Player::Two);

		InterlockedExchange(&referenceCentsAdjustment, (LONG)(-playerOneSemitones * 100));
		InterlockedExchange(&playerTwoReferenceCentsAdjustment, (LONG)(-playerTwoSemitones * 100));
	}

	/// <summary>
	/// The authored reference for the current arrangement. With the builder hooked,
	/// the stamped value already carries the shift, so authored is reconstructed
	/// from the cents the builder was given rather than read back transposed.
	/// </summary>
	float DeriveAuthoredTrueTuning(float currentTrueTuning)
	{
		if (referenceBuilderTrampoline != nullptr
			&& InterlockedCompareExchange(&hasAuthoredReferenceCents, 1, 1) == 1)
		{
			return 440.0f * powf(2.0f, (float)authoredReferenceCents / 1200.0f);
		}

		return currentTrueTuning;
	}

	bool AreTrueTuningValuesEqual(float first, float second)
	{
		return fabsf(first - second) <= TRUE_TUNING_COMPARISON_EPSILON;
	}

	bool WriteTrueTuningLocked(float value)
	{
		if (!MemUtil::PatchAdr(
			reinterpret_cast<LPVOID>(trueTuningAddress),
			reinterpret_cast<LPVOID>(&value),
			sizeof(value)))
		{
			LOG_ERROR("Drop pedal failed to write true tuning at 0x"
				<< std::hex << trueTuningAddress << std::dec << std::endl);
			return false;
		}

		return true;
	}

	void ClassifyPlayerTwoDetectionLocked(void* detection, LONG cents)
	{
		identifiedPlayerTwoDetection = detection;
		playerTwoAuthoredCents = cents;
		hasPlayerTwoAuthoredCents = true;
	}

	/// <summary>
	/// Identify players from the (detection, cents) pairs the builder detour
	/// captured. Player One's detection object is the one the ptr_trueTuning
	/// chain resolves into; any other captured object is Player Two's. Until a
	/// capture matches Player One, authored cents keep the pre-multiplayer
	/// last-wins behaviour and the unmatched pair is held for retroactive
	/// classification, so an unexpected game version cannot regress single player.
	/// </summary>
	void ProcessBuilderCapturesLocked()
	{
		const LONG published = InterlockedCompareExchange(&builderCapturePublishCount, 0, 0);
		if (builderCaptureConsumedCount == published) return;

		float currentTrueTuning = 0.0f;
		uintptr_t currentAddress = 0;
		if (!SongTuning::TryGetTrueTuning(currentTrueTuning, currentAddress))
		{
			// The chain is not up yet; captures stay pending for the next poll.
			return;
		}

		// The ring holds the newest BUILDER_CAPTURE_SLOTS captures; older ones
		// are already overwritten, so skip their sequence numbers.
		if (published - builderCaptureConsumedCount > BUILDER_CAPTURE_SLOTS)
		{
			builderCaptureConsumedCount = published - BUILDER_CAPTURE_SLOTS;
		}

		void* playerOneDetection = reinterpret_cast<void*>(
			currentAddress - Offsets::ptr_trueTuningOffsets.back());
		identifiedPlayerOneDetection = playerOneDetection;

		// The Player Two identity kept from the previous song is stale if the
		// allocator reused its address for this song's Player One detection.
		if (identifiedPlayerTwoDetection == playerOneDetection)
		{
			identifiedPlayerTwoDetection = nullptr;
		}

		while (builderCaptureConsumedCount < published)
		{
			const LONG slot = builderCaptureConsumedCount & (BUILDER_CAPTURE_SLOTS - 1);
			void* detection = builderCapturedDetections[slot];
			const LONG cents = builderCapturedCents[slot];
			builderCaptureConsumedCount++;

			if (detection == playerOneDetection)
			{
				sawPlayerOneBuilderCapture = true;
				InterlockedExchange(&authoredReferenceCents, cents);
				InterlockedExchange(&hasAuthoredReferenceCents, 1);

				if (hasPendingUnmatchedCapture
					&& pendingUnmatchedDetection != playerOneDetection)
				{
					ClassifyPlayerTwoDetectionLocked(pendingUnmatchedDetection, pendingUnmatchedCents);
				}

				hasPendingUnmatchedCapture = false;
			}
			else if (sawPlayerOneBuilderCapture)
			{
				ClassifyPlayerTwoDetectionLocked(detection, cents);
			}
			else
			{
				InterlockedExchange(&authoredReferenceCents, cents);
				InterlockedExchange(&hasAuthoredReferenceCents, 1);
				pendingUnmatchedDetection = detection;
				pendingUnmatchedCents = cents;
				hasPendingUnmatchedCapture = true;
			}
		}
	}

	/// <summary>
	/// Keep Player Two's detection reference in step with the pedal. Unlike
	/// Player One there is no pointer chain to re-resolve, so writes are guarded
	/// by a readability check and a plausibility window on the value already
	/// stamped there; a misidentified pointer fails those checks and is skipped.
	/// </summary>
	void ApplyPlayerTwoTrueTuningLocked()
	{
		void* detection = identifiedPlayerTwoDetection;
		if (detection == nullptr || !hasPlayerTwoAuthoredCents) return;

		const uintptr_t address =
			reinterpret_cast<uintptr_t>(detection) + Offsets::ptr_trueTuningOffsets.back();
		if (MemUtil::IsBadReadPtr(reinterpret_cast<void*>(address))) return;

		const float stampedValue = *reinterpret_cast<volatile float*>(address);
		if (!std::isfinite(stampedValue) || stampedValue < 100.0f || stampedValue > 1000.0f) return;

		const float authored = 440.0f * powf(2.0f, (float)playerTwoAuthoredCents / 1200.0f);
		const int targetSemitones = GetDetectionShiftSemitones(DropPedal::Player::Two);
		const float targetTrueTuning = authored
			* powf(2.0f, -(float)targetSemitones / SEMITONES_PER_OCTAVE);

		if (AreTrueTuningValuesEqual(stampedValue, targetTrueTuning)) return;

		if (!MemUtil::PatchAdr(
			reinterpret_cast<LPVOID>(address),
			reinterpret_cast<LPVOID>(const_cast<float*>(&targetTrueTuning)),
			sizeof(targetTrueTuning)))
		{
			LOG_ERROR("Drop pedal failed to write player 2 true tuning at 0x"
				<< std::hex << address << std::dec << std::endl);
			return;
		}

		LOG_INFO("Drop pedal player 2 true tuning: authored " << authored
			<< " Hz, applied " << targetTrueTuning << " Hz, target "
			<< targetSemitones << " semitone(s)" << std::endl);
	}

	bool CaptureOrRefreshTrueTuningLocked(float& currentTrueTuning)
	{
		uintptr_t currentAddress = 0;
		if (!SongTuning::TryGetTrueTuning(currentTrueTuning, currentAddress))
		{
			if (!hasReportedTrueTuningUnavailable)
			{
				hasReportedTrueTuningUnavailable = true;
				LOG_ERROR("Drop pedal could not resolve the arrangement true-tuning value" << std::endl);
			}

			return false;
		}

		hasReportedTrueTuningUnavailable = false;

		if (!hasCapturedTrueTuning || currentAddress != trueTuningAddress)
		{
			trueTuningAddress = currentAddress;
			authoredTrueTuning = DeriveAuthoredTrueTuning(currentTrueTuning);
			appliedTrueTuning = currentTrueTuning;
			hasCapturedTrueTuning = true;
			hasAppliedTrueTuning = !AreTrueTuningValuesEqual(appliedTrueTuning, authoredTrueTuning);

			LOG_INFO("Drop pedal captured authored true tuning " << authoredTrueTuning
				<< " Hz at 0x" << std::hex << trueTuningAddress << std::dec << std::endl);
			return true;
		}

		// Rocksmith rewrites this location when an arrangement loads. A value other
		// than the one the drop pedal applied is therefore a new stamp from the
		// reference builder, not a result to compound on the next pedal update.
		const float comparisonValue = hasAppliedTrueTuning ? appliedTrueTuning : authoredTrueTuning;
		if (!AreTrueTuningValuesEqual(currentTrueTuning, comparisonValue))
		{
			authoredTrueTuning = DeriveAuthoredTrueTuning(currentTrueTuning);
			appliedTrueTuning = currentTrueTuning;
			hasAppliedTrueTuning = !AreTrueTuningValuesEqual(appliedTrueTuning, authoredTrueTuning);
			LOG_INFO("Drop pedal observed a new authored true tuning "
				<< authoredTrueTuning << " Hz" << std::endl);
		}

		return true;
	}

	void ApplyTrueTuningLocked()
	{
		ProcessBuilderCapturesLocked();

		float currentTrueTuning = 0.0f;
		if (!CaptureOrRefreshTrueTuningLocked(currentTrueTuning))
		{
			return;
		}

		const int targetSemitones = GetDetectionShiftSemitones(DropPedal::Player::One);
		const float targetTrueTuning = authoredTrueTuning
			* powf(2.0f, -(float)targetSemitones / SEMITONES_PER_OCTAVE);
		const bool targetChanged = !AreTrueTuningValuesEqual(appliedTrueTuning, targetTrueTuning);
		bool wroteValue = false;

		if (!AreTrueTuningValuesEqual(currentTrueTuning, targetTrueTuning))
		{
			if (!WriteTrueTuningLocked(targetTrueTuning))
			{
				return;
			}

			wroteValue = true;
		}

		appliedTrueTuning = targetTrueTuning;
		hasAppliedTrueTuning = !AreTrueTuningValuesEqual(targetTrueTuning, authoredTrueTuning);

		if (targetChanged || wroteValue)
		{
			LOG_INFO("Drop pedal true tuning: authored " << authoredTrueTuning
				<< " Hz, applied " << targetTrueTuning << " Hz, target "
				<< targetSemitones << " semitone(s)" << std::endl);
		}

		ApplyPlayerTwoTrueTuningLocked();
	}

	void ApplyCapturedTrueTuning()
	{
		std::lock_guard<std::mutex> lock(trueTuningMutex);
		if (!hasCapturedTrueTuning)
		{
			return;
		}

		ApplyTrueTuningLocked();
	}

	void RestoreTrueTuningLocked()
	{
		if (!hasCapturedTrueTuning)
		{
			return;
		}

		float currentTrueTuning = 0.0f;
		uintptr_t currentAddress = 0;
		if (SongTuning::TryGetTrueTuning(currentTrueTuning, currentAddress)
			&& currentAddress == trueTuningAddress
			&& !AreTrueTuningValuesEqual(currentTrueTuning, authoredTrueTuning))
		{
			if (WriteTrueTuningLocked(authoredTrueTuning))
			{
				LOG_INFO("Drop pedal restored authored true tuning "
					<< authoredTrueTuning << " Hz" << std::endl);
			}
		}

		appliedTrueTuning = authoredTrueTuning;
		hasAppliedTrueTuning = false;
	}
}

void DropPedalHooks::Install()
{
	inputCaptureWaitStartTick = GetTickCount64();
	LOG_INFO("Drop pedal input path: waiting for capture." << std::endl);

	const uintptr_t referenceBuilder = Offsets::func_tuningReferenceBuilder.GetValue();
	if (referenceBuilder != 0)
	{
		const PBYTE trampoline = DetourFunction((PBYTE)referenceBuilder, (PBYTE)SpyReferenceBuilder);
		if (trampoline == nullptr)
		{
			LOG_ERROR("Drop pedal failed to hook the tuning reference builder at 0x"
				<< std::hex << referenceBuilder << std::dec << std::endl);
		}
		else
		{
			referenceBuilderTrampoline = trampoline;
		}
	}
	else
	{
		// Without the builder hook the live true-tuning writes still keep in-song
		// detection correct; only the tuner's load-time snapshot stays authored.
		LOG_INFO("Drop pedal has no reference builder address for this game version; "
			"the pre-song tuner will not follow the shift" << std::endl);
	}

	UpdateReferenceCentsAdjustment();
}

void DropPedalHooks::Poll()
{
	UpdateReferenceCentsAdjustment();
	if (tuningRestorePending.exchange(false, std::memory_order_acq_rel))
	{
		ApplyCapturedTrueTuning();
	}

	{
		std::lock_guard<std::mutex> lock(trueTuningMutex);
		ProcessBuilderCapturesLocked();
	}

}

void DropPedalHooks::QueuePitchRestore()
{
	tuningRestorePending.store(true, std::memory_order_release);
}

void DropPedalHooks::SetInputShifterActive(bool active)
{
	const bool wasActive = isInputShifterActive.exchange(active, std::memory_order_acq_rel);
	if (active == wasActive) return;

	LOG_INFO("Drop pedal input path: " << (active ? "ready" : "unavailable") << "." << std::endl);
}

bool DropPedalHooks::IsInputShifterActive()
{
	return isInputShifterActive.load(std::memory_order_acquire);
}

bool DropPedalHooks::TryGetAuthoredTrueTuning(float& trueTuning)
{
	std::lock_guard<std::mutex> lock(trueTuningMutex);
	if (hasCapturedTrueTuning && hasAppliedTrueTuning)
	{
		trueTuning = authoredTrueTuning;
		return true;
	}

	if (!DropPedalState::IsSpeakerModeEnabled()
		|| referenceBuilderTrampoline == nullptr
		|| InterlockedCompareExchange(&hasAuthoredReferenceCents, 1, 1) != 1) return false;

	trueTuning = 440.0f * powf(2.0f, (float)authoredReferenceCents / 1200.0f);
	return true;
}

void DropPedalHooks::ReportInputShifterUnavailable()
{
	if (hasReportedInputShifterUnavailable) return;
	if (GetTickCount64() - inputCaptureWaitStartTick < 10000) return;

	hasReportedInputShifterUnavailable = true;
	LOG_ERROR("Drop pedal input capture is unavailable. Pitch shifting remains inactive; "
		"there is no tone-based fallback." << std::endl);
}

void DropPedalHooks::HandleArrangementTuning()
{
	std::lock_guard<std::mutex> lock(trueTuningMutex);
	ApplyTrueTuningLocked();
}

void DropPedalHooks::ResetSongState()
{
	std::lock_guard<std::mutex> lock(trueTuningMutex);
	RestoreTrueTuningLocked();

	trueTuningAddress = 0;
	authoredTrueTuning = 0.0f;
	appliedTrueTuning = 0.0f;
	hasCapturedTrueTuning = false;
	hasAppliedTrueTuning = false;
	hasReportedTrueTuningUnavailable = false;
	InterlockedExchange(&authoredReferenceCents, 0);
	InterlockedExchange(&hasAuthoredReferenceCents, 0);

	// Player Two's identified detection pointer survives on purpose: the detour
	// only compares against it, never dereferences it, and keeping it lets the
	// next load's stamps take Player Two's adjustment before the tuner snapshots
	// them. ProcessBuilderCapturesLocked clears it if the address is reused.
	// Player Two's stamp is not restored: the builder re-stamps it on the next
	// load, and a write into a freed object would be worse than a stale value.
	identifiedPlayerOneDetection = nullptr;
	playerTwoAuthoredCents = 0;
	hasPlayerTwoAuthoredCents = false;
	sawPlayerOneBuilderCapture = false;
	hasPendingUnmatchedCapture = false;
	pendingUnmatchedCents = 0;
	builderCaptureConsumedCount = InterlockedCompareExchange(&builderCapturePublishCount, 0, 0);
}
