#include "../stdafx.h"
#include "NoteByNoteProbe.hpp"

#include "NoteByNoteNativeScoring.hpp"
#include "../D3D/NoteByNoteHighwayRenderer.hpp"
#include "../Research/ResearchBridge.hpp"
#include "../Settings.hpp"

namespace
{

	std::mutex controllerMutex;
	bool isAutomaticEnabled = false;
	std::atomic<bool> automaticEnabledSnapshot{ false };
	std::string selectedArrangement = "lead";
	std::string automaticTargetText;


	struct HostChordTemplateView
	{
		uint32_t mask;
		uint8_t frets[6];
		uint8_t fingers[6];
		int32_t notes[6];
		char name[32];
	};
	static_assert(sizeof(HostChordTemplateView) == 0x48, "SNG chord template stride is 0x48");

	template <typename T>
	bool TryReadGame(uintptr_t address, T& value)
	{
		if (address == 0) return false;
		MEMORY_BASIC_INFORMATION memory = {};
		if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) == 0) return false;
		if (memory.State != MEM_COMMIT
			|| (memory.Protect & PAGE_GUARD) != 0
			|| (memory.Protect & PAGE_NOACCESS) != 0)
		{
			return false;
		}
		const auto regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
		if (address > regionEnd || sizeof(T) > regionEnd - address) return false;
		value = *reinterpret_cast<const T*>(address);
		return true;
	}

	bool TryDescribeChordTargetHost(uintptr_t noteAddress, char* buffer, size_t bufferLength)
	{
		if (buffer == nullptr || bufferLength == 0) return false;
		buffer[0] = '\0';
		uintptr_t templateAddress = 0;
		if (!TryReadGame(noteAddress + 0x30, templateAddress) || templateAddress == 0) return false;
		HostChordTemplateView view = {};
		if (!TryReadGame(templateAddress, view)) return false;

		char name[sizeof(view.name) + 1] = {};
		for (size_t i = 0; i < sizeof(view.name) && view.name[i] != '\0'; ++i)
		{
			if (view.name[i] < 0x20 || view.name[i] > 0x7E) { name[0] = '\0'; break; }
			name[i] = view.name[i];
		}

		char frets[24] = {};
		size_t at = 0;
		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			const uint8_t fret = view.frets[stringIndex];
			char fretText[4];
			if (fret < 0x1A) std::snprintf(fretText, sizeof(fretText), "%u", fret);
			else { fretText[0] = 'x'; fretText[1] = '\0'; }
			at += std::snprintf(frets + at, sizeof(frets) - at, "%s%s",
				stringIndex == 0 ? "" : "/", fretText);
			if (at >= sizeof(frets)) break;
		}
		char fingers[24] = {};
		at = 0;
		bool hasFingering = false;
		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			const uint8_t fret = view.frets[stringIndex];
			const uint8_t finger = view.fingers[stringIndex];
			char fingerText[4];
			if (fret < 0x1A && finger >= 1 && finger <= 4)
			{
				std::snprintf(fingerText, sizeof(fingerText), "%u", finger);
				hasFingering = true;
			}
			else { fingerText[0] = 'x'; fingerText[1] = '\0'; }
			at += std::snprintf(fingers + at, sizeof(fingers) - at, "%s%s",
				stringIndex == 0 ? "" : "/", fingerText);
			if (at >= sizeof(fingers)) break;
		}

		if (name[0] != '\0')
		{
			std::snprintf(buffer, bufferLength, hasFingering
				? "%s [%s] fingers [%s]" : "%s [%s]", name, frets, fingers);
		}
		else
		{
			std::snprintf(buffer, bufferLength, hasFingering
				? "[%s] fingers [%s]" : "[%s]", frets, fingers);
		}
		return true;
	}
	bool TryReadChordTemplateFretsHost(
		uintptr_t noteAddress,
		int (&frets)[6],
		int (&fingers)[6])
	{
		uintptr_t templateAddress = 0;
		if (!TryReadGame(noteAddress + 0x30, templateAddress) || templateAddress == 0)
		{
			return false;
		}
		HostChordTemplateView view = {};
		if (!TryReadGame(templateAddress, view)) return false;
		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			const bool fretted = view.frets[stringIndex] < 0x1A;
			frets[stringIndex] = fretted
				? static_cast<int>(view.frets[stringIndex])
				: -1;
			fingers[stringIndex] = fretted
				&& view.fingers[stringIndex] >= 1 && view.fingers[stringIndex] <= 4
				? static_cast<int>(view.fingers[stringIndex])
				: 0;
		}
		return true;
	}

	void DisableAutomatic()
	{
		{
			std::lock_guard<std::mutex> lock(controllerMutex);
			if (!isAutomaticEnabled) return;
			isAutomaticEnabled = false;
			automaticEnabledSnapshot.store(false, std::memory_order_relaxed);
			automaticTargetText.clear();
		}

		NoteByNoteNativeScoring::Stop();
		NoteByNoteHighwayRenderer::EvictRememberedDimCandidates();
		LOG_INFO("(NBN AUTO) Disabled." << std::endl);
	}

	bool ToggleAutomaticInternal()
	{
		bool didEnable = false;
		{
			std::lock_guard<std::mutex> lock(controllerMutex);
			if (!isAutomaticEnabled)
			{
				if (!NoteByNoteNativeScoring::IsAvailable())
				{
					LOG_ERROR("(NBN AUTO) Start ignored because the authoritative native controller is unavailable."
						<< std::endl);
					return false;
				}
				if (!GameState::Menus::IsInRiffRepeaterMenus())
				{
					LOG_ERROR("(NBN AUTO) Start ignored because Note by Note can only be enabled from the Riff Repeater menu."
						<< std::endl);
					return false;
				}

				isAutomaticEnabled = true;
				automaticEnabledSnapshot.store(true, std::memory_order_relaxed);
				automaticTargetText = "Waiting for Rocksmith's native section epoch";
				LOG_INFO("(NBN AUTO) Enabled for " << selectedArrangement
					<< ". Rocksmith's live scoring vector is the only attack iterator; waiting for its native section-entry rollback."
					<< std::endl);
				NoteByNoteHighwayRenderer::SetNeckPlacementMode(3);
				didEnable = true;
			}
		}

		if (didEnable)
		{
			NoteByNoteNativeScoring::RequestReArm();
			return true;
		}

		DisableAutomatic();
		return true;
	}



	const char* GetExpectedEventName(NoteByNoteProbe::NativeExpectedAttackEventKind kind)
	{
		switch (kind)
		{
			case NoteByNoteProbe::NativeExpectedAttackEventKind::CandidateChanged:
				return "candidate-changed";
			case NoteByNoteProbe::NativeExpectedAttackEventKind::HoldEstablished:
				return "hold-established";
			case NoteByNoteProbe::NativeExpectedAttackEventKind::ScoringStateChanged:
				return "scoring-state-changed";
			default:
				return "unknown";
		}
	}

	const char* GetStringColour(uint8_t stringIndex)
	{
		static const char* STRING_COLOURS[] =
		{
			"Red",
			"Yellow",
			"Blue",
			"Orange",
			"Green",
			"Purple"
		};
		if (stringIndex >= sizeof(STRING_COLOURS) / sizeof(STRING_COLOURS[0])) return "Unknown";
		return STRING_COLOURS[stringIndex];
	}

	std::string RenderedAttackText(const NoteByNoteProbe::NativeRenderedAttack& attack)
	{
		std::ostringstream stream;
		for (size_t index = 0; index < attack.notes.size(); ++index)
		{
			if (index != 0) stream << ',';
			stream << attack.notes[index].stringIndex << ':' << attack.notes[index].fret;
		}
		return stream.str();
	}
}

void NoteByNoteProbe::HandleNativeControllerFault(const std::string& reason)
{
	LOG_ERROR("(NBN AUTO) Authoritative native controller fault: " << reason
		<< ". Note by Note has been disabled." << std::endl);
	DisableAutomatic();
}

void NoteByNoteProbe::HandleNativeRenderedAttack(const NativeRenderedAttack& attack)
{
	NoteByNoteNativeScoring::ObserveRenderedAttack(attack);
}

void NoteByNoteProbe::HandleNativeExpectedAttackEvent(const NativeExpectedAttackEvent& event)
{
	ResearchBridge::PublishNoteByNoteEvent(event);

	NativeRenderedAttack renderedAttack = {};
	bool isEnabled = false;
	{
		std::lock_guard<std::mutex> lock(controllerMutex);
		isEnabled = isAutomaticEnabled;
		if (!isEnabled) return;
		if (event.kind == NativeExpectedAttackEventKind::CandidateChanged
			|| event.kind == NativeExpectedAttackEventKind::HoldEstablished)
		{
			std::ostringstream target;
			if (event.note.stringIndex == 0xFF)
			{
				char chordIdentity[64];
				if (TryDescribeChordTargetHost(
					event.note.nativeNoteAddress, chordIdentity, sizeof(chordIdentity)))
				{
					target << "Target: " << chordIdentity << " chord";
				}
				else
				{
					target << "Target: chord (strum)";
				}
				int chordFrets[6];
				int chordFingers[6];
				if (TryReadChordTemplateFretsHost(
					event.note.nativeNoteAddress, chordFrets, chordFingers))
				{
					uint8_t anchorFret = 0;
					uint8_t anchorWidth = 0;
					const bool hasAnchor = event.note.recordAddress != 0
						&& TryReadGame(event.note.recordAddress + 0x12, anchorFret)
						&& TryReadGame(event.note.recordAddress + 0x13, anchorWidth);
					NoteByNoteHighwayRenderer::SetChordTargetShape(
						event.note.chordId, chordFrets, chordFingers,
						hasAnchor ? anchorFret : -1, hasAnchor ? anchorWidth : -1);
				}
				else
				{
					static int32_t loggedUnreadableChord = -1;
					if (loggedUnreadableChord != event.note.chordId)
					{
						loggedUnreadableChord = event.note.chordId;
						LOG_INFO("(NBN MARKERS) Chord " << event.note.chordId
							<< " template frets unreadable; fretboard markers stay unfiltered for this hold."
							<< std::endl);
					}
				}
			}
			else
			{
				target << "Target: " << GetStringColour(event.note.stringIndex)
					<< " string - fret " << event.note.fret;
				if (event.note.bendSemitones > 0)
				{
					target << " - bend +" << event.note.bendSemitones
						<< (event.note.bendSemitones == 1 ? " (half)"
							: event.note.bendSemitones == 2 ? " (full)" : "");
				}
				else if (event.note.bendSemitones < 0)
				{
					target << " - bend";
				}
			}
			automaticTargetText = target.str();
		}
	}

	const auto& note = event.note;
	LOG_INFO("(NBN NATIVE EXPECTED) stage=" << (event.isAfterUpdate ? "post" : "pre")
		<< " event=" << GetExpectedEventName(event.kind)
		<< " epoch=" << event.epoch
		<< " owner=0x" << std::hex << event.ownerAddress
		<< " note=0x" << note.nativeNoteAddress
		<< " record=0x" << note.recordAddress
		<< " mask=0x" << note.noteMask
		<< " flags=0x" << note.noteFlags
		<< " hash=0x" << note.noteHash << std::dec
		<< " updateTime=" << std::fixed << std::setprecision(6) << event.updateTime
		<< " authoredTime=" << note.authoredTime
		<< " nativeEventTime=" << note.nativeEventTime
		<< " rangeA4=" << note.rangeA4
		<< " rangeA8=" << note.rangeA8
		<< " rangeB0=" << note.rangeB0
		<< " string=" << note.stringIndex
		<< " fret=" << note.fret
		<< " chord=" << note.chordId
		<< " chordNotes=" << note.chordNotesId
		<< " phraseIteration=" << note.phraseIterationId
		<< " states=" << static_cast<int>(note.state50)
		<< static_cast<int>(note.state51)
		<< static_cast<int>(note.state52)
		<< '/' << static_cast<int>(note.stateC0)
		<< static_cast<int>(note.stateC1)
		<< static_cast<int>(note.stateC2)
		<< static_cast<int>(note.stateC3)
		<< '/' << static_cast<int>(note.state144)
		<< " previousRecord=0x" << std::hex
		<< (event.hasPreviousNote ? event.previousNote.recordAddress : 0) << std::dec
		<< " previousAuthoredTime=" << (event.hasPreviousNote
			? event.previousNote.authoredTime
			: 0.0f)
		<< " notePresent=" << std::boolalpha << event.isNotePresent
		<< " playableRange=" << event.isInPlayableRange
		<< " lastHit=" << event.wasLastNoteHit
		<< "." << std::endl);
}



void NoteByNoteProbe::Initialize()
{
	NoteByNoteNativeScoring::Initialize();
	if (NoteByNoteNativeScoring::IsAvailable())
	{
		LOG_INFO("(NBN PROBE) Authoritative native Note by Note controller ready. Enable it from Riff Repeater; N toggles it, and Ctrl+Shift+1/2/3 select Lead/Rhythm/Bass menu intent."
			<< std::endl);
	}
	else
	{
		LOG_ERROR("(NBN PROBE) Authoritative native Note by Note controller is unavailable."
			<< std::endl);
	}
}



bool NoteByNoteProbe::IsAutomaticEnabled()
{
	std::lock_guard<std::mutex> lock(controllerMutex);
	return isAutomaticEnabled;
}
bool NoteByNoteProbe::IsAutomaticEnabledFast()
{
	return automaticEnabledSnapshot.load(std::memory_order_relaxed);
}

bool NoteByNoteProbe::IsFreezeActive()
{
	if (!IsAutomaticEnabled()) return false;
	const bool practicing = GameState::Menus::IsInSongModes()
		&& !GameState::Menus::IsInRiffRepeaterMenus();
	static std::string lastLoggedMenu;
	if (GameState::currentMenu != lastLoggedMenu)
	{
		lastLoggedMenu = GameState::currentMenu;
		LOG_INFO("(NBN LIFECYCLE) menu=" << GameState::currentMenu
			<< " practicing=" << practicing
			<< " -> freeze " << (practicing ? "ALLOWED" : "blocked") << "." << std::endl);
	}
	return practicing;
}

void NoteByNoteProbe::TickLifecycle()
{
	static uint32_t outOfSongTicks = 0;
	if (!IsAutomaticEnabled() || GameState::Menus::IsInSongModes())
	{
		outOfSongTicks = 0;
		return;
	}
	constexpr uint32_t RETIRE_AFTER_TICKS = 15;
	if (++outOfSongTicks >= RETIRE_AFTER_TICKS)
	{
		LOG_INFO("(NBN LIFECYCLE) Sustained absence from the song/RR context (menu="
			<< GameState::currentMenu << ", " << outOfSongTicks
			<< " ticks); retiring Note by Note." << std::endl);
		outOfSongTicks = 0;
		DisableAutomatic();
	}
}

bool NoteByNoteProbe::ToggleAutomatic()
{
	return ToggleAutomaticInternal();
}

bool NoteByNoteProbe::SetAutomaticEnabled(bool shouldEnable)
{
	if (IsAutomaticEnabled() == shouldEnable) return true;
	return ToggleAutomaticInternal();
}



std::string NoteByNoteProbe::GetAutomaticTargetText()
{
	std::lock_guard<std::mutex> lock(controllerMutex);
	return automaticTargetText;
}
