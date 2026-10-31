#pragma once

#include "Framework/Framework.hpp"

// Recording takes from inside the game (TakeRecorder.hpp): the record key starts and stops one in the remembered
// format (wet + dry WAVs, or an MP4 through the helper exe's video take), with the REC badge on screen while one
// records (RecordingIndicator.hpp).
class AudioRecorderMod : public Framework::IMod {
public:
	MOD_ID(AudioRecorderMod)

	Framework::SettingDefs Settings() const override;

	void OnInitialize(Framework::ModContext& c) override;
	void OnShutdown(Framework::ModContext& c) override;
};
