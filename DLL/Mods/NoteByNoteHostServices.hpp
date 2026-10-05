#pragma once

#include "NoteByNoteTypes.hpp"

// The services the in-process Note by Note scoring engine (and any reloadable research probe)
// calls back into: tier-0 raw-tone evidence, ML pitch/note queries, the Drop Pedal input shift,
// the enable/freeze gate, fault handling, and logging. Every one of these is wired to a shipping
// subsystem, so this table lives in the shipping module rather than the debug bridge. The scoring
// engine is handed this exact HostApi at initialization.
namespace NoteByNoteHostServices
{
	// The HostApi the scoring engine is initialized with. Stable for the life of the module.
	const ResearchProtocol::HostApi& GetHostApi();

	// Optional mirror of host log lines onto the research event stream. The shipping module leaves
	// this null (logs still reach the file logger); the debug bridge installs one so a connected
	// research tool sees the same lines. Absent sink means no telemetry, never a crash.
	using LogTelemetrySink = void (*)(ResearchProtocol::LogLevel level, const char* message);
	void SetLogTelemetrySink(LogTelemetrySink sink);

	// RIGHT ARROW while a note is frozen, so the player is never stuck on one note. The window procedure
	// queues the press while a Learn a Song / Riff Repeater song is playing; the controller takes it
	// on its next tick (HostApi v12 ConsumeNoteNavigation) and acts only while it holds a note.
	// +1 = skip to the next note, -1 = back to the previous one (not acted on yet).
	void QueueNoteNavigation(int direction);
}
