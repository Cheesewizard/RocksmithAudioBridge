#include "ExternalAmpMod.hpp"

#include "ExternalAmp.hpp"

// Once (Retry-faulted runs OnInitialize again).
void ExternalAmpMod::OnInitialize(Framework::ModContext&) {
	if (started) return;
	started = true;
	ExternalAmp::Start();
}

// The framework's shutdown runs on MainThread after the game loop ends (not under the loader lock), so a bounded join
// is safe here. Stop never waits more than a second.
void ExternalAmpMod::OnShutdown(Framework::ModContext&) {
	if (!started) return;
	started = false;
	ExternalAmp::Stop();
}

static Framework::ModRegistrar<ExternalAmpMod> _externalAmpReg;
