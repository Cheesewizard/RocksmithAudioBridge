#pragma once

#include <atomic>

namespace RiffRepeater {
	float GetSpeed(bool realSpeed = false);
	void SetSpeed(float newSpeed, bool isRealSpeed = false);
	void RequestGameSpeed(float realPercent);
	float RealSpeedToSlider(float realPercent);
	float SliderToRealSpeed(float sliderPercent);
	float GetPlayerRealSpeed();
	float ConvertSpeed(float speed);
	void EnableTimeStretch();
	void DisableTimeStretch();
	void EnableLinearSpeeds();
	void DisableLinearSpeeds();
	bool LogSongID(const std::string& songKey);
	void HandleSongChange(const std::string& previousSongKey);
	void SaveSpeedToFileOnChange();

	inline std::map<std::string, AkUInt32> SongObjectIDs;
	inline AkUInt32 currentSongID;
	inline bool readyToLogSongID;
	inline bool loggedCurrentSongID = false;

	inline bool currentlyEnabled_Above100 = false;
	inline bool currentlyEnabled_LinearRR = false;

	// The Riff Repeater SPEED slider value the player set (Settings screen controller+0x300), -1 until
	// the screen has been seen. Read by Note by Note flow for the real song speed.
	inline std::atomic<float> playerSliderPercent{ -1.f };

	inline bool saveNewRRSpeedToFile = false;
}