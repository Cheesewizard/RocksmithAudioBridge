#pragma once

// Safe failure for the Tone designer: checks a saved tone, or a capture or cabinet a pedal is about to load, before
// Stompbox sees it, so a damaged file is refused with a plain-English reason and the rig keeps what it had.
//
// Why up front: Stompbox loads a saved tone on a thread of its own, and a model or IR it cannot read throws or
// crashes on that thread, which ends the host (NeuralAudio throws on bad JSON; the IR loader reads past its buffer
// on a silent or empty WAV). Nothing in the host can catch that, so the files are tried first in a probe: a Stompbox
// processor of its own, on the main thread, with every call guarded (StompboxEngine::Guard). A file passes when it
// loads without an error and changes a test tone into finite samples. Results are kept per file (size and time) so
// each file is tried once.
//
// Main thread only.

#include "StompboxEngine.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace VstHost
{
	class StompboxChecks
	{
	public:
		// The data folder and the catalogue (Stompbox's DumpConfig) of the engine the commands go to: which pedal
		// parameters name files, in which folder, and the files that engine has listed. Forgets earlier results.
		void Reset(const std::wstring& dataPath, const std::string& catalogue);

		// Empty when the command may run; otherwise why it was refused, ready for the page. Checks SetParam on a
		// file parameter and LoadPreset; anything else passes.
		std::string CheckCommand(const std::string& command);
		// Empty when the saved tone may load. `who` starts the sentence: "The tone" or "The song's tone".
		std::string CheckPreset(const std::string& name, const char* who = "The tone");
		// Empty when the pedal may take this value. `value` as Stompbox reads it (words after the parameter, joined).
		std::string CheckParameter(const std::string& pedalId, const std::string& parameter, const std::string& value);

		// The SetParam lines of a program (Stompbox's DumpProgram) whose file is refused, each with its reason.
		std::vector<std::pair<std::string, std::string>> RefusedLines(const std::string& program);

		static std::vector<std::string> Words(const std::string& line);
		static std::string KindOf(const std::string& pedalId) { return pedalId.substr(0, pedalId.find('_')); }

	private:
		enum class Outcome { Ok, Missing, Unreadable, Dry, Invalid };

		struct FileParameter
		{
			std::string folder;                 // under the data folder: "NAM", "Cabinets", "Reverb"
			std::set<std::string> names;        // what the engine lists there (file names without extension)
		};

		// "" or a clause for a sentence: "the capture 'Plexi' is damaged or is not a NAM model".
		std::string ParameterClause(const std::string& pedalId, const std::string& parameter, const std::string& value);
		std::string FileClause(const FileParameter& file, const std::string& name);
		Outcome Probe(const std::string& folder, const std::string& name);
		bool EnsureProbe();

		std::wstring dataPath;
		std::map<std::string, std::map<std::string, FileParameter>> fileParameters;   // pedal kind -> parameter -> file
		std::map<std::string, std::pair<std::string, std::string>> probePedals;       // folder -> pedal kind, parameter
		std::map<std::string, Outcome> results;   // "<folder>|<name>|<size>|<time>" -> what the probe found
		std::unique_ptr<StompboxEngine> probe;
		bool isProbeUnavailable = false;
	};
}
