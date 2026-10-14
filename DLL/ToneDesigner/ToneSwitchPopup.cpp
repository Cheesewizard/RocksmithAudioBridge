#include "stdafx.h"
#include "ToneSwitchPopup.hpp"

#include <cstdint>
#include <cstring>

namespace
{
	// ShowNotification (VA 0x7524A0 in the September 2022 Remastered build), as an RVA so a relocated exe (Mandatory
	// ASLR) still finds it. Custom convention: EDX = notification type, ESI = out id string, caller cleans 0x30 bytes;
	// [esp+0x04] context, [esp+0x08] title, [esp+0x0C] body (const game string*), [esp+0x10] duration.
	constexpr uintptr_t SHOW_NOTIFICATION_RVA = 0x003524A0;
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

	// Showing the readout on demand. The Tone designer loads its chain without the game knowing, so the game never
	// shows its tone readout for it (it does only when its own tone slot changes, e.g. the 1-4 hotkeys). Its song
	// object has a 0.5 s countdown that, on reaching zero, shows the readout for the current tone on the game thread
	// (GamePlaysongLAS+0x500, read by the readout producer at 0x7E8040); setting it brings the native readout up, and the detour
	// above names our tone in it. The song object is reached through the per-frame tone component, captured by a
	// one-instruction detour on its Update (component+0x0C = song object).
	constexpr uintptr_t TONES_UPDATE_RVA = 0x000649E0;          // GameComponentTones::Update, VA 0x4649E0
	const byte TONES_UPDATE_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00 };
	const byte TONES_UPDATE_OWNER_READ[] = { 0x8B, 0x71, 0x0C };   // MOV ESI,[ECX+0xC] at +0x22
	constexpr uintptr_t TONES_COMPONENT_VTABLE_RVA = 0x00D9FF9C; // GameComponentTones vtable, VA 0x119FF9C
	constexpr uintptr_t PLAYSONG_VTABLE_RVA = 0x00DD1430;        // GamePlaysongLAS vtable, VA 0x11D1430
	constexpr uintptr_t COMPONENT_OWNER = 0x0C;
	constexpr uintptr_t READOUT_COUNTDOWN = 0x500;                // the game arms 0.5 s; ShowNow sets 0.05 s so the readout lands with the switch
	constexpr uintptr_t SONG_CLOCK = 0x3B4;                       // float seconds into the song
	void* volatile g_tonesComponent = nullptr;
	void* g_originalTonesUpdate = nullptr;

	__declspec(naked) void TonesUpdateDetour()
	{
		__asm
		{
			mov dword ptr [g_tonesComponent], ecx
			jmp dword ptr [g_originalTonesUpdate]
		}
	}

	template <typename T>
	bool ReadGuarded(uintptr_t address, T& value)
	{
		__try { value = *reinterpret_cast<const volatile T*>(address); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
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

namespace
{
	// The playing song's object, or 0 with the reason (logged once per reason) when the readout cannot be shown yet.
	uintptr_t PlayingSongObject()
	{
		const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		const uintptr_t component = reinterpret_cast<uintptr_t>(g_tonesComponent);
		uintptr_t componentVtable = 0, owner = 0, ownerVtable = 0;
		// The component outlives no song: after one ends the pointer may be stale, so both class identities are checked
		// (guarded reads) before the one write.
		const char* missing = nullptr;
		if (!component) missing = "the game's tone component has not run yet";
		else if (!ReadGuarded(component, componentVtable) || componentVtable != base + TONES_COMPONENT_VTABLE_RVA) missing = "the tone component is gone";
		else if (!ReadGuarded(component + COMPONENT_OWNER, owner) || !owner) missing = "the tone component has no song object";
		else if (!ReadGuarded(owner, ownerVtable) || ownerVtable != base + PLAYSONG_VTABLE_RVA) missing = "its owner is not the song object";
		// Shown while the song is still loading (just after the tuner), the readout is cleared within half a second:
		// wait until the song has played a second (song clock, the float Note by Note also reads).
		float clock = 0.0f;
		if (!missing && (!ReadGuarded(owner + SONG_CLOCK, clock) || !(clock >= 1.0f))) missing = "the song is not playing yet";
		if (missing)
		{
			// Called again until it works (the song's objects appear a moment after the game reports the song started);
			// say why only once per reason.
			static const char* lastMissing = nullptr;
			if (missing != lastMissing)
			{
				lastMissing = missing;
				LOG_INFO("(TONE POPUP) not shown yet: " << missing << " (component 0x" << std::hex << component << " vtable 0x" << componentVtable
					<< ", owner 0x" << owner << " vtable 0x" << ownerVtable << ", expected 0x" << base + PLAYSONG_VTABLE_RVA << std::dec << ")" << std::endl);
			}
			return 0;
		}
		return owner;
	}
}

bool ToneSwitchPopup::SongPlaying() { return PlayingSongObject() != 0; }

bool ToneSwitchPopup::ShowNow()
{
	const uintptr_t owner = PlayingSongObject();
	if (!owner) return false;
	const bool written = WriteFloatGuarded(owner + READOUT_COUNTDOWN, 0.05f);
	LOG_INFO("(TONE POPUP) " << (written ? "asked the game to show its tone readout" : "could not reach the song object") << std::endl);
	return written;
}

void ToneSwitchPopup::Initialize()
{
	if (!g_originalTonesUpdate)
	{
		const uintptr_t update = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + TONES_UPDATE_RVA;
		if (MatchesBytes(update, TONES_UPDATE_PROLOGUE, sizeof(TONES_UPDATE_PROLOGUE)) && MatchesBytes(update + 0x22, TONES_UPDATE_OWNER_READ, sizeof(TONES_UPDATE_OWNER_READ)))
			g_originalTonesUpdate = DetourFunction(reinterpret_cast<byte*>(update), reinterpret_cast<byte*>(&TonesUpdateDetour));
		if (!g_originalTonesUpdate)
			LOG_WARNING("(TONE POPUP) The game's tone component does not match the expected build; the readout cannot be shown on demand."
				<< " Found " << MemUtil::DescribeCodeBytes(update, sizeof(TONES_UPDATE_PROLOGUE)) << "." << std::endl);
	}
	if (g_original) return;
	const uintptr_t target = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + SHOW_NOTIFICATION_RVA;
	if (!MatchesBytes(target, PROLOGUE, sizeof(PROLOGUE)) || !MatchesBytes(target + 0x10, BODY_SIGNATURE, sizeof(BODY_SIGNATURE)))
	{
		LOG_WARNING("(TONE POPUP) The game's notification function does not match the expected build; the tone readout keeps"
			<< " the game's own text. Found " << MemUtil::DescribeCodeBytes(target, sizeof(PROLOGUE)) << "." << std::endl);
		return;
	}
	g_original = DetourFunction(reinterpret_cast<byte*>(target), reinterpret_cast<byte*>(&ShowNotificationDetour));
	if (!g_original)
	{
		LOG_ERROR("(TONE POPUP) Could not hook the game's notification function." << std::endl);
		return;
	}
	LOG_INFO("(TONE POPUP) Notification function hooked at exe+0x" << std::hex << SHOW_NOTIFICATION_RVA << std::dec
		<< "; the tone readout names the Tone designer's tone while it plays." << std::endl);
}
