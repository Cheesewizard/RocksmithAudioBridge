#pragma once

#include "Framework/Framework.hpp"

// External amp: plays the guitar through an amp sim over the bridge driver's amp link (or the cable's), muting the
// game's own amp meanwhile (ExternalAmp.hpp). Its controller runs on its own thread from launch.
class ExternalAmpMod : public Framework::IMod {
public:
	MOD_ID(ExternalAmpMod)

	void OnInitialize(Framework::ModContext& c) override;
	void OnShutdown(Framework::ModContext& c) override;

private:
	bool started = false;
};
