#include "../stdafx.h"
#include "NoteByNoteNativeScoring.hpp"

#include "../D3D/NoteByNoteHighwayRenderer.hpp"
#include "../Research/ResearchBridge.hpp"
#include "../VersioningStruct.h"

#include <cstring>
namespace
{
	constexpr uintptr_t NATIVE_SCORING_UPDATE = 0x7E2880;
	constexpr uintptr_t NATIVE_HIT_DECISION = 0x7E2640;
	constexpr uintptr_t NATIVE_RENDER_PREPARATION = 0x7E4870;
	constexpr uintptr_t NATIVE_NOTE_HEAD_DRAW = 0xBEBCD0;

	using NativeRenderPreparation = void(__stdcall*)(void* owner, float updateTime);

	std::mutex hookMutex;
	bool areHooksInstalled = false;
	ResearchProtocol::ScoringUpdate originalScoringUpdate = nullptr;
	ResearchProtocol::HitDecision originalHitDecision = nullptr;
	NativeRenderPreparation originalRenderPreparation = nullptr;
	ResearchProtocol::NoteHeadDraw originalNoteHeadDraw = nullptr;
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
		struct SlotEntry
		{
			std::atomic<uint32_t> probeSlotId{ SLOT_UNCLAIMED };
			std::atomic<bool> claimed{ false };
		};

		SlotEntry slots[MAX_HOOKS];
		std::vector<InstalledHook> installedHooks;
		uint8_t* stubPool = nullptr;
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
	void __stdcall RenderPreparationDetour(void* owner, float updateTime)
	{
		GenericHooks::ApplyPending();
		NoteByNoteHighwayRenderer::RefreshSelectedTarget(owner);
		originalRenderPreparation(owner, updateTime);
	}
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
