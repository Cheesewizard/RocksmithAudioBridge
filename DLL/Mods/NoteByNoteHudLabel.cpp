#include "../stdafx.h"
#include "NoteByNoteHudLabel.hpp"

#include "NoteByNoteProbe.hpp"
#include "../OverlayToggles.hpp"

#include <cstdint>
#include <cstring>

namespace
{
	constexpr uintptr_t LOC_RESOLVER = 0x00832530;
	constexpr uintptr_t STRING_ASSIGN = 0x004079D0;
	constexpr uintptr_t CTX_LOC_ID_OFFSET = 0x2C;
	constexpr uint32_t MISSED_LOC_ID = 35863;
	const char NBN_LABEL[] = "NOTE BY NOTE";
	const byte RESOLVER_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x7C, 0xA1 };

	using ResolveFn = void* (__fastcall*)(void* self, void* edx, void* ctx);
	ResolveFn originalResolve = nullptr;
	volatile long totalCalls = 0;
	volatile long hits35863 = 0;
	volatile long substitutions = 0;
	void* volatile lastSelf = nullptr;
	void* volatile lastResult = nullptr;
	char lastOutReadback[24] = {};
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
		if (ok && id == MISSED_LOC_ID
			&& NoteByNoteProbe::IsAutomaticEnabledFast()
			&& OverlayToggles::Get("nbn_missed_label"))
		{
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
