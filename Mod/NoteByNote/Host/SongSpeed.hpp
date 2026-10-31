#pragma once

// The song speed Note by Note's flow mode reads and asks the game for. 4.x added these to its Mods/RiffRepeater
// (RequestGameSpeed, RealSpeedToSlider, SliderToRealSpeed, GetPlayerRealSpeed, playerSliderPercent); upstream's
// RiffRepeater has none of them, so they live here on top of upstream's RiffRepeater::GetSpeed and its Linear Riff
// Repeater flag. No song-speed claim: flow asks the game for a speed the way its slider does, and upstream's
// RiffRepeaterMod (Linear Riff Repeater, >100 % time stretch) follows the game's speed, so both stay on as in 4.x.
namespace NoteByNote::SongSpeed
{
	// The real song speed in percent (400 = 4x), from the game's Time_Stretch RTPC.
	float GetRealPercent();

	// Asks the GAME to play at a real speed (25..100 %), the way its own Riff Repeater slider does: GE_SetRRSpeed
	// 0x405880 (void __cdecl(float sliderPercent), the lesson-script builtin) broadcasts EventDataSongTimeStretch, so
	// the music, the highway and the game's speed notification all follow. Writing the RTPC does not. Game thread.
	void RequestGameSpeed(float realPercent);

	// Slider <-> real speed, honouring Linear Riff Repeater (real = slider) and RR speed above 100 %.
	float RealSpeedToSlider(float realPercent);
	float SliderToRealSpeed(float sliderPercent);   // -1 for an unknown slider

	// The Riff Repeater SPEED slider the player set (Settings screen controller+0x300), recorded by the Riff Repeater
	// menu poll; -1 until that screen has been seen.
	float GetPlayerSliderPercent();
	void SetPlayerSliderPercent(float sliderPercent);
	float GetPlayerRealPercent();
}
