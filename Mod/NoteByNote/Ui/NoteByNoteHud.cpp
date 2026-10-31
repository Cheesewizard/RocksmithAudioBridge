#include "NoteByNoteHud.hpp"

#include "HighwaySeam.hpp"
#include "NoteByNoteHudLayout.hpp"
#include "../NoteByNoteSettings.hpp"
#include "../Controller/NoteByNoteNativeScoring.hpp"
#include "../Detection/DetectionFeedback.hpp"
#include "../Host/ControllerBridge.hpp"
#include "../Host/NoteByNoteMode.hpp"
#include "../Ml/MlStringFretReader.hpp"
#include "../../Core/PitchNames.hpp"
#include "../../DropPedal/DropPedalShift.hpp"
#include "../../DropPedal/DropPedalState.hpp"
#include "../../DropPedal/SpeakerMode/SpeakerTargetSync.hpp"
#include "../../Shell/HudDraw.hpp"

#include "GameState.hpp"
#include "Log.hpp"
#include "Offsets.hpp"
#include "Settings.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

using AudioBridge::HudDraw::Argb;
using NoteByNote::ModSettings::HudBlock;
using NoteByNote::ModSettings::TargetPosition;
using NoteByNote::ModSettings::TargetStyle;

namespace NoteByNote::Hud
{
	namespace
	{
		// ---- Published game state (PublishGameState, MainThread) -------------------------------------------------

		std::atomic<int> publishedOpenMidi[6] = { -1, -1, -1, -1, -1, -1 };   // -1 = not readable
		std::atomic<unsigned int> stringColors[6] = {};                        // opaque ARGB; 0 = not readable

		// The game's string colour record (upstream's RSColor), three floats 0..1.
		struct StringColor { float r = 0.0f, g = 0.0f, b = 0.0f; };

		// The highway's "enabled" string colours (4.x's StringState Enabled, GuitarStringsEnabledColor).
		constexpr uintptr_t STRING_COLOR_ENABLED = 0x368;

		// 4.x's ERMode GetStringColor walk, guarded: the colour table moves with the game's colour-blind mode.
		uintptr_t StringColorAddress(uintptr_t stringIndex)
		{
			const uintptr_t pointerValue = *reinterpret_cast<const uintptr_t*>(static_cast<uintptr_t>(Offsets::ptr_stringColor));
			if (!pointerValue) return 0;
			uintptr_t table = *reinterpret_cast<const uintptr_t*>(pointerValue + 0x348);
			if (table >= 2) return 0;
			table = table * 0xA8 + stringIndex;
			return *reinterpret_cast<const uintptr_t*>(pointerValue + table * 0x4 + STRING_COLOR_ENABLED);
		}

		bool ReadStringColor(int stringIndex, StringColor& color)
		{
			__try
			{
				const uintptr_t address = StringColorAddress(static_cast<uintptr_t>(stringIndex));
				if (address == 0) return false;
				color = *reinterpret_cast<const StringColor*>(address);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
			return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b)
				&& color.r >= 0.0f && color.r <= 1.0f
				&& color.g >= 0.0f && color.g <= 1.0f
				&& color.b >= 0.0f && color.b <= 1.0f;
		}

		// ---- Drawing helpers ----------------------------------------------------------------------------------

		ImDrawList* List() { return ImGui::GetBackgroundDrawList(); }

		// 4.x's DX9DrawText(text, colour, left, top, right, bottom, font) with DT_LEFT / DT_CENTER / DT_RIGHT.
		enum class Align { Left, Center, Right };
		void DrawHudText(const std::string& text, std::uint32_t argb, float left, float top, float right, float height,
			Align align = Align::Left)
		{
			float x = left;
			if (align != Align::Left)
			{
				const float width = AudioBridge::HudDraw::Measure(height, text.c_str()).x;
				x = align == Align::Right ? right - width : (left + right - width) * 0.5f;
			}
			AudioBridge::HudDraw::Text(List(), height, ImVec2(x, top), Argb(argb), text.c_str());
		}

		ImVec2 MeasureHudText(const std::string& text, float height)
		{
			if (text.empty()) return ImVec2(0.0f, height);
			return AudioBridge::HudDraw::Measure(height, text.c_str());
		}

		// The default HUD font size (RSMods.ini OnScreenFontSize), 4.x's cachedFont, which the bend meter's rows used.
		float DefaultFontHeight()
		{
			const int size = Settings::GetModSetting(Settings::Setting::OnScreenFontSize);
			return static_cast<float>(size > 0 ? size : 18);
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

		// ---- Finger numerals (4.x's DisplayNoteByNoteStatus) --------------------------------------------------

		void DrawFingerNumerals(float windowWidth, float windowHeight)
		{
			// Shipped in every configuration (it was Debug-only): the frozen-chord fix hides finger rings
			// stranded off the neck, and this is what puts that member's number back.
			// Host-drawn finger numerals for chord holds: the game's own numeral glyphs
			// are screen-space quads laid out once and never repainted on frozen
			// retargets, so during chord holds the overlay paints the SNG template's
			// fingers itself, at screen anchors projected from the kept members' own
			// marker draws this frame. Repainted every frame, so repeated asks show the
			// chord again rather than blanking it.
			// 4.x drew these only while the game's back buffer was bound and widened the game's viewport for them (D3DX
			// text drawn into the offscreen 3D pass, or outside a sub-region viewport, was lost). ImGui draws once per
			// frame onto the back buffer with its own full-screen viewport, so neither applies.
			const bool isNumeralHostEnabled = NoteByNoteProbe::IsAutomaticEnabledFast();
			const bool numeralGatesOpen = isNumeralHostEnabled
				&& NoteByNote::HighwaySeam::AreFingerNumeralsEnabled()
				&& NoteByNote::HighwaySeam::IsChordHoldFrozen();
			int numeralsDrawn = 0;
			int heartbeatAnchorCount = -1;
			int numeralsWithoutFinger = 0;
			int numeralsLeftToGame = 0;
			// Placements kept in a fixed array; the text is only built when the trace line logs.
			int placementString[8] = {};
			int placementFret[8] = {};
			int placementFinger[8] = {};
			int placementX[8] = {};
			int placementY[8] = {};
			if (numeralGatesOpen)
			{
				int anchorStrings[8];
				int anchorFrets[8];
				float anchorX[8];
				float anchorY[8];
				const int anchorCount = NoteByNote::HighwaySeam::GetFreshFingerAnchors(
					anchorStrings, anchorFrets, anchorX, anchorY);
				heartbeatAnchorCount = anchorCount;
				const int numeralHeight = (std::max)(18, static_cast<int>(windowHeight / 40.0f));
				constexpr std::uint32_t whiteText = 0xFFFFFFFF;
				constexpr std::uint32_t shadowText = 0xC0000000;
				for (int index = 0; index < anchorCount; ++index)
				{
					int finger = 0;
					if (!NoteByNote::HighwaySeam::TryGetChordFingerForCoordinate(
						anchorStrings[index], anchorFrets[index], finger))
					{
						++numeralsWithoutFinger;
						continue;
					}
					// One number per box: where the game drew its own numeral this frame it already shows it.
					if (NoteByNote::HighwaySeam::HasRestingFingerRing(anchorStrings[index], anchorFrets[index]))
					{
						++numeralsLeftToGame;
						continue;
					}
					const std::string digit(1, static_cast<char>('0' + finger));
					// Anchors are [0..1] screen fractions (the 3D viewport is smaller than
					// the window on high-res rigs); scale into the overlay's window space.
					const int centreX = static_cast<int>(anchorX[index] * windowWidth);
					// The panel anchor projects ~35 px below the box at 2560x1600 (live log vs screenshot),
					// so the digit is lifted by the same share of the window height.
					const int centreY = static_cast<int>(anchorY[index] * windowHeight - windowHeight * 0.022f);
					if (numeralsDrawn < 8)
					{
						placementString[numeralsDrawn] = anchorStrings[index];
						placementFret[numeralsDrawn] = anchorFrets[index];
						placementFinger[numeralsDrawn] = finger;
						placementX[numeralsDrawn] = centreX;
						placementY[numeralsDrawn] = centreY;
					}
					// Centred on the anchor (4.x's DT_CENTER | DT_VCENTER box). A one-pixel shadow first, so the digit
					// reads on the bright marker art.
					const float height = static_cast<float>(numeralHeight);
					const ImVec2 size = AudioBridge::HudDraw::Measure(height, digit.c_str());
					const ImVec2 at(centreX - size.x * 0.5f, centreY - size.y * 0.5f);
					AudioBridge::HudDraw::Text(List(), height, ImVec2(at.x + 1.0f, at.y + 1.0f), Argb(shadowText), digit.c_str());
					AudioBridge::HudDraw::Text(List(), height, at, Argb(whiteText), digit.c_str());
					++numeralsDrawn;
				}
			}
			// Release-visible trace of the host numeral path, logged only when its outcome changes, so a
			// frozen chord that shows a box without a number says which gate held the digit back.
			{
				static int lastTrace[6] = { -2, -2, -2, -2, -2, -2 };
				const int trace[6] = {
					isNumeralHostEnabled ? 1 : 0,
					NoteByNote::HighwaySeam::IsChordHoldFrozen() ? 1 : 0,
					heartbeatAnchorCount, numeralsWithoutFinger, numeralsLeftToGame, numeralsDrawn };
				if (std::memcmp(trace, lastTrace, sizeof(trace)) != 0)
				{
					std::memcpy(lastTrace, trace, sizeof(trace));
					std::ostringstream numeralPlacements;
					for (int placed = 0; placed < numeralsDrawn && placed < 8; ++placed)
					{
						numeralPlacements << " " << placementString[placed] << ":" << placementFret[placed] << "="
							<< placementFinger[placed] << "@" << placementX[placed] << "," << placementY[placed];
					}
					LOG_INFO("(NBN NUMERALS) nbn=" << trace[0] << " chordHold=" << trace[1]
						<< " toggle=" << NoteByNote::HighwaySeam::AreFingerNumeralsEnabled()
						<< " anchorsFresh=" << trace[2] << " noFinger=" << trace[3]
						<< " gameNumeral=" << trace[4] << " drawn=" << trace[5]
						<< " window=" << windowWidth << "x" << windowHeight
						<< " at" << numeralPlacements.str() << std::endl);
				}
			}
		}

		// ---- Bend meter (4.x's DisplayNoteByNoteBendMeter) ----------------------------------------------------

		// The bend visualizer: a tuner-style ladder at the screen's
		// right edge while a bend gesture is the target. Five rungs - the bend target in
		// the middle context of two semitones above and below - and the sounding pitch
		// marked on its rung, so the player watches their bend climb into the target like
		// a tuner needle. Data comes from the research probe's state feed: fractional
		// pitch when one of Rocksmith's continuous trackers is live, the ungated integer
		// detector pitch otherwise.
		void DrawBendMeter(float windowWidth, float windowHeight)
		{
			// The bend visualizer is a FEATURE and draws in every configuration.
			if (!NoteByNoteProbe::IsAutomaticEnabledFast()) return;

			ResearchProtocol::NoteByNoteState state;
			if (!NoteByNoteNativeScoring::TryGetResearchState(state))
			{
				return;
			}
			// Appended-field guard: a probe older than the bend-visualizer fields reports
			// a smaller structSize and must not have these bytes interpreted.
			if (state.structSize < sizeof(ResearchProtocol::NoteByNoteState))
			{
				return;
			}
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

			const int rowHeight = (std::max)(16, static_cast<int>(windowHeight / 40.0f));
			const int left = static_cast<int>(windowWidth * 0.68f);
			const int right = static_cast<int>(windowWidth * 0.90f);
			const int railTop = static_cast<int>(windowHeight * 0.28f);
			constexpr std::uint32_t goldText = 0xFFFFD24C;
			constexpr std::uint32_t greenText = 0xFF66FF66;
			constexpr std::uint32_t greyText = 0xFF9A9A9A;
			constexpr std::uint32_t whiteText = 0xFFFFFFFF;
			const float fontHeight = DefaultFontHeight();

			int top = railTop;
			for (int r = 0; r < ROWS; ++r)
			{
				const bool isTargetRow = (r == TARGET_ROW);
				const bool isBaseRow = (r == baseRow);
				std::ostringstream row;
				row << (isTargetRow || isBaseRow ? "==" : " |");
				if (isTargetRow) row << " " << noteName(target) << " target";
				else if (isBaseRow) row << " " << noteName(base) << " base";
				DrawHudText(row.str(), isTargetRow ? goldText : greyText, static_cast<float>(left), static_cast<float>(top),
					static_cast<float>(right), fontHeight);
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
				DrawHudText("==>", onTarget ? greenText : whiteText, static_cast<float>(left - rowHeight * 2),
					static_cast<float>(needleTop), static_cast<float>(left), fontHeight, Align::Right);
			}
		}

		// ---- Readout and target (4.x's DisplayMlStringFretOverlay) ------------------------------------------

		// Paired detection readout: Rocksmith's OWN pitch read (line 1) with the FretNet ML
		// companion's read of the same audio directly under it (line 2). The game frequently
		// hears the correct pitch (line 1) but its quality-50 accept gate refuses a high/thin
		// note, while ML (line 2) confirms it. The ML line says "(service offline)" when the
		// companion is absent rather than vanishing.
		//
		// Shadow only, never a gameplay decision. The target remains visible whenever automatic Note-by-Note is active;
		// the diagnostic rows are gated by the detection-visibility setting.
		void DrawTargetAndReadout(float windowWidth, float windowHeight)
		{
			if (!NoteByNoteProbe::IsAutomaticEnabledFast()) return;
			// 4.x: GameState::Menus::IsInSongModes() && !IsInRiffRepeaterMenus(), published on MainThread.
			if (!NoteByNoteProbe::IsPracticingFast()) return;

			const ModSettings::Snapshot settings = ModSettings::Get();
			ResearchProtocol::NoteByNoteState state;
			const bool haveState = NoteByNoteNativeScoring::TryGetResearchState(state) && state.isInitialized;
			const int targetMidi = haveState ? state.expectedMidi : -1;
			const int targetString = haveState && state.selectedChordId == -1 ? state.selectedString : -1;
			int targetColorString = targetString;
			const int targetFret = haveState ? state.selectedFret : -1;
			const bool showDiagnostics = settings.detectionVisible;

			const TargetStyle targetStyle = settings.targetStyle;
			const std::string bendWords = BendWords(state.isBendTarget && state.bendAcceptMidi >= 0 && targetMidi >= 0
				? state.bendAcceptMidi - targetMidi : 0);
			// Frets per string (index 0 = low E, -1 = not played) for the Tab style.
			int tabFrets[6] = { -1, -1, -1, -1, -1, -1 };
			if (targetString >= 0 && targetString < 6 && targetFret >= 0) tabFrets[targetString] = targetFret;

			std::string targetText = NoteByNote::FormatPitch(targetMidi);
			if (targetString >= 0 && targetFret >= 0 && targetStyle != TargetStyle::Detailed)
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
				if (TryGetPhysicalOpenStringMidi(targetString, physicalOpenMidi))
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
			// window, see ModSettings::Placement). The target's dragged placement applies only in the Custom position and
			// is remembered while Left or Center is chosen. The one remaining link is deliberate: the "Left" (Under
			// readout) target follows the readout's bottom edge; Custom decouples it entirely.
			HudLayout::frameWidth = windowWidth;
			HudLayout::frameHeight = windowHeight;
			const ModSettings::Placement readoutPlacement = settings.readoutPlacement;
			const ModSettings::Placement targetPlacement = settings.targetPlacement;
			const int baseX = readoutPlacement.IsSet()
				? static_cast<int>(readoutPlacement.x * windowWidth)
				: static_cast<int>(windowWidth / 96.0f);
			const int baseY = readoutPlacement.IsSet()
				? static_cast<int>(readoutPlacement.y * windowHeight)
				: static_cast<int>(windowHeight / 5.2f);
			const TargetPosition targetPosition = settings.targetPosition;
			const bool targetIsCentred = targetPosition == TargetPosition::Center;
			// Custom with nothing dragged yet starts from the Under readout spot.
			const bool targetIsCustom = targetPosition == TargetPosition::Custom && targetPlacement.IsSet();
			const int defaultTextHeight = (std::max)(14, static_cast<int>(windowHeight / 80.0f));
			const int textHeight = (std::max)(7, defaultTextHeight * settings.uiSize / 100);
			const int targetTextHeight = (std::max)(7, defaultTextHeight * settings.targetSize / 100);
			const float textSize = static_cast<float>(textHeight);
			const float targetSize = static_cast<float>(targetTextHeight);
			// Tab style: six lines, high string on top, "E |--7--" with the frets in one aligned column. The string
			// names are drawn in each string's colour, which stands in for the swatch.
			bool useTab = false;
			for (const int fret : tabFrets) useTab |= targetStyle == TargetStyle::Tab && fret >= 0;
			const int tabLine = targetTextHeight + targetTextHeight / 6;
			const int tabLabelW = static_cast<int>(MeasureHudText("G#", targetSize).x) + targetTextHeight / 3;
			const int tabBarW = static_cast<int>(MeasureHudText("|", targetSize).x) + targetTextHeight / 8;
			const int tabDashW = static_cast<int>(MeasureHudText("--", targetSize).x);
			const int tabCellW = static_cast<int>(MeasureHudText("00", targetSize).x) + targetTextHeight / 3;
			const int tabBendW = bendWords.empty() ? 0 : static_cast<int>(MeasureHudText("  " + bendWords, targetSize).x);
			const int tabWidth = tabLabelW + tabBarW + tabDashW * 2 + tabCellW + tabBendW;
			const int tabHeight = tabLine * 6;
			// Line spacing only stretches the gap between readout lines; 100% is two text heights.
			const int rowHeight = (std::max)(textHeight, textHeight * 2 * settings.lineSpacing / 100);
			const int valueX = baseX + textHeight * 7;
			const int readoutBottom = baseY + rowHeight * 2 + textHeight;
			const float rightEdge = windowWidth;
			// The target is a fixed-top box: its top-left is anchored and never moves with the target size, and the
			// text is top-aligned, so a bigger target only grows right and downward. Built-in "Left" anchor: under the
			// readout plus a lyric clearance (two original rows) so it clears the game's two-line lyric band.
			int targetValueX, targetRowTop;
			if (targetIsCustom)
			{
				targetValueX = static_cast<int>(targetPlacement.x * windowWidth);
				targetRowTop = static_cast<int>(targetPlacement.y * windowHeight);
			}
			else if (targetIsCentred)
			{
				// Centre the whole block (swatch + text) on the screen, not its left edge, so a bigger target or a longer
				// chord name stays centred instead of growing to the right.
				const bool hasSwatch = targetColorString >= 0 && targetColorString < 6;
				const int blockWidth = useTab ? tabWidth : static_cast<int>(MeasureHudText(targetText, targetSize).x)
					+ (hasSwatch ? targetTextHeight + targetTextHeight / 2 : 0);
				targetValueX = (std::max)(0, static_cast<int>(windowWidth / 2) - blockWidth / 2);
				targetRowTop = static_cast<int>(windowHeight * 0.20f);
			}
			else
			{
				targetValueX = baseX;
				targetRowTop = readoutBottom + textHeight + textHeight * 4;
			}
			const int targetLeft = targetValueX;
			if (targetStyle != TargetStyle::TabTimeline && !useTab && targetColorString >= 0 && targetColorString < 6)
			{
				unsigned int stringColor = 0;
				static bool reportedMissingStringColor = false;
				if (TryGetStringColor(targetColorString, stringColor))
				{
					reportedMissingStringColor = false;
					// Part of the target block, so it scales with the TARGET size, not the readout size. 4.x drew the
					// U+25A0 square glyph, which the baked HUD font doesn't carry: a filled square of its size.
					const float side = targetSize * 0.62f;
					const ImVec2 corner(targetValueX + targetSize * 0.08f, targetRowTop + targetSize * 0.22f);
					List()->AddRectFilled(corner, ImVec2(corner.x + side, corner.y + side), Argb(stringColor));
					targetValueX += targetTextHeight + targetTextHeight / 2;
				}
				else if (!reportedMissingStringColor)
				{
					LOG_ERROR("(NBN HUD) Active string colour unavailable; target swatch omitted." << std::endl);
					reportedMissingStringColor = true;
				}
			}
			const DetectionPalette palette = ModSettings::Palette(settings);
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
				const int nativeFrameOffset = DropPedalState::GetPitchMode() == ::DropPedal::PitchMode::SpeakerMode
					? -DropPedalState::GetTargetSemitones(::DropPedal::Player::One) : 0;
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
					const int appliedShift = AudioBridge::DropPedal::Shift::AppliedInputShiftSemitones();
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
				float widestValue = static_cast<float>(textHeight * 4);
				for (int row = 0; row < 3; ++row)
				{
					const int y = baseY + row * rowHeight;
					DrawHudText(labels[row], colors[row], static_cast<float>(baseX), static_cast<float>(y), static_cast<float>(valueX), textSize);
					DrawHudText(values[row], colors[row], static_cast<float>(valueX), static_cast<float>(y), rightEdge, textSize);
					widestValue = (std::max)(widestValue, MeasureHudText(values[row], textSize).x);
				}
				HudLayout::Publish(HudBlock::Readout, static_cast<float>(baseX), static_cast<float>(baseY),
					static_cast<float>(valueX) + widestValue, static_cast<float>(readoutBottom));
			}

			if (targetStyle == TargetStyle::TabTimeline) return;

			if (useTab)
			{
				constexpr std::uint32_t dim = 0xFF8A8A8A;   // staff lines and empty strings recede behind the frets
				for (int row = 0; row < 6; ++row)
				{
					const int stringIndex = 5 - row;
					const float y = static_cast<float>(targetRowTop + row * tabLine);
					// String name from the physical tuning (Drop Pedal aware), standard tuning if unavailable; the top
					// string is lower case, as tabs write it.
					static const char* const standard[] = { "E", "A", "D", "G", "B", "e" };
					std::string name = standard[stringIndex];
					int openMidi = -1;
					if (TryGetPhysicalOpenStringMidi(stringIndex, openMidi)) {
						name = PitchNames::ForPitchClass(openMidi);
						if (stringIndex == 5 && !name.empty()) name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
					}
					unsigned int stringColor = 0;
					const std::uint32_t labelColor = TryGetStringColor(stringIndex, stringColor) ? stringColor : palette.neutral;
					float x = static_cast<float>(targetLeft);
					DrawHudText(name, labelColor, x, y, x + tabLabelW, targetSize);
					x += tabLabelW;
					DrawHudText("|--", dim, x, y, x + tabBarW + tabDashW, targetSize);
					x += tabBarW + tabDashW;
					const bool played = tabFrets[stringIndex] >= 0;
					DrawHudText(played ? std::to_string(tabFrets[stringIndex]) : "--", played ? palette.neutral : dim,
						x, y, x + tabCellW, targetSize, Align::Center);
					x += tabCellW;
					DrawHudText("--", dim, x, y, x + tabDashW, targetSize);
					x += tabDashW;
					if (played && !bendWords.empty())
						DrawHudText("  " + bendWords, palette.neutral, x, y, rightEdge, targetSize);
				}
				HudLayout::Publish(HudBlock::Target, static_cast<float>(targetLeft), static_cast<float>(targetRowTop),
					static_cast<float>(targetLeft + tabWidth), static_cast<float>(targetRowTop + tabHeight));
				return;
			}

			DrawHudText(targetText, palette.neutral, static_cast<float>(targetValueX), static_cast<float>(targetRowTop),
				rightEdge, targetSize);
			const ImVec2 measured = MeasureHudText(targetText, targetSize);
			HudLayout::Publish(HudBlock::Target, static_cast<float>(targetLeft), static_cast<float>(targetRowTop),
				static_cast<float>(targetValueX) + (std::max)(measured.x, targetSize * 2.0f),
				static_cast<float>(targetRowTop) + (std::max)(measured.y, targetSize));
		}
	}

	void PublishGameState()
	{
		// 4.x's DropPedal::TryGetPhysicalOpenStringMidi, read every frame there; the chart's tuning is a MainThread read.
		static constexpr int OPEN_MIDI[] = { 40, 45, 50, 55, 59, 64 };
		int stringTunings[6];
		const bool chartKnown = AudioBridge::SpeakerMode::TryReadChartTuning(stringTunings);
		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			int midi = -1;
			if (chartKnown)
			{
				midi = OPEN_MIDI[stringIndex] + stringTunings[stringIndex];
				if (DropPedalState::GetPitchMode() != ::DropPedal::PitchMode::Off)
				{
					midi += DropPedalState::GetBaseTuningSemitones(::DropPedal::Player::One)
						- AudioBridge::SpeakerMode::GetChartReference(stringTunings);
				}
			}
			publishedOpenMidi[stringIndex].store(midi, std::memory_order_relaxed);

			StringColor color;
			stringColors[stringIndex].store(ReadStringColor(stringIndex, color)
				? static_cast<unsigned int>(D3DCOLOR_COLORVALUE(color.r, color.g, color.b, 1.0f)) : 0u, std::memory_order_relaxed);
		}
	}

	bool TryGetPhysicalOpenStringMidi(int stringIndex, int& midi)
	{
		midi = -1;
		if (stringIndex < 0 || stringIndex >= 6) return false;
		midi = publishedOpenMidi[stringIndex].load(std::memory_order_relaxed);
		return midi >= 0;
	}

	bool TryGetStringColor(int stringIndex, unsigned int& argb)
	{
		if (stringIndex < 0 || stringIndex >= 6) return false;
		argb = stringColors[stringIndex].load(std::memory_order_relaxed);
		return argb != 0;
	}

	void Draw()
	{
		// 4.x drew the HUD only once the game had loaded.
		if (!GameState::GameLoaded) return;
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		if (display.x <= 0.0f || display.y <= 0.0f) return;
		DrawFingerNumerals(display.x, display.y);
		// 4.x's order: the numerals, then the bend meter, then the readout and target.
		DrawBendMeter(display.x, display.y);
		DrawTargetAndReadout(display.x, display.y);
	}
}
