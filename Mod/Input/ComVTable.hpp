#pragma once

// A COM vtable is shared by every instance of its implementation class, so patching a
// slot once redirects every object of that class, including ones created later. That is
// what lets a single sighting of an interface cover the game's whole audio path without
// tracking individual instances.
namespace Audio::ComVTable
{
	inline void** GetVTable(void* comObject)
	{
		return *reinterpret_cast<void***>(comObject);
	}

	// Returns the entry that was replaced, or nullptr if the slot could not be made writable.
	inline void* PatchSlot(void* comObject, size_t slotIndex, void* replacement)
	{
		if (!comObject) return nullptr;

		void** slot = &GetVTable(comObject)[slotIndex];
		void* original = *slot;
		if (!MemUtil::PatchAdr(slot, &replacement, sizeof(replacement))) return nullptr;
		return original;
	}
}
