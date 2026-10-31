#pragma once

// The Guitar Tone overlay page (4.x's AmpSim page): where the guitar's sound comes from, one tab each: Game amp, Tone
// designer (the built-in engine, StompboxPage.cpp), VST rig (ToneDesignerPage.cpp) and External app.
namespace AudioBridge::GuitarTone
{
	// MainThread, from ToneDesignerMod::OnInitialize.
	void RegisterPage();
}
