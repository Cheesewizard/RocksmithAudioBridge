#include "TablaturePreviewWindow.hpp"

#include "NoteByNoteHud.hpp"
#include "TablatureRenderer.hpp"
#include "../NoteByNoteSettings.hpp"
#include "../Host/ControllerBridge.hpp"
#include "../Host/NoteByNoteMode.hpp"
#include "../../Core/PitchNames.hpp"
#include "../../Shell/OverlayUi.hpp"
#include "../../Shell/ShellState.hpp"

#include "Framework/InputCapture.hpp"
#include "Log.hpp"

#include <cfloat>

namespace NoteByNote::Ui
{
	namespace
	{
		Tablature::Snapshot snapshot;
		std::vector<ResearchProtocol::PreviewEvent> previewEvents;
		// Defaults to the player's own level; the compare button flips to 100%.
		uint8_t requestedLevelMode = ResearchProtocol::TAB_PREVIEW_LEVEL_CURRENT;
		// Whether the window was drawn open (not collapsed) last frame. Sent on every
		// copy: while set, Note by Note holds every note. Never latched, and the
		// controller also lets it lapse when the window stops drawing.
		bool tabExpanded = false;

		void RefreshPreview()
		{
			ResearchProtocol::PreviewState state{};
			state.levelMode = requestedLevelMode;
			state.readerHold = tabExpanded ? 1 : 0;
			if (!ResearchBridge::CopyTablaturePreview(nullptr, 0, state))
			{
				snapshot = {};
				snapshot.error = "Waiting for the song's tablature.";
				return;
			}
			if (!state.available)
			{
				snapshot = {};
				snapshot.error = state.error;
				return;
			}
			if ((state.stringCount != 4 && state.stringCount != 6) || state.eventCount > 100000)
			{
				snapshot = {};
				snapshot.error = "The song supplied invalid tablature data.";
				LOG_ERROR("(NBN TAB) Invalid preview dimensions: " << state.stringCount << " strings, " << state.eventCount << " events." << std::endl);
				return;
			}
			if (snapshot.generation != state.generation || snapshot.events.size() != state.eventCount)
			{
				previewEvents.resize(state.eventCount);
				state.levelMode = requestedLevelMode;
				state.readerHold = tabExpanded ? 1 : 0;
				if (!ResearchBridge::CopyTablaturePreview(previewEvents.data(), static_cast<uint32_t>(previewEvents.size()), state)
					|| !state.available || state.eventCount > previewEvents.size())
				{
					snapshot = {};
					snapshot.error = "The song changed. Waiting for its tablature.";
					return;
				}
				snapshot.events.clear();
				snapshot.events.reserve(state.eventCount);
				for (uint32_t index = 0; index < state.eventCount; ++index)
				{
					const auto& source = previewEvents[index];
					if (source.positionCount > 6 || !std::isfinite(source.time))
					{
						snapshot = {};
						snapshot.error = "The song supplied an invalid tab event.";
						LOG_ERROR("(NBN TAB) Invalid event at index " << index << "." << std::endl);
						return;
					}
					Tablature::Event event;
					event.id = source.id;
					event.time = source.time;
					event.chordName.assign(source.chordName, strnlen(source.chordName, sizeof(source.chordName)));
					for (uint32_t position = 0; position < source.positionCount; ++position)
					{
						const auto& note = source.positions[position];
						Tablature::Position entry;
						entry.stringIndex = note.stringIndex;
						entry.fret = note.fret;
						entry.techniques = note.techniques;
						entry.bendSemitones = note.bendSemitones;
						entry.slideTargetFret = note.slideTargetFret;
						entry.sustain = std::isfinite(note.sustain) ? note.sustain : 0.0f;
						entry.bendStart = std::isfinite(note.bendStart) ? note.bendStart : 0.0f;
						entry.bendPeak = std::isfinite(note.bendPeak) ? note.bendPeak : 0.0f;
						entry.bendEnd = std::isfinite(note.bendEnd) ? note.bendEnd : 0.0f;
						event.positions.push_back(entry);
					}
					snapshot.events.push_back(std::move(event));
				}
				snapshot.generation = state.generation;
			}
			snapshot.currentTargetId = state.currentTargetId;
			snapshot.readerHolding = state.readerHolding != 0;
			snapshot.arrangementKey = state.arrangementKey;
			snapshot.showsCurrentLevels = state.levelMode == ResearchProtocol::TAB_PREVIEW_LEVEL_CURRENT;
			snapshot.currentLevelsRead = state.currentLevelsRead != 0;
			snapshot.iterationsBelowMax = state.iterationsBelowMax;
			snapshot.loopValid = state.loopValid != 0 && std::isfinite(state.loopStart) && std::isfinite(state.loopEnd)
				&& state.loopEnd > state.loopStart;
			snapshot.loopStart = state.loopStart;
			snapshot.loopEnd = state.loopEnd;
			snapshot.sectionStarts.assign(state.sectionStarts,
				state.sectionStarts + (std::min)(state.sectionCount, ResearchProtocol::PreviewState::MaxSections));
			snapshot.sectionNames.resize(snapshot.sectionStarts.size());
			for (std::size_t index = 0; index < snapshot.sectionNames.size(); ++index)
				snapshot.sectionNames[index].assign(state.sectionNames[index], strnlen(state.sectionNames[index], sizeof(state.sectionNames[index])));
			{
				const uint32_t bars = (std::min)(state.barCount, ResearchProtocol::PreviewState::MaxBars);
				snapshot.bars.resize(bars);
				for (uint32_t index = 0; index < bars; ++index) snapshot.bars[index] = { state.barStarts[index], state.barNumbers[index] };
			}
			snapshot.stringCount = static_cast<int>(state.stringCount);
			snapshot.error.clear();
			static const int standardMidi[] = { 40, 45, 50, 55, 59, 64 };
			for (int stringIndex = 0; stringIndex < snapshot.stringCount; ++stringIndex)
			{
				int openMidi = standardMidi[stringIndex] + state.tuningOffsets[stringIndex];
				int physicalOpenMidi = -1;
				if (Hud::TryGetPhysicalOpenStringMidi(stringIndex, physicalOpenMidi)) openMidi = physicalOpenMidi;
				snapshot.tuningLabels[stringIndex] = PitchNames::ForPitchClass(openMidi);
				if (snapshot.stringCount == 6 && stringIndex == 5 && !snapshot.tuningLabels[stringIndex].empty())
					snapshot.tuningLabels[stringIndex][0] = static_cast<char>(std::tolower(static_cast<unsigned char>(snapshot.tuningLabels[stringIndex][0])));
			}
		}
	}

	// 4.x read GameState::Menus::IsInSongModes() && !IsInRiffRepeaterMenus() here; the render thread reads the menu
	// state Note by Note publishes on MainThread instead.
	bool IsTablaturePreviewVisible()
	{
		return ModSettings::Get().targetStyle == ModSettings::TargetStyle::TabTimeline
			&& NoteByNoteProbe::IsAutomaticEnabledFast()
			&& NoteByNoteProbe::IsPracticingFast();
	}

	// The tab window stays up for the whole song, so unlike the menus it must
	// not keep a software cursor over the highway. The cursor shows while the
	// mouse is over an ImGui window (last frame's hover) or has moved in the
	// last two seconds, so the player can still find and click the window.
	bool TablaturePreviewWantsCursor()
	{
		static ImVec2 lastPosition(-FLT_MAX, -FLT_MAX);
		static ULONGLONG lastMovedAt = 0;
		if (!IsTablaturePreviewVisible()) return false;
		const ImGuiIO& io = ImGui::GetIO();
		const ULONGLONG now = GetTickCount64();
		if (io.MousePos.x != lastPosition.x || io.MousePos.y != lastPosition.y)
		{
			lastPosition = io.MousePos;
			lastMovedAt = now;
		}
		return io.WantCaptureMouse || now - lastMovedAt < 2000;
	}

	void DrawTablaturePreview()
	{
		if (!IsTablaturePreviewVisible())
		{
			snapshot = {};
			tabExpanded = false;
			bool closed = false;
			Tablature::Draw(snapshot, 1.0f, &closed);
			return;
		}
		RefreshPreview();
		bool open = true;
		// 4.x's overlay theme was ImGui's global style; here it is scoped (OverlayUi.hpp), so the tab puts it on itself
		// and upstream's own windows keep theirs.
		const Overlay::ThemeScope theme;
		auto* font = Overlay::GetFonts().body;
		if (font != nullptr) ImGui::PushFont(font);
		bool toggleLevels = false;
		bool toggleDock = false;
		const ModSettings::Snapshot settings = ModSettings::Get();
		const bool docked = settings.tabDocked;
		std::uint32_t highlight = settings.tabHighlight;
		const std::uint32_t shownHighlight = highlight;
		bool highlightSaved = false;
		Tablature::Draw(snapshot, settings.targetSize / 100.0f, &open, &toggleLevels, docked, &toggleDock, &tabExpanded,
			&highlight, &highlightSaved);
		if (toggleDock) ModSettings::SetTabDocked(!docked);
		if (highlight != shownHighlight || highlightSaved) ModSettings::SetTabHighlight(highlight, highlightSaved);
		if (font != nullptr) ImGui::PopFont();
		if (toggleLevels)
			requestedLevelMode = snapshot.showsCurrentLevels
				? ResearchProtocol::TAB_PREVIEW_LEVEL_MAXIMUM
				: ResearchProtocol::TAB_PREVIEW_LEVEL_CURRENT;
		if (!open) tabExpanded = false;
		if (!open) ModSettings::SetTargetStyle(ModSettings::TargetStyle::Tab);
	}

	// 4.x (dllmain.cpp's WndProc and EndScene) fed ImGui the game window's input, and kept the game's DirectInput mouse
	// away while ImGui wanted it, on every frame the menu, the overlay or this tab showed; the cursor followed
	// TablaturePreviewWantsCursor (Menu.cpp). Upstream does the same for a frame an overlay claims input in.
	void DrawOverlay()
	{
		if (IsTablaturePreviewVisible()) Framework::Input().ClaimOverlayInput();
		AudioBridge::Shell::VoteMouseCursor(TablaturePreviewWantsCursor());
		DrawTablaturePreview();
	}
}
