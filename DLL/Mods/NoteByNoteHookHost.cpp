#include "../stdafx.h"
#include "NoteByNoteNativeScoring.hpp"

#include "../D3D/NoteByNoteHighwayRenderer.hpp"
#include "../Research/ResearchBridge.hpp"
#include "../VersioningStruct.h"

#include <cstring>

// Rocksmith's presentability predicate (0x7A5CE0) also owns note retirement. It remains
// detoured only as a pass-through so presentation work cannot retire future scoring records.
// Visual focus must preserve the native lifecycle: skipping construction at 0x7A56F0 leaves
// 0x7A5D50 re-admitting the note to the string/fret grid every frame, and calling retirement
// 0x7A61F0 from outside races the engine's own iteration over the same note vector.
//
// The predicate reduces to "currentTime < noteEndTime". The controller freezes the transport,
// so during a hold no note fails the test and the whole remaining chart is redrawn every
// frame; the frame-coupled scoring tick drops with the frame rate. This does not starve the
// onset detector: it reports the analysis ring's current frame on every call, so refusals
// come from the native input queries' amplitude gates, not the poll rate.
//
// Fretboard focus is applied after the native grid write has completed. This keeps the
// original side effects, clears only the resulting non-gesture cell, and never moves or
// retires the underlying note object.
namespace
{
	constexpr uintptr_t NATIVE_SCORING_UPDATE = 0x7E2880;
	constexpr uintptr_t NATIVE_HIT_DECISION = 0x7E2640;
	constexpr uintptr_t NATIVE_RENDER_PREPARATION = 0x7E4870;

	// The note-head draw named by the render snapshot: __thiscall(renderCtx, noteArray), where
	// noteArray[1] is the count and noteArray[0] points to an array of note-record pointers. A
	// pass-through detour forwards its arguments to the probe so the note records can be read.
	// Research instrumentation, so its install is not allowed to disable the controller.
	constexpr uintptr_t NATIVE_NOTE_HEAD_DRAW = 0xBEBCD0;

	using NativeRenderPreparation = void(__stdcall*)(void* owner, float updateTime);

	std::mutex hookMutex;
	bool areHooksInstalled = false;
	ResearchProtocol::ScoringUpdate originalScoringUpdate = nullptr;
	ResearchProtocol::HitDecision originalHitDecision = nullptr;
	NativeRenderPreparation originalRenderPreparation = nullptr;
	ResearchProtocol::NoteHeadDraw originalNoteHeadDraw = nullptr;

	// ---- Generic observation-hook service ------------------------------------------------
	//
	// The probe names native addresses it wants observed; this installs a pass-through detour
	// on each and forwards the entry register file to the probe. The point is to take new-hook
	// churn out of this DLL: observing a new address costs a line in the probe's request list
	// and a reload, not an edit here and a game restart.
	//
	// Each hook gets a 27-byte stub generated at runtime, unique because it bakes in a dense
	// host slot index and its own trampoline pointer:
	//
	//     pushad                     ; save the entry register file
	//     pushfd                     ; and flags
	//     push esp                   ; -> HookContext overlay (points at the saved flags)
	//     push <hostIndex>           ; the host slot this stub belongs to
	//     mov eax, <dispatcher>
	//     call eax                   ; __stdcall, cleans its own two args
	//     popfd
	//     popad                      ; restore exactly, so the original is entered unperturbed
	//     jmp dword ptr [trampoline] ; always run the original (inert when unclaimed)
	//
	// The dispatcher forwards only when the slot is currently claimed, so a detour left behind
	// by a probe that no longer wants it becomes a pure pass-through rather than being removed
	// (DetourFunction has no safe uninstall). All stub allocation and detouring happens on the
	// game's main thread from the render-preparation seam; the pipe thread only stages the
	// request. Hooks placed on functions that run on other threads keep a one-time install race.
	namespace GenericHooks
	{
		constexpr uint32_t SLOT_UNCLAIMED = 0xFFFFFFFFu;
		constexpr size_t MAX_HOOKS = 64;
		constexpr size_t STUB_SIZE = 32;      // 27 bytes used, padded for alignment
		constexpr size_t STUB_TRAMP_OFFSET = 23;

		struct InstalledHook
		{
			uint32_t address = 0;
			void* trampoline = nullptr;
			uint8_t* stub = nullptr;
		};

		// Hot-path slot state, read by the dispatcher on whatever thread the hooked function
		// runs on. Atomic so reconcile can claim, re-point, or unclaim without locking the
		// dispatcher. Indexed by the dense host slot index baked into each stub.
		struct SlotEntry
		{
			std::atomic<uint32_t> probeSlotId{ SLOT_UNCLAIMED };
			std::atomic<bool> claimed{ false };
		};

		SlotEntry slots[MAX_HOOKS];

		// installedHooks and the stub pool are touched only on the main thread during reconcile.
		std::vector<InstalledHook> installedHooks;
		uint8_t* stubPool = nullptr;

		// Staged by the pipe thread, drained on the main thread.
		std::mutex requestMutex;
		std::vector<ResearchProtocol::HookRequest> pendingRequests;
		std::atomic<bool> pendingDirty{ false };

		void __stdcall Dispatch(uint32_t hostIndex, const ResearchProtocol::HookContext* context)
		{
			if (hostIndex >= MAX_HOOKS) return;
			if (!slots[hostIndex].claimed.load(std::memory_order_acquire)) return;
			ResearchBridge::DispatchGenericHook(
				slots[hostIndex].probeSlotId.load(std::memory_order_acquire),
				context);
		}

		bool EnsureStubPool()
		{
			if (stubPool != nullptr) return true;
			stubPool = static_cast<uint8_t*>(VirtualAlloc(
				nullptr,
				MAX_HOOKS * STUB_SIZE,
				MEM_COMMIT | MEM_RESERVE,
				PAGE_EXECUTE_READWRITE));
			return stubPool != nullptr;
		}

		void WriteStub(uint8_t* stub, uint32_t hostIndex)
		{
			const uint32_t dispatcher = static_cast<uint32_t>(
				reinterpret_cast<uintptr_t>(&Dispatch));
			const uint32_t storagePtr = static_cast<uint32_t>(
				reinterpret_cast<uintptr_t>(stub + STUB_TRAMP_OFFSET));
			size_t i = 0;
			stub[i++] = 0x60;                                   // pushad
			stub[i++] = 0x9C;                                   // pushfd
			stub[i++] = 0x54;                                   // push esp
			stub[i++] = 0x68;                                   // push imm32 (hostIndex)
			std::memcpy(stub + i, &hostIndex, 4); i += 4;
			stub[i++] = 0xB8;                                   // mov eax, imm32 (dispatcher)
			std::memcpy(stub + i, &dispatcher, 4); i += 4;
			stub[i++] = 0xFF; stub[i++] = 0xD0;                 // call eax
			stub[i++] = 0x9D;                                   // popfd
			stub[i++] = 0x61;                                   // popad
			stub[i++] = 0xFF; stub[i++] = 0x25;                 // jmp dword ptr [imm32]
			std::memcpy(stub + i, &storagePtr, 4); i += 4;      // -> STUB_TRAMP_OFFSET
			uint32_t placeholder = 0;
			std::memcpy(stub + i, &placeholder, 4);             // trampoline, patched post-detour
		}

		// Main thread only. Installs a pass-through detour on address and returns its dense host
		// slot index, or SLOT_UNCLAIMED on failure.
		uint32_t Install(uint32_t address)
		{
			if (installedHooks.size() >= MAX_HOOKS || !EnsureStubPool())
			{
				return SLOT_UNCLAIMED;
			}
			const uint32_t hostIndex = static_cast<uint32_t>(installedHooks.size());
			uint8_t* stub = stubPool + hostIndex * STUB_SIZE;
			WriteStub(stub, hostIndex);

			void* trampoline = DetourFunction(
				reinterpret_cast<PBYTE>(static_cast<uintptr_t>(address)),
				reinterpret_cast<PBYTE>(stub));
			if (trampoline == nullptr)
			{
				LOG_ERROR("(NBN GENERIC HOOK) DetourFunction failed for 0x" << std::hex
					<< address << std::dec << "." << std::endl);
				return SLOT_UNCLAIMED;
			}
			std::memcpy(stub + STUB_TRAMP_OFFSET, &trampoline, 4);
			FlushInstructionCache(GetCurrentProcess(), stub, STUB_SIZE);

			installedHooks.push_back({ address, trampoline, stub });
			LOG_INFO("(NBN GENERIC HOOK) Installed pass-through detour on 0x" << std::hex
				<< address << std::dec << " as host slot " << hostIndex << "." << std::endl);
			return hostIndex;
		}

		// Main thread only. Reconciles the installed hooks against the latest request list:
		// installs any new address, (re)claims requested slots to the probe's slot id, and
		// unclaims slots the current probe no longer asks for.
		void ApplyPending()
		{
			if (!pendingDirty.exchange(false, std::memory_order_acq_rel)) return;

			std::vector<ResearchProtocol::HookRequest> requests;
			{
				std::lock_guard<std::mutex> lock(requestMutex);
				requests = pendingRequests;
			}

			bool requested[MAX_HOOKS] = {};
			for (const auto& request : requests)
			{
				uint32_t hostIndex = SLOT_UNCLAIMED;
				for (size_t index = 0; index < installedHooks.size(); ++index)
				{
					if (installedHooks[index].address == request.address)
					{
						hostIndex = static_cast<uint32_t>(index);
						break;
					}
				}
				if (hostIndex == SLOT_UNCLAIMED) hostIndex = Install(request.address);
				if (hostIndex == SLOT_UNCLAIMED) continue;

				slots[hostIndex].probeSlotId.store(request.slotId, std::memory_order_release);
				slots[hostIndex].claimed.store(true, std::memory_order_release);
				requested[hostIndex] = true;
			}

			for (size_t index = 0; index < installedHooks.size(); ++index)
			{
				if (!requested[index])
				{
					slots[index].claimed.store(false, std::memory_order_release);
				}
			}
		}

		void SetRequested(const ResearchProtocol::HookRequest* requests, uint32_t count)
		{
			std::vector<ResearchProtocol::HookRequest> copy;
			if (requests != nullptr)
			{
				const uint32_t limited = count > MAX_HOOKS ? MAX_HOOKS : count;
				if (count > MAX_HOOKS)
				{
					LOG_WARNING("(NBN GENERIC HOOK) Probe requested " << count << " hooks; only "
						<< MAX_HOOKS << " can be installed." << std::endl);
				}
				copy.assign(requests, requests + limited);
			}
			{
				std::lock_guard<std::mutex> lock(requestMutex);
				pendingRequests = std::move(copy);
			}
			pendingDirty.store(true, std::memory_order_release);
		}
	}

	void __stdcall ScoringUpdateDetour(void* owner, float updateTime)
	{
		ResearchBridge::DispatchScoringUpdate(owner, updateTime, originalScoringUpdate);
	}

	bool __fastcall HitDecisionDetour(void* owner, void* unusedEdx, void* note)
	{
		bool result = false;
		ResearchBridge::DispatchHitDecision(
			owner,
			unusedEdx,
			note,
			originalHitDecision,
			result);
		return result;
	}

	// The only work done here is refreshing the cached target so the per-note
	// predicate stays a load and a compare. Nothing walks the note vector, nothing
	// allocates, and nothing mutates native presentation from out here.
	void __stdcall RenderPreparationDetour(void* owner, float updateTime)
	{
		// A known main-thread seam, so all generic-hook detour installation happens here rather
		// than on the pipe thread that stages the request.
		GenericHooks::ApplyPending();
		NoteByNoteHighwayRenderer::RefreshSelectedTarget(owner);
		originalRenderPreparation(owner, updateTime);
	}

	// The probe may move a classified non-target preview marker off-screen. Keep the probe
	// locked across the original call and restore the record immediately on return, so the
	// visibility change cannot leak into later notes or depend on a D3D submission boundary.
	void __fastcall NoteHeadDrawDetour(void* renderCtx, void* unusedEdx, int* noteArray)
	{
		ResearchBridge::ProcessNoteDrawList(
			renderCtx,
			unusedEdx,
			noteArray,
			originalNoteHeadDraw);
	}
}

void NoteByNoteNativeScoring::Initialize()
{
	std::lock_guard<std::mutex> lock(hookMutex);
	if (areHooksInstalled) return;

	// Every address below (and the highway renderer's) is from the September 2022 exe. Other builds (the December 2024
	// "LP" build upstream supports, or a modified exe) have different code there, and writing these detours into it
	// crashed or corrupted the game at boot even for players who never use Note by Note. Anything but the exact
	// build leaves Note by Note unavailable, which the rest of the mod already handles (IsAvailable false).
	if (!IsExactGameBuild(VersionType::RemasteredSeptember2022))
	{
		LOG_ERROR("(NBN HOOKS) This Rocksmith build is not the September 2022 release Note by Note is mapped for;"
			<< " Note by Note stays off and none of its hooks are installed." << std::endl);
		return;
	}

	originalScoringUpdate = reinterpret_cast<ResearchProtocol::ScoringUpdate>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_SCORING_UPDATE),
		reinterpret_cast<PBYTE>(&ScoringUpdateDetour)));
	originalHitDecision = reinterpret_cast<ResearchProtocol::HitDecision>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_HIT_DECISION),
		reinterpret_cast<PBYTE>(&HitDecisionDetour)));
	originalRenderPreparation = reinterpret_cast<NativeRenderPreparation>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_RENDER_PREPARATION),
		reinterpret_cast<PBYTE>(&RenderPreparationDetour)));
	if (originalScoringUpdate == nullptr
		|| originalHitDecision == nullptr
		|| originalRenderPreparation == nullptr)
	{
		LOG_ERROR("(NBN HOOK HOST) Installing the permanent scoring, hit-decision, and"
			<< " preparation detours failed"
			<< " (scoring=" << (originalScoringUpdate != nullptr)
			<< ", decision=" << (originalHitDecision != nullptr)
			<< ", preparation=" << (originalRenderPreparation != nullptr)
			<< ")." << std::endl);
		return;
	}

	const bool isGateInstalled = NoteByNoteHighwayRenderer::InstallPresentationGate();

	areHooksInstalled = true;
	LOG_INFO("(NBN HOOK HOST) Permanent native scoring, hit-decision, and preparation detours"
		<< " installed; non-destructive presentation hooks installed=" << isGateInstalled << "."
		<< " Controller behavior is supplied by the reloadable research probe." << std::endl);

	// Research instrumentation, installed separately so a failure cannot disable the controller.
	originalNoteHeadDraw = reinterpret_cast<ResearchProtocol::NoteHeadDraw>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_NOTE_HEAD_DRAW),
		reinterpret_cast<PBYTE>(&NoteHeadDrawDetour)));
	LOG_INFO("(NBN HOOK HOST) Note-head draw research detour installed="
		<< (originalNoteHeadDraw != nullptr) << " at 0x" << std::hex << NATIVE_NOTE_HEAD_DRAW
		<< std::dec << "." << std::endl);
}

bool NoteByNoteNativeScoring::IsAvailable()
{
	std::lock_guard<std::mutex> lock(hookMutex);
	return areHooksInstalled && ResearchBridge::IsProbeLoaded();
}

void NoteByNoteNativeScoring::SetRequestedGenericHooks(
	const ResearchProtocol::HookRequest* requests,
	uint32_t count)
{
	GenericHooks::SetRequested(requests, count);
}

void NoteByNoteNativeScoring::ObserveRenderedAttack(
	const NoteByNoteProbe::NativeRenderedAttack& attack)
{
	ResearchBridge::DispatchRenderedAttack(attack);
}

void NoteByNoteNativeScoring::Stop()
{
	ResearchBridge::DispatchStop();
	NoteByNoteHighwayRenderer::ClearSelectedTarget();
}

void NoteByNoteNativeScoring::RequestReArm()
{
	ResearchBridge::DispatchRequestReArm();
}

bool NoteByNoteNativeScoring::TryGetResearchState(ResearchProtocol::NoteByNoteState& state)
{
	return ResearchBridge::TryGetNoteByNoteState(state);
}
