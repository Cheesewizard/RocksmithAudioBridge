#include "stdafx.h"
#include "GameTones.hpp"
#include "../Core/GameVersion.hpp"
#include "../Core/Hooking.hpp"
#include <cstring>
#include <cmath>
#include <atomic>

namespace
{
	const uintptr_t TONES_UPDATE_RVA = AudioBridge::GameVersion::Rva(0x000649E0, 0x00064E50);       // GameComponentTones::Update, VA 0x4649E0
	const byte TONES_UPDATE_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00 };
	const byte TONES_UPDATE_OWNER_READ[] = { 0x8B, 0x71, 0x0C };   // MOV ESI,[ECX+0xC] at +0x22
	const uintptr_t TONES_COMPONENT_VTABLE_RVA = AudioBridge::GameVersion::Rva(0x00D9FF9C, 0x00DA17AC); // GameComponentTones vtable, VA 0x119FF9C
	const uintptr_t PLAYSONG_VTABLE_RVA = AudioBridge::GameVersion::Rva(0x00DD1430, 0x00DD2C60);     // GamePlaysongLAS vtable, VA 0x11D1430
	constexpr uintptr_t COMPONENT_OWNER = 0x0C;
	constexpr uintptr_t SONG_CLOCK = 0x3B4;                       // float seconds into the song
	void* volatile g_tonesComponent = nullptr;
	void* g_originalTonesUpdate = nullptr;
	constexpr uintptr_t CURRENT_SLOT = 0x10;
	constexpr uintptr_t CURRENT_TONE_KEY = 0x198;
	constexpr uintptr_t SLOT_COMPARE_OFFSET = 0x1AA;
	constexpr uintptr_t SLOT_STORE_OFFSET = 0x559;
	const byte SLOT_COMPARE_SIGNATURE[] = { 0x3B, 0x7B, 0x10 };
	const byte SLOT_STORE_SIGNATURE[] = { 0x89, 0x53, 0x10 };
	std::atomic<bool> g_slotReadVerified{ false };
	std::string g_currentToneKey;   // render thread (OnFrame)


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
	bool MatchesBytes(uintptr_t address, const byte* expected, size_t count)
	{
		__try { return std::memcmp(reinterpret_cast<const void*>(address), expected, count) == 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
}

// The playing song's object, or 0 with the reason (logged once per reason) when the readout cannot be shown yet.
uintptr_t GameTones::PlayingSongObject()
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

void GameTones::Initialize()
{
	if (g_originalTonesUpdate) return;
	const uintptr_t update = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + TONES_UPDATE_RVA;
	if (MatchesBytes(update, TONES_UPDATE_PROLOGUE, sizeof(TONES_UPDATE_PROLOGUE)) && MatchesBytes(update + 0x22, TONES_UPDATE_OWNER_READ, sizeof(TONES_UPDATE_OWNER_READ)))
		g_originalTonesUpdate = AudioBridge::Hooking::DetourFunction(reinterpret_cast<byte*>(update), reinterpret_cast<byte*>(&TonesUpdateDetour));
	if (!g_originalTonesUpdate)
		LOG_WARNING("(TONE POPUP) The game's tone component does not match the expected build; the readout cannot be shown on demand."
			<< " Found " << AudioBridge::Hooking::DescribeCodeBytes(update, sizeof(TONES_UPDATE_PROLOGUE)) << "." << std::endl);
	if (!g_originalTonesUpdate) return;
	g_slotReadVerified = MatchesBytes(update + SLOT_COMPARE_OFFSET, SLOT_COMPARE_SIGNATURE, sizeof(SLOT_COMPARE_SIGNATURE))
		&& MatchesBytes(update + SLOT_STORE_OFFSET, SLOT_STORE_SIGNATURE, sizeof(SLOT_STORE_SIGNATURE));
	if (!g_slotReadVerified)
		LOG_WARNING("(GAME TONES) slot-reader signatures do not match; M0a diagnostics are disabled for this game build." << std::endl);
	else
		LOG_INFO("(GAME TONES) tone tracking enabled: component+0x10 and song+0x198 (the current tone key, which the Tone designer's song tones follow)." << std::endl);
}

namespace
{
	bool ReadToneKey(uintptr_t address, char (&text)[128])
	{
		uintptr_t begin = 0, end = 0, capacityEnd = 0;
		if (!ReadGuarded(address + 0x10, end) || !ReadGuarded(address + 0x14, capacityEnd)) return false;
		if (capacityEnd == address + 0x10) begin = address;
		else if (!ReadGuarded(address, begin)) return false;
		if (!begin || end < begin || end - begin >= sizeof(text)) return false;
		const size_t length = end - begin;
		for (size_t index = 0; index < length; ++index)
		{
			if (!ReadGuarded(begin + index, text[index])) return false;
		}
		text[length] = '\0';
		return true;
	}
}

const std::string& GameTones::CurrentToneKey() { return g_currentToneKey; }

void GameTones::OnFrame()
{
	if (!g_slotReadVerified) return;
	static uintptr_t lastOwner = 0;
	static int lastSlot = -1;
	static float lastClock = 0.0f;
	static char lastKey[128]{};
	const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	const uintptr_t component = reinterpret_cast<uintptr_t>(g_tonesComponent);
	uintptr_t componentVtable = 0, owner = 0, ownerVtable = 0;
	int slot = -1;
	float clock = 0.0f;
	if (!component || !ReadGuarded(component, componentVtable) || componentVtable != base + TONES_COMPONENT_VTABLE_RVA
		|| !ReadGuarded(component + COMPONENT_OWNER, owner) || !owner
		|| !ReadGuarded(owner, ownerVtable) || ownerVtable != base + PLAYSONG_VTABLE_RVA
		|| !ReadGuarded(owner + SONG_CLOCK, clock) || !std::isfinite(clock) || clock < 0.0f
		|| !ReadGuarded(component + CURRENT_SLOT, slot))
	{
		lastOwner = 0;
		lastSlot = -1;
		lastClock = 0.0f;
		lastKey[0] = '\0';
		g_currentToneKey.clear();
		return;
	}
	char key[128]{};
	const bool hasKey = ReadToneKey(owner + CURRENT_TONE_KEY, key);
	if (hasKey) g_currentToneKey = key;
	else
	{
		g_currentToneKey.clear();
		strcpy_s(key, "<unreadable>");
	}
	if (owner != lastOwner)
	{
		LOG_INFO("(GAME TONES) song object 0x" << std::hex << owner << ", component 0x" << component << std::dec
			<< "; arrangement PersistentID is not resolved yet." << std::endl);
	}
	const bool rewound = clock + 0.25f < lastClock;
	lastClock = clock;
	if (owner != lastOwner || slot != lastSlot || strcmp(key, lastKey) != 0 || rewound)
	{
		LOG_INFO("(GAME TONES) raw slot -> " << slot << " at songTime " << clock << ", current key '" << key
			<< "' (static interpretation: 0..3=A..D, 5=base/default; awaiting live confirmation)" << std::endl);
		lastOwner = owner;
		lastSlot = slot;
		strcpy_s(lastKey, key);
	}
}
