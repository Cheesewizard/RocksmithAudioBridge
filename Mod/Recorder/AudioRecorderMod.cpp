#include "AudioRecorderMod.hpp"

#include "../Core/BespokeSettings.hpp"
#include "GameAudioRecorder.hpp"
#include "RecordingIndicator.hpp"
#include "TakeRecorder.hpp"
#include "../AudioBridge/Engine/AudioBridgeEngine.hpp"
#include "../Shell/HudDraw.hpp"
#include "../Shell/Pages/RecordPage.hpp"

#include "Log.hpp"

using Framework::Availability;
using Framework::KeyEdge;
using Framework::KeyEvent;
using Framework::ModContext;
using Framework::SettingDefs;

namespace {
	// 4.x's [Keybinds] name.
	constexpr char kRecordKey[] = "RecordingHotkey";
	// How long shutdown waits for a dry take's background normalization.
	constexpr DWORD kFinalizeTimeoutMs = 10000;
	// The REC badge draws on the background draw list, so its order among the overlays doesn't show.
	constexpr int kIndicatorOverlayOrder = 100;
}

SettingDefs AudioRecorderMod::Settings() const {
	return AudioBridge::EditedByAudioBridge({
		Framework::KeyBind(kRecordKey, "Record a take (start / stop)", "VK_F6")
			.Heading(Framework::SettingHeading::Audio),
	});
}

void AudioRecorderMod::OnInitialize(ModContext& c) {
	c.Commands().BindSetting(kRecordKey, KeyEdge::Up, Availability::Active,
		[](ModContext&, const KeyEvent&) { Audio::Takes::Toggle(Audio::Takes::PreferVideo()); }, {}, "Record a take");
	AudioBridge::Pages::RegisterRecordPage();

	// 4.x's D3DX REC badge, top right while a take records.
	c.Draw().RegisterFrame([](IDirect3DDevice9*) { AudioBridge::HudDraw::PrepareFonts(); });
	c.Menu().RegisterOverlay("audio-bridge-recording", kIndicatorOverlayOrder, [] { AudioBridge::RecordingIndicator::Draw(); });
}

// Finish recordings before the process goes away: stop a take that is still open (video included), then let the dry
// take's normalization complete, or quitting mid-take leaves a half-written WAV. 4.x did this on WM_CLOSE; the
// framework's shutdown is the closest point it offers (upstream ask F asks for a bounded wait on close).
void AudioRecorderMod::OnShutdown(ModContext&) {
	if (Audio::SharedOutput::HasTake()) Audio::Takes::Toggle(false);
	if (!Audio::GameAudioRecorder::WaitForPendingCloses(kFinalizeTimeoutMs))
		LOG_WARNING("(AUDIO ROUTING) Game closing while a dry take was still being finalized" << std::endl);
}

static Framework::ModRegistrar<AudioRecorderMod> _audioRecorderReg;
