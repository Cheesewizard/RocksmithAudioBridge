#include "../stdafx.h"
#include "NoteByNoteHudLabel.hpp"

#include "NoteByNoteProbe.hpp"
#include "../OverlayToggles.hpp"

#include <cstdint>
#include <cstring>

// Relabel the Riff Repeater "MISSED" HUD row to "NOTE BY NOTE" in the NATIVE font while
// Note by Note is enabled, so the HUD shows which mode is active. In Note by Note the notes
// freeze and wait, so the miss count is always 0.
//
// WHY A RESOLVER HOOK (and not an overlay). The HUD text is the game's own font, laid out
// once per string through the localized-string resolver and drawn as glyph quads (the
// 0x00DFC98C flush D3DHooks filters). An overlay could only add Arial text over the top;
// to get the game's font the game itself has to lay out our string, which means changing
// what the resolver returns for the label's loc-ID. The label lives in heap UI data as the
// token "$[35863]MISSED" (loc-ID 35863 = "MISSED" in maingame.csv, next to 35855 "Riff
// Repeater" / 35856 "SPEED"); neither the id nor the token is a static literal in the
// module, so a code hook on the resolver is the only seam.
//
// THE RESOLVER (0x00832530, read from the runtime-decrypted image; Rocksmith2014.exe is
// SteamStub-packed so on-disk disassembly is meaningless). Shape:
//     char-string* __thiscall Resolve(this /*ecx = out string*/, Context* ctx /*stack*/)
//   - ret 4 (callee cleans the single stack arg); the out string is `this`, not EAX.
//   - The loc-ID being resolved is ctx->+0x2C (the key it binary-searches ctx->+0x78's
//     table for; the missing-loc fallback the songlist mod hooks at 0x00832647 lives in
//     this same function).
// Expressed to MSVC as __fastcall (this->ecx, a dummy edx, ctx on the stack); one stack
// arg makes __fastcall clean 4 bytes, matching the target's ret 4 exactly.
//
// THE SUBSTITUTION. Let the original resolve first (out string now holds the localized
// "MISSED"), then reassign it to "NOTE BY NOTE" through the game's OWN string assign helper
// (0x004079D0: this in EAX, [begin,end) pushed, ret 8) so we never guess the game's string
// layout. Only when Note by Note is enabled AND the resolved id is exactly 35863 -- the id
// gate is self-protecting: if the +0x2C offset were ever wrong the compare simply fails and
// nothing is touched, so a bad guess is an inert no-op, never a crash.
//
// TIMING. The game resolves a HUD label once when the HUD is built and caches it. So the
// relabel lands when NBN is on as the loop is entered (the menu-rocker path) and, for a
// mid-song toggle, on the next loop rebuild.

namespace
{
	constexpr uintptr_t LOC_RESOLVER = 0x00832530;
	constexpr uintptr_t STRING_ASSIGN = 0x004079D0;
	constexpr uintptr_t CTX_LOC_ID_OFFSET = 0x2C;
	constexpr uint32_t MISSED_LOC_ID = 35863;
	const char NBN_LABEL[] = "NOTE BY NOTE";

	// push ebp; mov ebp,esp; and esp,-8; sub esp,7Ch; mov eax,[cookie]
	const byte RESOLVER_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x7C, 0xA1 };

	using ResolveFn = void* (__fastcall*)(void* self, void* edx, void* ctx);
	ResolveFn originalResolve = nullptr;

	// Diagnostics, read live over the bridge (addresses resolved from the .map). totalCalls
	// proves the resolver is hot; hits35863 counts every MISSED resolution regardless of NBN
	// state (so timing can be told apart from wrong-path); substitutions counts the rewritten
	// ones; lastOutReadback is the out string's first bytes AFTER the rewrite, to confirm
	// "NOTE BY NOTE" actually landed (vs. a wrong assign convention or a cached duplicate).
	volatile long totalCalls = 0;
	volatile long hits35863 = 0;
	volatile long substitutions = 0;
	void* volatile lastSelf = nullptr;
	void* volatile lastResult = nullptr;
	char lastOutReadback[24] = {};

	// Copy up to 20 bytes of the out string's character data for the readback. This string
	// class stores its data pointer at offset 0 (0x4079D0 writes [this]=buffer), SEH-guarded.
	void CaptureReadback(void* out)
	{
		__try
		{
			const char* data = *reinterpret_cast<char* const volatile*>(out);
			for (int i = 0; i < 20; ++i) lastOutReadback[i] = data != nullptr ? data[i] : 0;
			lastOutReadback[20] = 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { lastOutReadback[0] = '?'; lastOutReadback[1] = 0; }
	}

	bool MatchesBytes(uintptr_t address, const byte* expected, size_t count)
	{
		__try { return std::memcmp(reinterpret_cast<const void*>(address), expected, count) == 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	uint32_t ReadLocId(void* ctx, bool& ok)
	{
		ok = false;
		__try
		{
			const uint32_t id = *reinterpret_cast<volatile uint32_t*>(
				reinterpret_cast<uintptr_t>(ctx) + CTX_LOC_ID_OFFSET);
			ok = true;
			return id;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	// out->assign(begin, end) via the game's string assign helper. The addresses live in
	// variables so `call dword ptr[fn]` is a real memory operand (a `call [const]` would run
	// the constant as code); begin is pushed last so it lands at the callee's [ebp+8].
	void ReassignStringUnguarded(void* out, const char* text, size_t length)
	{
		const uintptr_t fn = STRING_ASSIGN;
		const char* begin = text;
		const char* end = text + length;
		__asm
		{
			mov eax, out
			push end
			push begin
			call dword ptr[fn]
		}
	}

	// SEH must live in a POD-only function (no C++ objects that require unwinding), so the
	// reassign is wrapped here and the logging stays out in ResolveDetour.
	bool ReassignStringGuarded(void* out, const char* text, size_t length)
	{
		__try
		{
			ReassignStringUnguarded(out, text, length);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	void* __fastcall ResolveDetour(void* self, void* edx, void* ctx)
	{
		void* result = originalResolve(self, edx, ctx);
		InterlockedIncrement(&totalCalls);
		bool ok = false;
		const uint32_t id = ReadLocId(ctx, ok);
		if (ok && id == MISSED_LOC_ID) InterlockedIncrement(&hits35863);
		// Cheap, LOCK-FREE gate. The resolver runs on the UI thread for every localized string,
		// and the N-key toggle resolves strings while holding the controller mutex, so taking
		// that mutex here (via IsAutomaticEnabled) would self-deadlock the UI thread.
		if (ok && id == MISSED_LOC_ID
			&& NoteByNoteProbe::IsAutomaticEnabledFast()
			&& OverlayToggles::Get("nbn_missed_label"))
		{
			// The output string is the caller-provided `this` (ECX == self). The resolver does
			// NOT return it in EAX (EAX holds a non-object value), so targeting the return would
			// corrupt a random object.
			lastSelf = self;
			lastResult = result;
			if (ReassignStringGuarded(self, NBN_LABEL, sizeof(NBN_LABEL) - 1))
			{
				CaptureReadback(self);
				InterlockedIncrement(&substitutions);
			}
		}
		return result;
	}
}

void NoteByNoteHudLabel::Initialize()
{
	if (!MatchesBytes(LOC_RESOLVER, RESOLVER_PROLOGUE, sizeof(RESOLVER_PROLOGUE)))
	{
		LOG_ERROR("(NBN HUD LABEL) The localized-string resolver does not match the expected"
			<< " game build; the MISSED row keeps its label." << std::endl);
		return;
	}

	originalResolve = reinterpret_cast<ResolveFn>(DetourFunction(
		reinterpret_cast<byte*>(LOC_RESOLVER),
		reinterpret_cast<byte*>(ResolveDetour)));
	if (originalResolve == nullptr)
	{
		LOG_ERROR("(NBN HUD LABEL) Could not hook the localized-string resolver." << std::endl);
		return;
	}

	LOG_INFO("(NBN HUD LABEL) Resolver hooked at 0x" << std::hex << LOC_RESOLVER << std::dec
		<< "; MISSED -> \"" << NBN_LABEL << "\" while Note by Note is on"
		<< " (toggle Overlay_nbn_missed_label)." << std::endl);
}
