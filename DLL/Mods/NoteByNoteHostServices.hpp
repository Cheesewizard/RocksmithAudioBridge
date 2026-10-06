#pragma once

#include "NoteByNoteTypes.hpp"
namespace NoteByNoteHostServices
{
	const ResearchProtocol::HostApi& GetHostApi();
	using LogTelemetrySink = void (*)(ResearchProtocol::LogLevel level, const char* message);
	void SetLogTelemetrySink(LogTelemetrySink sink);
	void QueueNoteNavigation(int direction);
}
