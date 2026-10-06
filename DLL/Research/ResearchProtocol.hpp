#pragma once

#include <cstddef>
#include <cstdint>
#include "../Mods/NoteByNoteTypes.hpp"

namespace ResearchProtocol
{
	constexpr uint32_t PROBE_API_VERSION = 13;
	constexpr uint32_t PROBE_API_MIN_VERSION = 13;
	constexpr uint32_t NECK_PLACEMENT_STEP_PRE = 0x100;
	struct HookRequest
	{
		uint32_t address = 0;
		uint32_t slotId = 0;
	};
	struct HookContext
	{
		uint32_t eflags;
		uint32_t edi;
		uint32_t esi;
		uint32_t ebp;
		uint32_t esp;
		uint32_t ebx;
		uint32_t edx;
		uint32_t ecx;
		uint32_t eax;
		uint32_t returnAddress;
		uint32_t stackArgs[16];
	};

	struct ProbeApi
	{
		uint32_t version = PROBE_API_VERSION;
		uint32_t structSize = sizeof(ProbeApi);
		const char* name = nullptr;
		const char* buildId = nullptr;
		uint8_t(__cdecl* Initialize)() = nullptr;
		void(__cdecl* Shutdown)() = nullptr;
		void(__stdcall* ProcessScoringUpdate)(void* owner, float updateTime, ScoringUpdate original) = nullptr;
		bool(__fastcall* ProcessHitDecision)(void* owner, void* unusedEdx, void* note, HitDecision original) = nullptr;
		void(__cdecl* ObserveRenderedAttack)(const RenderedAttack* attack) = nullptr;
		void(__cdecl* Stop)() = nullptr;
		uint8_t(__cdecl* GetState)(NoteByNoteState* state) = nullptr;
		void(__cdecl* ArmRenderSnapshot)() = nullptr;
		void(__cdecl* ObserveNativeDraw)(const NativeDrawObservation* draw) = nullptr;
		void(__cdecl* NotifyRenderFrameComplete)(uint64_t renderFrame) = nullptr;
		void(__cdecl* ArmNoteDrawListSnapshot)() = nullptr;
		void(__cdecl* ObserveNoteDrawList)(void* renderCtx, void* noteArray) = nullptr;
		void(__cdecl* ArmScreenMapSnapshot)() = nullptr;
		uint8_t(__cdecl* HandleProbeCommand)(
			const char* requestJson,
			char* responseBuffer,
			uint32_t responseCapacity) = nullptr;
		uint8_t(__cdecl* PrepareNoteDrawList)(void* renderCtx, void* noteArray) = nullptr;
		void(__cdecl* CompleteNoteDrawList)() = nullptr;
		void(__cdecl* GetRequestedHooks)(const HookRequest** requests, uint32_t* count) = nullptr;
		void(__cdecl* ObserveGenericHook)(uint32_t slotId, const HookContext* context) = nullptr;
		void(__cdecl* ObserveNeckPlacementStep)(uint32_t site, void* stepContext) = nullptr;
		void(__cdecl* ObserveFullDraw)(const NativeDrawObservation* draw) = nullptr;
		void(__cdecl* RequestReArm)() = nullptr;
		uint8_t(__cdecl* DescribeSelectedChordFingering)(uintptr_t record, char* buffer,
			uint32_t bufferLength, int32_t* lowestPlayedString) = nullptr;
	};
	constexpr size_t REQUIRED_PROBE_API_SIZE = offsetof(ProbeApi, GetRequestedHooks);

	using GetProbeApi = const ProbeApi* (__cdecl*)(uint32_t hostApiVersion, const HostApi* hostApi);
}
