#include "ToneDesignerMod.hpp"

#include "PluginPictureView.hpp"
#include "SongToneSets.hpp"
#include "StompboxLink.hpp"
#include "ToneBackup.hpp"
#include "ToneSwitchPopup.hpp"
#include "VstHostLink.hpp"
#include "Ui/GuitarTonePage.hpp"
#include "../AudioBridge/ToneDesignerLink.hpp"
#include "../Shell/ShellState.hpp"

#include "GameState.hpp"

using Framework::ModContext;

namespace {
	// What routing (LatencyCalibration) and External amp (CableAmpLink's guest loading) ask of the VST host. Each call
	// is VstHostLink's raw read of the shared block, safe from any thread.
	class VstToneDesignerHost final : public AudioBridge::IToneDesignerHost {
	public:
		bool IsHostGuest(const std::string& guestName) const override { return VstHostLink::IsHostGuest(guestName); }
		int ChainLatencySamples() const override { return VstHostLink::ChainLatencySamples(); }
		bool IsLoading() const override { return VstHostLink::IsLoading(); }
	};

	VstToneDesignerHost toneDesignerHost;
}

// The detours install once and stay (Retry-faulted runs OnInitialize again). 4.x installed ToneSwitchPopup (which
// installs GameTones first) from dllmain after the mods started; both check the game's bytes before hooking and are
// inert until the overlay sets a tone name / a song plays.
void ToneDesignerMod::OnInitialize(ModContext& c) {
	AudioBridge::RegisterToneDesignerHost(toneDesignerHost);

	// 4.x's Menu.cpp ran these every rendered frame after the overlay drew, in this order. GameState::GameLoaded is an
	// atomic; the rest of the game state VstHostLink reads comes from Core/GameView (published by the routing mod).
	c.Draw().RegisterFrame([](IDirect3DDevice9* device) {
		VstHostLink::OnFrame(AudioBridge::Shell::IsOverlayOpen());     // the VST host's plugin windows show only with the overlay
		PluginPictureView::OnFrame(device);                            // the Inspector's picture of the plugin's own window
		ToneBackup::OnFrame(GameState::GameLoaded);                    // the daily backup, and a restore's steps
		SongToneSets::OnFrame(StompboxLink::GetSnapshot().songKey);    // a chosen CDLC's own tones (.rabsong), imported in the background
	});

	// The Guitar Tone page (Game amp, Tone designer, VST rig, External app), first in the overlay as in 4.x.
	AudioBridge::GuitarTone::RegisterPage();

	if (installed) return;
	installed = true;
	ToneSwitchPopup::Initialize();
}

static Framework::ModRegistrar<ToneDesignerMod> _toneDesignerReg;
