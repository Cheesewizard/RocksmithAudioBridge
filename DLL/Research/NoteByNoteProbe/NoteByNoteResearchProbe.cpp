#include "../../Mods/ResearchProbeRuntime.hpp"
#include "../../Mods/NoteByNoteScoringCore.hpp"

#include "../../Mods/NoteByNoteNativeScoring.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	std::atomic<bool> isMarkerDimEnabled{ false };
	constexpr char PROBE_NAME[] = "RSModsPlus Note by Note Controller";
	const std::string PROBE_BUILD_ID = std::string(__DATE__) + " " + __TIME__;
	namespace StoppedPreviewFilter
	{
		void RestorePersistent(const char* reason);
	}

	uint8_t __cdecl InitializeProbe()
	{
		NoteByNoteScoringCore::Initialize();
		return NoteByNoteScoringCore::IsAvailable() ? 1 : 0;
	}

	void __cdecl ShutdownProbe()
	{
		StoppedPreviewFilter::RestorePersistent("probe-shutdown");
		NoteByNoteNativeScoring::Shutdown();
		ResearchProbeRuntime::Shutdown();
	}

	void __stdcall ProcessScoringUpdate(
		void* owner,
		float updateTime,
		ResearchProtocol::ScoringUpdate original)
	{
		NoteByNoteNativeScoring::ProcessScoringUpdate(owner, updateTime, original);
	}

	bool __fastcall ProcessHitDecision(
		void* owner,
		void* unusedEdx,
		void* note,
		ResearchProtocol::HitDecision original)
	{
		return NoteByNoteNativeScoring::ProcessHitDecision(owner, unusedEdx, note, original);
	}

	void __cdecl ObserveRenderedAttack(const ResearchProtocol::RenderedAttack* source)
	{
		if (source == nullptr) return;

		NoteByNoteProbe::NativeRenderedAttack attack;
		attack.isTransition = source->isTransition != 0;
		attack.renderFrame = source->renderFrame;
		attack.songTime = source->songTime;
		attack.longitudinalPosition = source->longitudinalPosition;
		attack.notes.reserve(source->noteCount);
		for (size_t index = 0; index < source->noteCount; ++index)
		{
			attack.notes.push_back({ source->notes[index].stringIndex, source->notes[index].fret });
		}
		NoteByNoteScoringCore::ObserveRenderedAttack(attack);
	}

	void __cdecl StopProbe()
	{
		StoppedPreviewFilter::RestorePersistent("probe-stop");
		NoteByNoteScoringCore::Stop();
	}

	void __cdecl RequestReArmProbe()
	{
		NoteByNoteScoringCore::RequestReArm();
	}

	uint8_t __cdecl GetState(ResearchProtocol::NoteByNoteState* state)
	{
		if (state == nullptr) return 0;

		const auto callerSize = state->structSize;
		if (callerSize != sizeof(ResearchProtocol::NoteByNoteState))
		{
			static std::atomic<bool> hasLoggedStateSizeMismatch{ false };
			if (!hasLoggedStateSizeMismatch.exchange(true))
			{
				std::ostringstream message;
				message << "(NBN CONTROLLER) State ABI mismatch: caller provides "
					<< callerSize << " bytes, controller requires "
					<< sizeof(ResearchProtocol::NoteByNoteState)
					<< ". Rebuild and deploy the host and controller together.";
				ResearchProbeRuntime::Log(ResearchProtocol::LogLevel::Error, message.str());
			}
			return 0;
		}

		*state = NoteByNoteNativeScoring::GetResearchState();
		return 1;
	}
	namespace StoppedPreviewFilter
	{
		void RestorePending();
		bool HideNonTarget(void* renderCtx, void* noteArray);
	}
	namespace NoteListSnapshot
	{

		bool LooksReadable(uintptr_t p)
		{
			return p >= 0x10000 && p < 0x7FFF0000;
		}

		float ReadFloat(uintptr_t base, unsigned int offset)
		{
			return *reinterpret_cast<const float*>(base + offset);
		}

		int ReadInt(uintptr_t base, unsigned int offset)
		{
			return *reinterpret_cast<const int*>(base + offset);
		}


	}
	namespace StoppedPreviewFilter
	{
		constexpr uintptr_t PREVIEW_CONTEXT_VTABLE = 0x0121B6F0;
		constexpr float MARKER_OFFSET = 0.5f;
		constexpr float OFFSCREEN_STRING_POSITION = -999.5f;
		constexpr float MARKER_SCALE_MIN = 0.70f;
		constexpr float MARKER_SCALE_MAX = 1.05f;
		constexpr float MARKER_W_MIN = 0.80f;
		constexpr float MARKER_W_MAX = 0.95f;
		constexpr float FRET_EPSILON = 0.35f;
		constexpr float STRING_EPSILON = 0.20f;
		constexpr std::array<float, 24> FRET_CENTERS =
		{
			-50.469f, -44.429f, -38.739f, -33.163f, -27.755f, -22.552f,
			-17.478f, -12.512f, -7.723f, -3.061f, 1.496f, 5.932f,
			10.242f, 14.448f, 18.593f, 22.627f, 26.565f, 30.107f,
			33.896f, 37.366f, 40.977f, 44.478f, 47.915f, 51.351f,
		};
		constexpr std::array<float, 6> STRING_HEIGHTS =
		{
			4.018f, 2.410f, 0.803f, -0.803f, -2.410f, -4.018f,
		};
		constexpr std::array<float, 4> BASS_STRING_HEIGHTS =
		{
			4.018f, 1.339f, -1.339f, -4.018f,
		};
		constexpr float BASS_STRING_EPSILON = 0.90f;

		std::atomic<uint32_t> hiddenCount{ 0 };
		constexpr size_t PENDING_CAPACITY = 16;
		thread_local uintptr_t pendingNotes[PENDING_CAPACITY] = {};
		thread_local float pendingStringPositions[PENDING_CAPACITY] = {};
		thread_local size_t pendingCount = 0;

		bool TryWriteFloat(uintptr_t address, float value)
		{
			__try
			{
				*reinterpret_cast<float*>(address) = value;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool TryReadFloat(uintptr_t address, float* value)
		{
			__try
			{
				*value = *reinterpret_cast<const float*>(address);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool TryReadDword(uintptr_t address, uint32_t* value)
		{
			__try
			{
				*value = *reinterpret_cast<const uint32_t*>(address);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
		std::atomic<bool> isPersistentRelocationEnabled{ false };
		constexpr size_t PERSISTENT_CAPACITY = 128;
		struct PersistentEntry
		{
			uintptr_t note;
			float originalPosition;
			uint32_t headerWord;
			uint32_t fretWord;
		};
		std::mutex persistentMutex;
		PersistentEntry persistentEntries[PERSISTENT_CAPACITY] = {};
		size_t persistentCount = 0;
		int persistentTargetString = -1;
		int persistentTargetFret = -1;
		std::atomic<uint32_t> persistentHiddenCount{ 0 };
		std::atomic<bool> wasCapacityReported{ false };
		constexpr int PER_HOLD_LOG_BUDGET = 6;
		int perHoldLogBudget = PER_HOLD_LOG_BUDGET;
		uint32_t perHoldRelocations = 0;
		uint32_t perHoldRewrites = 0;

		void RestorePersistent(const char* reason)
		{
			std::lock_guard<std::mutex> lock(persistentMutex);
			persistentTargetString = -1;
			persistentTargetFret = -1;
			const auto holdRelocations = perHoldRelocations;
			const auto holdRewrites = perHoldRewrites;
			perHoldLogBudget = PER_HOLD_LOG_BUDGET;
			perHoldRelocations = 0;
			perHoldRewrites = 0;
			if (persistentCount == 0)
			{
				LOG_INFO("(NBN STOPPED PREVIEW) restore pass with nothing tracked, "
					<< "reason=" << reason << "." << std::endl);
				return;
			}

			size_t restored = 0;
			size_t recycled = 0;
			for (size_t index = 0; index < persistentCount; ++index)
			{
				const auto& entry = persistentEntries[index];
				float current = 0.0f;
				uint32_t headerWord = 0;
				uint32_t fretWord = 0;
				if (!TryReadFloat(entry.note + 0x34, &current)
					|| std::fabs(current - OFFSCREEN_STRING_POSITION) > 0.01f
					|| !TryReadDword(entry.note + 0x00, &headerWord)
					|| headerWord != entry.headerWord
					|| !TryReadDword(entry.note + 0x30, &fretWord)
					|| fretWord != entry.fretWord)
				{
					++recycled;
					continue;
				}
				if (TryWriteFloat(entry.note + 0x34, entry.originalPosition))
				{
					++restored;
				}
			}
			LOG_INFO("(NBN STOPPED PREVIEW) restored " << restored
				<< " persistent marker(s), " << recycled << " recycled, holdRelocations="
				<< holdRelocations << " holdRewrites=" << holdRewrites << ", reason="
				<< reason << "." << std::endl);
			persistentCount = 0;
		}

		void HidePersistently(
			uintptr_t note,
			float originalPosition,
			const ResearchProtocol::NoteByNoteState& state,
			int stringIndex,
			int fret)
		{
			std::lock_guard<std::mutex> lock(persistentMutex);
			for (size_t index = 0; index < persistentCount; ++index)
			{
				if (persistentEntries[index].note != note) continue;
				if (TryWriteFloat(note + 0x34, OFFSCREEN_STRING_POSITION))
				{
					persistentEntries[index].originalPosition = originalPosition;
					TryReadDword(note + 0x00, &persistentEntries[index].headerWord);
					TryReadDword(note + 0x30, &persistentEntries[index].fretWord);
					++perHoldRewrites;
					if (perHoldLogBudget > 0)
					{
						--perHoldLogBudget;
						LOG_INFO("(NBN STOPPED PREVIEW) re-relocated after game rewrite "
							<< stringIndex << ':' << fret
							<< " note=0x" << std::hex << note << std::dec
							<< " holdRewrites=" << perHoldRewrites << std::endl);
					}
				}
				return;
			}
			if (persistentCount >= PERSISTENT_CAPACITY)
			{
				if (!wasCapacityReported.exchange(true, std::memory_order_acq_rel))
				{
					LOG_ERROR("(NBN STOPPED PREVIEW) persistent list full at "
						<< PERSISTENT_CAPACITY
						<< " records; further markers stay visible." << std::endl);
				}
				return;
			}
			uint32_t headerWord = 0;
			uint32_t fretWord = 0;
			if (!TryReadDword(note + 0x00, &headerWord)
				|| !TryReadDword(note + 0x30, &fretWord))
			{
				return;
			}
			if (!TryWriteFloat(note + 0x34, OFFSCREEN_STRING_POSITION))
			{
				LOG_ERROR("(NBN STOPPED PREVIEW) Could not relocate marker record 0x"
					<< std::hex << note << std::dec << "." << std::endl);
				return;
			}
			persistentEntries[persistentCount].note = note;
			persistentEntries[persistentCount].originalPosition = originalPosition;
			persistentEntries[persistentCount].headerWord = headerWord;
			persistentEntries[persistentCount].fretWord = fretWord;
			++persistentCount;
			persistentTargetString = state.visualString;
			persistentTargetFret = state.visualFret;

			++perHoldRelocations;
			const auto total = persistentHiddenCount.fetch_add(1, std::memory_order_acq_rel) + 1;
			const bool hasHoldBudget = perHoldLogBudget > 0;
			if (hasHoldBudget) --perHoldLogBudget;
			if (hasHoldBudget || total <= 18 || total % 500 == 0)
			{
				LOG_INFO("(NBN STOPPED PREVIEW) relocated persistently "
					<< stringIndex << ':' << fret
					<< " target=" << state.visualString << ':' << state.visualFret
					<< " note=0x" << std::hex << note << std::dec
					<< " tracked=" << persistentCount
					<< " holdRelocations=" << perHoldRelocations
					<< " count=" << total << std::endl);
			}
		}

		void RestorePending()
		{
			for (size_t index = 0; index < pendingCount; ++index)
			{
				if (!TryWriteFloat(pendingNotes[index] + 0x34, pendingStringPositions[index]))
				{
					LOG_ERROR("(NBN STOPPED PREVIEW) Could not restore marker record 0x"
						<< std::hex << pendingNotes[index] << std::dec << "." << std::endl);
				}
				pendingNotes[index] = 0;
			}
			pendingCount = 0;
		}

		int DecodeClosest(float value, const float* values, size_t count, float epsilon)
		{
			int closest = -1;
			float closestDistance = epsilon;
			for (size_t index = 0; index < count; ++index)
			{
				const float distance = std::fabs(value - values[index]);
				if (distance >= closestDistance) continue;

				closest = static_cast<int>(index);
				closestDistance = distance;
			}
			return closest;
		}

		bool IsSelectedCoordinate(
			const ResearchProtocol::NoteByNoteState& state,
			int stringIndex,
			int fret)
		{
			if (stringIndex == state.visualString && fret == state.visualFret) return true;

			for (uint32_t index = 0; index < state.visualGroupCount
				&& index < ResearchProtocol::NoteByNoteState::MaxVisualGroup; ++index)
			{
				if (stringIndex == state.visualGroupStrings[index]
					&& fret == state.visualGroupFrets[index])
				{
					return true;
				}
			}
			return false;
		}

		bool HideNonTarget(void* renderCtx, void* noteArray)
		{
			RestorePending();
			const auto state = NoteByNoteNativeScoring::GetResearchState();
			const bool isPersistent =
				isPersistentRelocationEnabled.load(std::memory_order_relaxed);
			const bool isHoldActive = state.ownsNativeHold != 0 && state.visualChordId < 0;
			const char* restoreReason = nullptr;
			{
				std::lock_guard<std::mutex> lock(persistentMutex);
				if (persistentCount > 0)
				{
					if (!isPersistent) restoreReason = "persist-disabled";
					else if (!isHoldActive) restoreReason = "hold-ended";
					else if (persistentTargetString != state.visualString
						|| persistentTargetFret != state.visualFret)
					{
						restoreReason = "target-changed";
					}
				}
			}
			if (restoreReason != nullptr) RestorePersistent(restoreReason);

			if (!isHoldActive) return false;
			if (!NoteListSnapshot::LooksReadable(reinterpret_cast<uintptr_t>(renderCtx))
				|| *reinterpret_cast<const uintptr_t*>(renderCtx) != PREVIEW_CONTEXT_VTABLE)
			{
				return false;
			}

			const auto arrayAddress = reinterpret_cast<uintptr_t>(noteArray);
			if (!NoteListSnapshot::LooksReadable(arrayAddress)) return false;
			const auto notes = static_cast<uintptr_t>(
				static_cast<unsigned int>(NoteListSnapshot::ReadInt(arrayAddress, 0)));
			const int count = NoteListSnapshot::ReadInt(arrayAddress, 4);
			if (!NoteListSnapshot::LooksReadable(notes) || count < 1 || count > 64) return false;
			bool didHide = false;
			const bool isBass = NoteByNoteNativeScoring::IsBassArrangementActive();
			for (int index = 0; index < count && pendingCount < PENDING_CAPACITY; ++index)
			{
				const auto note = static_cast<uintptr_t>(static_cast<unsigned int>(
					NoteListSnapshot::ReadInt(notes + static_cast<uintptr_t>(index) * 4, 0)));
				if (!NoteListSnapshot::LooksReadable(note)) continue;

				const int fretIndex = DecodeClosest(
					NoteListSnapshot::ReadFloat(note, 0x30) - MARKER_OFFSET,
					FRET_CENTERS.data(),
					FRET_CENTERS.size(),
					FRET_EPSILON);
				const int stringIndex = isBass
					? DecodeClosest(
						NoteListSnapshot::ReadFloat(note, 0x34) - MARKER_OFFSET,
						BASS_STRING_HEIGHTS.data(),
						BASS_STRING_HEIGHTS.size(),
						BASS_STRING_EPSILON)
					: DecodeClosest(
						NoteListSnapshot::ReadFloat(note, 0x34) - MARKER_OFFSET,
						STRING_HEIGHTS.data(),
						STRING_HEIGHTS.size(),
						STRING_EPSILON);
				if (fretIndex < 0 || stringIndex < 0) continue;

				const int fret = fretIndex + 1;
				if (IsSelectedCoordinate(state, stringIndex, fret)) continue;

				const float originalPosition = NoteListSnapshot::ReadFloat(note, 0x34);
				if (isPersistent)
				{
					HidePersistently(note, originalPosition, state, stringIndex, fret);
					continue;
				}
				if (!TryWriteFloat(note + 0x34, OFFSCREEN_STRING_POSITION))
				{
					LOG_ERROR("(NBN STOPPED PREVIEW) Could not hide marker record 0x"
						<< std::hex << note << std::dec << "." << std::endl);
					continue;
				}
				pendingNotes[pendingCount] = note;
				pendingStringPositions[pendingCount] = originalPosition;
				++pendingCount;
				didHide = true;

				const auto total = hiddenCount.fetch_add(1, std::memory_order_acq_rel) + 1;
				if (total <= 18 || total % 500 == 0)
				{
					LOG_INFO("(NBN STOPPED PREVIEW) hidden transiently " << stringIndex << ':' << fret
						<< " target=" << state.visualString << ':' << state.visualFret
						<< " note=0x" << std::hex << note << std::dec
						<< " count=" << total << std::endl);
				}
			}
			return didHide;
		}
	}



	uint8_t __cdecl PrepareNoteDrawList(void* renderCtx, void* noteArray)
	{
		return StoppedPreviewFilter::HideNonTarget(renderCtx, noteArray) ? 1 : 0;
	}



	void __cdecl CompleteNoteDrawList()
	{
		StoppedPreviewFilter::RestorePending();
	}
	const std::vector<ResearchProtocol::HookRequest>& GenericHookList()
	{
		static const std::vector<ResearchProtocol::HookRequest> hooks =
		{
		};
		return hooks;
	}

	void __cdecl GetRequestedHooks(
		const ResearchProtocol::HookRequest** requests,
		uint32_t* count)
	{
		const auto& hooks = GenericHookList();
		if (requests != nullptr) *requests = hooks.data();
		if (count != nullptr) *count = static_cast<uint32_t>(hooks.size());
	}
	void __cdecl ObserveGenericHook(uint32_t slotId, const ResearchProtocol::HookContext* context)
	{
		if (context == nullptr) return;
		LOG_INFO("(GENERIC HOOK) slot=" << slotId
			<< " caller=0x" << std::hex << context->returnAddress
			<< " ecx=0x" << context->ecx
			<< " edx=0x" << context->edx
			<< " arg0=0x" << context->stackArgs[0]
			<< " arg1=0x" << context->stackArgs[1]
			<< std::dec);
	}
	struct NeckElementIdentity
	{
		uintptr_t parent = 0;
		uintptr_t sub = 0;
		int stringIndex = -1;
		int fret = -1;
	};

	bool TryReadNeckElementIdentity(uintptr_t ctx, NeckElementIdentity* out) noexcept
	{
		__try
		{
			const auto parent = *reinterpret_cast<volatile uintptr_t*>(ctx + 0x8);
			const auto sub = *reinterpret_cast<volatile uintptr_t*>(ctx + 0xC);
			if (parent < 0x10000 || (parent & 3) != 0) return false;
			if (sub < 0x10000 || (sub & 3) != 0) return false;
			out->parent = parent;
			out->sub = sub;
			out->stringIndex = *reinterpret_cast<volatile uint8_t*>(parent + 0xC);
			out->fret = *reinterpret_cast<volatile uint8_t*>(parent + 0xD);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
	int TryStepNeckElementExpiry(uintptr_t sub) noexcept
	{
		__try
		{
			auto flag0 = reinterpret_cast<volatile uint8_t*>(sub + 0x50);
			auto flag1 = reinterpret_cast<volatile uint8_t*>(sub + 0x51);
			if (*flag0 != 0)
			{
				*flag0 = 0;
				return 0;
			}
			if (*flag1 != 0)
			{
				*flag1 = 0;
				return 1;
			}
			return -1;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	constexpr size_t NECK_EXPIRY_CLEARED_CAPACITY = 64;
	uintptr_t neckExpiryClearedSubs[NECK_EXPIRY_CLEARED_CAPACITY] = {};
	size_t neckExpiryClearedCount = 0;
	int neckExpiryHoldString = -1;
	int neckExpiryHoldFret = -1;
	volatile long neckStepDumpBudget = 0;

	bool TryDumpStepContext(uintptr_t ctx, uint32_t* words, int wordCount) noexcept
	{
		__try
		{
			for (int index = 0; index < wordCount; ++index)
			{
				words[index] = *reinterpret_cast<volatile uint32_t*>(
					ctx + static_cast<uintptr_t>(index) * 4);
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
	volatile long markerDimLogBudget = 60;
	volatile long markerDimPostLogBudget = 48;
	constexpr uint32_t FRETBOARD_MARKER_COMPONENT = 0x533365E4u;
	std::atomic<uint32_t> learnedLitColorBits{ 0 };
	std::atomic<uint32_t> learnedDimColorBits{ 0 };

	bool TryClassifyFretboardElement(uintptr_t element, int* placementFamily) noexcept
	{
		__try
		{
			*placementFamily = static_cast<int>(
				*reinterpret_cast<volatile uint32_t*>(element + 0x10));
			if (*placementFamily != 0) return false;
			const auto resource = *reinterpret_cast<volatile uintptr_t*>(element + 0x74);
			if (resource < 0x10000 || (resource & 3) != 0) return false;
			const auto definition = *reinterpret_cast<volatile uintptr_t*>(resource + 0x5C);
			if (definition < 0x10000 || (definition & 3) != 0) return false;
			const auto head = definition + 0xCC;
			auto node = *reinterpret_cast<volatile uintptr_t*>(head);
			for (int guard = 0; guard < 32 && node != head; ++guard)
			{
				if (node < 0x10000 || (node & 3) != 0) return false;
				if (*reinterpret_cast<volatile uint32_t*>(node + 0x8)
					== FRETBOARD_MARKER_COMPONENT)
				{
					return true;
				}
				node = *reinterpret_cast<volatile uintptr_t*>(node);
			}
			return false;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool IsWhitelistedCoordinate(
		const ResearchProtocol::NoteByNoteState& state,
		int stringIndex,
		int fret) noexcept
	{
		if (stringIndex == state.selectedString && fret == state.selectedFret) return true;
		return StoppedPreviewFilter::IsSelectedCoordinate(state, stringIndex, fret);
	}

	bool TryReadMarkerCtxComposition(
		uintptr_t ctx,
		uint32_t* colorState,
		float* pendingColor,
		bool* isMarkerPath) noexcept
	{
		__try
		{
			*isMarkerPath = *reinterpret_cast<volatile uint8_t*>(ctx + 0x28) != 0;
			*colorState = *reinterpret_cast<volatile uint32_t*>(ctx + 0x24);
			*pendingColor = *reinterpret_cast<volatile float*>(ctx + 0x2C);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
	const char* TryRepaintMarkerCtx(
		uintptr_t ctx,
		bool wantDim,
		uint32_t armColorBits) noexcept
	{
		__try
		{
			auto statePtr = reinterpret_cast<volatile uint32_t*>(ctx + 0x24);
			auto colorPtr = reinterpret_cast<volatile uint32_t*>(ctx + 0x2C);
			const uint32_t current = *statePtr;
			if (wantDim)
			{
				if (current == 1 || current == 3)
				{
					*statePtr = 2;
					if (*colorPtr == 0)
					{
						if (armColorBits == 0) return "dim-state-only";
						*colorPtr = armColorBits;
						return "dim-rearmed";
					}
					return "dim-pending";
				}
				if (current == 2 && *colorPtr == 0 && armColorBits != 0)
				{
					*colorPtr = armColorBits;
					return "dim-armed";
				}
				return nullptr;
			}
			if (current != 2) return nullptr;
			*statePtr = 1;
			if (*colorPtr == 0)
			{
				if (armColorBits == 0) return "lit-state-only";
				*colorPtr = armColorBits;
				return "lit-rearmed";
			}
			return "lit-pending";
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	void __cdecl ObserveNeckPlacementStep(uint32_t site, void* stepContext)
	{
		const bool isPreCall =
			site == (2u | ResearchProtocol::NECK_PLACEMENT_STEP_PRE);
		const bool isPostCall = site == 2u;
		if ((!isPreCall && !isPostCall) || stepContext == nullptr) return;
		if (!isMarkerDimEnabled.load(std::memory_order_relaxed)) return;

		const auto state = NoteByNoteNativeScoring::GetResearchState();
		if (state.isInitialized == 0 || state.visualString < 0) return;
		if (state.visualChordId >= 0) return;

		NeckElementIdentity identity;
		if (!TryReadNeckElementIdentity(
			reinterpret_cast<uintptr_t>(stepContext),
			&identity))
		{
			return;
		}

		int placementFamily = -1;
		if (!TryClassifyFretboardElement(identity.parent, &placementFamily)) return;

		uint32_t colorState = 0;
		float pendingColor = 0.0f;
		bool isMarkerPath = false;
		if (!TryReadMarkerCtxComposition(
			reinterpret_cast<uintptr_t>(stepContext),
			&colorState, &pendingColor, &isMarkerPath))
		{
			return;
		}
		if (!isMarkerPath) return;
		if (pendingColor != 0.0f)
		{
			uint32_t bits = 0;
			std::memcpy(&bits, &pendingColor, sizeof(bits));
			if (colorState == 1 || colorState == 3)
			{
				learnedLitColorBits.store(bits, std::memory_order_relaxed);
			}
			else if (colorState == 2)
			{
				learnedDimColorBits.store(bits, std::memory_order_relaxed);
			}
		}

		if (isPostCall)
		{
			if (InterlockedDecrement(&markerDimPostLogBudget) >= 0)
			{
				LOG_INFO("(NBN MARKDIM POST) " << identity.stringIndex << ":"
					<< identity.fret
					<< " state=" << colorState
					<< " pendingColor=" << pendingColor
					<< " phase=" << static_cast<uint32_t>(state.gatePhase)
					<< " sel=" << state.selectedString << ":" << state.selectedFret
					<< " vis=" << state.visualString << ":" << state.visualFret
					<< " ctx=0x" << std::hex
					<< reinterpret_cast<uintptr_t>(stepContext)
					<< std::dec << std::endl);
			}
			return;
		}

		const bool isTarget = IsWhitelistedCoordinate(
			state, identity.stringIndex, identity.fret);
		const uint32_t armBits = isTarget
			? learnedLitColorBits.load(std::memory_order_relaxed)
			: learnedDimColorBits.load(std::memory_order_relaxed);

		const char* action = TryRepaintMarkerCtx(
			reinterpret_cast<uintptr_t>(stepContext),
			!isTarget,
			armBits);
		if (action != nullptr && InterlockedDecrement(&markerDimLogBudget) >= 0)
		{
			LOG_INFO("(NBN MARKDIM) " << action << " " << identity.stringIndex << ":"
				<< identity.fret
				<< " phase=" << static_cast<uint32_t>(state.gatePhase)
				<< " sel=" << state.selectedString << ":" << state.selectedFret
				<< " vis=" << state.visualString << ":" << state.visualFret
				<< " ctx=0x" << std::hex << reinterpret_cast<uintptr_t>(stepContext)
				<< std::dec << std::endl);
		}
	}

	uint8_t __cdecl DescribeSelectedChordFingering(uintptr_t record, char* buffer, uint32_t bufferLength,
		int32_t* lowestPlayedString)
	{
		int lowest = -1;
		const bool described = NoteByNoteNativeScoring::TryDescribeSelectedChordFingering(
			record, buffer, bufferLength, lowest);
		if (lowestPlayedString != nullptr) *lowestPlayedString = lowest;
		return described ? 1 : 0;
	}

	const ResearchProtocol::ProbeApi probeApi =
	{
		ResearchProtocol::PROBE_API_VERSION,
		sizeof(ResearchProtocol::ProbeApi),
		PROBE_NAME,
		PROBE_BUILD_ID.c_str(),
		&InitializeProbe,
		&ShutdownProbe,
		&ProcessScoringUpdate,
		&ProcessHitDecision,
		&ObserveRenderedAttack,
		&StopProbe,
		&GetState,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		&PrepareNoteDrawList,
		&CompleteNoteDrawList,
		&GetRequestedHooks,
		&ObserveGenericHook,
		&ObserveNeckPlacementStep,
		nullptr,
		&RequestReArmProbe,
		&DescribeSelectedChordFingering
	};
}

#define RSMP_PROBE_EXPORT
extern "C" RSMP_PROBE_EXPORT const ResearchProtocol::ProbeApi* __cdecl RSMP_GetResearchProbeApi(
	uint32_t hostApiVersion,
	const ResearchProtocol::HostApi* hostApi)
{
	if (hostApiVersion != ResearchProtocol::HOST_API_VERSION) return nullptr;
	if (!ResearchProbeRuntime::Initialize(hostApi)) return nullptr;
	return &probeApi;
}
