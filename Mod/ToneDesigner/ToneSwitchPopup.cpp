#include "stdafx.h"
#include "ToneSwitchPopup.hpp"
#include "GameTones.hpp"

#include <cstdint>
#include <cstring>
#include "../Core/GameVersion.hpp"
#include "../Core/Hooking.hpp"

namespace
{
	// ShowNotification (VA 0x7524A0 in the September 2022 Remastered build), as an RVA so a relocated exe (Mandatory
	// ASLR) still finds it. Custom convention: EDX = notification type, ESI = out id string, caller cleans 0x30 bytes;
	// [esp+0x04] context, [esp+0x08] title, [esp+0x0C] body (const game string*), [esp+0x10] duration.
	const uintptr_t SHOW_NOTIFICATION_RVA = AudioBridge::GameVersion::Rva(0x003524A0, 0x00352F80);
	constexpr uint32_t TYPE_TONE_CHANGE = 7;
	const byte PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x38 };
	// Unique body bytes 0x10 in, so a different function that happens to start the same way is not hooked.
	const byte BODY_SIGNATURE[] = { 0x8B, 0x45, 0x10, 0xD9, 0x45, 0x14, 0x89, 0x45, 0xD4, 0xD9, 0x5D, 0xD8, 0x8A, 0x45, 0x18, 0x88, 0x45, 0xCC };

	// The game's 24-byte string in its heap form: +0x00 data, +0x10 end, +0x14 capacity end (inline form only when
	// the capacity end points at +0x10). The callee only reads it and copies the text.
	struct GameString
	{
		const char* data;
		char unused[12];
		const char* end;
		const char* capacityEnd;
	};
	static_assert(sizeof(GameString) == 24, "game string layout");

	SRWLOCK g_lock = SRWLOCK_INIT;
	char g_published[128] = {};     // written by SetToneName
	char g_shown[128] = {};         // the copy the game reads during one call (game thread only)
	GameString g_body{};
	volatile long g_substitutions = 0;
	void* g_original = nullptr;

	constexpr uintptr_t READOUT_COUNTDOWN = 0x500;
	bool WriteFloatGuarded(uintptr_t address, float value)
	{
		__try { *reinterpret_cast<volatile float*>(address) = value; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool MatchesBytes(uintptr_t address, const byte* expected, size_t count)
	{
		__try { return std::memcmp(reinterpret_cast<const void*>(address), expected, count) == 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Game thread, inside the detour: points the body argument at our name. Never blocks: if the overlay is writing
	// the name this instant, the game's own text shows this once.
	void __stdcall SubstituteBody(const GameString** bodySlot)
	{
		if (!TryAcquireSRWLockShared(&g_lock)) return;
		const size_t length = strnlen(g_published, sizeof(g_published) - 1);
		if (length > 0) memcpy(g_shown, g_published, length + 1);
		ReleaseSRWLockShared(&g_lock);
		if (length == 0) return;
		g_body.data = g_shown;
		g_body.end = g_shown + length;
		g_body.capacityEnd = g_shown + length;
		*bodySlot = &g_body;
		if (InterlockedIncrement(&g_substitutions) == 1)
			LOG_INFO("(TONE POPUP) the game's tone readout now names the Tone designer's tone ('" << g_shown << "')" << std::endl);
	}

	// Entered by the detour's jump from ShowNotification's first instruction, with the game's registers and stack
	// untouched. Only type 7 is looked at; everything is restored before continuing into the original.
	__declspec(naked) void ShowNotificationDetour()
	{
		__asm
		{
			cmp edx, 7                     // TYPE_TONE_CHANGE (a literal: MSVC inline asm reads a named constant as an address)
			jne passthrough
			pushfd
			pushad
			lea eax, [esp + 0x24 + 0x0C]   // the body argument: entry esp + 0x0C, under pushad (0x20) and pushfd (4)
			push eax
			call SubstituteBody
			popad
			popfd
		passthrough:
			jmp dword ptr [g_original]
		}
	}
}

void ToneSwitchPopup::SetToneName(const std::string& name)
{
	AcquireSRWLockExclusive(&g_lock);
	const size_t length = (std::min)(name.size(), sizeof(g_published) - 1);
	memcpy(g_published, name.data(), length);
	g_published[length] = '\0';
	ReleaseSRWLockExclusive(&g_lock);
}

bool ToneSwitchPopup::SongPlaying() { return GameTones::PlayingSongObject() != 0; }

bool ToneSwitchPopup::ShowNow()
{
	const uintptr_t owner = GameTones::PlayingSongObject();
	if (!owner) return false;
	const bool written = WriteFloatGuarded(owner + READOUT_COUNTDOWN, 0.05f);
	LOG_INFO("(TONE POPUP) " << (written ? "asked the game to show its tone readout" : "could not reach the song object") << std::endl);
	return written;
}

void ToneSwitchPopup::Initialize()
{
	GameTones::Initialize();
	if (g_original) return;
	const uintptr_t target = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + SHOW_NOTIFICATION_RVA;
	if (!MatchesBytes(target, PROLOGUE, sizeof(PROLOGUE)) || !MatchesBytes(target + 0x10, BODY_SIGNATURE, sizeof(BODY_SIGNATURE)))
	{
		LOG_WARNING("(TONE POPUP) The game's notification function does not match the expected build; the tone readout keeps"
			<< " the game's own text. Found " << AudioBridge::Hooking::DescribeCodeBytes(target, sizeof(PROLOGUE)) << "." << std::endl);
		return;
	}
	g_original = AudioBridge::Hooking::DetourFunction(reinterpret_cast<byte*>(target), reinterpret_cast<byte*>(&ShowNotificationDetour));
	if (!g_original)
	{
		LOG_ERROR("(TONE POPUP) Could not hook the game's notification function." << std::endl);
		return;
	}
	LOG_INFO("(TONE POPUP) Notification function hooked at exe+0x" << std::hex << SHOW_NOTIFICATION_RVA << std::dec
		<< "; the tone readout names the Tone designer's tone while it plays." << std::endl);
}
