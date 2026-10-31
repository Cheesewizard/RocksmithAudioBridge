
#include "NoteByNoteHudLabel.hpp"

// The Riff Repeater HUD's "MISSED" row reads "NOTE BY NOTE" while Note by Note is on: the game's localized-string
// resolver is detoured and string 35863 (MISSED) is reassigned after it resolves. 4.x installed it from dllmain.

#include "../Host/NoteByNoteMode.hpp"
#include "../../Core/GameVersion.hpp"
#include "../../Core/Hooking.hpp"

#include "Log.hpp"

#include <cstdint>
#include <cstring>

namespace
{
	const uintptr_t LOC_RESOLVER = AudioBridge::GameVersion::Address(0x00832530, 0x00432800);
	const uintptr_t STRING_ASSIGN = AudioBridge::GameVersion::Address(0x004079D0, 0x00007920);
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
	// `destination` was `out` in 4.x; /W4 refuses that name inside __asm (C4405, a reserved word there).
	void ReassignStringUnguarded(void* destination, const char* text, size_t length)
	{
		const uintptr_t fn = STRING_ASSIGN;
		const char* begin = text;
		const char* end = text + length;
		__asm
		{
			mov eax, destination
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
		// 4.x also gated this on its OverlayToggles "nbn_missed_label", a research switch that shipped on and that only
		// its research bridge flipped; it is always on here.
		if (ok && id == MISSED_LOC_ID
			&& NoteByNoteProbe::IsAutomaticEnabledFast())
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
			<< " game build; the MISSED row keeps its label. Found "
			<< AudioBridge::Hooking::DescribeCodeBytes(LOC_RESOLVER, sizeof(RESOLVER_PROLOGUE)) << "." << std::endl);
		return;
	}

	originalResolve = reinterpret_cast<ResolveFn>(AudioBridge::Hooking::DetourFunction(
		reinterpret_cast<byte*>(LOC_RESOLVER),
		reinterpret_cast<byte*>(ResolveDetour)));
	if (originalResolve == nullptr)
	{
		LOG_ERROR("(NBN HUD LABEL) Could not hook the localized-string resolver." << std::endl);
		return;
	}

	LOG_INFO("(NBN HUD LABEL) Resolver hooked at 0x" << std::hex << LOC_RESOLVER << std::dec
		<< "; MISSED -> \"" << NBN_LABEL << "\" while Note by Note is on." << std::endl);
}
