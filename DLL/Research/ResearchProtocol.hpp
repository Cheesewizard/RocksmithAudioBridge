#pragma once

#include <cstddef>
#include <cstdint>

// The Note by Note domain types (ScoringNote, NoteByNoteState, RawToneEvidence, and the rest)
// live in Mods/NoteByNoteTypes.hpp so the in-process scoring has no research dependency. This
// header holds only the reloadable-probe ABI (HostApi / ProbeApi) and the generic-hook plumbing,
// which are research-only.
#include "../Mods/NoteByNoteTypes.hpp"

namespace ResearchProtocol
{
	// v13: NoteByNoteState is a lockstep ABI. The hold-veto readout and bend verdict changed
	// its size, so a controller built against the previous layout must be rejected at startup.
	//
	// v12 introduced generic observation hooks. The probe names addresses it wants observed; the host
	// installs pass-through detours and calls back with the saved register file. From here on,
	// appending optional ProbeApi entries no longer bumps the version: the bridge accepts any
	// probe at or above PROBE_API_MIN_VERSION whose struct covers the required prefix, so a new
	// entry point costs a probe rebuild and reload, never a game restart.
	constexpr uint32_t PROBE_API_VERSION = 13;
	// The oldest probe the resident bridge still accepts. Raise this only when the required
	// prefix (through CompleteNoteDrawList) changes shape; appending optional entries does not.
	constexpr uint32_t PROBE_API_MIN_VERSION = 13;

	// OR-ed into the site argument of ObserveNeckPlacementStep when the forward happens at
	// the native step's entry rather than after its return. Only site 2 forwards pre-call;
	// see the ObserveNeckPlacementStep comment.
	constexpr uint32_t NECK_PLACEMENT_STEP_PRE = 0x100;

	// One address the probe wants observed. slotId is the probe's own tag, echoed back to
	// ObserveGenericHook so one callback can serve many hooks. The host installs a
	// pass-through detour on address that always runs the original, so requesting a hook can
	// never change behavior, only observe it.
	struct HookRequest
	{
		uint32_t address = 0;
		uint32_t slotId = 0;
	};

	// The machine state captured at the entry of a generically hooked function, as an overlay
	// on the live stack rather than a copy: the host's per-hook stub does pushad/pushfd and
	// hands the probe a pointer into that frame. registers are the general-purpose file at
	// entry (ecx = thiscall `this` or fastcall arg0, edx = fastcall arg1). returnAddress is the
	// native caller that invoked the hooked function, which usually names the calling function
	// outright. stackArgs is a window over the caller's stack immediately past the return
	// address: for cdecl/stdcall stackArgs[0] is the first argument, for thiscall the first
	// argument past `this`, for fastcall the third argument onward. The overlay is valid only
	// for the duration of the callback; the probe must not retain the pointer.
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

		// Render snapshot, kept in the probe so it iterates without a game restart.
		// The host installs no new hook for this: it forwards the note-head draw it already
		// filters at DrawIndexedPrimitive, plus a per-frame boundary tick, and the probe owns
		// the arm/capture/log. Reload replaces all three, so what is captured and how it is
		// logged changes at probe-build speed. Appended after GetState; the exact-version gate
		// in the bridge guarantees a loaded probe supplies them.
		void(__cdecl* ArmRenderSnapshot)() = nullptr;
		void(__cdecl* ObserveNativeDraw)(const NativeDrawObservation* draw) = nullptr;
		void(__cdecl* NotifyRenderFrameComplete)(uint64_t renderFrame) = nullptr;

		// Native note-list snapshot. A one-time host detour on FUN_00BEBCD0 (the note-head
		// draw) forwards its arguments here: renderCtx is param_1, noteArray is param_2, an
		// int* whose [1] is the count and [0] points to an array of note-record pointers. The
		// probe dereferences them in-process to read each note's transform and time field, so a
		// not-yet-due note placed on the neck can be told apart from legitimate highway
		// read-ahead. This exists because the D3D-layer snapshot cannot see the note records;
		// only the native draw has them. Appended after the render-snapshot slots.
		void(__cdecl* ArmNoteDrawListSnapshot)() = nullptr;
		void(__cdecl* ObserveNoteDrawList)(void* renderCtx, void* noteArray) = nullptr;

		// Screen-map snapshot: joins the note identity seen at the FUN_00BEBCD0 detour with
		// the vertex-shader constants of the D3D draw it issues, projecting each note's
		// position to screen coordinates. This answers which of the ~140 drawn note heads
		// are the two fretboard markers, which neither capture could answer alone: the D3D
		// layer has the matrices but no note identity, the native layer has the notes but no
		// screen mapping. Appended for version 5.
		void(__cdecl* ArmScreenMapSnapshot)() = nullptr;

		// Escape hatch so future probe-side captures do not need a protocol bump: the bridge
		// forwards any pipe command starting with "probe_" here verbatim. requestJson is the
		// full request; the probe writes a JSON object into responseBuffer (NUL-terminated,
		// at most responseCapacity bytes) and returns 1, or returns 0 to report the command
		// as unknown. Adding a new arm verb then costs a probe rebuild plus a reload, never
		// a game restart. Appended for version 5.
		uint8_t(__cdecl* HandleProbeCommand)(
			const char* requestJson,
			char* responseBuffer,
			uint32_t responseCapacity) = nullptr;

		// The fix seam for the fretboard read-ahead markers. When the transport is stopped,
		// the game assigns a pool of preview-marker quads (neck-anchored
		// space: x = time-until-due, y = fret position, z = string height) to upcoming notes
		// at x = 0 (native pause-preview behavior that Note by Note's frozen transport
		// triggers permanently). PrepareNoteDrawList may temporarily move a classified
		// non-target marker off-screen and returns 1 when it did so. The host always runs the
		// original draw, then calls CompleteNoteDrawList before releasing the probe lock.
		uint8_t(__cdecl* PrepareNoteDrawList)(void* renderCtx, void* noteArray) = nullptr;
		void(__cdecl* CompleteNoteDrawList)() = nullptr;

		// ---- Optional from here down: structSize-gated, no version bump to append. ----

		// Generic observation hooks (v12). GetRequestedHooks reports the addresses the probe
		// wants observed; the host installs a pass-through detour on each and calls
		// ObserveGenericHook on every hit with the entry register file. Both null means the
		// probe wants no generic hooks. This is the seam that lets a newly discovered address
		// be observed at probe-rebuild speed with no host change: the address list lives in the
		// probe, so adding one is a probe rebuild and a reload, not a game restart. The pointer
		// returned by GetRequestedHooks must stay valid until the next call or Shutdown.
		void(__cdecl* GetRequestedHooks)(const HookRequest** requests, uint32_t* count) = nullptr;
		void(__cdecl* ObserveGenericHook)(uint32_t slotId, const HookContext* context) = nullptr;

		// Neck-placement step forwarding. The host's placement-site detours forward sites
		// 2, 4 and 5 here with the native step context, so marker-expiry policy lives in the
		// reloadable probe and iterates without a game restart. The element state bytes at
		// (ctx+0xC)+0x50/0x51 gate marker visibility but also feed the note state machine
		// the scoring commit waits on, so host-side writes there can fault that commit.
		// Null means the probe does not observe placement steps.
		//
		// Site 2 (the color step 0x7A8E90) additionally forwards BEFORE the native call
		// with NECK_PLACEMENT_STEP_PRE OR-ed into the site. The color step applies its
		// marker color one-shot per context (it consumes ctx+0x2C), so a policy that
		// overrides the context's baked color state at ctx+0x24 must run at entry; the
		// post-call forward arrives after the application. Probes that predate the flag
		// filter on `site != 2` and ignore the flagged calls.
		void(__cdecl* ObserveNeckPlacementStep)(uint32_t site, void* stepContext) = nullptr;

		// Full draw feed. The host forwards EVERY in-song draw from all four device entry
		// points (DrawIndexedPrimitive, DrawPrimitive, DrawPrimitiveUP,
		// DrawIndexedPrimitiveUP), not just the filtered note-head classes ObserveNativeDraw
		// receives, for captures of draws that do not match the note-head signatures.
		// drawSite and userPointerData are filled on this path. Null means the probe does
		// not consume the feed; the host also gates dispatch behind its full-draw-feed toggle.
		void(__cdecl* ObserveFullDraw)(const NativeDrawObservation* draw) = nullptr;

		// Re-arm the bootstrap on the next scoring tick. The enable event (the Riff
		// Repeater N toggle, or a debug-state restore) fires host-side, independent of the
		// scoring loop, so a section change that keeps the SAME GamePlaysongLAS owner is
		// invisible to the owner-identity reset and would leave stale bootstrap state
		// pointing at the previous section. The host calls this on every false->true
		// enable; the probe latches it and runs ResetBootstrap on its next tick. Null means
		// the probe predates the seam. Appended after ObserveFullDraw.
		void(__cdecl* RequestReArm)() = nullptr;

		// Selected chord's fingering for the HUD ("[x/x/x/7/7/x]", low E first) plus the lowest
		// played string for the colour swatch. Read from the probe because the host's own copy of
		// NoteByNoteNativeScoring stops tracking the song once a probe is hot-loaded. Returns 1
		// when record is the selected chord. Null means the probe predates the seam.
		uint8_t(__cdecl* DescribeSelectedChordFingering)(uintptr_t record, char* buffer,
			uint32_t bufferLength, int32_t* lowestPlayedString) = nullptr;
	};

	// Everything through CompleteNoteDrawList is required; a probe whose struct is at least this
	// large and supplies those pointers is accepted. Entries after this are optional and probed
	// by structSize, so appending one needs no version bump.
	constexpr size_t REQUIRED_PROBE_API_SIZE = offsetof(ProbeApi, GetRequestedHooks);

	using GetProbeApi = const ProbeApi* (__cdecl*)(uint32_t hostApiVersion, const HostApi* hostApi);
}
