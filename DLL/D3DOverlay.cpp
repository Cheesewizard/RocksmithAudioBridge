#include "stdafx.h"
#include "D3DOverlay.hpp"
#include "Audio/SharedOutput.hpp"
#include "Mods/DropPedal/DropPedalOverlay.hpp"
#include "Mods/DropPedal/DropPedal.hpp"
#include "Mods/NoteByNoteNativeScoring.hpp"
#include "Mods/FakeGuitar/FakeGuitarInjector.hpp"
#include "D3D/NoteByNoteHighwayRenderer.hpp"
#include "Audio/MlStringFretReader.hpp"
#include "Research/ResearchBridge.hpp"
#include "Audio/CableInput.hpp"
#include "OverlayToggles.hpp"
#include "PitchNames.hpp"
#include "Mods/ExtendedRangeMode.hpp"
#include "ProductVersion.hpp"
#include "Overlay/HudLayout.hpp"

/// <returns>Size of Rocksmith Window</returns>
Resolution GameOverlay::GetWindowSize() {
	RECT windowSize;

	Resolution currentSize;
	if (GetWindowRect(D3DHooks::GetGameWindow(), &windowSize))
	{
		currentSize.width = windowSize.right - windowSize.left;
		currentSize.height = windowSize.bottom - windowSize.top;
	}

	return currentSize;
}

/// <summary>
/// Draw text on screen
/// </summary>
/// <param name="textToDraw"> - What text should be written?</param>
/// <param name="textColorHex"> - What color? Given in hex in the AA,RR,GG,BB format.</param>
/// <param name="topLeftX"> - top LEFT of textbox</param>
/// <param name="topLeftY"> - TOP left of textbox</param>
/// <param name="bottomRightX"> - bottom RIGHT of textbox</param>
/// <param name="bottomRightY"> - BOTTOM right of textbox</param>
/// <param name="pDevice"> - Device Pointer</param>
/// <param name="setFontSize"> - Override font size</param>
/// <param name="format"> - DrawText format</param>
void GameOverlay::DX9DrawText(const std::string& textToDraw, int textColorHex, int topLeftX, int topLeftY, int bottomRightX, int bottomRightY, LPDIRECT3DDEVICE9 pDevice, Resolution setFontSize, DWORD format)
{
	CComPtr<ID3DXFont> font;
	bool useInputFontSize = setFontSize.height != 0;

	if (useInputFontSize) {
		int targetH = setFontSize.height;
		const std::string face = Settings::ReturnSettingValue("OnScreenFont");
		FontKey key = FontKey::Make(face, targetH, 0, FW_NORMAL, false);

		if (!fontCache.Get(pDevice, key, font)) {
			LOG_ERROR("Could not acquire custom-sized font." << std::endl);
			return;
		}
	}
	else {
		if (cachedFont) {
			font = cachedFont;
		}
		else {
			LOG_ERROR("Default font is not cached!" << std::endl);
			return;
		}
	}

	RECT TextRectangle{ topLeftX, topLeftY, bottomRightX, bottomRightY }; // Left, Top, Right, Bottom

	// Preload And Draw The Text (Supposed to reduce the performance hit (It's D3D/DX9 but still good practice))
	font->PreloadTextA(textToDraw.c_str(), textToDraw.length());
	font->DrawTextA(nullptr, textToDraw.c_str(), -1, &TextRectangle, format, textColorHex);
}

// Wide-string sibling of DX9DrawText. The font objects are Unicode-capable regardless of
// having been created through D3DXCreateFontA, so a glyph like U+266A (musical note) renders
// correctly here where the ANSI path would mangle it. Font size is always explicit (a badge
// picks its own size); `weight` reaches the cache key so a bold face is a distinct entry.
void GameOverlay::DX9DrawTextW(const std::wstring& textToDraw, int textColorHex, int topLeftX, int topLeftY, int bottomRightX, int bottomRightY, LPDIRECT3DDEVICE9 pDevice, int fontHeight, DWORD format, int weight)
{
	CComPtr<ID3DXFont> font;
	const std::string face = Settings::ReturnSettingValue("OnScreenFont");
	FontKey key = FontKey::Make(face, fontHeight, 0, weight, false);
	if (!fontCache.Get(pDevice, key, font)) {
		LOG_ERROR("Could not acquire wide-text font." << std::endl);
		return;
	}

	RECT TextRectangle{ topLeftX, topLeftY, bottomRightX, bottomRightY }; // Left, Top, Right, Bottom
	font->DrawTextW(nullptr, textToDraw.c_str(), -1, &TextRectangle, format, textColorHex);
}

namespace
{
	// Pixel size DX9DrawText would give `text` at `fontHeight` (same font cache entry), for the HUD layout editor's
	// hit boxes. Falls back to a rough estimate if the font cannot be acquired.
	SIZE MeasureHudText(const std::string& text, int fontHeight, LPDIRECT3DDEVICE9 device)
	{
		CComPtr<ID3DXFont> font;
		const auto key = GameOverlay::FontKey::Make(Settings::ReturnSettingValue("OnScreenFont"), fontHeight, 0, FW_NORMAL, false);
		if (!text.empty() && GameOverlay::fontCache.Get(device, key, font)) {
			RECT rect{ 0, 0, 0, 0 };
			font->DrawTextA(nullptr, text.c_str(), -1, &rect, DT_LEFT | DT_SINGLELINE | DT_CALCRECT, 0);
			return { rect.right - rect.left, rect.bottom - rect.top };
		}
		return { static_cast<LONG>(text.size()) * fontHeight / 2, fontHeight };
	}

	// Bend depth in the words players use ("half bend", "full bend"); empty for no bend.
	std::string BendWords(int semitones)
	{
		switch (semitones) {
		case 0: return {};
		case 1: return "half bend";
		case 2: return "full bend";
		case 3: return "1 1/2 bend";
		case 4: return "2 step bend";
		default: return semitones > 0 ? std::to_string(semitones) + " semitone bend" : std::string{};
		}
	}
}

void GameOverlay::DisplayMixer() {
	// Display the whole mixer if displayMixer is true
	if (Settings::ReturnSettingValue("VolumeControlEnabled") == "on" && displayMixer) {

		float offset = 0;
		for (int volumeIndex = 0; volumeIndex < mixerInternalNames.size(); ++volumeIndex) {

			float volume = 0;
			RTPCValue_type type = RTPCValue_GameObject;
			Wwise::SoundEngine::Query::GetRTPCValue(mixerInternalNames[volumeIndex].c_str(), AK_INVALID_GAME_OBJECT, &volume, &type);

			DX9DrawText(
				drawMixerTextName[volumeIndex] + std::to_string(static_cast<int>(volume)) + "%",
				whiteText,
				static_cast<int>(WindowSize.width / 96.0f),  // 20 pixels from left in 1920x1080 resolution
				static_cast<int>(WindowSize.height / 54.0f + offset), // 20 pixels from top (plus an offset to display multiple values)
				static_cast<int>(WindowSize.width / 19.2f),  // 120 pixels from left
				static_cast<int>(WindowSize.height / 16.0f), // 120 pixels from top
				pDevice);

			// Adjust the offset to display the next value
			offset += WindowSize.height / 54.0f;
		}
	}
	// Display just the current volume based on context (This will display the last volume that was adjusted for a few seconds after adjusting it)
	else if (Settings::ReturnSettingValue("VolumeControlEnabled") == "on" && displayCurrentVolume) {
		float volume = 0;
		RTPCValue_type type = RTPCValue_GameObject;
		Wwise::SoundEngine::Query::GetRTPCValue(mixerInternalNames[currentVolumeIndex].c_str(), AK_INVALID_GAME_OBJECT, &volume, &type);

		DX9DrawText(
			drawMixerTextName[currentVolumeIndex] + std::to_string(static_cast<int>(volume)) + "%",
			whiteText,
			static_cast<int>(WindowSize.width / 96.0f),  // 20 pixels from left in 1920x1080 resolution
			static_cast<int>(WindowSize.height / 54.0f), // 20 pixels from top 
			static_cast<int>(WindowSize.width / 19.2f),  // 120 pixels from left
			static_cast<int>(WindowSize.height / 16.0f), // 120 pixels from top
			pDevice);
	}
}

void GameOverlay::DisplayProductVersion()
{
	if (!GameState::Menus::IsOnMainMenu()) return;

	const auto left = static_cast<int>(WindowSize.width * 0.70f);
	const auto top = static_cast<int>(WindowSize.height * 0.895f);
	const auto right = static_cast<int>(WindowSize.width * 0.985f);
	const auto bottom = static_cast<int>(WindowSize.height * 0.93f);
	const auto shadowOffset = std::max(1, static_cast<int>(WindowSize.height / 720.0f));
	const Resolution fontSize = { 0u, std::max(16u, WindowSize.height / 60u) };

	DX9DrawText(
		ProductVersion::DISPLAY_NAME,
		0xCC000000,
		left + shadowOffset,
		top + shadowOffset,
		right + shadowOffset,
		bottom + shadowOffset,
		pDevice,
		fontSize,
		DT_RIGHT | DT_NOCLIP);

	DX9DrawText(
		ProductVersion::DISPLAY_NAME,
		0xFF28D7F2,
		left,
		top,
		right,
		bottom,
		pDevice,
		fontSize,
		DT_RIGHT | DT_NOCLIP);
}

void GameOverlay::DisplaySongTimer()
{
	if (!D3DHooks::showSongTimerOnScreen) return;

	const float songTime = SongTimer::SongTimer();
	if (songTime == 0.f) return;

	DX9DrawText(
		D3DHooks::ConvertFloatTimeToStringTime(songTime),
		whiteText,
		static_cast<int>(WindowSize.width - WindowSize.width / 16.0f), // 120 pixels left from right edge in 1920x1080 resolution
		static_cast<int>(WindowSize.height / 54.0f),                   // 20 pixels from top
		static_cast<int>(WindowSize.width - WindowSize.width / 96.0f), // 20 left from right edge
		static_cast<int>(WindowSize.height / 16.0f),                   // 120 pixels from top
		pDevice,
		{ NULL, NULL },
		DT_RIGHT | DT_NOCLIP);
}

void GameOverlay::DisplayCurrentNote()
{
	if (Settings::ReturnSettingValue("ShowCurrentNoteOnScreen") == "on" && GuitarSpeak::GetCurrentNoteName() != (std::string)"") {

		if (GameState::IsInSong()) {
			DX9DrawText(
				GuitarSpeak::GetCurrentNoteName(),
				whiteText,
				static_cast<int>(WindowSize.width / 5.5),		// 349 pixels left of the center in 1920x1080 resolution.
				static_cast<int>(WindowSize.height / 1.75),	// 617 pixels from the top
				static_cast<int>(WindowSize.width / 5.75),		// 334 pixels right of center
				static_cast<int>(WindowSize.height / 8),		// 135 pixels from the top
				pDevice);
		}
		else { // Show outside of the song at the top of the screen.
			DX9DrawText(
				"Current Note: " + GuitarSpeak::GetCurrentNoteName(),
				whiteText,
				static_cast<int>(WindowSize.width / 3.87),		// 496 pixels left of the center in 1920x1080 resolution
				static_cast<int>(WindowSize.height / 30.85),	// 35 pixels from the top
				static_cast<int>(WindowSize.width / 4),		// 480 pixel right of the center
				static_cast<int>(WindowSize.height / 8),		// 135 pixels from the top
				pDevice);
		}
	}
}

void GameOverlay::DisplayRiffRepeaterOverHundredPercentSpeed()
{
	if (Settings::ReturnSettingValue("RRSpeedAboveOneHundred") == "on" && RiffRepeater::loggedCurrentSongID &&
		(GameState::Menus::IsInModesWithAllowedFastRiffRepeater() || GameState::Menus::IsOnScoreScreens()) || RiffRepeater::currentlyEnabled_Above100) {
		realSongSpeed = RiffRepeater::GetSpeed(true); // While this should almost always be the same value, the user might enable riff repeater, which could cause this number to be wrong.

		DX9DrawText(
			"Song Speed: " + std::to_string(static_cast<int>(roundf(realSongSpeed))) + "%",
			whiteText,
			static_cast<int>(WindowSize.width / 2.0f - WindowSize.width / 38.4f), // 50 pixels left of center in 1920x1080 resolution
			static_cast<int>(WindowSize.height / 54.0f),                          // 20 pixels from top
			static_cast<int>(WindowSize.width / 2.0f + WindowSize.width / 38.4f), // 50 pixels right of center
			static_cast<int>(WindowSize.height / 16.0f),                          // 120 pixels from top
			pDevice,
			{ NULL, NULL },
			DT_CENTER | DT_NOCLIP);
	}
}

void GameOverlay::DisplayNoteByNoteStatus()
{
#if !defined(RSMODS_PUBLIC_RELEASE)
	// Debug and Release+probe: the harness is installed in both (never Master), and a player
	// must always see when the cable is synthetic, so the indicator is not Debug-only.
	// Fake-guitar harness indicator, also before the early-return so it shows whenever the
	// synthetic input is armed even with NBN off. Amber so it is never mistaken for a
	// real-cable state; shows the note currently being injected as live feedback.
	if (FakeGuitar::IsInstalled() && FakeGuitar::IsSynthEnabled())
	{
		const int nowMidi = FakeGuitar::CurrentMidi();
		std::string fakeLine = "FAKE GUITAR ARMED  [P]";
		if (nowMidi >= 0) fakeLine += "   playing " + std::to_string(nowMidi);
		// The pitch route the synth is running through, so the test state (mode + shift) is
		// visible at a glance: in Drop Pedal mode the synth passes through the real shifter.
		fakeLine += "   [" + DropPedal::GetPitchModeName();
		const int pitchShift = DropPedal::GetShiftSemitones();
		if (DropPedal::GetPitchMode() == DropPedal::PitchMode::DropPedal && pitchShift != 0)
			fakeLine += (pitchShift > 0 ? " +" : " ") + std::to_string(pitchShift);
		fakeLine += "]";
		if (!FakeGuitar::IsCaptureReady()) fakeLine += "   (waiting for song input)";
		DX9DrawText(
			fakeLine,
			0xFFFFC033,
			static_cast<int>(WindowSize.width / 96.0f),
			static_cast<int>(WindowSize.height / 6.5f),
			static_cast<int>(WindowSize.width),
			static_cast<int>(WindowSize.height / 4.5f),
			pDevice,
			{ NULL, NULL },
			DT_LEFT | DT_NOCLIP);
	}
#endif

#if defined(_DEBUG)
	const bool isEnabled = NoteByNoteProbe::IsAutomaticEnabled();

	// J-press acknowledgement, drawn BEFORE the enabled/menu early-return so the
	// flash shows in every state a press is legal in (including N off, section
	// playing - the recommended seek-test state). Two-second flash.
	{
		double secondsAgo = 0.0;
		bool accepted = false;
		if (NoteByNoteProbe::TryGetSeekRequestFlash(secondsAgo, accepted)
			&& secondsAgo < 2.0)
		{
			DX9DrawText(
				accepted ? "NATIVE SEEK requested [J]" : "NATIVE SEEK refused (no probe)",
				accepted ? whiteText : 0xFFFF6666,
				static_cast<int>(WindowSize.width / 96.0f),
				static_cast<int>(WindowSize.height / 9.0f),
				static_cast<int>(WindowSize.width / 2.0f),
				static_cast<int>(WindowSize.height / 6.0f),
				pDevice,
				{ NULL, NULL },
				DT_LEFT | DT_NOCLIP);
		}
	}

	// The string/fret ML companion overlay is DisplayMlStringFretOverlay(), called outside
	// this _DEBUG block so the ML feature draws in Release too.

	if (!isEnabled && !GameState::Menus::IsInRiffRepeaterMenus()) return;

	DX9DrawText(
		std::string("Note by Note: ") + (isEnabled ? "ON" : "OFF") + "  [N]",
		isEnabled ? whiteText : greyText,
		static_cast<int>(WindowSize.width / 2.0f - WindowSize.width / 8.0f),
		static_cast<int>(WindowSize.height / 13.5f),
		static_cast<int>(WindowSize.width / 2.0f + WindowSize.width / 8.0f),
		static_cast<int>(WindowSize.height / 7.0f),
		pDevice,
		{ NULL, NULL },
		DT_CENTER | DT_NOCLIP);

	const auto targetText = NoteByNoteProbe::GetAutomaticTargetText();
	if (!targetText.empty())
	{
		DX9DrawText(
			targetText,
			whiteText,
			static_cast<int>(WindowSize.width / 2.0f - WindowSize.width / 5.0f),
			static_cast<int>(WindowSize.height / 27.0f),
			static_cast<int>(WindowSize.width / 2.0f + WindowSize.width / 5.0f),
			static_cast<int>(WindowSize.height / 15.0f),
			pDevice,
			{ 0, static_cast<unsigned int>(std::max(24, static_cast<int>(WindowSize.height / 32.0f))) },
			DT_CENTER | DT_NOCLIP);
	}

	// Input-health line: the live detector level, top-left under the pitch status. A live
	// chain moves this with every strum; a dead capture session pins near -77 dB. Red once
	// the level sits in the dead band: if strums do not move the number, the capture session
	// is gone and a game restart is what re-initializes it.
	// The audio diagnostics signal bar occupies these same rows, so this line only draws when
	// the bar is not visible (toggle off, overlay switched off in RSMods.ini, or no input path).
	if (isEnabled && !IsAudioDiagnosticsVisible())
	{
		ResearchProtocol::NoteByNoteState inputState;
		if (NoteByNoteNativeScoring::TryGetResearchState(inputState)
			&& inputState.structSize >= sizeof(ResearchProtocol::NoteByNoteState)
			&& inputState.detectorSampleValid != 0)
		{
			// The DEAD hint only makes sense while a hold is actively waiting on
			// input; a pause menu legitimately quiets the capture.
			const bool looksDead = inputState.detectorLevelDb < -68.0f
				&& inputState.ownsNativeHold != 0;
			const int deadRedText = 0xFFFF6666;
			std::ostringstream inputLine;
			inputLine << "Input: " << std::fixed << std::setprecision(1)
				<< inputState.detectorLevelDb << " dB";
			if (looksDead) inputLine << "  DEAD? strum - if this stays put, restart the game";
			DX9DrawText(
				inputLine.str(),
				looksDead ? deadRedText : greyText,
				static_cast<int>(WindowSize.width / 96.0f),
				static_cast<int>(WindowSize.height / 22.0f),
				static_cast<int>(WindowSize.width / 2.0f),
				static_cast<int>(WindowSize.height / 14.0f),
				pDevice,
				{ NULL, NULL },
				DT_LEFT | DT_NOCLIP);
		}
	}

	// Host-drawn finger numerals for chord holds: the game's own numeral glyphs
	// are screen-space quads laid out once and never repainted on frozen
	// retargets, so during chord holds the overlay paints the SNG template's
	// fingers itself, at screen anchors projected from the kept members' own
	// marker draws this frame. Repainted every frame, so repeated asks show the
	// chord again rather than blanking it.
	// Numeral-path heartbeat: every ~10 s log which stage starves (the enable
	// gates, the chord-hold gate, the anchor supply, or the finger lookup).
	static LONG numeralHeartbeat = 0;
	const bool numeralGatesOpen = isEnabled
		&& D3DHooks::AreFingerNumeralsEnabled()
		&& NoteByNoteHighwayRenderer::IsChordHoldActive();
	int numeralsDrawn = 0;
	int heartbeatAnchorCount = -1;
	if (isEnabled
		&& D3DHooks::AreFingerNumeralsEnabled()
		&& NoteByNoteHighwayRenderer::IsChordHoldActive())
	{
		int anchorStrings[8];
		int anchorFrets[8];
		float anchorX[8];
		float anchorY[8];
		const int anchorCount = D3DHooks::GetFreshFingerAnchors(
			anchorStrings, anchorFrets, anchorX, anchorY);
		heartbeatAnchorCount = anchorCount;
		const int numeralHeight = std::max(18, static_cast<int>(WindowSize.height / 40.0f));
		const int whiteText = 0xFFFFFFFF;
		const int shadowText = 0xC0000000;
		for (int index = 0; index < anchorCount; ++index)
		{
			int finger = 0;
			if (!NoteByNoteHighwayRenderer::TryGetChordFingerForCoordinate(
				anchorStrings[index], anchorFrets[index], finger))
			{
				continue;
			}
			const std::string digit(1, static_cast<char>('0' + finger));
			// Anchors are [0..1] screen fractions (the 3D viewport is smaller than
			// the window on high-res rigs); scale into the overlay's window space.
			const int centreX = static_cast<int>(anchorX[index] * WindowSize.width);
			const int centreY = static_cast<int>(anchorY[index] * WindowSize.height);
			const int halfBox = numeralHeight;
			// A one-pixel shadow first, so the digit reads on the bright marker art.
			DX9DrawText(
				digit,
				shadowText,
				centreX - halfBox + 1,
				centreY - halfBox + 1,
				centreX + halfBox + 1,
				centreY + halfBox + 1,
				pDevice,
				{ 0, static_cast<unsigned int>(numeralHeight) },
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
			DX9DrawText(
				digit,
				whiteText,
				centreX - halfBox,
				centreY - halfBox,
				centreX + halfBox,
				centreY + halfBox,
				pDevice,
				{ 0, static_cast<unsigned int>(numeralHeight) },
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
			++numeralsDrawn;
		}
	}
	if ((InterlockedIncrement(&numeralHeartbeat) % 600) == 1)
	{
		LOG_INFO("(NBN NUMERALS) gates=" << numeralGatesOpen
			<< " (enabled=" << isEnabled
			<< " toggle=" << D3DHooks::AreFingerNumeralsEnabled()
			<< " chordHold=" << NoteByNoteHighwayRenderer::IsChordHoldActive()
			<< ") anchorsFresh=" << heartbeatAnchorCount
			<< " drawn=" << numeralsDrawn << std::endl);
	}
#endif
	// Outside the _DEBUG block: the rest of DisplayNoteByNoteStatus is diagnostic overlay
	// that ships Debug-only, but the bend visualizer is a shipped FEATURE and must draw
	// in Release too. The meter function is Release-safe on its own (only its internal log
	// lines are _DEBUG-gated), so calling it here makes it render in every configuration.
	DisplayNoteByNoteBendMeter();
	DisplayMlStringFretOverlay();
}

// Single source of truth for "the audio diagnostics rows are on screen". The NBN input
// line (DisplayNoteByNoteStatus) shares those rows and yields to this block, so both must
// agree on the gate; keep every visibility condition here.
bool GameOverlay::IsAudioDiagnosticsVisible()
{
	if (!OverlayToggles::Get("audio_diag") || !Audio::CableInput::IsOverlayEnabled()) return false;
	const Audio::CableInput::Diagnostics d = Audio::CableInput::GetDiagnostics();
	return d.installed || d.rsAsio;
}

namespace
{
	// The game's EndScene often runs with a SUB-REGION viewport active (measured 1920x1080 inside a
	// 2560x1600 backbuffer). D3D clips ALL rendering to the viewport, DT_NOCLIP or not, so text anchored
	// to the window's right edge is cut off at the viewport's edge (the "invisible wall"). This widens the
	// viewport to the whole backbuffer for the scope of one overlay element and restores it afterwards.
	// Used by every right-anchored element (REC badge, SIGNAL row).
	struct FullSurfaceViewport
	{
		LPDIRECT3DDEVICE9 device = nullptr;
		D3DVIEWPORT9 saved{};
		bool overridden = false;
		int width = 0;
		int height = 0;

		FullSurfaceViewport(LPDIRECT3DDEVICE9 pDevice, int fallbackWidth, int fallbackHeight)
			: device(pDevice), width(fallbackWidth), height(fallbackHeight)
		{
			if (!device) return;
			IDirect3DSurface9* renderTarget = nullptr;
			if (SUCCEEDED(device->GetRenderTarget(0, &renderTarget)) && renderTarget)
			{
				D3DSURFACE_DESC desc{};
				if (SUCCEEDED(renderTarget->GetDesc(&desc)) && desc.Width > 0 && desc.Height > 0)
				{
					width = static_cast<int>(desc.Width);
					height = static_cast<int>(desc.Height);
				}
				renderTarget->Release();
			}
			if (SUCCEEDED(device->GetViewport(&saved)))
			{
				D3DVIEWPORT9 full{ 0, 0, static_cast<DWORD>(width), static_cast<DWORD>(height), 0.0f, 1.0f };
				overridden = SUCCEEDED(device->SetViewport(&full));
			}
		}
		~FullSurfaceViewport() { if (overridden) device->SetViewport(&saved); }
		FullSurfaceViewport(const FullSurfaceViewport&) = delete;
		FullSurfaceViewport& operator=(const FullSurfaceViewport&) = delete;
	};
}

void GameOverlay::DisplayAudioDiagnostics()
{
	if (!IsAudioDiagnosticsVisible()) return;
	const Audio::CableInput::Diagnostics d = Audio::CableInput::GetDiagnostics();

	// One formatter for both players: the bar, the dB figure and the packet rate read the same
	// way on each row, so the eye compares them directly.
	auto formatLive = [](std::wostringstream& out, const wchar_t* label, float meterPeak,
		double packetsPerSecond, uint64_t dropouts)
	{
		const double db = meterPeak > 0.0f ? 20.0 * std::log10(meterPeak) : -90.0;
		const int bars = std::clamp(static_cast<int>((db + 60.0) / 60.0 * 12.0 + 0.5), 0, 12);
		out << label << L"  ";
		for (int i = 0; i < 12; ++i) out << (i < bars ? L'\u2588' : L'\u2591');
		out << L"  " << std::fixed << std::setprecision(0) << std::max(db, -90.0) << L" dB   "
			<< packetsPerSecond << L" pkt/s";
		if (dropouts > 0) out << L"   dropouts " << dropouts;
	};

	std::wostringstream signal;
	if (d.proxyInputMode == 1)
	{
		signal << L"NO INPUT  connect a guitar input";
	}
	else if (!d.streamActive)
	{
		signal << L"SIGNAL  waiting for input";
	}
	else if (d.stalled)
	{
		signal << L"NO SIGNAL  the input stream has stalled";
	}
	else
	{
		formatLive(signal, L"SIGNAL", d.meterPeak, d.packetsPerSecond, d.dropouts);
	}

	// Player 2's row. Drawn whenever the game is in 2-player, directly under Player 1's
	// row, mirroring the Drop Pedal overlay's second Pitch row on the left. Its states name what
	// Player 2 actually has, so a silent second guitar is diagnosable from the screen alone.
	std::wostringstream playerTwo;
	const bool showPlayerTwo = GameState::IsMultiplayer();
	if (showPlayerTwo)
	{
		if (!d.playerTwoInput)
		{
			playerTwo << L"P2 NO INPUT  no second input is configured";
		}
		else if (d.playerTwoCableFeedOff)
		{
			playerTwo << L"P2 CABLE OFF  enable the Real Tone Cable for Player 2";
		}
		else if (!d.playerTwoStreamActive)
		{
			playerTwo << L"P2 SIGNAL  waiting for input";
		}
		else if (d.playerTwoStalled)
		{
			playerTwo << L"P2 NO SIGNAL  the input stream has stalled";
		}
		else
		{
			formatLive(playerTwo, L"P2 SIGNAL", d.playerTwoMeterPeak, d.playerTwoPacketsPerSecond, 0);
		}
	}

	// Top row (the Drop Pedal "Pitch" row; that overlay draws one Pitch row per player, so the signal
	// readout must not take a row of its own), anchored to the TRUE right edge of the backbuffer with the
	// viewport widened for the draw (see FullSurfaceViewport). While a take is recording the REC badge
	// owns the corner, so the readout steps left of it. The Player 2 row uses the same row pitch as the
	// Drop Pedal overlay (height / 36) so the two players' rows line up across the screen.
	FullSurfaceViewport viewport(pDevice, static_cast<int>(WindowSize.width), static_cast<int>(WindowSize.height));
	const int surfaceW = viewport.width;
	const int surfaceH = viewport.height;
	const int margin = std::max(8, surfaceW / 96);
	const int fontSize = std::max(14, surfaceH / 62);
	const int rowPitch = surfaceH / 36;
	const int top = std::max(8, surfaceH / 54);
	const int recBadgeWidth = Audio::SharedOutput::IsRecording() ? std::max(18, surfaceH / 44) * 5 : 0;
	const int right = surfaceW - margin - recBadgeWidth;
	const int left = surfaceW / 3;
	const DWORD fmt = DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOCLIP;
	auto drawRow = [&](const std::wstring& text, int row)
	{
		const int rowTop = top + row * rowPitch;
		const int rowBottom = rowTop + rowPitch;
		DX9DrawTextW(text, 0xFF000000, left + 2, rowTop + 2, right + 2, rowBottom + 2, pDevice, fontSize, fmt, FW_BOLD);
		DX9DrawTextW(text, 0xFFFFFFFF, left, rowTop, right, rowBottom, pDevice, fontSize, fmt, FW_BOLD);
	};
	drawRow(signal.str(), 0);
	if (showPlayerTwo) drawRow(playerTwo.str(), 1);
}

// Paired detection readout: Rocksmith's OWN pitch read (line 1) with the FretNet ML
// companion's read of the same audio directly under it (line 2). The game frequently
// hears the correct pitch (line 1) but its quality-50 accept gate refuses a high/thin
// note, while ML (line 2) confirms it. The ML line says "(service offline)" when the
// companion is absent rather than vanishing.
//
// Deliberately OUTSIDE the _DEBUG block (like the bend visualizer) so it is visible in
// Release at true FPS. Shadow only, never a gameplay decision. The target remains visible
// whenever automatic Note-by-Note is active; diagnostic rows are gated by "ml_fret" and the
// detection-visibility setting.
void GameOverlay::DisplayMlStringFretOverlay()
{
	if (!NoteByNoteProbe::IsAutomaticEnabledFast()) return;
	if (!GameState::Menus::IsInSongModes() || GameState::Menus::IsInRiffRepeaterMenus()) return;

	ResearchProtocol::NoteByNoteState state;
	const bool haveState = NoteByNoteNativeScoring::TryGetResearchState(state) && state.isInitialized;
	const int targetMidi = haveState ? state.expectedMidi : -1;
	const int targetString = haveState && state.selectedChordId == -1 ? state.selectedString : -1;
	int targetColorString = targetString;
	const int targetFret = haveState ? state.selectedFret : -1;
	// The game leaves a viewport smaller than the screen (an "invisible wall" that clips the target when
	// dragged low and right), so draw the whole Note by Note HUD against the full backbuffer.
	FullSurfaceViewport viewport(pDevice, static_cast<int>(WindowSize.width), static_cast<int>(WindowSize.height));
	const bool showDiagnostics = OverlayToggles::Get("ml_fret")
		&& Settings::IsNoteByNoteDetectionVisible();

	const Settings::NoteByNoteTargetStyle targetStyle = Settings::GetNoteByNoteTargetStyle();
	const std::string bendWords = BendWords(state.isBendTarget && state.bendAcceptMidi >= 0 && targetMidi >= 0
		? state.bendAcceptMidi - targetMidi : 0);
	// Frets per string (index 0 = low E, -1 = not played) for the Tab style.
	int tabFrets[6] = { -1, -1, -1, -1, -1, -1 };
	if (targetString >= 0 && targetString < 6 && targetFret >= 0) tabFrets[targetString] = targetFret;

	std::string targetText = NoteByNote::FormatPitch(targetMidi);
	if (targetString >= 0 && targetFret >= 0 && targetStyle != Settings::NoteByNoteTargetStyle::Detailed)
	{
		// Simple style: the string colour swatch carries the string, so the text is just the fret, plus the
		// bend depth. Needs no tuning lookup. (Also the fallback text if Tab cannot draw.)
		targetText = std::to_string(targetFret);
		if (!bendWords.empty()) targetText += " " + bendWords;
	}
	else if (targetString >= 0 && targetFret >= 0)
	{
		int physicalOpenMidi = -1;
		static bool reportedMissingPhysicalTuning = false;
		if (DropPedal::TryGetPhysicalOpenStringMidi(targetString, physicalOpenMidi))
		{
			reportedMissingPhysicalTuning = false;
			const int physicalTargetMidi = physicalOpenMidi + targetFret;
			targetText = NoteByNote::FormatPosition(targetString, targetFret, physicalOpenMidi)
				+ " (" + NoteByNote::FormatPitch(physicalTargetMidi) + ")";
			if (state.isBendTarget && state.bendAcceptMidi >= 0 && targetMidi >= 0)
				targetText += " bend to "
					+ NoteByNote::FormatPitch(physicalTargetMidi + state.bendAcceptMidi - targetMidi);
		}
		else
		{
			targetText = "(tuning unavailable)";
			if (!reportedMissingPhysicalTuning)
			{
				LOG_ERROR("(NBN HUD) Physical string tuning unavailable for target display." << std::endl);
				reportedMissingPhysicalTuning = true;
			}
		}
	}
	else if (haveState && state.selectedChordId >= 0)
	{
		char fingering[24] = {};
		// Through the loaded controller, not the host's own copy (stale after a probe hot-load).
		if (ResearchBridge::DescribeSelectedChordFingering(
			state.selectedRecord,
			fingering,
			sizeof(fingering),
			targetColorString))
		{
			targetText = fingering;
			// "[x/3/3/x/x/x]", low E first: the Tab style's per-string frets.
			int stringIndex = 0;
			for (const char* at = fingering + 1; *at && *at != ']' && stringIndex < 6; ++stringIndex) {
				tabFrets[stringIndex] = (*at >= '0' && *at <= '9') ? std::atoi(at) : -1;
				while (*at && *at != '/' && *at != ']') ++at;
				if (*at == '/') ++at;
			}
		}
		else
			targetText = "(fingering unavailable)";
	}

	// Two independent blocks: the readout (Native/Enhanced/ML) sized by NoteByNoteUiSize and the
	// target (string-colour swatch + note) sized ONLY by NoteByNoteTargetSize. Each block's top-left is either
	// the built-in layout or a position the player dragged in the overlay's layout editor (fractions of the
	// window, see Settings::HudPlacement). The target's dragged placement applies only in the Custom position and
	// is remembered while Left or Center is chosen. The one remaining link is deliberate: the "Left" (Under
	// readout) target follows the readout's bottom edge; Custom decouples it entirely.
	using HudBlock = Settings::NoteByNoteHudBlock;
	Overlay::HudLayout::frameWidth = static_cast<float>(WindowSize.width);
	Overlay::HudLayout::frameHeight = static_cast<float>(WindowSize.height);
	const Settings::HudPlacement readoutPlacement = Settings::GetNoteByNoteHudPlacement(HudBlock::Readout);
	const Settings::HudPlacement targetPlacement = Settings::GetNoteByNoteHudPlacement(HudBlock::Target);
	const int baseX = readoutPlacement.IsSet()
		? static_cast<int>(readoutPlacement.x * WindowSize.width)
		: static_cast<int>(WindowSize.width / 96.0f);
	const int baseY = readoutPlacement.IsSet()
		? static_cast<int>(readoutPlacement.y * WindowSize.height)
		: static_cast<int>(WindowSize.height / 5.2f);
	const Settings::NoteByNoteTargetPosition targetPosition = Settings::GetNoteByNoteTargetPosition();
	const bool targetIsCentred = targetPosition == Settings::NoteByNoteTargetPosition::Center;
	// Custom with nothing dragged yet starts from the Under readout spot.
	const bool targetIsCustom = targetPosition == Settings::NoteByNoteTargetPosition::Custom && targetPlacement.IsSet();
	const int defaultTextHeight = std::max(14, static_cast<int>(WindowSize.height / 80.0f));
	const int textHeight = std::max(7, defaultTextHeight * Settings::GetNoteByNoteUiSize() / 100);
	const int targetTextHeight = std::max(7, defaultTextHeight * Settings::GetNoteByNoteTargetSize() / 100);
	// Tab style: six lines, high string on top, "E |--7--" with the frets in one aligned column. The string
	// names are drawn in each string's colour, which stands in for the swatch.
	bool useTab = false;
	for (const int fret : tabFrets) useTab |= targetStyle == Settings::NoteByNoteTargetStyle::Tab && fret >= 0;
	const int tabLine = targetTextHeight + targetTextHeight / 6;
	const int tabLabelW = MeasureHudText("G#", targetTextHeight, pDevice).cx + targetTextHeight / 3;
	const int tabBarW = MeasureHudText("|", targetTextHeight, pDevice).cx + targetTextHeight / 8;
	const int tabDashW = MeasureHudText("--", targetTextHeight, pDevice).cx;
	const int tabCellW = MeasureHudText("00", targetTextHeight, pDevice).cx + targetTextHeight / 3;
	const int tabBendW = bendWords.empty() ? 0 : MeasureHudText("  " + bendWords, targetTextHeight, pDevice).cx;
	const int tabWidth = tabLabelW + tabBarW + tabDashW * 2 + tabCellW + tabBendW;
	const int tabHeight = tabLine * 6;
	// Line spacing only stretches the gap between readout lines; 100% is two text heights.
	const int rowHeight = std::max(textHeight, textHeight * 2 * Settings::GetNoteByNoteLineSpacing() / 100);
	const int valueX = baseX + textHeight * 7;
	const int readoutBottom = baseY + rowHeight * 2 + textHeight;
	const int rightEdge = static_cast<int>(WindowSize.width);
	// The target is a fixed-top box: its top-left is anchored and never moves with the target size, and the
	// text is top-aligned, so a bigger target only grows right and downward. Built-in "Left" anchor: under the
	// readout plus a lyric clearance (two original rows) so it clears the game's two-line lyric band.
	int targetValueX, targetRowTop;
	if (targetIsCustom)
	{
		targetValueX = static_cast<int>(targetPlacement.x * WindowSize.width);
		targetRowTop = static_cast<int>(targetPlacement.y * WindowSize.height);
	}
	else if (targetIsCentred)
	{
		// Centre the whole block (swatch + text) on the screen, not its left edge, so a bigger target or a longer
		// chord name stays centred instead of growing to the right.
		const bool hasSwatch = targetColorString >= 0 && targetColorString < 6;
		const int blockWidth = useTab ? tabWidth : MeasureHudText(targetText, targetTextHeight, pDevice).cx
			+ (hasSwatch ? targetTextHeight + targetTextHeight / 2 : 0);
		targetValueX = std::max(0, static_cast<int>(WindowSize.width / 2) - blockWidth / 2);
		targetRowTop = static_cast<int>(WindowSize.height * 0.20f);
	}
	else
	{
		targetValueX = baseX;
		targetRowTop = readoutBottom + textHeight + textHeight * 4;
	}
	const int targetLeft = targetValueX;
	if (!useTab && targetColorString >= 0 && targetColorString < 6)
	{
		RSColor stringColor;
		const bool hasStringColor = ERMode::TryGetActiveStringColor(targetColorString, stringColor);
		static bool reportedMissingStringColor = false;
		if (hasStringColor)
		{
			reportedMissingStringColor = false;
			// Part of the target block, so it scales with the TARGET size, not the readout size.
			const int y = targetRowTop;
			DX9DrawTextW(L"\u25A0", D3DCOLOR_COLORVALUE(stringColor.r, stringColor.g, stringColor.b, 1.0f),
				targetValueX, y, targetValueX + targetTextHeight, y + targetTextHeight * 2, pDevice, targetTextHeight, DT_LEFT | DT_NOCLIP);
			targetValueX += targetTextHeight + targetTextHeight / 2;
		}
		else if (!reportedMissingStringColor)
		{
			LOG_ERROR("(NBN HUD) Active string colour unavailable; target swatch omitted." << std::endl);
			reportedMissingStringColor = true;
		}
	}
	const auto palette = Settings::GetNoteByNoteDetectionPalette();
	if (showDiagnostics)
	{
		const auto& feedback = state.detectionFeedback;
		const auto now = GetTickCount64();
		auto nativeColor = haveState
			? NoteByNote::GetDetectorColor(feedback.nativeRole, feedback.tick, now) : 0xFFFFFFFF;
		auto mlColor = haveState
			? NoteByNote::GetDetectorColor(feedback.mlRole, feedback.tick, now) : 0xFFFFFFFF;
		const auto enhancedColor = haveState
			? NoteByNote::GetDetectorColor(feedback.enhancedRole, feedback.tick, now) : 0xFFFFFFFF;
		const bool showDecision = haveState
			&& (nativeColor != 0xFFFFFFFF || enhancedColor != 0xFFFFFFFF || mlColor != 0xFFFFFFFF);
		const int matchingMidi = haveState && state.isBendTarget ? state.bendAcceptMidi : targetMidi;
		// Speaker Mode leaves the input unshifted, so the game's detector reads in the chart's tuning
		// frame, one route-shift away from what the player physically plays (Eb chart, E guitar: every
		// pick read one semitone low, so a correct A showed "Ab" and the row could never go green).
		// Shift native reads into the player's frame; Drop Pedal shifts the input itself, so 0 there.
		const int nativeFrameOffset = DropPedal::GetPitchMode() == DropPedal::PitchMode::SpeakerMode
			? -DropPedal::GetShiftSemitones() : 0;
		auto toPlayerFrame = [&](int midi) { return midi >= 0 ? midi + nativeFrameOffset : -1; };
		bool nativeMatchesLive = false;
		bool mlMatchesLive = false;
		int heard = -1;
		if (showDecision) heard = toPlayerFrame(feedback.nativeMidi);
		else if (haveState && state.detectorSampleValid && state.detectorPassesLevel)
			heard = toPlayerFrame(state.detectorLoudestMidi);
		if (!showDecision && targetString >= 0 && NoteByNote::MatchesDetectorTarget(heard, matchingMidi))
			nativeMatchesLive = true;
		// After a chord pass, a detector that matched the chord shows the chord's root note ("D" for a D chord)
		// instead of the bare word "chord" or native's single loudest note. A detector
		// that did not match it keeps showing what it heard.
		auto decisionText = [&](int midi, NoteByNote::DetectorRole role) {
			if (feedback.stringIndex < 0 && role == NoteByNote::DetectorRole::Confirmed)
				return feedback.chordLabel[0] != '\0' ? std::string(feedback.chordLabel) : std::string("chord");
			return midi >= 0 ? NoteByNote::FormatPitch(midi) : std::string("--");
		};
		const std::string nativeText = showDecision
			? decisionText(heard, feedback.nativeRole) : NoteByNote::FormatPitch(heard);
		const std::string enhancedText = showDecision
			? decisionText(feedback.enhancedMidi, feedback.enhancedRole) : std::string("--");

		MlStringFretReader::StringFret sample;
		std::string mlText;
		if (showDecision)
		{
			mlText = decisionText(feedback.mlMidi, feedback.mlRole);
		}
		else if (!MlStringFretReader::TryGet(sample))
		{
			mlText = "(service offline)";
		}
		else
		{
			bool displayedPitches[12] = {};
			const int appliedShift = DropPedal::GetAppliedInputShiftSemitones();
			for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
			{
				const int midi = NoteByNote::GetStringFretMidi(stringIndex, sample.fret[stringIndex]);
				// ML hears the post-shifter audio, the same frame as native and the target, so its text
				// needs no shift and it matches on the target alone. The physical-frame alias
				// (target - appliedShift) is not accepted: under Drop Pedal E->Eb it is exactly a note
				// fretted one ABOVE the target. Matches the pass logic (NoteByNoteHostServices.cpp
				// EvaluateMlNote).
				if (targetString >= 0 && sample.shift == appliedShift
					&& NoteByNote::MatchesDetectorTarget(midi, matchingMidi, sample.conf[stringIndex]))
				{
					mlMatchesLive = true;
				}
				if (midi < 0 || displayedPitches[midi % 12]) continue;
				displayedPitches[midi % 12] = true;
				if (!mlText.empty()) mlText += "  ";
				mlText += NoteByNote::FormatPitch(midi);
			}
			if (mlText.empty()) mlText = "--";
		}
		// Two separate paths:
		// - LIVE (no accept on screen): a row is green when the pitch it shows IS the target - pure
		//   detection truth, neutral otherwise (no red on a transient).
		// - DECISION (for ~1.3 s after an accept): a row is green only if that detector was used to pass
		//   the note (its role in the accept, set by the scoring controller), for single notes and chords
		//   alike. It must not compare against the HUD's target: by then the target is already the NEXT
		//   note.
		// A short hold keeps each row's text and colour on screen together so a flickering live reading
		// does not strobe; they refresh and expire as one unit.
		// A detector that helped pass the note is only GREEN when the pitch its row shows IS the passed
		// note. The pass logic lets native count from +-1 semitone (NativeCorroboratesPick, the Speaker
		// Mode frame slip) and one fallback marks native used without looking at its read, so a role
		// alone could paint "B" green on a Bb note. Counted-but-read-off is amber (partial). A chord
		// pass has no single pitch ("chord"), so its role stands alone.
		auto decisionColor = [&](NoteByNote::DetectorRole role, int shownMidi) -> uint32_t {
			if (role != NoteByNote::DetectorRole::Confirmed && role != NoteByNote::DetectorRole::Partial)
				return palette.neutral;
			// A chord row that matched shows the chord's name, not a pitch to compare: its role is the truth.
			if (feedback.stringIndex < 0 || shownMidi < 0 || shownMidi == feedback.targetMidi)
				return role == NoteByNote::DetectorRole::Confirmed ? palette.confirmed : palette.partial;
			return palette.partial;
		};
		const uint32_t nativeColorNow = showDecision ? decisionColor(feedback.nativeRole, heard)
			: nativeMatchesLive ? palette.confirmed : palette.neutral;
		const uint32_t enhancedColorNow = showDecision ? decisionColor(feedback.enhancedRole, feedback.enhancedMidi)
			: palette.neutral;
		const uint32_t mlColorNow = showDecision ? decisionColor(feedback.mlRole, feedback.mlMidi)
			: mlMatchesLive ? palette.confirmed : palette.neutral;
		struct RowHold { std::string text = "--"; uint32_t color = 0; uint64_t tick = 0; };
		static RowHold holds[3];
		// The hold must never mix the two paths: otherwise, when the accept readout expires, a row with
		// no live reading keeps the ACCEPTED note's text+green while ML already shows a live read of the
		// NEXT target, and the rows describe two different notes on one frame.
		static bool holdsFromDecision = false;
		if (holdsFromDecision != showDecision)
		{
			for (auto& hold : holds) hold = {};
			holdsFromDecision = showDecision;
		}
		constexpr uint64_t ROW_HOLD_MS = 300;
		auto resolveRow = [&](int index, const std::string& text, uint32_t color, bool hasReading)
			-> std::pair<std::string, uint32_t> {
			if (hasReading) holds[index] = { text, color, now };
			else if (holds[index].tick && now - holds[index].tick > ROW_HOLD_MS)
				holds[index] = { "--", palette.neutral, 0 };
			return holds[index].tick
				? std::pair<std::string, uint32_t>{ holds[index].text, holds[index].color }
				: std::pair<std::string, uint32_t>{ std::string("--"), palette.neutral };
		};
		const auto nativeRow = resolveRow(0, nativeText, nativeColorNow, nativeText != "--");
		const auto enhancedRow = resolveRow(1, enhancedText, enhancedColorNow, enhancedText != "--");
		const auto mlRow = resolveRow(2, mlText, mlColorNow, mlText != "--");
		const std::string values[] = { nativeRow.first, enhancedRow.first, mlRow.first };
		const uint32_t colors[] = { nativeRow.second, enhancedRow.second, mlRow.second };
		const char* labels[] = { "Native:", "Enhanced:", "ML:" };
		// Readout hit box for the layout editor: at least a few characters wide so a "--" block is still grabbable.
		LONG widestValue = textHeight * 4;
		for (int row = 0; row < 3; ++row)
		{
			const int y = baseY + row * rowHeight;
			DX9DrawText(labels[row], colors[row], baseX, y, valueX, y + textHeight * 2,
				pDevice, { 0, static_cast<unsigned>(textHeight) }, DT_LEFT | DT_NOCLIP);
			DX9DrawText(values[row], colors[row], valueX, y, rightEdge, y + textHeight * 2,
				pDevice, { 0, static_cast<unsigned>(textHeight) }, DT_LEFT | DT_NOCLIP);
			widestValue = std::max(widestValue, MeasureHudText(values[row], textHeight, pDevice).cx);
		}
		Overlay::HudLayout::Publish(HudBlock::Readout, static_cast<float>(baseX), static_cast<float>(baseY),
			static_cast<float>(valueX + widestValue), static_cast<float>(readoutBottom));
	}

	if (useTab)
	{
		const Resolution font{ 0, static_cast<unsigned>(targetTextHeight) };
		constexpr int dim = static_cast<int>(0xFF8A8A8A);   // staff lines and empty strings recede behind the frets
		for (int row = 0; row < 6; ++row)
		{
			const int stringIndex = 5 - row;
			const int y = targetRowTop + row * tabLine;
			const int bottom = y + tabLine * 2;
			// String name from the physical tuning (Drop Pedal aware), standard tuning if unavailable; the top
			// string is lower case, as tabs write it.
			static const char* const standard[] = { "E", "A", "D", "G", "B", "e" };
			std::string name = standard[stringIndex];
			int openMidi = -1;
			if (DropPedal::TryGetPhysicalOpenStringMidi(stringIndex, openMidi)) {
				name = PitchNames::ForPitchClass(openMidi);
				if (stringIndex == 5 && !name.empty()) name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
			}
			RSColor stringColor;
			const int labelColor = ERMode::TryGetActiveStringColor(stringIndex, stringColor)
				? static_cast<int>(D3DCOLOR_COLORVALUE(stringColor.r, stringColor.g, stringColor.b, 1.0f)) : static_cast<int>(palette.neutral);
			int x = targetLeft;
			DX9DrawText(name, labelColor, x, y, x + tabLabelW, bottom, pDevice, font, DT_LEFT | DT_NOCLIP);
			x += tabLabelW;
			DX9DrawText("|--", dim, x, y, x + tabBarW + tabDashW, bottom, pDevice, font, DT_LEFT | DT_NOCLIP);
			x += tabBarW + tabDashW;
			const bool played = tabFrets[stringIndex] >= 0;
			DX9DrawText(played ? std::to_string(tabFrets[stringIndex]) : "--", played ? static_cast<int>(palette.neutral) : dim,
				x, y, x + tabCellW, bottom, pDevice, font, DT_CENTER | DT_NOCLIP);
			x += tabCellW;
			DX9DrawText("--", dim, x, y, x + tabDashW, bottom, pDevice, font, DT_LEFT | DT_NOCLIP);
			x += tabDashW;
			if (played && !bendWords.empty())
				DX9DrawText("  " + bendWords, palette.neutral, x, y, rightEdge, bottom, pDevice, font, DT_LEFT | DT_NOCLIP);
		}
		Overlay::HudLayout::Publish(HudBlock::Target, static_cast<float>(targetLeft), static_cast<float>(targetRowTop),
			static_cast<float>(targetLeft + tabWidth), static_cast<float>(targetRowTop + tabHeight));
		return;
	}

	DX9DrawText(targetText, palette.neutral, targetValueX, targetRowTop, rightEdge,
		targetRowTop + targetTextHeight * 2, pDevice,
		{ 0, static_cast<unsigned>(targetTextHeight) }, DT_LEFT | DT_NOCLIP);
	const SIZE targetSize = MeasureHudText(targetText, targetTextHeight, pDevice);
	Overlay::HudLayout::Publish(HudBlock::Target, static_cast<float>(targetLeft), static_cast<float>(targetRowTop),
		static_cast<float>(targetValueX + std::max<LONG>(targetSize.cx, targetTextHeight * 2)),
		static_cast<float>(targetRowTop + std::max<LONG>(targetSize.cy, targetTextHeight)));
}
// The bend visualizer: a tuner-style ladder at the screen's
// right edge while a bend gesture is the target. Five rungs - the bend target in
// the middle context of two semitones above and below - and the sounding pitch
// marked on its rung, so the player watches their bend climb into the target like
// a tuner needle. Data comes from the research probe's state feed: fractional
// pitch when one of Rocksmith's continuous trackers is live, the ungated integer
// detector pitch otherwise.
void GameOverlay::DisplayNoteByNoteBendMeter()
{
	// The bend visualizer is a FEATURE and draws in every configuration. Only the
	// diagnostic log lines are Debug-only. Visibility is the OverlayToggles "bend_meter"
	// tag (.ini + bridge controllable); it ships on.
	if (!OverlayToggles::Get("bend_meter")) return;
	if (!NoteByNoteProbe::IsAutomaticEnabled()) return;

	ResearchProtocol::NoteByNoteState state;
	if (!NoteByNoteNativeScoring::TryGetResearchState(state))
	{
#if defined(_DEBUG)
		static LONG noStateBudget = 5;
		if (InterlockedDecrement(&noStateBudget) >= 0)
			LOG_INFO("(NBN BEND METER) no probe state available." << std::endl);
#endif
		return;
	}
	// Appended-field guard: a probe older than the bend-visualizer fields reports
	// a smaller structSize and must not have these bytes interpreted.
	if (state.structSize < sizeof(ResearchProtocol::NoteByNoteState))
	{
#if defined(_DEBUG)
		static LONG structSizeBudget = 5;
		if (InterlockedDecrement(&structSizeBudget) >= 0)
			LOG_INFO("(NBN BEND METER) probe structSize " << state.structSize
				<< " predates the bend fields (host expects "
				<< sizeof(ResearchProtocol::NoteByNoteState) << ")." << std::endl);
#endif
		return;
	}
#if defined(_DEBUG)
	// Diagnostics, one budget PER REASON (a shared budget drained in the opening frames
	// and explained nothing). The heartbeat proves the function runs and shows what
	// bendTargetMidi actually is over time; the drawing budget refills on every bend's
	// first frame so every bend leaves evidence in the log.
	static LONG meterHeartbeat = 0;
	static LONG drawingBudget = 10;
	static LONG lastBendTarget = -1;
	if ((InterlockedIncrement(&meterHeartbeat) % 600) == 1)
		LOG_INFO("(NBN BEND METER) alive: bendTarget=" << state.bendTargetMidi
			<< " bendBase=" << state.bendBaseMidi
			<< " sounding=" << state.soundingMidi
			<< " phase=" << static_cast<int>(state.gatePhase)
			<< " holds=" << state.ownsNativeHold << std::endl);
	const LONG thisBendTarget = state.bendTargetMidi;
	if (thisBendTarget != InterlockedExchange(&lastBendTarget, thisBendTarget)
		&& thisBendTarget >= 0)
	{
		InterlockedExchange(&drawingBudget, 3);
	}
	if (state.bendTargetMidi >= 0 && InterlockedDecrement(&drawingBudget) >= 0)
		LOG_INFO("(NBN BEND METER) drawing: base=" << state.bendBaseMidi
			<< " target=" << state.bendTargetMidi
			<< " sounding=" << state.soundingMidi
			<< " quality=" << state.soundingQuality << std::endl);
#endif
	if (state.bendTargetMidi < 0) return;

	const auto noteName = [](int midi) -> std::string
	{
		if (midi < 0) return "?";
		return std::string(PitchNames::ForPitchClass(midi)) + std::to_string(midi / 12 - 1);
	};

	// --- Normalized bend meter ---
	// The rail is a FIXED height whatever the bend depth: the base sits on the bottom rung and
	// the target near the top, so a half-step bend and a two-step bend travel the SAME visual
	// distance; a short bend simply climbs faster per semitone.
	const int base = state.bendBaseMidi >= 0 ? state.bendBaseMidi : (state.bendTargetMidi - 2);
	const int target = state.bendTargetMidi;
	const float depth = static_cast<float>(target - base);
	
	// Anti-flicker: only a read that is PLAUSIBLE for this bend (a little below the base to a
	// little above the target) and not too weak moves the needle; a sub-harmonic or a stray
	// tracker pitch far outside that band is a bad frame that would fling the needle to a rail.
	// The last good value is held through a few bad frames, and a light low-pass removes jitter.
	static int meterTarget = -1;
	static float displaySounding = -1.0f;
	static double staleSeconds = 0.0;
	static float lastRecordTime = -1.0e30f;
	// Reset the pin to the base when a NEW bend note begins, not only when the target value
	// changes. Two consecutive bends to the same target are different chart notes with different
	// record times, so keying only on the target would leave the previous bend's high value in
	// place. selectedRecordTime changes for every note, so it is the reset.
	if (target != meterTarget || state.selectedRecordTime != lastRecordTime)
	{
		meterTarget = target;
		lastRecordTime = state.selectedRecordTime;
		displaySounding = -1.0f;
		staleSeconds = 0.0;
	}
	// Frame time for the smoothing below: the ease factor and the signal-loss hold are
	// time-based so the meter's feel does not change with frame rate. A first draw or a long
	// stall clamps to one nominal frame.
	LARGE_INTEGER meterPerfNow;
	QueryPerformanceCounter(&meterPerfNow);
	LARGE_INTEGER meterPerfFreq;
	QueryPerformanceFrequency(&meterPerfFreq);
	static double lastDrawSeconds = 0.0;
	const double nowSeconds = static_cast<double>(meterPerfNow.QuadPart)
		/ static_cast<double>(meterPerfFreq.QuadPart);
	double frameDelta = nowSeconds - lastDrawSeconds;
	lastDrawSeconds = nowSeconds;
	if (frameDelta <= 0.0 || frameDelta > 0.25) frameDelta = 1.0 / 60.0;
	const float bandLow = static_cast<float>(base) - 1.0f;
	const float bandHigh = static_cast<float>(target) + 1.5f;
	bool haveNeedle = false;
	// Critically damped spring follower. The pitch feed updates at the scoring-tick rate (20-60
	// Hz) and the raw-tap estimate at ~25 Hz, so the target the pin chases moves in steps. The
	// spring carries velocity, so the pin glides between feed updates in both
	// directions with no overshoot; BEND_PIN_SMOOTH_SECONDS is the one feel knob (~time to cover
	// most of a step). 0.045 s keeps it visibly 1:1 with the bend; raise toward 0.08 = smoother,
	// lower toward 0.03 = snappier.
	static float displayVelocity = 0.0f;
	constexpr float BEND_PIN_SMOOTH_SECONDS = 0.045f;
	const auto springTo = [&](float goal)
	{
		const float omega = 2.0f / BEND_PIN_SMOOTH_SECONDS;
		const float dt = static_cast<float>(frameDelta);
		const float x = omega * dt;
		const float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
		const float change = displaySounding - goal;
		const float temp = (displayVelocity + omega * change) * dt;
		displayVelocity = (displayVelocity - omega * temp) * decay;
		displaySounding = goal + (change + temp) * decay;
	};
	// The signal-loss hold: how long the pin keeps gliding home after the pitch read dies.
	// 150 ms, independent of frame rate.
	constexpr double BEND_PIN_HOLD_SECONDS = 0.15;
	if (state.soundingMidi >= bandLow && state.soundingMidi <= bandHigh
		&& state.soundingQuality >= 20.0f)
	{
		if (displaySounding < 0.0f)
		{
			// SEED at the base and let the spring climb from there. A bend starts
			// at the base, so the pin must start at the bottom and rise with it; seeding at the
			// first read would teleport it wherever a late-registering source first appears.
			displaySounding = state.soundingMidi < static_cast<float>(base)
				? state.soundingMidi
				: static_cast<float>(base);
			displayVelocity = 0.0f;
		}
		springTo(state.soundingMidi);
		staleSeconds = 0.0;
		haveNeedle = true;
	}
	// Signal gone/weak: glide the pin back toward base (the "back to zero" release) on the same
	// spring so the return feels consistent, instead of freezing at the high point. It keeps
	// drawing for a short wall-clock window while it glides home, then clears below.
	else if (displaySounding >= 0.0f
		&& (staleSeconds += frameDelta) < BEND_PIN_HOLD_SECONDS)
	{
		springTo(static_cast<float>(base));
		haveNeedle = true;
	}
	else
	{
		// Signal gone: drop the pin AND clear the held value so the next bend re-seeds at the
		// base instead of resuming from the previous bend's top.
		displaySounding = -1.0f;
		displayVelocity = 0.0f;
		staleSeconds = 0.0;
	}
	
	const float frac = depth > 0.0f
		? (displaySounding - static_cast<float>(base)) / depth
		: 0.0f;
	// Green is the ENGINE's verdict, not a pitch re-derivation. The probe publishes
	// bendReachedTarget: 1 only where its own bend evaluation counted the pitch (the tracker
	// band read, the held raw-detector streak, the native sounding table, or the ML confirm,
	// all after their vetoes), which is the same verdict that advances the note. Green
	// therefore means progress by construction: white while climbing, green only when it
	// counts. A flash too short to accept legitimately never greens.
	const bool onTarget = haveNeedle && state.bendReachedTarget != 0;
	
	constexpr int ROWS = 12;
	constexpr int TARGET_ROW = 2;            // rows above the target are overbend headroom
	const int baseRow = ROWS - 1;
	const int travel = baseRow - TARGET_ROW; // rows from base up to target

	const int rowHeight = std::max(16, static_cast<int>(WindowSize.height / 40.0f));
	const int left = static_cast<int>(WindowSize.width * 0.68f);
	const int right = static_cast<int>(WindowSize.width * 0.90f);
	const int railTop = static_cast<int>(WindowSize.height * 0.28f);
	const int goldText = 0xFFFFD24C;
	const int greenText = 0xFF66FF66;

	int top = railTop;
	for (int r = 0; r < ROWS; ++r)
	{
		const bool isTargetRow = (r == TARGET_ROW);
		const bool isBaseRow = (r == baseRow);
		std::ostringstream row;
		row << (isTargetRow || isBaseRow ? "==" : " |");
		if (isTargetRow) row << " " << noteName(target) << " target";
		else if (isBaseRow) row << " " << noteName(base) << " base";
		DX9DrawText(row.str(), isTargetRow ? goldText : greyText, left, top, right, top + rowHeight,
			pDevice, { NULL, NULL }, DT_LEFT | DT_NOCLIP);
		top += rowHeight;
	}

	// The needle is drawn at a CONTINUOUS pixel height beside the rail, not appended to whichever
	// text rung the pitch rounded to, so a smooth pitch does not hop rung to rung. It sits left of
	// the rail pointing in, so it never collides with the rung labels.
	if (haveNeedle)
	{
		float rowPosition = static_cast<float>(baseRow) - frac * static_cast<float>(travel);
		if (rowPosition < 0.0f) rowPosition = 0.0f;
		if (rowPosition > static_cast<float>(baseRow)) rowPosition = static_cast<float>(baseRow);
		const int needleTop = railTop + static_cast<int>(std::lround(rowPosition * static_cast<float>(rowHeight)));
		DX9DrawText("==>", onTarget ? greenText : whiteText, left - rowHeight * 2, needleTop, left,
			needleTop + rowHeight, pDevice, { NULL, NULL }, DT_RIGHT | DT_NOCLIP);
	}
}

void GameOverlay::DisplayCurrentTuningForAutoTune()
{
	if (Settings::ReturnSettingValue("AutoTuneForSong") == "on" && Settings::GetKeyBind("TuningOffsetKey") != NULL && GameState::Menus::IsInTuningMenus()) {
		DX9DrawText(
			"Auto Tune For: " + Midi::GetTuningOffsetName(Midi::tuningOffset),
			whiteText,
			static_cast<int>(WindowSize.width / 5.5),		// 349 pixels left of the center in 1920x1080 resolution
			static_cast<int>(WindowSize.height / 30.85),	// 35 pixels from the top
			static_cast<int>(WindowSize.width / 5.65),		// 339 pixels right of center
			static_cast<int>(WindowSize.height / 8),		// 135 pixels from the top
			pDevice);
	}
}

void GameOverlay::DisplayLoopStartEndTimes(float loopStart, float loopEnd)
{
	DX9DrawText(
		"Loop: " + D3DHooks::ConvertFloatTimeToStringTime(loopStart) + " - " + D3DHooks::ConvertFloatTimeToStringTime(loopEnd),
		whiteText,
		static_cast<int>(WindowSize.width / 2.0f - WindowSize.width / 38.4f), // 50 pixels left of center in 1920x1080 resolution
		static_cast<int>(WindowSize.height / 21.6f),                          // 50 pixels from top
		static_cast<int>(WindowSize.width / 2.0f + WindowSize.width / 38.4f), // 50 pixels right of center
		static_cast<int>(WindowSize.height / 7.2f),                           // 150 pixels from top
		pDevice,
		{ NULL, NULL },
		DT_CENTER | DT_NOCLIP);
}

void HandleLooping() {
	if (Settings::ReturnSettingValue("AllowLooping") == "on" && (Keybindings::loopStart != NULL || Keybindings::loopEnd != NULL)) {
		// Only enable looping in learn a song modes (learn a song & non-stop play)
		if (GameState::Menus::IsInLearnASongModes()) {
			GameOverlay::DisplayLoopStartEndTimes(Keybindings::loopStart, Keybindings::loopEnd);

			// Prevent the user from creating a loop that starts at a negative timestamp.
			if ((Settings::GetModSetting("LoopingLeadUp") / 1000.f) >= Keybindings::loopStart) {
				Keybindings::roughLoopStart = 0.f;
			}
			else {
				Keybindings::roughLoopStart = Keybindings::loopStart - (Settings::GetModSetting("LoopingLeadUp") / 1000.f);
			}

			// If we are paused, reset the grey note timer.
			if (GameState::Menus::IsInLearnASongPauseModes()) {
				// Resets grey note timer to loopStart. This makes it so notes in the loop are not deactivated.
				// Deactivated notes are greyed out, and do not register with note detection.
				// As an added bonus the game also automatically adds a bit of lead time so the player has some time to prepare.
				if (SongTimer::GetGreyNoteTimer() != Keybindings::loopStart) {
					SongTimer::SetGreyNoteTimer(Keybindings::loopStart);
				}
			}

			// If not paused AND we are at the end of the loop, seek to the start of the loop.
			else if (Keybindings::loopStart != NULL && Keybindings::loopEnd != NULL && (SongTimer::SongTimer() >= Keybindings::loopEnd)) {
				Wwise::SoundEngine::SeekOnEvent(std::string("Play_" + GameState::GetSongKey()).c_str(), 0x1234, (AkTimeMs)(Keybindings::roughLoopStart * 1000), false);
			}
		}
		// Difference between learnASongModes & fastRRModes is the inclusion of RR. This means that this check is only gets the RR menus.
		else if (GameState::Menus::IsInModesWithAllowedFastRiffRepeater()) {
			// Reset loopStart and loopEnd to NULL as the user wants to do a loop with RR, or is changing some settings.
			Keybindings::loopStart = NULL;
			Keybindings::loopEnd = NULL;
		}
	}

}

static int MeasureLineHeight(ID3DXFont* font, const std::string& text, const RECT& rect, DWORD fmt) {
	RECT r = rect;
	int h = font->DrawTextA(nullptr, text.c_str(), -1, &r, fmt | DT_CALCRECT, 0);
	if (h <= 0) h = (r.bottom - r.top);
	return h;
}

static float ReadAccuracy() {
	const bool isLAS = GameState::Menus::IsInLearnASongModes();
	const bool isSA = GameState::Menus::IsInScoreAttackModes();

	uintptr_t addr = 0;
	if (isLAS) {
		addr = MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_noteData,
			Offsets::ptr_noteDataOffsets);
	}
	else if (isSA) {
		addr = MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_scoreAttackNoteData,
			Offsets::ptr_scoreAttackNoteDataOffsets);
	}
	else {
		return 0.0f;
	}

	if (!addr) return 0.0f;

	if (isLAS) {
		const LearnASongNoteData* data = reinterpret_cast<LearnASongNoteData*>(addr);

		return data->getAccuracy();
	}
	else if (isSA) {
		const ScoreAttackNoteData* data = reinterpret_cast<ScoreAttackNoteData*>(addr);
		return data->getAccuracy();
	}

	return 0.0f;
}

void GameOverlay::DisplaySongAccuracy() {
	if (Settings::ReturnSettingValue("DisplayCurrentAccuracy") == "on" &&
		GameState::IsInSong() && SongTimer::SongTimer() != 0.f) {
		auto left = static_cast<int>(WindowSize.width - WindowSize.width / 16.0f);
		auto right = static_cast<int>(WindowSize.width - WindowSize.width / 96.0f);
		auto top = static_cast<int>(WindowSize.height / 54.0f);
		auto bottom = static_cast<int>(WindowSize.height / 16.0f);
		RECT baseRect{ left, top, right, bottom };

		float accuracy = ReadAccuracy();
		std::stringstream ss;
		ss << std::fixed << std::setprecision(2) << accuracy << "%";
		std::string accuracyText = ss.str();

		if (cachedFont) {
			int lh = MeasureLineHeight(cachedFont, accuracyText, baseRect, DT_RIGHT | DT_NOCLIP);
			int gap = (std::max)(1, lh / 4);
			top += lh + 2 * gap;
			bottom += lh + 2 * gap;
		}
		else { //JIC
			auto line = static_cast<int>(WindowSize.height / 54.0f);
			top += line;
			bottom += line;
		}

		DX9DrawText(
			accuracyText,
			whiteText,
			left, top, right, bottom,
			pDevice,
			{ NULL, NULL },
			DT_RIGHT | DT_NOCLIP);
	}
}

void GameOverlay::DisplayRecordingIndicator()
{
	// Authoritative in-process take state. Audio::SharedOutput::IsRecording() unifies both recording
	// backends (the managed output session AND the passive proxy output-tap used for wet takes), so
	// this is true for every recording the audio bridge can start, including wet proxy takes (the
	// common ASIO path).
	const bool isRecording = Audio::SharedOutput::IsRecording();

	// Log the ON/off transition once, so the debug log shows whether the overlay knew a take was
	// running independently of whether the badge drew.
	static bool loggedRecording = false;
	if (isRecording != loggedRecording)
	{
		LOG_INFO("(REC OVERLAY) recording indicator " << (isRecording ? "ON" : "off") << std::endl);
		loggedRecording = isRecording;
	}

	if (!isRecording) return;

	// The badge must sit at the true right edge of the presented image. Two traps make that non-trivial:
	//   1. GetWindowRect (WindowSize) is the OS window, which can be larger than the backbuffer.
	//   2. When the EndScene hook runs, the game frequently has a SUB-REGION viewport active (measured
	//      1920x1080 inside a 2560x1600 backbuffer). The D3D viewport clips ALL rendering to itself,
	//      DT_NOCLIP or not, so anything drawn outside it is discarded. Anchoring to the window's right
	//      edge falls outside that sub-viewport and vanishes; anchoring to the sub-viewport's right
	//      edge lands ~75% across, not at the window edge.
	// So: read the actual backbuffer (render target 0) size, widen the viewport to cover the whole
	// backbuffer for this one badge, draw against the backbuffer's right edge, then restore the game's
	// viewport so nothing else is affected.
	FullSurfaceViewport viewport(pDevice, static_cast<int>(WindowSize.width), static_cast<int>(WindowSize.height));
	const int surfaceW = viewport.width;
	const int surfaceH = viewport.height;
	const D3DVIEWPORT9& savedViewport = viewport.saved;

	// One-shot geometry log so a take's log shows window vs backbuffer vs the game's live viewport.
	static bool loggedGeometry = false;
	if (!loggedGeometry)
	{
		LOG_INFO("(REC OVERLAY) window=" << static_cast<int>(WindowSize.width) << "x" << static_cast<int>(WindowSize.height)
			<< " backbuffer=" << surfaceW << "x" << surfaceH
			<< " gameViewport=" << savedViewport.Width << "x" << savedViewport.Height << std::endl);
		loggedGeometry = true;
	}

	// Wide, single-line, right-anchored rect (no wrap, no self-clip). Shadow first, then the red fill.
	const int margin = (std::max)(8, surfaceW / 96);
	const int fontSize = (std::max)(18, surfaceH / 44);
	const int top = (std::max)(8, surfaceH / 54);
	const int left = surfaceW / 2;
	const int right = surfaceW - margin;
	const int bottom = top + fontSize * 2;
	const DWORD fmt = DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOCLIP;
	DX9DrawTextW(L"\x25CF  REC", 0xFF000000, left + 2, top + 2, right + 2, bottom + 2, pDevice, fontSize, fmt, FW_BOLD);
	DX9DrawTextW(L"\x25CF  REC", 0xFFFF2020, left, top, right, bottom, pDevice, fontSize, fmt, FW_BOLD);
}

void GameOverlay::CheckCurrentFont() {
	const std::string currentFontName = Settings::ReturnSettingValue("OnScreenFont");
	const int currentFontSize = Settings::GetModSetting("OnScreenFontSize");

	if (cachedFontName != currentFontName || cachedFontSize != currentFontSize || !cachedFont) {
		LOG_INFO("Font settings changed. Re-caching default font..." << std::endl);

		FontKey newKey = FontKey::Make(currentFontName, currentFontSize, 0, FW_NORMAL, false);
		CComPtr<ID3DXFont> newFont;

		if (fontCache.Get(pDevice, newKey, newFont)) {
			cachedFont = newFont;
			cachedFontName = currentFontName;
			cachedFontSize = currentFontSize;
		}
		else {
			LOG_ERROR("Failed to create and cache new default font!" << std::endl);
		}
	}
}

void GameOverlay::RenderOverlay(IDirect3DDevice9* device) {
	// Draw text on screen
	// NOTE: NEVER USE SET VALUES. Always do division of WindowSize width AND heigh so every resolution should have the text in around the same spot.
	WindowSize = GetWindowSize();
	pDevice = device;

	CheckCurrentFont();

	DisplayRecordingIndicator();
	if (!GameState::GameLoaded) return;

	DisplayProductVersion();
	DisplayMixer();
	DisplaySongTimer();
	DisplayRiffRepeaterOverHundredPercentSpeed();
	DisplayNoteByNoteStatus();
	DisplayAudioDiagnostics();
	DisplayCurrentNote();
	DisplayCurrentTuningForAutoTune();
	static DropPedal::Overlay dropPedalOverlay;
	dropPedalOverlay.Render(cachedFont, WindowSize);
	DisplaySongAccuracy();

	HandleLooping();
}
