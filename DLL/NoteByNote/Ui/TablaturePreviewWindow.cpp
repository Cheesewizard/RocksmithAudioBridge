#include "stdafx.h"
#include "NoteByNote/Ui/TablaturePreviewWindow.hpp"
#include "NoteByNote/Ui/TablatureRenderer.hpp"
#include "NoteByNote/Host/ControllerBridge.hpp"
#include "NoteByNote/Host/NoteByNoteMode.hpp"
#include "Settings.hpp"
#include "GameState.hpp"
#include "DropPedal/DropPedal.hpp"
#include "Core/PitchNames.hpp"
#include "Overlay/OverlayUi.hpp"

namespace NoteByNote::Ui
{
	namespace
	{
		Tablature::Snapshot snapshot;
		std::vector<ResearchProtocol::PreviewEvent> previewEvents;
		// Defaults to the player's own level; the compare button flips to 100%.
		uint8_t requestedLevelMode = ResearchProtocol::TAB_PREVIEW_LEVEL_CURRENT;

		void RefreshPreview()
		{
			ResearchProtocol::PreviewState state{};
			state.levelMode = requestedLevelMode;
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
					for (uint32_t position = 0; position < source.positionCount; ++position)
					{
						const auto& note = source.positions[position];
						event.positions.push_back({ note.stringIndex, note.fret, note.techniques, note.bendSemitones, note.slideTargetFret });
					}
					snapshot.events.push_back(std::move(event));
				}
				snapshot.generation = state.generation;
			}
			snapshot.currentTargetId = state.currentTargetId;
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
				if (DropPedal::TryGetPhysicalOpenStringMidi(stringIndex, physicalOpenMidi)) openMidi = physicalOpenMidi;
				snapshot.tuningLabels[stringIndex] = PitchNames::ForPitchClass(openMidi);
				if (snapshot.stringCount == 6 && stringIndex == 5 && !snapshot.tuningLabels[stringIndex].empty())
					snapshot.tuningLabels[stringIndex][0] = static_cast<char>(std::tolower(static_cast<unsigned char>(snapshot.tuningLabels[stringIndex][0])));
			}
		}
	}

	bool IsTablaturePreviewVisible()
	{
		return Settings::GetNoteByNoteTargetStyle() == Settings::NoteByNoteTargetStyle::TabTimeline
			&& NoteByNoteProbe::IsAutomaticEnabledFast()
			&& GameState::Menus::IsInSongModes()
			&& !GameState::Menus::IsInRiffRepeaterMenus();
	}

	void DrawTablaturePreview()
	{
		if (!IsTablaturePreviewVisible())
		{
			snapshot = {};
			bool closed = false;
			Tablature::Draw(snapshot, 1.0f, &closed);
			return;
		}
		RefreshPreview();
		bool open = true;
		auto* font = Overlay::GetFonts().body;
		if (font != nullptr) ImGui::PushFont(font);
		bool toggleLevels = false;
		bool toggleDock = false;
		const bool docked = Settings::GetNoteByNoteTabDocked();
		Tablature::Draw(snapshot, Settings::GetNoteByNoteTargetSize() / 100.0f, &open, &toggleLevels, docked, &toggleDock);
		if (toggleDock) Settings::SetNoteByNoteTabDocked(!docked);
		if (font != nullptr) ImGui::PopFont();
		if (toggleLevels)
			requestedLevelMode = snapshot.showsCurrentLevels
				? ResearchProtocol::TAB_PREVIEW_LEVEL_MAXIMUM
				: ResearchProtocol::TAB_PREVIEW_LEVEL_CURRENT;
		if (!open) Settings::SetNoteByNoteTargetStyle(Settings::NoteByNoteTargetStyle::Tab);
	}
}
