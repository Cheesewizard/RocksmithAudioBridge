#include "NoteByNoteMod.hpp"

#include "../Core/BespokeSettings.hpp"
#include "NoteByNoteSettings.hpp"
#include "Controller/NoteByNoteNativeScoring.hpp"
#include "Host/ControllerBridge.hpp"
#include "Host/NoteByNoteHostServices.hpp"
#include "Host/NoteByNoteMode.hpp"
#include "Ml/MlAudioExporter.hpp"
#include "Ml/MlServiceLauncher.hpp"
#include "Ui/HighwayInterceptors.hpp"
#include "Ui/NoteByNoteHud.hpp"
#include "Ui/NoteByNoteHudLabel.hpp"
#include "Ui/NoteByNoteMenu.hpp"
#include "Ui/NoteByNotePage.hpp"
#include "Ui/TablaturePreviewWindow.hpp"
#include "../Shell/HudDraw.hpp"
#include "../DropPedal/DropPedalShift.hpp"

#include "Log.hpp"

#include <atomic>

using Framework::Availability;
using Framework::KeyEdge;
using Framework::KeyEvent;
using Framework::ModContext;
using Framework::SettingDefs;

namespace {
	// Last in the input chain (conditioner -100, latency capture -50, Drop Pedal 0): the observers hear what the game's
	// detection hears.
	constexpr int kObserverOrder = 1000;

	// After the Audio Bridge overlay (order 0), as 4.x drew the tab after its overlay window. The HUD draws on the
	// background draw list, so its order doesn't show.
	constexpr int kLookAheadTabOrder = 10;
	constexpr int kHudOrder = 100;

	// 4.x's WndProc: the arrows navigate notes only while a song plays (not the pause menu) with Note by Note on. The
	// framework never swallows a key, so the game still gets them, as in 4.x (it has no in-song use for them).
	bool NavigationAvailable(const ModContext&, const KeyEvent&) {
		return NoteByNoteProbe::IsInLasPlayingFast() && NoteByNoteProbe::IsAutomaticEnabledFast();
	}
}

SettingDefs NoteByNoteMod::Settings() const {
	return AudioBridge::EditedByAudioBridge(NoteByNote::ModSettings::Declare());
}

// The detours install once and stay (Retry-faulted runs OnInitialize again). 4.x installed them from dllmain after the
// mods started (ResearchBridge, NoteByNoteProbe, NoteByNoteMenu, NoteByNoteHudLabel); OnInitialize runs at the same
// point.
void NoteByNoteMod::OnInitialize(ModContext& c) {
	NoteByNote::ModSettings::Load(c);
	c.Audio().AddInputProcessor(observer, kObserverOrder);

	// The highway drawing (4.x's NoteByNoteHighwayRenderer and D3DHooks' Note by Note code): the seam implementation the
	// detours below call into, the draw interceptors and the vertex-shader constant shadow. Before the detours install.
	NoteByNote::HighwayInterceptors::Register(c);

	// 4.x polled the Riff Repeater row from D3DHooks::FinishNoteByNoteRenderFrame, every presented frame: the game's
	// menu controllers are only safe to call from the render thread. The frame count the highway filters key their
	// "this frame or the last" rules on advanced at the end of the same call.
	c.Draw().RegisterFrame([](IDirect3DDevice9*) {
		NoteByNoteMenu::Poll();
		AudioBridge::HudDraw::PrepareFonts();
		NoteByNote::HighwayInterceptors::FinishRenderFrame();
	});

	// 4.x's D3DX HUD blocks (readout, target, bend meter, finger numerals) and the Look ahead tab, which 4.x drew every
	// frame from its ImGui frame, overlay open or not. The overlay page brings the HUD's layout editor.
	c.Menu().RegisterOverlay("note-by-note-hud", kHudOrder, [] { NoteByNote::Hud::Draw(); });
	c.Menu().RegisterOverlay("note-by-note-look-ahead-tab", kLookAheadTabOrder, [] { NoteByNote::Ui::DrawOverlay(); });
	NoteByNote::Ui::RegisterNoteByNotePage();

	// 4.x queued these on WM_KEYUP.
	c.Commands().BindKey("NoteByNoteNextNote", VK_RIGHT, KeyEdge::Up, Availability::Active,
		[](ModContext&, const KeyEvent&) { NoteByNoteHostServices::QueueNoteNavigation(1); }, NavigationAvailable);
	c.Commands().BindKey("NoteByNotePreviousNote", VK_LEFT, KeyEdge::Up, Availability::Active,
		[](ModContext&, const KeyEvent&) { NoteByNoteHostServices::QueueNoteNavigation(-1); }, NavigationAvailable);

	if (installed) return;
	installed = true;
	ResearchBridge::Initialize();      // the built-in controller (NoteByNoteController.cpp) behind the host API
	NoteByNoteProbe::Initialize();     // scoring, hit-decision, render-preparation and note-head draw detours
	NoteByNoteMenu::Initialize();      // the Riff Repeater input dispatcher and Advanced Settings / Settings builders
	NoteByNoteHudLabel::Initialize();  // the Riff Repeater HUD's MISSED row reads NOTE BY NOTE while it is on
}

void NoteByNoteMod::OnShutdown(ModContext&) {
	MlServiceLauncher::Shutdown();
	ResearchBridge::Shutdown();
}

void NoteByNoteMod::OnSettingsChanged(ModContext& c) {
	NoteByNote::ModSettings::Load(c);
}

// 4.x split this across three loops: TickLifecycle on its 100 ms Riff Repeater thread, PublishMenuState and
// MlAudioExporter::Poll in ModManager's 250 ms game loop, MlServiceLauncher::Poll in dllmain's 250 ms loop. Here they
// all run on the maintenance tick (250 ms, 33 ms while a mod watches a deadline); the lifecycle's 1.5 s retire is
// measured in wall-clock time. GameState is read only outside Loading, when MainThread has refreshed currentMenu.
void NoteByNoteMod::OnTick(ModContext& c) {
	if (c.phase != Framework::GamePhase::Loading) {
		NoteByNoteProbe::PublishMenuState();
		// The physical tuning and string colours the HUD and the look-ahead tab read on the render thread.
		NoteByNote::Hud::PublishGameState();
		// Creates the ML audio export's mapping off the audio thread, and stamps the pedal shift the audio is in.
		MlAudioExporter::Poll(AudioBridge::DropPedal::Shift::AppliedInputShiftSemitones());
		// The service only feeds Note by Note: started the first time Note by Note is on or the Riff Repeater menus
		// are open (warm before the first note), then kept for the session.
		if (!mlServiceWanted && (NoteByNoteProbe::IsAutomaticEnabledFast() || NoteByNoteProbe::IsInRiffRepeaterMenusFast()))
			mlServiceWanted = true;
	}
	NoteByNoteProbe::TickLifecycle();
	MlServiceLauncher::Poll(mlServiceWanted);
}

static Framework::ModRegistrar<NoteByNoteMod> _noteByNoteReg;
