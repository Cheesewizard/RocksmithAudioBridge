#include "../stdafx.h"
#include "Loft.hpp"

namespace Loft {
	/// <summary>
	/// Turn the background / "map" on or off.
	/// </summary>
	void ToggleLoft() {
		// Runs as songs start and end (ToggleLoftWhen = song) and from Twitch effects at any time, so every link is
		// checked and the walk, read and write are guarded against a link freed during the screen change.
		uintptr_t farAddr = MemUtil::FindDMAAddyGuarded(Offsets::baseHandle + Offsets::ptr_loft, Offsets::ptr_loft_farOffsets);
		float farValue = 0.f;

		if (!MemUtil::TryRead(farAddr, farValue)) {
			LOG_ERROR("Invalid Pointer: ToggleLoft()" << std::endl);
			return;
		}

		if (farValue == 10000)
			MemUtil::TryWrite(farAddr, 1.f); // Loft Off
		else
			MemUtil::TryWrite(farAddr, 10000.f); // Loft On
	}

	/// <summary>
	/// Shake the camera around randomly.
	/// </summary>
	/// <param name="enable"> - Should we turn it on, or off?</param>
	void ToggleDrunkMode(bool enable) {
		// Twitch and Crowd Control turn this on and off at any time, including as a song ends. The chain used to be
		// read without a null check; now an unresolved chain leaves the loft alone.
		uintptr_t noLoft = MemUtil::FindDMAAddyGuarded(Offsets::baseHandle + Offsets::ptr_loft, Offsets::ptr_loft_farOffsets);
		float farValue = 0.f;

		if (enable) {
			// Turn on loft so the effects of the mod are actually shown.
			if (MemUtil::TryRead(noLoft, farValue) && farValue == 1) {
				D3DHooks::ToggleOffLoftWhenDoneWithMod = true;
				ToggleLoft();
			}
		}
		else {
			MemUtil::SetStaticValue(Offsets::ptr_drunkShit.Get(), 0.3333333333f, sizeof(float));

			// User originally had the loft off, but then we turned on this mod, so turn the loft back off.
			if (D3DHooks::ToggleOffLoftWhenDoneWithMod) {
				ToggleLoft();
				D3DHooks::ToggleOffLoftWhenDoneWithMod = false;
			}
		}
	}
}