#pragma once

#include "../Detection/ChordAttackGate.hpp"
#include "../Detection/NoteTechniqueClassification.hpp"
#include "../Protocol/TablaturePreview.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NoteByNote::Controller
{
	namespace AuthoredTablatureDetail
	{
		struct Record
		{
			uintptr_t address = 0;
			uint32_t mask = 0;
			float time = 0.0f;
			uint8_t stringIndex = 0;
			uint8_t fret = 0;
			int32_t chordId = -1;
			int32_t phraseIteration = -1;
			float bendAmount = 0.0f;
			uint8_t slideTo = 0xFF;
			float sustain = 0.0f;
			// Bend shape from the bend curve (semitones); see ReadBendShape.
			float bendStart = 0.0f;
			float bendPeak = 0.0f;
			float bendEnd = 0.0f;
		};

		struct Iteration
		{
			int32_t id = -1;
			int32_t phraseId = -1;
		};

		struct Layout
		{
			std::vector<std::vector<Record>> levels;
			std::vector<std::array<uint8_t, 6>> chordFrets;
			std::vector<std::string> chordNames;
			std::vector<uint32_t> phraseMaxDifficulties;
			std::vector<Iteration> iterations;
		};
	}

	// Optional level selection. With no levels the build shows every phrase at
	// its maximum (100%) difficulty.
	struct TablatureLevelRequest
	{
		// The game's current level per phrase iteration (DD state owner+0x7C).
		// Ignored unless it has exactly one entry per iteration.
		const std::vector<int32_t>* iterationLevels = nullptr;
		// The current target record. The level vector that contains it is
		// ground truth for its iteration and overrides iterationLevels there.
		uintptr_t observedRecord = 0;
	};

	struct TablatureLevelResult
	{
		bool usedIterationLevels = false;
		uint32_t rejectedLevels = 0;  // entries outside 0..phrase max, shown at max
		uint32_t iterationsBelowMax = 0;  // iterations shown below their maximum level
		int32_t observedIteration = -1;
		int32_t observedLevel = -1;
		bool observedDisagreed = false;  // iterationLevels named a different level
		// How many shown records carry each note mask bit; logged so the
		// technique bits can be checked against a song's known techniques.
		std::array<uint32_t, 32> maskBitCounts = {};
	};

	// Reader contract: read(address, value) copies one typed value, and
	// read.ReadBytes(address, destination, size) copies a whole block. Note,
	// chord, phrase and iteration vectors are each fetched with one ReadBytes
	// call because the game build validates every read with VirtualQuery and
	// tens of thousands of per-field reads stalled the scoring thread.
	class AuthoredTablatureReader
	{
	public:
		static constexpr uintptr_t LEVELS_BEGIN = 0x40;
		static constexpr uintptr_t LEVELS_END = 0x44;
		static constexpr uintptr_t CHORDS_BEGIN = 0x94;
		static constexpr uintptr_t CHORDS_END = 0x98;
		static constexpr uintptr_t PHRASES_BEGIN = 0x58;
		static constexpr uintptr_t PHRASES_END = 0x5C;
		static constexpr uintptr_t ITERATIONS_BEGIN = 0x64;
		static constexpr uintptr_t ITERATIONS_END = 0x68;
		static constexpr uintptr_t PHRASE_STRIDE = 0x2C;
		static constexpr uintptr_t PHRASE_MAX_DIFFICULTY = 0x04;
		static constexpr uintptr_t ITERATION_STRIDE = 0x18;
		static constexpr uintptr_t ITERATION_PHRASE_ID = 0x00;
		static constexpr uintptr_t LEVEL_STRIDE = 0x64;
		static constexpr uintptr_t LEVEL_NOTES_BEGIN = 0x30;
		static constexpr uintptr_t LEVEL_NOTES_END = 0x34;
		static constexpr uintptr_t NOTE_STRIDE = 0x1C8;
		static constexpr uintptr_t RECORD_MASK = 0x00;
		static constexpr uintptr_t RECORD_TIME = 0x0C;
		static constexpr uintptr_t RECORD_STRING = 0x10;
		static constexpr uintptr_t RECORD_FRET = 0x11;
		static constexpr uintptr_t RECORD_CHORD_ID = 0x14;
		static constexpr uintptr_t RECORD_PHRASE_ITERATION = 0x20;
		static constexpr uintptr_t RECORD_BEND_AMOUNT = 0x40;
		// Pitched slide target fret, 0xFF = none. Proven by decompile 2026-10-19:
		// NeckNoteVisualBuild 0x79D240 passes this byte to the slide builder
		// 0x79E920 when the mask has SLIDE and not SLIDE_UNPITCHED; the builder
		// returns on 0xFF and compares it with the next note's fret.
		static constexpr uintptr_t RECORD_SLIDE_TO = 0x34;
		// Sustain, max bend and the inline bend curve, proven by decompile
		// 2026-10-23 (atlas Generated/decomp-note-record-tab-fields-2026-10-23.txt):
		// the note constructor 0x557480 fills 32 bend points of 12 bytes from
		// +0x44 {time, step in semitones, int16, u8, u8}; 0x79D240 tests the count
		// at +0x1C4; 0x8747F0 sets +0x3C sustain (mask 0x2000 when > 0); 0x551990
		// sets +0x40 max bend (mask 0x1000). 0x44 + 32 * 12 + 4 = the 0x1C8 stride.
		static constexpr uintptr_t RECORD_SUSTAIN = 0x3C;
		static constexpr uintptr_t RECORD_BEND_POINTS = 0x44;
		static constexpr uintptr_t BEND_POINT_STRIDE = 0x0C;
		static constexpr uint32_t BEND_POINT_CAPACITY = 32;
		static constexpr uintptr_t RECORD_BEND_POINT_COUNT = 0x1C4;
		static constexpr uintptr_t CHORD_STRIDE = 0x48;
		static constexpr uintptr_t CHORD_FRETS = 0x04;
		static constexpr uintptr_t CHORD_NAME = 0x28;   // char[32]; proven, drawn by 0x7A14A0

		// Dynamic Difficulty state, shared by DD and the Riff Repeater difficulty
		// slider. Proven by decompile (FUN_0043C660 feeds GameComponentDD's
		// per-iteration cache) and by NoteByNoteMenu's working note cap.
		static constexpr uintptr_t OWNER_DD_STATE = 0x7C;
		static constexpr uintptr_t DD_ITERATIONS_BEGIN = 0x18;
		static constexpr uintptr_t DD_ITERATIONS_END = 0x1C;
		static constexpr uintptr_t DD_ITERATION_STRIDE = 0x40;
		static constexpr uintptr_t DD_ITERATION_LEVEL = 0x04;

		// Arrangement sections (the Riff Repeater sections), also read by the
		// Note by Note timeline controller.
		static constexpr uintptr_t SECTIONS_BEGIN = 0xF4;
		static constexpr uintptr_t SECTIONS_END = 0xF8;
		static constexpr uintptr_t SECTION_STRIDE = 0x58;
		static constexpr uintptr_t SECTION_START = 0x24;
		static constexpr uintptr_t SECTION_NAME = 0x00;  // char[32]; proven, 0x55D050 compares it with "noguitar"
		static constexpr size_t NAME_CAPACITY = 32;

		// Beat grid (SNG BPM / XML ebeats): one record per beat. Live-checked
		// 2026-10-19 (1454 beats, 364 measures, every section starts on a beat);
		// evidence in the atlas, Generated/decomp-arrangement-beats-sept-2026-10-19.txt.
		static constexpr uintptr_t BEATS_BEGIN = 0x34;
		static constexpr uintptr_t BEATS_END = 0x38;
		static constexpr uintptr_t BEAT_STRIDE = 0x10;
		static constexpr uintptr_t BEAT_TIME = 0x00;
		static constexpr uintptr_t BEAT_MEASURE = 0x04;   // int16
		static constexpr uintptr_t BEAT_IN_MEASURE = 0x06; // int16, 0 = downbeat

		// SNG note mask bits (same table as the hammer-on, pull-off, tap and
		// fret-hand mute bits already in use).
		static constexpr uint32_t NOTE_MASK_HARMONIC = 0x00000020u;
		static constexpr uint32_t NOTE_MASK_PALM_MUTE = 0x00000040u;
		static constexpr uint32_t NOTE_MASK_PINCH_HARMONIC = 0x00008000u;
		static constexpr uint32_t NOTE_MASK_SLIDE = 0x00000800u;
		static constexpr uint32_t NOTE_MASK_SLIDE_UNPITCHED = 0x00400000u;
		static constexpr uint32_t NOTE_MASK_BEND = 0x00001000u;
		static constexpr uint32_t NOTE_MASK_IGNORE = 0x00040000u;
		static constexpr uint32_t NOTE_MASK_CHILD = 0x10000000u;
		// Consistent with decompile, not proven: tremolo has its own effect path
		// (0x8760D0), accent changes the marker colour state (0x79D240); vibrato
		// is only seen on another struct carrying the same mask (0x877E70).
		static constexpr uint32_t NOTE_MASK_TREMOLO = 0x00000010u;
		static constexpr uint32_t NOTE_MASK_VIBRATO = 0x00010000u;
		static constexpr uint32_t NOTE_MASK_ACCENT = 0x04000000u;

		template <typename Reader>
		static bool ReadAuthoredTablatureIdentity(
			uintptr_t container,
			const Reader& read,
			uint64_t& signature,
			std::string& error)
		{
			return ReadMetadata(container, read, signature, error);
		}

		// Reads the level the player is on for every phrase iteration. Callers
		// must still validate the count; BuildAuthoredTablatureEvents does.
		template <typename Reader>
		static bool ReadCurrentIterationLevels(
			uintptr_t owner,
			const Reader& read,
			std::vector<int32_t>& levels,
			std::string& error)
		{
			levels.clear();
			uint32_t state = 0;
			if (owner == 0 || !ReadValue(read, owner + OWNER_DD_STATE, state, "difficulty state", error)) return false;
			if (state == 0)
			{
				error = "difficulty state is null";
				return false;
			}
			uintptr_t begin = 0;
			uintptr_t end = 0;
			if (!ReadVector(state, DD_ITERATIONS_BEGIN, DD_ITERATIONS_END, DD_ITERATION_STRIDE, 10000, read,
				begin, end, "difficulty iteration vector", error)) return false;
			std::vector<uint8_t> bytes;
			if (!ReadBlock(read, begin, end, bytes, "difficulty iteration vector", error)) return false;
			levels.resize(bytes.size() / DD_ITERATION_STRIDE);
			for (size_t index = 0; index < levels.size(); ++index)
				levels[index] = Field<int32_t>(bytes, index * DD_ITERATION_STRIDE + DD_ITERATION_LEVEL);
			return true;
		}

		// Start times of the arrangement's sections, in authored order.
		template <typename Reader>
		static bool ReadSectionStarts(uintptr_t container, const Reader& read, std::vector<float>& starts, std::string& error)
		{
			std::vector<std::string> names;
			return ReadSections(container, read, starts, names, error);
		}

		// Section start times and names ("verse", "chorus"), in authored order.
		template <typename Reader>
		static bool ReadSections(uintptr_t container, const Reader& read, std::vector<float>& starts,
			std::vector<std::string>& names, std::string& error)
		{
			starts.clear();
			names.clear();
			uintptr_t begin = 0;
			uintptr_t end = 0;
			if (container == 0 || !ReadVector(container, SECTIONS_BEGIN, SECTIONS_END, SECTION_STRIDE, 1024, read,
				begin, end, "section vector", error)) return false;
			std::vector<uint8_t> bytes;
			if (!ReadBlock(read, begin, end, bytes, "section vector", error)) return false;
			for (size_t index = 0; index < bytes.size() / SECTION_STRIDE; ++index)
			{
				const float start = Field<float>(bytes, index * SECTION_STRIDE + SECTION_START);
				if (!std::isfinite(start) || start < 0.0f) continue;
				starts.push_back(start);
				names.push_back(Name(bytes, index * SECTION_STRIDE + SECTION_NAME));
			}
			return true;
		}

		// Bar lines: the time and measure number of every downbeat, in order.
		template <typename Reader>
		static bool ReadMeasureStarts(
			uintptr_t container,
			const Reader& read,
			std::vector<std::pair<float, int32_t>>& measures,
			std::string& error)
		{
			measures.clear();
			uintptr_t begin = 0;
			uintptr_t end = 0;
			if (container == 0 || !ReadVector(container, BEATS_BEGIN, BEATS_END, BEAT_STRIDE, 100000, read,
				begin, end, "beat vector", error)) return false;
			std::vector<uint8_t> bytes;
			if (!ReadBlock(read, begin, end, bytes, "beat vector", error)) return false;
			float previous = -1.0f;
			for (size_t index = 0; index < bytes.size() / BEAT_STRIDE; ++index)
			{
				const size_t base = index * BEAT_STRIDE;
				const float time = Field<float>(bytes, base + BEAT_TIME);
				if (!std::isfinite(time) || time < previous)
				{
					error = "beat times are not in order";
					measures.clear();
					return false;
				}
				previous = time;
				if (Field<int16_t>(bytes, base + BEAT_IN_MEASURE) == 0)
					measures.emplace_back(time, static_cast<int32_t>(Field<int16_t>(bytes, base + BEAT_MEASURE)));
			}
			return true;
		}

		template <typename Reader>
		static bool BuildAuthoredTablatureEvents(
			uintptr_t container,
			const Reader& read,
			std::vector<ResearchProtocol::PreviewEvent>& events,
			std::string& error,
			const TablatureLevelRequest& request = {},
			TablatureLevelResult* result = nullptr)
		{
			Layout layout;
			if (!ReadLayout(container, read, layout, error)) return false;
			TablatureLevelResult local;
			TablatureLevelResult& outcome = result != nullptr ? *result : local;
			outcome = {};
			const bool useLevels = request.iterationLevels != nullptr
				&& request.iterationLevels->size() == layout.iterations.size();
			outcome.usedIterationLevels = useLevels;
			if (request.observedRecord != 0)
			{
				for (uint32_t level = 0; level < layout.levels.size() && outcome.observedLevel < 0; ++level)
					for (const Record& record : layout.levels[level])
						if (record.address == request.observedRecord)
						{
							outcome.observedLevel = static_cast<int32_t>(level);
							outcome.observedIteration = record.phraseIteration;
							break;
						}
			}

			std::unordered_map<int32_t, uint32_t> selectedLevelByIteration;
			for (const Iteration& iteration : layout.iterations)
			{
				if (iteration.phraseId < 0 || static_cast<size_t>(iteration.phraseId) >= layout.phraseMaxDifficulties.size())
				{
					error = "phrase iteration references an invalid phrase id";
					return false;
				}
				const uint32_t maxDifficulty = layout.phraseMaxDifficulties[static_cast<size_t>(iteration.phraseId)];
				if (maxDifficulty >= layout.levels.size())
				{
					error = "phrase max difficulty exceeds the authored level count";
					return false;
				}
				uint32_t selectedLevel = maxDifficulty;
				if (useLevels)
				{
					const int32_t current = (*request.iterationLevels)[static_cast<size_t>(iteration.id)];
					if (current >= 0 && static_cast<uint32_t>(current) <= maxDifficulty)
						selectedLevel = static_cast<uint32_t>(current);
					else
						++outcome.rejectedLevels;
					if (iteration.id == outcome.observedIteration
						&& outcome.observedLevel >= 0
						&& static_cast<uint32_t>(outcome.observedLevel) != selectedLevel)
					{
						outcome.observedDisagreed = true;
					}
				}
				if (useLevels && iteration.id == outcome.observedIteration
					&& outcome.observedLevel >= 0 && static_cast<uint32_t>(outcome.observedLevel) <= maxDifficulty)
				{
					selectedLevel = static_cast<uint32_t>(outcome.observedLevel);
				}
				if (selectedLevel < maxDifficulty) ++outcome.iterationsBelowMax;
				selectedLevelByIteration[iteration.id] = selectedLevel;
			}

			events.clear();
			for (uint32_t level = 0; level < layout.levels.size(); ++level)
			{
				for (const Record& record : layout.levels[level])
				{
					// Linked children (the chart's linkNext continuations) are kept and
					// drawn as tied notes; they are never a target.
					if ((record.mask & NOTE_MASK_IGNORE) != 0) continue;
					auto selected = selectedLevelByIteration.find(record.phraseIteration);
					if (selected == selectedLevelByIteration.end() || selected->second != level) continue;

					for (uint32_t bit = 0; bit < 32; ++bit)
						if ((record.mask & (1u << bit)) != 0) ++outcome.maskBitCounts[bit];
					ResearchProtocol::PreviewEvent event = {};
					if (!BuildEvent(record, layout.chordFrets, layout.chordNames, event, error)) return false;
					if (event.positionCount == 0) continue;
					events.push_back(event);
				}
			}

			std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right)
			{
				if (left.time != right.time) return left.time < right.time;
				return left.id < right.id;
			});
			return true;
		}

	private:
		using Record = AuthoredTablatureDetail::Record;
		using Iteration = AuthoredTablatureDetail::Iteration;
		using Layout = AuthoredTablatureDetail::Layout;

		template <typename Reader, typename T>
		static bool ReadValue(const Reader& read, uintptr_t address, T& value, const char* stage, std::string& error)
		{
			if (address != 0 && read(address, value)) return true;
			std::ostringstream message;
			message << stage << " at 0x" << std::hex << address;
			error = message.str();
			return false;
		}

		template <typename Reader>
		static bool ReadBlock(
			const Reader& read,
			uintptr_t begin,
			uintptr_t end,
			std::vector<uint8_t>& bytes,
			const char* stage,
			std::string& error)
		{
			bytes.resize(end - begin);
			if (bytes.empty() || read.ReadBytes(begin, bytes.data(), bytes.size())) return true;
			std::ostringstream message;
			message << stage << " block 0x" << std::hex << begin << "..0x" << end << " is unreadable";
			error = message.str();
			return false;
		}

		template <typename T>
		static T Field(const std::vector<uint8_t>& bytes, size_t offset)
		{
			T value{};
			std::memcpy(&value, bytes.data() + offset, sizeof(T));
			return value;
		}

		// A fixed char[32] name: up to the first NUL, printable ASCII only.
		static std::string Name(const std::vector<uint8_t>& bytes, size_t offset)
		{
			std::string name;
			for (size_t index = 0; index < NAME_CAPACITY && offset + index < bytes.size(); ++index)
			{
				const uint8_t c = bytes[offset + index];
				if (c == 0) break;
				if (c < 0x20 || c > 0x7E) return {};
				name.push_back(static_cast<char>(c));
			}
			return name;
		}

		// The bend's shape from its curve. A first point at the attack that is
		// already bent is a pre-bend; a last point below the peak is a release.
		// An unreadable curve keeps the max bend as a plain bend up.
		static void ReadBendShape(const std::vector<uint8_t>& bytes, size_t base, Record& record)
		{
			const float peak = std::isfinite(record.bendAmount) && record.bendAmount > 0.0f && record.bendAmount <= 4.0f
				? record.bendAmount : 0.0f;
			record.bendStart = 0.0f;
			record.bendPeak = peak;
			record.bendEnd = peak;
			const int32_t count = Field<int32_t>(bytes, base + RECORD_BEND_POINT_COUNT);
			if (count <= 0 || count > static_cast<int32_t>(BEND_POINT_CAPACITY)) return;
			float firstTime = 0.0f, firstStep = 0.0f, lastStep = 0.0f, highest = 0.0f;
			float previousTime = -FLT_MAX;
			for (int32_t index = 0; index < count; ++index)
			{
				const size_t point = base + RECORD_BEND_POINTS + static_cast<size_t>(index) * BEND_POINT_STRIDE;
				const float time = Field<float>(bytes, point);
				const float step = Field<float>(bytes, point + 4);
				if (!std::isfinite(time) || !std::isfinite(step) || step < 0.0f || step > 4.0f || time < previousTime) return;
				previousTime = time;
				if (index == 0) { firstTime = time; firstStep = step; }
				lastStep = step;
				highest = (std::max)(highest, step);
			}
			if (highest <= 0.0f) return;
			// A pre-bend only when the first point is on the attack itself: the
			// highway's own test (0x79F880, |first - note time| < 0.001). Scoring
			// (0x7E3700) allows 0.05 s, but custom charts often put the first
			// point of an ordinary bend 10..50 ms in, which is not a pre-bend.
			record.bendStart = std::fabs(firstTime - record.time) < 0.001f ? firstStep : 0.0f;
			record.bendPeak = highest;
			record.bendEnd = lastStep;
		}

		static void Mix(uint64_t& hash, uintptr_t value)
		{
			hash ^= static_cast<uint64_t>(value);
			hash *= 1099511628211ull;
		}

		template <typename Reader>
		static bool ReadVector(
			uintptr_t object,
			uintptr_t beginOffset,
			uintptr_t endOffset,
			uintptr_t stride,
			uint32_t maximum,
			const Reader& read,
			uintptr_t& begin,
			uintptr_t& end,
			const char* stage,
			std::string& error)
		{
			uint32_t begin32 = 0;
			uint32_t end32 = 0;
			if (!ReadValue(read, object + beginOffset, begin32, stage, error)
				|| !ReadValue(read, object + endOffset, end32, stage, error)) return false;
			begin = begin32;
			end = end32;
			if (begin == 0 && end == 0) return true;
			if (begin == 0 || end < begin || (end - begin) % stride != 0 || (end - begin) / stride > maximum)
			{
				std::ostringstream message;
				message << stage << " has invalid bounds 0x" << std::hex << begin << "..0x" << end;
				error = message.str();
				return false;
			}
			return true;
		}

		template <typename Reader>
		static bool ReadMetadata(uintptr_t container, const Reader& read, uint64_t& signature, std::string& error)
		{
			if (container == 0)
			{
				error = "authored arrangement container is null";
				return false;
			}
			uintptr_t levelsBegin = 0;
			uintptr_t levelsEnd = 0;
			uintptr_t chordsBegin = 0;
			uintptr_t chordsEnd = 0;
			uintptr_t phrasesBegin = 0;
			uintptr_t phrasesEnd = 0;
			uintptr_t iterationsBegin = 0;
			uintptr_t iterationsEnd = 0;
			if (!ReadVector(container, LEVELS_BEGIN, LEVELS_END, LEVEL_STRIDE, 256, read, levelsBegin, levelsEnd, "level vector", error)
				|| !ReadVector(container, CHORDS_BEGIN, CHORDS_END, CHORD_STRIDE, 10000, read, chordsBegin, chordsEnd, "chord vector", error)
				|| !ReadVector(container, PHRASES_BEGIN, PHRASES_END, PHRASE_STRIDE, 10000, read, phrasesBegin, phrasesEnd, "phrase vector", error)
				|| !ReadVector(container, ITERATIONS_BEGIN, ITERATIONS_END, ITERATION_STRIDE, 10000, read, iterationsBegin, iterationsEnd, "phrase iteration vector", error)) return false;

			signature = 1469598103934665603ull;
			Mix(signature, levelsBegin);
			Mix(signature, levelsEnd);
			Mix(signature, chordsBegin);
			Mix(signature, chordsEnd);
			Mix(signature, phrasesBegin);
			Mix(signature, phrasesEnd);
			Mix(signature, iterationsBegin);
			Mix(signature, iterationsEnd);
			const uint32_t levelCount = static_cast<uint32_t>((levelsEnd - levelsBegin) / LEVEL_STRIDE);
			for (uint32_t level = 0; level < levelCount; ++level)
			{
				const uintptr_t levelAddress = levelsBegin + static_cast<uintptr_t>(level) * LEVEL_STRIDE;
				uintptr_t notesBegin = 0;
				uintptr_t notesEnd = 0;
				if (!ReadVector(levelAddress, LEVEL_NOTES_BEGIN, LEVEL_NOTES_END, NOTE_STRIDE, 200000, read,
					notesBegin, notesEnd, "level note vector", error)) return false;
				Mix(signature, notesBegin);
				Mix(signature, notesEnd);
			}
			return true;
		}

		template <typename Reader>
		static bool ReadLayout(uintptr_t container, const Reader& read, Layout& layout, std::string& error)
		{
			layout = {};
			if (container == 0)
			{
				error = "authored arrangement container is null";
				return false;
			}
			uintptr_t levelsBegin = 0;
			uintptr_t levelsEnd = 0;
			if (!ReadVector(container, LEVELS_BEGIN, LEVELS_END, LEVEL_STRIDE, 256, read,
				levelsBegin, levelsEnd, "level vector", error)) return false;
			uintptr_t chordsBegin = 0;
			uintptr_t chordsEnd = 0;
			if (!ReadVector(container, CHORDS_BEGIN, CHORDS_END, CHORD_STRIDE, 10000, read,
				chordsBegin, chordsEnd, "chord vector", error)) return false;
			uintptr_t phrasesBegin = 0;
			uintptr_t phrasesEnd = 0;
			if (!ReadVector(container, PHRASES_BEGIN, PHRASES_END, PHRASE_STRIDE, 10000, read,
				phrasesBegin, phrasesEnd, "phrase vector", error)) return false;
			uintptr_t iterationsBegin = 0;
			uintptr_t iterationsEnd = 0;
			if (!ReadVector(container, ITERATIONS_BEGIN, ITERATIONS_END, ITERATION_STRIDE, 10000, read,
				iterationsBegin, iterationsEnd, "phrase iteration vector", error)) return false;

			std::vector<uint8_t> bytes;
			if (!ReadBlock(read, chordsBegin, chordsEnd, bytes, "chord vector", error)) return false;
			layout.chordFrets.resize(bytes.size() / CHORD_STRIDE);
			for (size_t index = 0; index < layout.chordFrets.size(); ++index)
				layout.chordFrets[index] = Field<std::array<uint8_t, 6>>(bytes, index * CHORD_STRIDE + CHORD_FRETS);
			layout.chordNames.resize(layout.chordFrets.size());
			for (size_t index = 0; index < layout.chordNames.size(); ++index)
				layout.chordNames[index] = Name(bytes, index * CHORD_STRIDE + CHORD_NAME);

			if (!ReadBlock(read, phrasesBegin, phrasesEnd, bytes, "phrase vector", error)) return false;
			layout.phraseMaxDifficulties.resize(bytes.size() / PHRASE_STRIDE);
			for (size_t index = 0; index < layout.phraseMaxDifficulties.size(); ++index)
				layout.phraseMaxDifficulties[index] = Field<uint32_t>(bytes, index * PHRASE_STRIDE + PHRASE_MAX_DIFFICULTY);

			if (!ReadBlock(read, iterationsBegin, iterationsEnd, bytes, "phrase iteration vector", error)) return false;
			layout.iterations.resize(bytes.size() / ITERATION_STRIDE);
			for (size_t index = 0; index < layout.iterations.size(); ++index)
			{
				layout.iterations[index].id = static_cast<int32_t>(index);
				layout.iterations[index].phraseId = Field<int32_t>(bytes, index * ITERATION_STRIDE + ITERATION_PHRASE_ID);
			}

			const uint32_t levelCount = static_cast<uint32_t>((levelsEnd - levelsBegin) / LEVEL_STRIDE);
			layout.levels.resize(levelCount);
			for (uint32_t level = 0; level < levelCount; ++level)
			{
				const uintptr_t levelAddress = levelsBegin + static_cast<uintptr_t>(level) * LEVEL_STRIDE;
				uintptr_t notesBegin = 0;
				uintptr_t notesEnd = 0;
				if (!ReadVector(levelAddress, LEVEL_NOTES_BEGIN, LEVEL_NOTES_END, NOTE_STRIDE, 200000, read,
					notesBegin, notesEnd, "level note vector", error)) return false;
				if (!ReadBlock(read, notesBegin, notesEnd, bytes, "level note vector", error)) return false;
				const size_t noteCount = bytes.size() / NOTE_STRIDE;
				layout.levels[level].reserve(noteCount);
				for (size_t index = 0; index < noteCount; ++index)
				{
					const size_t base = index * NOTE_STRIDE;
					Record record;
					record.address = notesBegin + base;
					record.mask = Field<uint32_t>(bytes, base + RECORD_MASK);
					record.time = Field<float>(bytes, base + RECORD_TIME);
					record.stringIndex = Field<uint8_t>(bytes, base + RECORD_STRING);
					record.fret = Field<uint8_t>(bytes, base + RECORD_FRET);
					record.chordId = Field<int32_t>(bytes, base + RECORD_CHORD_ID);
					record.phraseIteration = Field<int32_t>(bytes, base + RECORD_PHRASE_ITERATION);
					record.bendAmount = Field<float>(bytes, base + RECORD_BEND_AMOUNT);
					record.slideTo = Field<uint8_t>(bytes, base + RECORD_SLIDE_TO);
					record.sustain = Field<float>(bytes, base + RECORD_SUSTAIN);
					if (!std::isfinite(record.sustain) || record.sustain < 0.0f || record.sustain > 60.0f) record.sustain = 0.0f;
					if ((record.mask & NOTE_MASK_BEND) != 0) ReadBendShape(bytes, base, record);
					if (!std::isfinite(record.time))
					{
						error = "note time is not finite";
						return false;
					}
					layout.levels[level].push_back(record);
				}
			}
			return true;
		}

		static bool BuildEvent(
			const Record& record,
			const std::vector<std::array<uint8_t, 6>>& chordFrets,
			const std::vector<std::string>& chordNames,
			ResearchProtocol::PreviewEvent& event,
			std::string& error)
		{
			event.id = static_cast<uint64_t>(record.address);
			event.time = record.time;
			if (record.chordId < 0)
			{
				if (record.stringIndex < 6 && record.fret < 0x1A)
					AddPosition(event, record.stringIndex, record.fret, record);
				return true;
			}
			if (static_cast<size_t>(record.chordId) >= chordFrets.size())
			{
				std::ostringstream message;
				message << "chord id " << record.chordId << " is outside the global chord vector ("
					<< chordFrets.size() << " templates)";
				error = message.str();
				return false;
			}
			const auto& frets = chordFrets[static_cast<size_t>(record.chordId)];
			// A linked chord continuation repeats its chord; the name shows once.
			if ((record.mask & NOTE_MASK_CHILD) == 0 && static_cast<size_t>(record.chordId) < chordNames.size())
			{
				const std::string& name = chordNames[static_cast<size_t>(record.chordId)];
				const size_t length = (std::min)(name.size(), sizeof(event.chordName) - 1);
				std::copy_n(name.begin(), length, event.chordName);
				event.chordName[length] = '\0';
			}
			for (uint8_t stringIndex = 0; stringIndex < 6; ++stringIndex)
				if (frets[stringIndex] < 0x1A)
					AddPosition(event, stringIndex, frets[stringIndex], record);
			return true;
		}

		static void AddPosition(ResearchProtocol::PreviewEvent& event, int stringIndex, int fret, const Record& record)
		{
			if (event.positionCount >= 6) return;
			auto& position = event.positions[event.positionCount++];
			position.stringIndex = stringIndex;
			position.fret = fret;
			const uint32_t mask = record.mask;
			if ((mask & NoteByNote::NOTE_MASK_HAMMER_ON) != 0) position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_HAMMER_ON;
			if ((mask & NoteByNote::NOTE_MASK_PULL_OFF) != 0) position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_PULL_OFF;
			if ((mask & NoteByNote::NOTE_MASK_TAP) != 0) position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_TAP;
			if (NoteByNote::ChordAttackGate::IsFretHandMuted(mask)) position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_MUTED;
			if ((mask & NOTE_MASK_HARMONIC) != 0) position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_HARMONIC;
			if ((mask & NOTE_MASK_PALM_MUTE) != 0) position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_PALM_MUTE;
			if ((mask & NOTE_MASK_PINCH_HARMONIC) != 0) position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_PINCH_HARMONIC;
			if ((mask & NOTE_MASK_VIBRATO) != 0) position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_VIBRATO;
			if ((mask & NOTE_MASK_TREMOLO) != 0) position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_TREMOLO;
			if ((mask & NOTE_MASK_ACCENT) != 0) position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_ACCENT;
			if ((mask & NOTE_MASK_CHILD) != 0) position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_TIED;
			position.sustain = record.sustain;
			// Pitched slides carry their target fret; an unpitched slide has none
			// (its target field is not proven) and is drawn as a falling line.
			if ((mask & NOTE_MASK_SLIDE) != 0 && (mask & NOTE_MASK_SLIDE_UNPITCHED) == 0 && record.slideTo < 0x1A)
			{
				position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_SLIDE;
				position.slideTargetFret = record.slideTo;
			}
			else if ((mask & NOTE_MASK_SLIDE_UNPITCHED) != 0)
			{
				position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_SLIDE;
				position.slideTargetFret = -1;
			}
			if ((mask & NOTE_MASK_BEND) != 0)
			{
				position.techniques |= ResearchProtocol::TAB_PREVIEW_TECHNIQUE_BEND;
				if (std::isfinite(record.bendAmount))
					position.bendSemitones = static_cast<int32_t>(std::lround(record.bendAmount));
				position.bendStart = record.bendStart;
				position.bendPeak = record.bendPeak;
				position.bendEnd = record.bendEnd;
			}
		}
	};

	template <typename Reader>
	bool ReadAuthoredTablatureIdentity(uintptr_t container, const Reader& read, uint64_t& signature, std::string& error)
	{
		return AuthoredTablatureReader::ReadAuthoredTablatureIdentity(container, read, signature, error);
	}

	template <typename Reader>
	bool ReadCurrentIterationLevels(uintptr_t owner, const Reader& read, std::vector<int32_t>& levels, std::string& error)
	{
		return AuthoredTablatureReader::ReadCurrentIterationLevels(owner, read, levels, error);
	}

	template <typename Reader>
	bool ReadMeasureStarts(uintptr_t container, const Reader& read, std::vector<std::pair<float, int32_t>>& measures, std::string& error)
	{
		return AuthoredTablatureReader::ReadMeasureStarts(container, read, measures, error);
	}

	template <typename Reader>
	bool ReadSections(uintptr_t container, const Reader& read, std::vector<float>& starts, std::vector<std::string>& names, std::string& error)
	{
		return AuthoredTablatureReader::ReadSections(container, read, starts, names, error);
	}

	template <typename Reader>
	bool ReadSectionStarts(uintptr_t container, const Reader& read, std::vector<float>& starts, std::string& error)
	{
		return AuthoredTablatureReader::ReadSectionStarts(container, read, starts, error);
	}

	template <typename Reader>
	bool BuildAuthoredTablatureEvents(
		uintptr_t container,
		const Reader& read,
		std::vector<ResearchProtocol::PreviewEvent>& events,
		std::string& error,
		const TablatureLevelRequest& request = {},
		TablatureLevelResult* result = nullptr)
	{
		return AuthoredTablatureReader::BuildAuthoredTablatureEvents(container, read, events, error, request, result);
	}
}
