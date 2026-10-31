#include "TablatureRenderer.hpp"

// Classic guitar tablature: six tightly spaced string lines, small fret numbers
// sitting on their string (the line is knocked out behind them), small h/p slurs
// just above the two notes they join, and horizontal spacing that follows song
// time so a fast run reads as a tight cluster and a rest as a gap. Bar lines
// (from the chart's beat grid), section lines and the game's loop range give the
// eye an anchor; notes already played are dimmed and the current note sits on a
// playhead a quarter of the way in, or the loop starts at the left edge when the
// whole loop fits. The staff stretches a little toward the window's height and is
// centred, so no empty band under the bottom string makes a note read as one
// string lower; a taller window adds rows of bars (page mode) instead.

#include "Log.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <utility>

namespace NoteByNote
{
	namespace Tablature
	{
		namespace
		{
			struct CachedPosition
			{
				int stringIndex = 0;
				int fret = 0;
				std::uint32_t techniques = 0;
				int bendSemitones = 0;
				int slideTargetFret = -1;
				float sustain = 0.0f;
				float bendStart = 0.0f;
				float bendPeak = 0.0f;
				float bendEnd = 0.0f;
				float labelWidth = 0.0f;
				char label[16] = {};
			};

			// A mark drawn above the staff. Marks are stacked in lanes, as engraving
			// libraries do (VexFlow's modifier context), so a bend label, a vibrato
			// line and an accent never sit on each other or on the top string's
			// numbers. left / right are relative to the event's x.
			enum MarkKind : std::uint8_t { MarkBend, MarkVibrato, MarkTremolo, MarkAccent, MarkTap, MarkPinch };
			struct Mark
			{
				MarkKind kind = MarkBend;
				std::uint8_t position = 0;  // the bent position, for MarkBend
				std::uint8_t lane = 0;
				float left = 0.0f;
				float right = 0.0f;
			};

			struct CachedEvent
			{
				std::uint64_t id = 0;
				float time = 0.0f;
				float x = 0.0f;
				float width = 0.0f;
				std::vector<CachedPosition> positions;
				std::vector<Mark> marks;
				std::string chordName;
				bool showChordName = false;
				bool slurOnTop = false;  // an h / p slur over the top string
			};

			struct Metrics
			{
				float number = 13.0f;    // fret number font size
				float caption = 11.0f;     // h / p / bend / loop label font size
				float spacing = 14.0f;   // distance between string lines
				float above = 30.0f;     // room above the top string for slurs and bends
				float below = 18.0f;     // room below the bottom string for P.M. and the loop label
				float palmMute = 8.0f;   // bottom string to the top of the P.M. row
				float maxSpacing = 21.0f; // the most Draw stretches the strings apart
				float laneBase = 26.0f;   // top string to the bottom of the first mark lane
				float laneHeight = 14.0f; // one lane of marks above the staff
				float gutter = 26.0f;    // fixed column for the tuning labels
				float pixelsPerSecond = 120.0f;
			};

			struct LayoutCache
			{
				std::uint64_t generation = std::numeric_limits<std::uint64_t>::max();
				const ImFont* font = nullptr;
				float textScale = 0.0f;
				float contentWidth = 0.0f;
				std::vector<CachedEvent> events;
				std::unordered_map<std::uint64_t, std::size_t> eventById;
				int lanes = 0;               // mark lanes the song uses
				bool hasChordNames = false;  // a lane for chord names
				bool topSlurs = false;       // h / p slurs over the top string
			};

			struct ViewState
			{
				std::uint64_t targetId = 0;
				bool followCurrent = true;
				bool hasScrollSample = false;
				float lastScroll = 0.0f;
				std::uint64_t generation = std::numeric_limits<std::uint64_t>::max();
				std::uint64_t arrangementKey = 0;
				bool wasOpen = false;
				// Song time at the left edge while peeking. A level change rebuilds
				// the layout, so the view is re-anchored here to keep the reader's
				// place (comparing 100% against the current level relies on this).
				float anchorTime = -1.0f;
				bool pendingAnchor = false;
				// Page mode: drag distance not yet turned into whole rows.
				float dragRows = 0.0f;
			};

			ViewState viewState;
			const ImVec2 TabWindowPadding(10.0f, 6.0f);
			std::uint64_t validationGeneration = std::numeric_limits<std::uint64_t>::max();

			float ClampScale(float textScale)
			{
				return std::max(0.5f, std::min(textScale, 3.0f));
			}

			int ClampStringCount(int stringCount)
			{
				return std::max(4, std::min(stringCount, 6));
			}

			LayoutCache layoutCache;
			Metrics metrics;

			// Top string to the bottom of the first mark lane. An h / p slur over the
			// top string, with its letter, needs the full room; without one the
			// lanes start just over the top string's numbers.
			float LaneBaseFor(const Metrics& m, bool topSlurs)
			{
				return topSlurs ? m.number * 1.05f + m.caption + 1.0f : m.number * 0.6f + 2.0f;
			}

			// Room above the top string for `lanes` lanes (marks plus chord names).
			float AboveFor(const Metrics& m, int lanes)
			{
				return m.laneBase + m.laneHeight * static_cast<float>(lanes) + m.caption + 6.0f;
			}

			// Room above the top string for the lanes the whole song uses.
			float AboveHeight(const Metrics& m)
			{
				return AboveFor(m, layoutCache.lanes + (layoutCache.hasChordNames ? 1 : 0));
			}

			// Top of a lane's text; lane 0 is the lowest.
			float LaneTop(float staffTop, int lane)
			{
				return staffTop - metrics.laneBase - metrics.laneHeight * static_cast<float>(lane + 1);
			}

			Metrics MakeMetrics(float scale)
			{
				const float base = ImGui::GetFont()->FontSize * scale;
				Metrics m;
				m.number = std::max(11.0f, base * 0.85f);
				m.caption = std::max(11.0f, base * 0.65f);
				m.spacing = m.number * 1.1f;   // the minimum; Draw stretches it to the window
				// Kept low so extra window height becomes another row of bars rather
				// than wider gaps between the strings.
				m.maxSpacing = m.number * 1.5f;
				// Above the staff, bottom to top: the h / p slurs of the top string,
				// the mark lanes, a chord-name lane and the measure-number band.
				m.laneBase = LaneBaseFor(m, layoutCache.topSlurs);
				m.laneHeight = m.caption + 3.0f;
				m.above = AboveHeight(m);
				m.palmMute = m.number * 0.6f;
				m.below = m.palmMute + m.caption * 2.0f + 8.0f * scale;
				m.gutter = m.number * 1.6f;
				m.pixelsPerSecond = 120.0f * scale;
				return m;
			}

			float Measure(float size, const char* text)
			{
				return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
			}

			void Text(float size, const ImVec2& position, ImU32 color, const char* text)
			{
				ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), size, position, color, text);
			}

			ImU32 WithAlpha(ImU32 color, std::uint32_t alpha)
			{
				return (color & 0x00FFFFFFu) | (alpha << 24);
			}

			// The player's highlight colour (0xRRGGBB, 0 = the overlay accent) for this
			// frame's drawing: the current-note pill, its marks and the loop band.
			std::uint32_t highlightRgb = 0;

			ImU32 HighlightColor()
			{
				if (highlightRgb == 0) return ImGui::GetColorU32(ImGuiCol_PlotHistogram);
				return IM_COL32((highlightRgb >> 16) & 0xFF, (highlightRgb >> 8) & 0xFF, highlightRgb & 0xFF, 255);
			}

			// White or black, whichever reads better on the pill (WCAG contrast), so a
			// light highlight such as yellow still shows its fret number.
			ImU32 OnAccent(ImU32 accent)
			{
				const auto channel = [](std::uint32_t value) {
					const float c = static_cast<float>(value & 0xFF) / 255.0f;
					return c <= 0.03928f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
				};
				const float luminance = 0.2126f * channel(accent >> IM_COL32_R_SHIFT)
					+ 0.7152f * channel(accent >> IM_COL32_G_SHIFT) + 0.0722f * channel(accent >> IM_COL32_B_SHIFT);
				const bool black = (luminance + 0.05f) / 0.05f > 1.05f / (luminance + 0.05f);
				return black ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
			}

			void ReportInvalidSnapshot(const Snapshot& snapshot)
			{
				if (validationGeneration == snapshot.generation) return;
				validationGeneration = snapshot.generation;
				if (snapshot.stringCount < 4 || snapshot.stringCount > 6)
				{
					LOG_ERROR("[NoteByNote] Tablature snapshot has invalid string count " << snapshot.stringCount << "." << std::endl);
					return;
				}
				for (const Event& event : snapshot.events)
					for (const Position& position : event.positions)
						if (position.stringIndex < 0 || position.stringIndex >= snapshot.stringCount)
						{
							LOG_ERROR("[NoteByNote] Tablature snapshot contains an invalid string index "
								<< position.stringIndex << " in event " << event.id << "." << std::endl);
							return;
						}
			}

			float TimeToX(float time);

			// "1/2", "full", "1 1/2": a bend in whole tones, to the nearest quarter.
			void BendText(float semitones, char* text, std::size_t size)
			{
				const int quarters = std::max(1, static_cast<int>(std::lround(semitones * 2.0f)));
				const int whole = quarters / 4;
				const int rest = quarters % 4;
				static const char* const fractions[] = { "", "1/4", "1/2", "3/4" };
				if (rest == 0) std::snprintf(text, size, whole == 1 ? "full" : "%d", whole);
				else if (whole == 0) std::snprintf(text, size, "%s", fractions[rest]);
				else std::snprintf(text, size, "%d %s", whole, fractions[rest]);
			}

			// What a bend draws: the label at its top (0 = no label) and whether it
			// rises from the note (a pre-bend rises straight up), falls back after
			// the top (a release), or only falls (a tied note released).
			struct BendShape
			{
				bool rise = false;
				bool preBend = false;
				bool release = false;
				float label = 0.0f;
				float endLabel = 0.0f;
			};

			BendShape ShapeOf(const CachedPosition& position)
			{
				BendShape shape;
				const float peak = position.bendPeak > 0.0f ? position.bendPeak : static_cast<float>(std::max(1, position.bendSemitones));
				if ((position.techniques & Tied) != 0)
				{
					// Continuing a bend: only a release is drawn; a held bend is just the bracket.
					shape.release = position.bendStart > position.bendEnd + 0.2f;
					shape.endLabel = position.bendEnd > 0.2f ? position.bendEnd : 0.0f;
					return shape;
				}
				shape.rise = true;
				shape.preBend = position.bendStart > 0.2f;
				shape.label = shape.preBend ? position.bendStart : peak;
				shape.release = position.bendPeak > 0.0f && position.bendEnd < peak - 0.2f;
				shape.endLabel = shape.release && position.bendEnd > 0.2f ? position.bendEnd : 0.0f;
				return shape;
			}

			// Bend geometry relative to the event x: where the arrow leaves the
			// number, the top's x and, for a release, where it lands.
			struct BendGeometry { float start = 0.0f; float tip = 0.0f; float land = 0.0f; };
			BendGeometry GeometryOf(const CachedPosition& position, const BendShape& shape)
			{
				BendGeometry geometry;
				geometry.start = position.labelWidth * 0.5f + 2.0f;
				geometry.tip = shape.rise ? geometry.start + metrics.number * (shape.preBend ? 0.3f : 0.5f) : geometry.start;
				geometry.land = geometry.tip + metrics.number * 0.7f;
				return geometry;
			}

			// Builds each event's marks and stacks them in lanes: a mark takes the
			// lowest lane that is clear of every earlier mark, so neighbours rise
			// above each other only where they would touch. Chord names are shown
			// when the chord changes or a new bar starts, where they fit.
			void AssignMarks(const Snapshot& snapshot)
			{
				constexpr int MaxLanes = 4;
				float laneRight[MaxLanes];
				for (float& right : laneRight) right = -FLT_MAX;
				const float gap = metrics.caption * 0.4f;
				const float size = metrics.number;
				int used = 0;
				auto& events = layoutCache.events;
				for (std::size_t eventIndex = 0; eventIndex < events.size(); ++eventIndex)
				{
					CachedEvent& event = events[eventIndex];
					event.marks.clear();
					std::uint32_t all = 0;
					float sustain = 0.0f;
					for (std::size_t index = 0; index < event.positions.size(); ++index)
					{
						const CachedPosition& position = event.positions[index];
						all |= position.techniques;
						sustain = std::max(sustain, position.sustain);
						if ((position.techniques & Bend) == 0) continue;
						const BendShape shape = ShapeOf(position);
						if (!shape.rise && !shape.release) continue;
						const BendGeometry geometry = GeometryOf(position, shape);
						char text[16] = {};
						float right = shape.release ? geometry.land + 3.0f : geometry.tip + 3.0f;
						float left = geometry.start;
						if (shape.label > 0.0f)
						{
							BendText(shape.label, text, sizeof(text));
							const float half = Measure(metrics.caption, text) * 0.5f;
							left = std::min(left, geometry.tip - half);
							right = std::max(right, geometry.tip + half);
						}
						if (shape.endLabel > 0.0f)
						{
							BendText(shape.endLabel, text, sizeof(text));
							right = std::max(right, geometry.land + Measure(metrics.caption, text) * 0.5f);
						}
						event.marks.push_back({ MarkBend, static_cast<std::uint8_t>(index), 0, left, right });
					}
					if ((all & Vibrato) != 0)
					{
						// Over the note's sustain, at least a short squiggle, at most 4 s.
						const float end = TimeToX(event.time + std::min(std::max(sustain, 0.0f), 4.0f)) - event.x;
						event.marks.push_back({ MarkVibrato, 0, 0, -event.width * 0.5f, std::max(end, size * 1.2f) });
					}
					if ((all & Tremolo) != 0) event.marks.push_back({ MarkTremolo, 0, 0, -size * 0.4f, size * 0.4f });
					if ((all & Accent) != 0) event.marks.push_back({ MarkAccent, 0, 0, -size * 0.35f, size * 0.35f });
					if ((all & Tap) != 0)
					{
						const float half = Measure(metrics.caption, "T") * 0.5f;
						event.marks.push_back({ MarkTap, 0, 0, -half, half });
					}
					if ((all & PinchHarmonic) != 0)
					{
						const float half = Measure(metrics.caption, "PH") * 0.5f;
						event.marks.push_back({ MarkPinch, 0, 0, -half, half });
					}
					for (Mark& mark : event.marks)
					{
						const float left = event.x + mark.left;
						int lane = -1;
						for (int index = 0; index < MaxLanes && lane < 0; ++index)
							if (laneRight[index] + gap <= left) lane = index;
						if (lane < 0)
						{
							lane = 0;
							for (int index = 1; index < MaxLanes; ++index)
								if (laneRight[index] < laneRight[lane]) lane = index;
						}
						mark.lane = static_cast<std::uint8_t>(lane);
						laneRight[lane] = std::max(laneRight[lane], event.x + mark.right);
						used = std::max(used, lane + 1);
					}
				}
				layoutCache.lanes = used;

				std::string lastName;
				float lastRight = -FLT_MAX;
				std::size_t bar = 0;
				float lastNameTime = -FLT_MAX;
				for (CachedEvent& event : events)
				{
					if (event.chordName.empty()) continue;
					bool newBar = false;
					while (bar < snapshot.bars.size() && snapshot.bars[bar].first <= event.time + 0.01f)
					{
						newBar = newBar || snapshot.bars[bar].first > lastNameTime + 0.01f;
						++bar;
					}
					const float half = Measure(metrics.caption, event.chordName.c_str()) * 0.5f;
					if ((event.chordName != lastName || newBar) && event.x - half >= lastRight + gap)
					{
						event.showChordName = true;
						layoutCache.hasChordNames = true;
						lastName = event.chordName;
						lastRight = event.x + half;
						lastNameTime = event.time;
					}
				}
			}

			// Places events by song time, never closer than their labels allow. A
			// dense run is pushed apart and the notes after it fall back onto the
			// time grid as soon as a rest leaves room.
			void RebuildLayout(const Snapshot& snapshot, float textScale)
			{
				layoutCache = {};
				layoutCache.generation = snapshot.generation;
				layoutCache.font = ImGui::GetFont();
				layoutCache.textScale = textScale;

				std::vector<std::size_t> order(snapshot.events.size());
				for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
				std::stable_sort(order.begin(), order.end(), [&snapshot](std::size_t left, std::size_t right)
				{
					return snapshot.events[left].time < snapshot.events[right].time;
				});

				const int stringCount = ClampStringCount(snapshot.stringCount);
				const float origin = metrics.gutter + metrics.number;
				const float firstTime = order.empty() ? 0.0f : snapshot.events[order.front()].time;
				struct LastOnString
				{
					bool valid = false;
					std::uint32_t techniques = 0;
					int slideTarget = -1;
					int fret = -1;
					float end = 0.0f;  // time + sustain
					float bendEnd = 0.0f;
				};
				LastOnString lastOnString[6];
				bool hasPrevious = false;
				float previousX = 0.0f;
				float previousHalf = 0.0f;
				for (const std::size_t eventIndex : order)
				{
					const Event& event = snapshot.events[eventIndex];
					CachedEvent cached;
					cached.id = event.id;
					cached.time = event.time;
					cached.chordName = event.chordName;
					bool bend = false;
					bool release = false;
					for (const Position& position : event.positions)
					{
						if (position.stringIndex < 0 || position.stringIndex >= stringCount) continue;
						CachedPosition entry;
						entry.stringIndex = position.stringIndex;
						entry.fret = position.fret;
						entry.techniques = position.techniques;
						entry.bendSemitones = position.bendSemitones;
						entry.slideTargetFret = position.slideTargetFret;
						entry.sustain = position.sustain;
						entry.bendStart = position.bendStart;
						entry.bendPeak = position.bendPeak;
						entry.bendEnd = position.bendEnd;
						// A tied note is bracketed, as in printed tab, except where a
						// slide lands on it: "5/7" shows the slide's target plainly.
						bool bracket = false;
						const LastOnString& last = lastOnString[position.stringIndex];
						if ((position.techniques & Tied) != 0)
							bracket = !(last.valid && (last.techniques & Slide) != 0 && last.slideTarget == position.fret);
						// A bend held into the next note: charts split a long bend into a
						// bend up and a note that starts already bent ([0:2]) on the same
						// fret, right where the first one ends. That is a hold, not a new
						// pre-bend, so it is drawn like a tied note.
						if ((position.techniques & (Bend | Tied)) == Bend && last.valid && (last.techniques & Bend) != 0
							&& last.fret == position.fret && std::fabs(last.end - event.time) < 0.03f
							&& last.bendEnd > 0.2f && std::fabs(position.bendStart - last.bendEnd) < 0.25f)
						{
							entry.techniques |= Tied;
							bracket = true;
						}
						const int fret = std::max(0, position.fret);
						if ((position.techniques & Muted) != 0) std::snprintf(entry.label, sizeof(entry.label), bracket ? "(x)" : "x");
						else if ((position.techniques & Harmonic) != 0) std::snprintf(entry.label, sizeof(entry.label), bracket ? "(<%d>)" : "<%d>", fret);
						else std::snprintf(entry.label, sizeof(entry.label), bracket ? "(%d)" : "%d", fret);
						if (!bracket && (entry.techniques & Tied) != 0) entry.techniques &= ~static_cast<std::uint32_t>(Tied);
						release = release || ((position.techniques & Bend) != 0 && position.bendEnd < position.bendPeak - 0.2f);
						entry.labelWidth = Measure(metrics.number, entry.label);
						cached.width = std::max(cached.width, entry.labelWidth);
						bend = bend || (position.techniques & (Bend | Slide)) != 0;
						cached.slurOnTop = cached.slurOnTop
							|| (entry.stringIndex == stringCount - 1 && (entry.techniques & (HammerOn | PullOff)) != 0);
						cached.positions.push_back(entry);
					}
					if (cached.positions.empty()) continue;
					layoutCache.topSlurs = layoutCache.topSlurs || cached.slurOnTop;
					for (const CachedPosition& entry : cached.positions)
						lastOnString[entry.stringIndex] = { true, entry.techniques, entry.slideTargetFret, entry.fret,
							event.time + std::max(0.0f, entry.sustain), (entry.techniques & Bend) != 0 ? entry.bendEnd : 0.0f };
					// A bend arrow sits to the right of its number and needs the room;
					// a release adds its falling arrow.
					const float half = cached.width * 0.5f + (bend ? metrics.number * 0.7f : 0.0f)
						+ (release ? metrics.number * 0.7f : 0.0f);
					const float timeX = origin + (event.time - firstTime) * metrics.pixelsPerSecond;
					cached.x = hasPrevious
						? std::max(timeX, previousX + previousHalf + half + metrics.number * 0.55f)
						: origin;
					hasPrevious = true;
					previousX = cached.x;
					previousHalf = half;
					layoutCache.events.push_back(std::move(cached));
					layoutCache.eventById[layoutCache.events.back().id] = layoutCache.events.size() - 1;
				}
				layoutCache.contentWidth = layoutCache.events.empty()
					? origin + metrics.number * 4.0f
					: layoutCache.events.back().x + metrics.number * 4.0f;
				AssignMarks(snapshot);
			}

			void EnsureLayout(const Snapshot& snapshot, float textScale)
			{
				if (layoutCache.generation != snapshot.generation
					|| layoutCache.font != ImGui::GetFont()
					|| layoutCache.textScale != textScale)
					RebuildLayout(snapshot, textScale);
			}

			const CachedEvent* FindTarget(std::uint64_t targetId)
			{
				const auto found = layoutCache.eventById.find(targetId);
				return found != layoutCache.eventById.end() ? &layoutCache.events[found->second] : nullptr;
			}

			// Song time to content x, interpolating between the placed events.
			float TimeToX(float time)
			{
				const auto& events = layoutCache.events;
				if (events.empty()) return metrics.gutter;
				if (time <= events.front().time) return events.front().x - (events.front().time - time) * metrics.pixelsPerSecond;
				if (time >= events.back().time) return events.back().x + (time - events.back().time) * metrics.pixelsPerSecond;
				const auto next = std::lower_bound(events.begin(), events.end(), time,
					[](const CachedEvent& event, float value) { return event.time < value; });
				const auto& right = *next;
				const auto& left = *(next - 1);
				const float span = right.time - left.time;
				const float fraction = span > 0.0f ? (time - left.time) / span : 0.0f;
				return left.x + (right.x - left.x) * fraction;
			}

			std::size_t VisibleStart(float left)
			{
				return static_cast<std::size_t>(std::lower_bound(layoutCache.events.begin(), layoutCache.events.end(), left,
					[](const CachedEvent& event, float value) { return event.x < value; }) - layoutCache.events.begin());
			}

			std::size_t VisibleEnd(float right)
			{
				return static_cast<std::size_t>(std::upper_bound(layoutCache.events.begin(), layoutCache.events.end(), right,
					[](float value, const CachedEvent& event) { return value < event.x; }) - layoutCache.events.begin());
			}

			float StringY(float staffTop, int stringCount, int stringIndex)
			{
				return staffTop + static_cast<float>(stringCount - 1 - stringIndex) * metrics.spacing;
			}

			// The previous note on the same string, if it is close enough for a
			// slur to read as joining the two (a hammer-on after a rest does not).
			const CachedPosition* FindSlurStart(std::size_t eventIndex, int stringIndex, const CachedEvent** owner)
			{
				const CachedEvent& current = layoutCache.events[eventIndex];
				std::size_t steps = 0;
				for (std::size_t index = eventIndex; index-- > 0 && steps < 6; ++steps)
				{
					const CachedEvent& previous = layoutCache.events[index];
					if (current.time - previous.time > 1.5f) return nullptr;
					for (const CachedPosition& position : previous.positions)
						if (position.stringIndex == stringIndex)
						{
							*owner = &previous;
							return &position;
						}
				}
				return nullptr;
			}

			// Page mode justifies each row like printed tab: content x past the row's
			// left edge is spread by `scale` so every row ends at the same right edge.
			// Fret numbers and marks keep their size; only positions move. Identity
			// (left 0, scale 1) in the single-row view.
			struct RowTransform
			{
				float left = 0.0f;
				float scale = 1.0f;
			};
			RowTransform rowTransform;

			// Content x to screen x.
			float SX(const ImVec2& origin, float x)
			{
				return origin.x + rowTransform.left + (x - rowTransform.left) * rowTransform.scale;
			}

			// Learn a Song reports the whole song as its range; only a real loop counts.
			bool LoopIsPartial(const Snapshot& snapshot)
			{
				const auto& events = layoutCache.events;
				return snapshot.loopValid && !events.empty()
					&& (snapshot.loopStart > events.front().time + 0.05f || snapshot.loopEnd < events.back().time - 0.05f);
			}

			// "chorus" -> "Chorus", "noguitar" (any case, as the game checks) -> "No guitar".
			std::string SectionLabel(const std::string& name)
			{
				std::string lower = name;
				for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				if (lower == "noguitar") return "No guitar";
				std::string label = name;
				if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
				return label;
			}

			void DrawLoopAndSections(const Snapshot& snapshot, const ImVec2& origin, float staffTop, float staffBottom,
				float visibleLeft, float visibleRight, ImU32 lineColor, ImU32 accent)
			{
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const auto& events = layoutCache.events;
				if (events.empty()) return;
				if (LoopIsPartial(snapshot))
				{
					const float left = TimeToX(snapshot.loopStart);
					const float right = TimeToX(snapshot.loopEnd);
					if (right > visibleLeft && left < visibleRight)
					{
						// The band covers everything drawn for the notes inside it (bends,
						// slurs and bar numbers above the top string, the current-note pill),
						// so no mark is cut by its edge. No vertical edge lines: at the loop
						// start they doubled up with the bar and section lines.
						const float top = origin.y + staffTop - metrics.above + 1.0f;
						const float bottom = origin.y + staffBottom + metrics.palmMute + metrics.caption + 3.0f;
						drawList->AddRectFilled(ImVec2(SX(origin, left), top), ImVec2(SX(origin, right), bottom), WithAlpha(accent, 0x22));
						drawList->AddRectFilled(ImVec2(SX(origin, left), top), ImVec2(SX(origin, right), top + 3.0f), WithAlpha(accent, 0xB0));
						const float labelX = SX(origin, std::max(left, visibleLeft)) + 4.0f;
						Text(metrics.caption, ImVec2(labelX, bottom + 2.0f), WithAlpha(accent, 0xE0), "Loop");
					}
				}
				// Bar lines with the measure number above, thinned out where bars are
				// close together; section starts get a heavier line and their name.
				// Numbers and names share the top band, above the chord names and
				// mark lanes, so nothing drawn for the notes runs into them.
				const float barNumberY = origin.y + staffTop - metrics.above + 3.0f;
				const ImU32 barColor = WithAlpha(lineColor, 0xC0);
				const ImU32 nameColor = ImGui::GetColorU32(ImGuiCol_Text);
				// Section names first, each after the number of the bar it starts on
				// ("102 Interlude"); other bar numbers give way to the names.
				std::vector<std::pair<float, float>> nameSpans;
				std::vector<std::size_t> namedBars;
				float lastNameRight = -FLT_MAX;
				for (std::size_t index = 0; index < snapshot.sectionStarts.size() && index < snapshot.sectionNames.size(); ++index)
				{
					const std::string name = SectionLabel(snapshot.sectionNames[index]);
					if (name.empty()) continue;
					const float start = snapshot.sectionStarts[index];
					const float x = TimeToX(start);
					std::size_t barIndex = snapshot.bars.size();
					for (std::size_t bar = 0; bar < snapshot.bars.size(); ++bar)
						if (std::fabs(snapshot.bars[bar].first - start) < 0.05f) { barIndex = bar; break; }
					char number[16] = {};
					if (barIndex < snapshot.bars.size()) std::snprintf(number, sizeof(number), "%d ", snapshot.bars[barIndex].second + 1);
					const float numberWidth = number[0] != '\0' ? Measure(metrics.caption, number) : 0.0f;
					const float width = numberWidth + Measure(metrics.caption, name.c_str());
					if (x + width < visibleLeft - 2.0f || x > visibleRight + 2.0f) continue;
					const float screenX = SX(origin, x) + 2.0f;
					if (screenX < lastNameRight + metrics.caption * 0.5f) continue;
					if (number[0] != '\0')
					{
						Text(metrics.caption, ImVec2(screenX, barNumberY), lineColor, number);
						namedBars.push_back(barIndex);
					}
					Text(metrics.caption, ImVec2(screenX + numberWidth, barNumberY), nameColor, name.c_str());
					nameSpans.emplace_back(screenX, screenX + width);
					lastNameRight = screenX + width;
				}
				float lastNumberRight = -FLT_MAX;
				for (std::size_t barIndex = 0; barIndex < snapshot.bars.size(); ++barIndex)
				{
					const auto& bar = snapshot.bars[barIndex];
					const float x = TimeToX(bar.first);
					if (x < visibleLeft - 2.0f || x > visibleRight + 2.0f) continue;
					const float screenX = SX(origin, x);
					drawList->AddLine(ImVec2(screenX, origin.y + staffTop), ImVec2(screenX, origin.y + staffBottom), barColor, 1.0f);
					if (std::find(namedBars.begin(), namedBars.end(), barIndex) != namedBars.end()) continue;
					char number[16] = {};
					std::snprintf(number, sizeof(number), "%d", bar.second + 1);
					const float width = Measure(metrics.caption, number);
					const float left = screenX + 2.0f;
					bool clear = left > lastNumberRight + metrics.caption;
					for (const auto& span : nameSpans)
						clear = clear && (left + width < span.first - 3.0f || left > span.second + 3.0f);
					if (clear)
					{
						Text(metrics.caption, ImVec2(left, barNumberY), lineColor, number);
						lastNumberRight = left + width;
					}
				}
				for (const float start : snapshot.sectionStarts)
				{
					const float x = TimeToX(start);
					if (x < visibleLeft - 2.0f || x > visibleRight + 2.0f) continue;
					drawList->AddLine(ImVec2(SX(origin, x), origin.y + staffTop), ImVec2(SX(origin, x), origin.y + staffBottom), lineColor, 2.5f);
				}
			}

			// A slide is the usual tab stroke between two numbers: "/" up to a higher
			// fret, "\" down to a lower one, ending at the next note on the string
			// when it is the slide's target. An unpitched slide (no target) is a short
			// falling stroke off the note.
			void DrawSlide(std::size_t eventIndex, const CachedPosition& position, float left, float y, ImU32 color, const ImVec2& origin)
			{
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const float size = metrics.number;
				const float startX = left + position.labelWidth + 2.0f;
				const float rise = size * 0.35f;
				if (position.slideTargetFret < 0)
				{
					drawList->AddLine(ImVec2(startX, y - rise), ImVec2(startX + size * 0.7f, y + rise), color, 1.3f);
					return;
				}
				const bool up = position.slideTargetFret > position.fret;
				float endX = startX + size * 0.7f;
				const CachedEvent& current = layoutCache.events[eventIndex];
				for (std::size_t index = eventIndex + 1, steps = 0; index < layoutCache.events.size() && steps < 6; ++index, ++steps)
				{
					const CachedEvent& next = layoutCache.events[index];
					if (next.time - current.time > 3.0f) break;
					bool found = false;
					for (const CachedPosition& candidate : next.positions)
						if (candidate.stringIndex == position.stringIndex)
						{
							if (candidate.fret == position.slideTargetFret)
								endX = std::max(startX + 4.0f, SX(origin, next.x) - candidate.labelWidth * 0.5f - 2.0f);
							found = true;
							break;
						}
					if (found) break;
				}
				drawList->AddLine(ImVec2(startX, up ? y + rise : y - rise), ImVec2(endX, up ? y - rise : y + rise), color, 1.3f);
			}

			bool IsPalmMuted(const CachedEvent& event)
			{
				for (const CachedPosition& position : event.positions)
					if ((position.techniques & PalmMute) != 0) return true;
				return false;
			}

			// Palm mutes as in printed tab: "P.M." under the first muted note and a
			// dashed line over the rest of the run, closed by a short tick. A gap of
			// more than a second, or one unmuted note, ends the run.
			// ownLeft / ownRight: in page mode, only events whose x is in
			// [ownLeft, ownRight) belong to the row, so a note on the bar line that
			// ends one row is drawn once, at the start of the next.
			bool IsOwned(const CachedEvent& event, float ownLeft, float ownRight)
			{
				return event.x >= ownLeft - 0.5f && event.x < ownRight - 0.5f;
			}

			void DrawPalmMutes(const ImVec2& origin, float staffBottom, float visibleLeft, float visibleRight,
				const CachedEvent* target, ImU32 normal, ImU32 played, float ownLeft = -FLT_MAX, float ownRight = FLT_MAX)
			{
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const auto& events = layoutCache.events;
				const auto Muted = [ownLeft, ownRight](const CachedEvent& event)
				{
					return IsPalmMuted(event) && IsOwned(event, ownLeft, ownRight);
				};
				const float margin = metrics.number * 3.0f;
				std::size_t index = VisibleStart(visibleLeft - margin);
				while (index > 0 && index < events.size() && Muted(events[index]) && Muted(events[index - 1])
					&& events[index].time - events[index - 1].time <= 1.0f) --index;
				const std::size_t last = VisibleEnd(visibleRight + margin);
				const float targetTime = target != nullptr ? target->time : -FLT_MAX;
				const float textY = origin.y + staffBottom + metrics.palmMute;
				const float lineY = textY + metrics.caption * 0.55f;
				const float labelWidth = Measure(metrics.caption, "P.M.");
				while (index < last && index < events.size())
				{
					if (!Muted(events[index])) { ++index; continue; }
					const std::size_t start = index;
					while (index + 1 < events.size() && Muted(events[index + 1])
						&& events[index + 1].time - events[index].time <= 1.0f) ++index;
					const CachedEvent& first = events[start];
					const CachedEvent& end = events[index];
					const ImU32 color = end.time < targetTime ? played : normal;
					const float labelX = SX(origin, first.x) - first.width * 0.5f;
					Text(metrics.caption, ImVec2(labelX, textY), color, "P.M.");
					const float dashStart = labelX + labelWidth + 3.0f;
					const float dashEnd = SX(origin, end.x) + end.width * 0.5f;
					if (dashEnd > dashStart + 4.0f)
					{
						const float dash = metrics.caption * 0.4f;
						for (float x = dashStart; x < dashEnd; x += dash * 2.0f)
							drawList->AddLine(ImVec2(x, lineY), ImVec2(std::min(x + dash, dashEnd), lineY), color, 1.0f);
						drawList->AddLine(ImVec2(dashEnd, lineY - metrics.caption * 0.35f), ImVec2(dashEnd, lineY), color, 1.0f);
					}
					++index;
				}
			}

			void DrawNotes(const Snapshot& snapshot, const ImVec2& origin, float staffTop, float visibleLeft, float visibleRight,
				const CachedEvent* target, ImU32 normal, ImU32 played, ImU32 accent, ImU32 onAccent, ImU32 knockout,
				float ownLeft = -FLT_MAX, float ownRight = FLT_MAX)
			{
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const int stringCount = ClampStringCount(snapshot.stringCount);
				const float margin = metrics.number * 3.0f;
				const std::size_t first = VisibleStart(visibleLeft - margin);
				const std::size_t last = VisibleEnd(visibleRight + margin);
				const float targetTime = target != nullptr ? target->time : -FLT_MAX;
				const float size = metrics.number;
				for (std::size_t eventIndex = first; eventIndex < last; ++eventIndex)
				{
					const CachedEvent& event = layoutCache.events[eventIndex];
					if (!IsOwned(event, ownLeft, ownRight)) continue;
					const bool isTarget = target == &event;
					const bool isPlayed = !isTarget && event.time < targetTime;
					const ImU32 color = isPlayed ? played : normal;
					const float x = SX(origin, event.x);
					for (const CachedPosition& position : event.positions)
					{
						const float y = origin.y + StringY(staffTop, stringCount, position.stringIndex);
						const float left = x - position.labelWidth * 0.5f;
						const ImVec2 textPosition(left, y - size * 0.5f);
						if (isTarget)
						{
							const float pad = size * 0.3f;
							drawList->AddRectFilled(ImVec2(left - pad, y - size * 0.55f),
								ImVec2(left + position.labelWidth + pad, y + size * 0.55f), accent, size * 0.3f);
							Text(size, textPosition, onAccent, position.label);
						}
						else
						{
							drawList->AddRectFilled(ImVec2(left - 2.0f, y - size * 0.45f),
								ImVec2(left + position.labelWidth + 2.0f, y + size * 0.45f), knockout);
							Text(size, textPosition, color, position.label);
						}

						const ImU32 markColor = isTarget ? accent : color;
						if ((position.techniques & (HammerOn | PullOff)) != 0)
						{
							const CachedEvent* previousEvent = nullptr;
							const CachedPosition* previous = FindSlurStart(eventIndex, position.stringIndex, &previousEvent);
							if (previous != nullptr)
							{
								const float startX = SX(origin, previousEvent->x) + previous->labelWidth * 0.5f;
								const float endX = left;
								const float baseY = y - size * 0.6f;
								const float peakY = baseY - size * 0.45f;
								drawList->AddBezierQuadratic(ImVec2(startX, baseY), ImVec2((startX + endX) * 0.5f, peakY),
									ImVec2(endX, baseY), markColor, 1.0f);
								const char* letter = (position.techniques & HammerOn) != 0 ? "h" : "p";
								Text(metrics.caption, ImVec2((startX + endX - Measure(metrics.caption, letter)) * 0.5f,
									peakY - metrics.caption), markColor, letter);
							}
						}
						if ((position.techniques & Slide) != 0) DrawSlide(eventIndex, position, left, y, markColor, origin);
					}
				}
			}

			// The stacked marks above the staff and the chord names over them.
			// Bends start at their number on its string and rise to their lane.
			// chordLane: the lane for chord names; below 0, the one over every mark
			// lane of the song (in page mode, the one over the row's own lanes).
			void DrawAbove(const Snapshot& snapshot, const ImVec2& origin, float staffTop, float visibleLeft, float visibleRight,
				const CachedEvent* target, ImU32 normal, ImU32 played, ImU32 accent,
				float ownLeft = -FLT_MAX, float ownRight = FLT_MAX, int chordLane = -1)
			{
				if (chordLane < 0) chordLane = layoutCache.lanes;
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const int stringCount = ClampStringCount(snapshot.stringCount);
				const float margin = metrics.number * 3.0f;
				const std::size_t first = VisibleStart(visibleLeft - margin * 4.0f);
				const std::size_t last = VisibleEnd(visibleRight + margin);
				const float targetTime = target != nullptr ? target->time : -FLT_MAX;
				const float size = metrics.number;
				const float caption = metrics.caption;
				for (std::size_t eventIndex = first; eventIndex < last; ++eventIndex)
				{
					const CachedEvent& event = layoutCache.events[eventIndex];
					if (!IsOwned(event, ownLeft, ownRight)) continue;
					const bool isTarget = target == &event;
					const ImU32 color = isTarget ? accent : (event.time < targetTime ? played : normal);
					const float x = SX(origin, event.x);
					for (const Mark& mark : event.marks)
					{
						const float top = origin.y + LaneTop(staffTop, mark.lane);
						const float middle = top + caption * 0.5f;
						switch (mark.kind)
						{
						case MarkBend:
						{
							if (mark.position >= event.positions.size()) break;
							const CachedPosition& position = event.positions[mark.position];
							const BendShape shape = ShapeOf(position);
							const BendGeometry geometry = GeometryOf(position, shape);
							const float y = origin.y + StringY(staffTop, stringCount, position.stringIndex);
							const float startX = x + geometry.start;
							const float tipX = x + geometry.tip;
							const float tipY = top + caption + 2.0f;
							char text[16] = {};
							if (shape.rise)
							{
								if (shape.preBend)
									drawList->AddLine(ImVec2(tipX, y - size * 0.45f), ImVec2(tipX, tipY + 4.0f), color, 1.2f);
								else
									drawList->AddBezierQuadratic(ImVec2(startX, y), ImVec2(tipX, y), ImVec2(tipX, tipY + 4.0f), color, 1.2f);
								drawList->AddTriangleFilled(ImVec2(tipX, tipY), ImVec2(tipX - 3.0f, tipY + 5.0f), ImVec2(tipX + 3.0f, tipY + 5.0f), color);
								BendText(shape.label, text, sizeof(text));
								Text(caption, ImVec2(tipX - Measure(caption, text) * 0.5f, top), color, text);
							}
							if (shape.release)
							{
								// Back down from the top (or, tied, from where the bend was held).
								const float fromX = shape.rise ? tipX : startX;
								const float landX = x + geometry.land;
								const float landY = shape.endLabel > 0.0f ? top + caption + 6.0f : y - size * 0.55f;
								drawList->AddBezierQuadratic(ImVec2(fromX, tipY + 2.0f), ImVec2(landX, tipY + 2.0f), ImVec2(landX, landY - 4.0f), color, 1.2f);
								drawList->AddTriangleFilled(ImVec2(landX, landY), ImVec2(landX - 3.0f, landY - 5.0f), ImVec2(landX + 3.0f, landY - 5.0f), color);
								if (shape.endLabel > 0.0f)
								{
									BendText(shape.endLabel, text, sizeof(text));
									Text(caption, ImVec2(landX + 3.0f, top), color, text);
								}
							}
							break;
						}
						case MarkVibrato:
						{
							// A wavy line over the sustain.
							const float left = x + mark.left;
							const float right = SX(origin, event.x + mark.right);
							const float amplitude = caption * 0.22f;
							const float wave = caption * 0.55f;
							ImVec2 previous(left, middle);
							for (float px = left + wave * 0.25f; px <= right; px += wave * 0.25f)
							{
								const float phase = (px - left) / wave * 6.2831853f;
								const ImVec2 point(px, middle - std::sin(phase) * amplitude);
								drawList->AddLine(previous, point, color, 1.2f);
								previous = point;
							}
							break;
						}
						case MarkTremolo:
							// Three short slashes, the tremolo picking mark.
							for (int line = 0; line < 3; ++line)
							{
								const float dy = (static_cast<float>(line) - 1.0f) * caption * 0.28f;
								drawList->AddLine(ImVec2(x - size * 0.35f, middle + dy + caption * 0.15f),
									ImVec2(x + size * 0.35f, middle + dy - caption * 0.15f), color, 1.4f);
							}
							break;
						case MarkAccent:
							drawList->AddLine(ImVec2(x - size * 0.3f, middle - caption * 0.3f), ImVec2(x + size * 0.3f, middle), color, 1.4f);
							drawList->AddLine(ImVec2(x + size * 0.3f, middle), ImVec2(x - size * 0.3f, middle + caption * 0.3f), color, 1.4f);
							break;
						case MarkTap:
							Text(caption, ImVec2(x - Measure(caption, "T") * 0.5f, top), color, "T");
							break;
						case MarkPinch:
							Text(caption, ImVec2(x - Measure(caption, "PH") * 0.5f, top), color, "PH");
							break;
						}
					}
					if (event.showChordName)
					{
						const float top = origin.y + LaneTop(staffTop, chordLane);
						const ImU32 nameColor = isTarget ? accent : (event.time < targetTime ? played : normal);
						Text(caption, ImVec2(x - Measure(caption, event.chordName.c_str()) * 0.5f, top), nameColor, event.chordName.c_str());
					}
				}
			}

			// Page mode: the content x of each row's left edge. Rows break at bar
			// lines so each holds as many whole bars as fit; a loop start always
			// starts a row. A bar wider than a row is cut at the row width.
			std::vector<float> BuildRowStarts(const Snapshot& snapshot, float rowWidth)
			{
				std::vector<float> starts;
				const auto& events = layoutCache.events;
				if (events.empty() || rowWidth <= metrics.number * 4.0f) return starts;
				const float contentEnd = events.back().x + events.back().width * 0.5f + metrics.number;
				std::vector<std::pair<float, bool>> breaks;  // content x, forced
				breaks.reserve(snapshot.bars.size() + 2);
				for (const auto& bar : snapshot.bars) breaks.emplace_back(TimeToX(bar.first), false);
				if (LoopIsPartial(snapshot)) breaks.emplace_back(TimeToX(snapshot.loopStart) - metrics.number * 0.5f, true);
				breaks.emplace_back(contentEnd, true);
				std::stable_sort(breaks.begin(), breaks.end(), [](const auto& left, const auto& right) { return left.first < right.first; });

				// The first row starts at the bar holding the first note, not at the
				// empty bars of an intro.
				const float firstLeft = events.front().x - events.front().width * 0.5f - metrics.number * 0.5f;
				float current = firstLeft;
				for (const auto& candidate : breaks)
					if (candidate.first <= firstLeft) current = candidate.first;
				starts.push_back(current);
				float lastFit = -FLT_MAX;
				for (std::size_t index = 0; index < breaks.size(); ++index)
				{
					const float x = breaks[index].first;
					if (x <= current + 1.0f) continue;
					if (x - current > rowWidth)
					{
						if (lastFit > current + 1.0f)
						{
							current = lastFit;
							starts.push_back(current);
							lastFit = -FLT_MAX;
							--index;  // look at this break again from the new row start
							continue;
						}
						while (x - current > rowWidth)
						{
							current += rowWidth;
							starts.push_back(current);
						}
					}
					lastFit = x;
					if (breaks[index].second && x < contentEnd - 1.0f)
					{
						current = x;
						starts.push_back(current);
						lastFit = -FLT_MAX;
					}
				}
				starts.push_back(contentEnd);  // the end of the last row
				return starts;
			}

			std::size_t RowOf(const std::vector<float>& starts, float x)
			{
				if (starts.size() < 2) return 0;
				const auto found = std::upper_bound(starts.begin(), starts.end() - 1, x);
				const std::size_t row = found == starts.begin() ? 0 : static_cast<std::size_t>(found - starts.begin()) - 1;
				return std::min(row, starts.size() - 2);
			}

			// The lanes one page row needs: the mark lanes of the events it owns,
			// and a chord-name lane over them when it shows a chord name.
			struct RowLanes
			{
				int marks = 0;
				bool chords = false;
				bool topSlurs = false;
			};

			RowLanes LanesOf(float ownLeft, float ownRight)
			{
				RowLanes lanes;
				const auto& events = layoutCache.events;
				for (std::size_t index = VisibleStart(ownLeft - 1.0f); index < events.size() && events[index].x < ownRight; ++index)
				{
					const CachedEvent& event = events[index];
					if (!IsOwned(event, ownLeft, ownRight)) continue;
					for (const Mark& mark : event.marks) lanes.marks = std::max(lanes.marks, mark.lane + 1);
					lanes.chords = lanes.chords || event.showChordName;
					lanes.topSlurs = lanes.topSlurs || event.slurOnTop;
				}
				return lanes;
			}

			// Page mode. Each row is only as tall as its own marks need, so a song
			// with a few stacked bends does not make every row tall; the window's
			// height decides how many whole rows fit.
			void DrawPage(const Snapshot& snapshot, float pageHeight, float rowGap)
			{
				const int stringCount = ClampStringCount(snapshot.stringCount);
				const float gaps = static_cast<float>(stringCount - 1);
				const float minimumSpacing = metrics.spacing;
				if (!ImGui::BeginChild("TablaturePage", ImVec2(0.0f, 0.0f), false,
					ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs))
				{
					ImGui::EndChild();
					return;
				}
				const ImVec2 windowPosition = ImGui::GetWindowPos();
				const float viewportWidth = ImGui::GetWindowWidth();
				// Room left of a row's first bar line, so a wide number on it ("12",
				// "(12)") clears the tuning gutter.
				const float pad = metrics.number;
				const float rowWidth = viewportWidth - metrics.gutter - pad * 2.0f;
				const std::vector<float> starts = BuildRowStarts(snapshot, rowWidth);
				if (starts.size() < 2)
				{
					ImGui::EndChild();
					return;
				}
				const std::size_t rowCount = starts.size() - 1;
				const CachedEvent* target = FindTarget(snapshot.currentTargetId);
				const auto& input = ImGui::GetIO();
				const float wheel = input.MouseWheelH != 0.0f ? -input.MouseWheelH : input.MouseWheel;

				// Each row's room above the staff, from its own lanes.
				std::vector<float> rowAbove(rowCount);
				std::vector<int> rowChordLane(rowCount);
				std::vector<float> rowLaneBase(rowCount);
				for (std::size_t row = 0; row < rowCount; ++row)
				{
					const RowLanes lanes = LanesOf(row == 0 ? -FLT_MAX : starts[row], row + 1 == rowCount ? FLT_MAX : starts[row + 1]);
					Metrics rowMetrics = metrics;
					rowMetrics.laneBase = LaneBaseFor(metrics, lanes.topSlurs);
					rowChordLane[row] = lanes.marks;
					rowLaneBase[row] = rowMetrics.laneBase;
					rowAbove[row] = AboveFor(rowMetrics, lanes.marks + (lanes.chords ? 1 : 0));
				}
				const auto RowCost = [&](std::size_t row)
				{
					return rowAbove[row] + metrics.below + rowGap + minimumSpacing * gaps;
				};
				// The whole rows from `from` that fit at the narrowest string spacing.
				// Past the song's end the last row is repeated as blank slots, so the
				// last page is spaced like the others instead of spreading out.
				struct Fit
				{
					int rows = 0;   // real rows drawn
					int slots = 0;  // real plus blank slots
					float used = 0.0f;
				};
				const auto FitFrom = [&](std::size_t from)
				{
					Fit fit;
					for (std::size_t row = from; ; ++row)
					{
						const float cost = RowCost(std::min(row, rowCount - 1));
						if (fit.slots > 0 && fit.used + cost > pageHeight) break;
						fit.used += cost;
						++fit.slots;
						if (row < rowCount) ++fit.rows;
						if (fit.slots >= 64) break;
					}
					return fit;
				};

				// Which row is at the top. Following, the current note's row sits at
				// the top (second from the top when three or more rows show, so the
				// notes just played stay in view); a loop that fits starts the page and
				// stays put on every repeat. Peeking, the row is kept by song time so
				// a level change keeps the reader's place.
				const auto TopRow = [&](int rows)
				{
					std::size_t top = 0;
					if (viewState.followCurrent && target != nullptr)
					{
						const std::size_t targetRow = RowOf(starts, target->x);
						top = rows >= 3 && targetRow > 0 ? targetRow - 1 : targetRow;
						if (LoopIsPartial(snapshot) && target->time >= snapshot.loopStart - 0.05f && target->time <= snapshot.loopEnd + 0.05f)
						{
							const std::size_t loopFirst = RowOf(starts, TimeToX(snapshot.loopStart));
							const std::size_t loopLast = RowOf(starts, TimeToX(snapshot.loopEnd));
							if (loopLast - loopFirst + 1 <= static_cast<std::size_t>(rows)) top = loopFirst;
						}
					}
					else if (viewState.anchorTime >= 0.0f)
					{
						top = RowOf(starts, TimeToX(viewState.anchorTime) + 1.0f);
					}
					return std::min(top, rowCount - 1);
				};
				// How many rows show depends on which rows they are, and the top row
				// depends on how many show: settle it in two passes.
				int shownRows = FitFrom(TopRow(2)).slots;
				std::size_t firstRow = TopRow(shownRows);
				shownRows = FitFrom(firstRow).slots;
				firstRow = TopRow(shownRows);
				const float rowHeight = pageHeight / static_cast<float>(std::max(1, shownRows));
				long long rowMove = 0;
				if (ImGui::IsWindowHovered() && wheel != 0.0f) rowMove = wheel > 0.0f ? -1 : 1;
				// Click and drag slides the tab: dragging left (or up) brings the
				// next rows in, a quarter of the row width (or one row height) per
				// row. The button also stops a drag from moving the window.
				ImGui::InvisibleButton("TablatureDrag", ImVec2(std::max(1.0f, viewportWidth), std::max(1.0f, pageHeight)));
				if (ImGui::IsItemHovered() || ImGui::IsItemActive())
					ImGui::SetMouseCursor(ImGui::IsItemActive() ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
				if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
				{
					viewState.dragRows -= input.MouseDelta.x / std::max(1.0f, rowWidth * 0.25f)
						+ input.MouseDelta.y / std::max(1.0f, rowHeight);
					const float whole = viewState.dragRows >= 0.0f ? std::floor(viewState.dragRows) : std::ceil(viewState.dragRows);
					viewState.dragRows -= whole;
					rowMove += static_cast<long long>(whole);
				}
				else
				{
					viewState.dragRows = 0.0f;
				}
				if (rowMove != 0)
				{
					viewState.followCurrent = false;
					const long long moved = static_cast<long long>(firstRow) + rowMove;
					firstRow = static_cast<std::size_t>(std::max(0LL, std::min(moved, static_cast<long long>(rowCount) - 1)));
				}
				firstRow = std::min(firstRow, rowCount - 1);
				{
					const std::size_t first = VisibleStart(starts[firstRow]);
					viewState.anchorTime = first < layoutCache.events.size() ? layoutCache.events[first].time : -1.0f;
				}
				viewState.pendingAnchor = false;
				viewState.hasScrollSample = false;

				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const ImU32 lineColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
				const ImU32 stringColor = WithAlpha(ImGui::GetColorU32(ImGuiCol_Text), 0x70);
				const ImU32 normal = ImGui::GetColorU32(ImGuiCol_Text);
				const ImU32 played = ImGui::GetColorU32(ImGuiCol_TextDisabled);
				const ImU32 accent = HighlightColor();
				const ImU32 onAccent = OnAccent(accent);
				const ImU32 knockout = WithAlpha(ImGui::GetColorU32(ImGuiCol_WindowBg), 0xFF);
				// The height left over first widens the strings (up to maxSpacing), then
				// is shared out between the rows, each centred in its share.
				const Fit page = FitFrom(firstRow);
				const float slots = static_cast<float>(std::max(1, page.slots));
				const float spare = std::max(0.0f, pageHeight - page.used);
				metrics.spacing = std::min(std::max(minimumSpacing, metrics.maxSpacing), minimumSpacing + spare / (slots * gaps));
				const float stretch = (metrics.spacing - minimumSpacing) * gaps;
				const float slotExtra = std::max(0.0f, (spare - stretch * slots) / slots);
				const float songAbove = metrics.above;
				const float songLaneBase = metrics.laneBase;
				float top = windowPosition.y;
				for (int slot = 0; slot < page.rows; ++slot)
				{
					const std::size_t row = firstRow + static_cast<std::size_t>(slot);
					const float rowLeft = starts[row];
					const float rowRight = starts[row + 1];
					const float slotHeight = RowCost(row) + stretch + slotExtra;
					// Bar numbers, section names and the loop band sit at the top of
					// this row's own room above the staff.
					metrics.above = rowAbove[row];
					metrics.laneBase = rowLaneBase[row];
					const float verticalOffset = slotExtra * 0.5f;
					// Content x rowLeft lands just right of the tuning gutter.
					const ImVec2 origin(windowPosition.x + metrics.gutter + pad - rowLeft, top);
					// Every row but the song's last is spread to the full row width, so
					// the rows' ends line up.
					rowTransform.left = rowLeft;
					rowTransform.scale = row + 1 < rowCount && rowRight - rowLeft > 1.0f ? rowWidth / (rowRight - rowLeft) : 1.0f;
					const float staffTop = metrics.above + verticalOffset;
					const float staffBottom = staffTop + metrics.spacing * static_cast<float>(stringCount - 1);
					const float clipRight = std::min(windowPosition.x + viewportWidth, SX(origin, rowRight) + 2.0f);
					ImGui::PushClipRect(ImVec2(windowPosition.x + metrics.gutter, top), ImVec2(clipRight, top + slotHeight), true);
					DrawLoopAndSections(snapshot, origin, staffTop, staffBottom, rowLeft - pad, rowRight + 1.0f, lineColor, accent);
					for (int stringIndex = 0; stringIndex < stringCount; ++stringIndex)
					{
						const float y = origin.y + StringY(staffTop, stringCount, stringIndex);
						drawList->AddLine(ImVec2(SX(origin, rowLeft) - pad, y), ImVec2(SX(origin, rowRight), y), stringColor, 1.0f);
					}
					DrawNotes(snapshot, origin, staffTop, rowLeft - pad, rowRight, target, normal, played, accent, onAccent, knockout,
						row == 0 ? -FLT_MAX : rowLeft, row + 1 == rowCount ? FLT_MAX : rowRight);
					DrawAbove(snapshot, origin, staffTop, rowLeft - pad, rowRight, target, normal, played, accent,
						row == 0 ? -FLT_MAX : rowLeft, row + 1 == rowCount ? FLT_MAX : rowRight, rowChordLane[row]);
					DrawPalmMutes(origin, staffBottom, rowLeft - pad, rowRight, target, normal, played,
						row == 0 ? -FLT_MAX : rowLeft, row + 1 == rowCount ? FLT_MAX : rowRight);
					ImGui::PopClipRect();
					for (int stringIndex = 0; stringIndex < stringCount; ++stringIndex)
					{
						const std::string& label = snapshot.tuningLabels[stringIndex];
						if (label.empty()) continue;
						const float y = origin.y + StringY(staffTop, stringCount, stringIndex);
						Text(metrics.caption, ImVec2(windowPosition.x + (metrics.gutter - Measure(metrics.caption, label.c_str())) * 0.5f,
							y - metrics.caption * 0.5f), lineColor, label.c_str());
					}
					top += slotHeight;
				}
				metrics.above = songAbove;
				metrics.laneBase = songLaneBase;
				rowTransform = {};
				ImGui::EndChild();
			}

			void DrawLevelControls(const Snapshot& snapshot, bool* toggleLevels)
			{
				if (toggleLevels == nullptr) return;
				if (!snapshot.showsCurrentLevels)
				{
					if (ImGui::SmallButton("Back to my level")) *toggleLevels = true;
					ImGui::SameLine();
					ImGui::TextDisabled("Showing 100%%");
				}
				else if (!snapshot.currentLevelsRead)
				{
					ImGui::TextDisabled("My level unavailable, showing 100%%");
				}
				else if (snapshot.iterationsBelowMax == 0)
				{
					// Nothing to compare: the player's level already is the full chart.
					ImGui::TextDisabled("My level is 100%%");
				}
				else
				{
					if (ImGui::SmallButton("Compare with 100%")) *toggleLevels = true;
					ImGui::SameLine();
					ImGui::TextDisabled("Showing my level");
				}
				ImGui::SameLine();
				ImGui::TextDisabled("|");
				ImGui::SameLine();
			}
		}

		void Draw(const Snapshot& snapshot, float textScale, bool* open, bool* toggleLevels, bool docked, bool* toggleDock, bool* expanded,
			std::uint32_t* highlight, bool* highlightSaved)
		{
			if (highlightSaved != nullptr) *highlightSaved = false;
			highlightRgb = highlight != nullptr ? *highlight : 0;
			if (expanded != nullptr) *expanded = false;
			const bool isOpen = open == nullptr || *open;
			if (!isOpen)
			{
				viewState.wasOpen = false;
				return;
			}
			ReportInvalidSnapshot(snapshot);
			const float scale = ClampScale(textScale);
			if (!viewState.wasOpen || viewState.arrangementKey != snapshot.arrangementKey)
			{
				viewState.followCurrent = true;
				viewState.hasScrollSample = false;
				viewState.anchorTime = -1.0f;
				viewState.pendingAnchor = false;
				viewState.arrangementKey = snapshot.arrangementKey;
				viewState.generation = snapshot.generation;
			}
			else if (viewState.generation != snapshot.generation)
			{
				viewState.generation = snapshot.generation;
				viewState.pendingAnchor = !viewState.followCurrent && viewState.anchorTime >= 0.0f;
			}
			viewState.wasOpen = true;

			metrics = MakeMetrics(scale);
			const int stringCount = ClampStringCount(snapshot.stringCount);
			const float staffHeight = metrics.spacing * static_cast<float>(stringCount - 1);
			const float canvasHeight = metrics.above + staffHeight + metrics.below;
			const ImGuiStyle& style = ImGui::GetStyle();
			const float initialHeight = canvasHeight + ImGui::GetFrameHeightWithSpacing() * 2.0f + style.ScrollbarSize
				+ TabWindowPadding.y * 2.0f + 8.0f;
			const ImVec2 display = ImGui::GetIO().DisplaySize;
			static bool wasDocked = false;
			// The window never takes keyboard navigation or focus on appearing: while
			// ImGui wants the keyboard the WndProc hook swallows key presses, which
			// stopped the mod hotkeys (F6 record) and the Note by Note skip keys for
			// the whole song. The window has no text input, so it needs no keys.
			ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavInputs;
			if (docked)
			{
				// Across the top: full width, the height stays resizable. Up to 90% of
				// the screen, so it can grow into a page of several rows (60% was
				// too short for two rows at large text sizes).
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
				ImGui::SetNextWindowSizeConstraints(ImVec2(display.x, initialHeight * 0.6f), ImVec2(display.x, display.y * 0.9f));
				if (!wasDocked) ImGui::SetNextWindowSize(ImVec2(display.x, std::min(initialHeight, display.y * 0.6f)), ImGuiCond_Always);
				windowFlags |= ImGuiWindowFlags_NoMove;
			}
			else
			{
				if (wasDocked)
				{
					ImGui::SetNextWindowPos(ImVec2(display.x * 0.1f, display.y * 0.1f), ImGuiCond_Always);
					ImGui::SetNextWindowSize(ImVec2(760.0f, std::min(initialHeight, display.y * 0.85f)), ImGuiCond_Always);
				}
				ImGui::SetNextWindowSize(ImVec2(760.0f, std::min(initialHeight, display.y * 0.85f)), ImGuiCond_FirstUseEver);
			}
			wasDocked = docked;
			// Starts collapsed every time it appears (Note by Note switched on, a new
			// song, back from the Riff Repeater menus): opened, it covers the highway
			// and holds every note, so the player opens it when they want to read.
			ImGui::SetNextWindowCollapsed(true, ImGuiCond_Appearing);
			// Solid, so the HUD text and highway it covers do not show through the tab.
			ImGui::SetNextWindowBgAlpha(1.0f);
			// The overlay style has no window padding; the tab gets a little, so the
			// toolbar text does not start on the screen's edge when docked.
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, TabWindowPadding);
			const bool visible = ImGui::Begin("Tablature", open, windowFlags);
			ImGui::PopStyleVar();
			if (!visible)
			{
				ImGui::End();
				return;
			}
			if (snapshot.stringCount < 4 || snapshot.stringCount > 6)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Invalid tablature string count: %d", snapshot.stringCount);
				ImGui::End();
				return;
			}
			EnsureLayout(snapshot, scale);
			metrics.laneBase = LaneBaseFor(metrics, layoutCache.topSlurs);
			metrics.above = AboveHeight(metrics);
			if (!snapshot.error.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", snapshot.error.c_str());

			if (viewState.targetId != snapshot.currentTargetId)
			{
				viewState.targetId = snapshot.currentTargetId;
				if (viewState.followCurrent) viewState.hasScrollSample = false;
			}
			if (expanded != nullptr)
			{
				// While the tab is open Note by Note holds every note and ignores
				// what is played, so the player reads at their own pace; Right
				// Arrow moves on a note. Collapsing the window plays as normal.
				*expanded = true;
				ImGui::TextDisabled(snapshot.readerHolding ? "Held: Right Arrow for the next note" : "Detection off while open");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("While this window is open, Note by Note holds each note whatever you play.\nRight Arrow moves on to the next note. Collapse the window to play as normal.");
				ImGui::SameLine();
				ImGui::TextDisabled("|");
				ImGui::SameLine();
			}
			if (toggleDock != nullptr)
			{
				if (ImGui::SmallButton(docked ? "Undock" : "Dock to top")) *toggleDock = true;
				ImGui::SameLine();
				ImGui::TextDisabled("|");
				ImGui::SameLine();
			}
			if (highlight != nullptr && highlightSaved != nullptr)
			{
				// Highlight colour for the current note and the loop, picked on a wheel.
				// Edits show live; the setting is saved once when the picker closes.
				static bool pickerDirty = false;
				const ImU32 accent = HighlightColor();
				const ImVec4 swatch = ImGui::ColorConvertU32ToFloat4(accent);
				const float side = ImGui::GetFrameHeight() * 0.7f;
				if (ImGui::ColorButton("##TabHighlight", swatch, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(side, side)))
					ImGui::OpenPopup("TabHighlightPicker");
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Highlight colour for the current note and the loop");
				if (ImGui::BeginPopup("TabHighlightPicker"))
				{
					float rgb[3] = { swatch.x, swatch.y, swatch.z };
					ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
					if (ImGui::ColorPicker3("##TabHighlightWheel", rgb, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoAlpha
						| ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayHex))
					{
						const auto byte = [](float value) { return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
						const std::uint32_t picked = (byte(rgb[0]) << 16) | (byte(rgb[1]) << 8) | byte(rgb[2]);
						*highlight = picked == 0 ? 1u : picked;
						pickerDirty = true;
					}
					if (ImGui::SmallButton("Default"))
					{
						*highlight = 0;
						pickerDirty = true;
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
				else if (pickerDirty)
				{
					pickerDirty = false;
					*highlightSaved = true;
				}
				highlightRgb = *highlight;
				ImGui::SameLine();
				ImGui::TextDisabled("|");
				ImGui::SameLine();
			}
			DrawLevelControls(snapshot, toggleLevels);
			if (!viewState.followCurrent)
			{
				if (ImGui::SmallButton("Back to current"))
				{
					viewState.followCurrent = true;
					viewState.hasScrollSample = false;
					viewState.pendingAnchor = false;
				}
			}
			else
			{
				ImGui::TextDisabled("Scroll or drag to look ahead");
			}

			// A window tall enough for two rows of strings becomes a page of tab:
			// rows of whole bars filling the width, stacked to fill the height. A
			// shorter window keeps the single scrolling row below. The test uses a
			// row with no mark lanes: page rows are only as tall as their own marks
			// need, and judging by the song's tallest row (every lane it uses
			// anywhere) kept a 2000x740 window on one row with room for two.
			{
				const float pageHeight = ImGui::GetContentRegionAvail().y;
				const float rowGap = metrics.caption * 0.5f;
				Metrics lean = metrics;
				lean.laneBase = LaneBaseFor(metrics, false);
				const float leanestRow = AboveFor(lean, 0) + metrics.below + rowGap + metrics.spacing * static_cast<float>(stringCount - 1);
				if (!layoutCache.events.empty() && pageHeight >= leanestRow * 2.0f)
				{
					DrawPage(snapshot, pageHeight, rowGap);
					ImGui::End();
					return;
				}
			}

			// Stretch the strings to the window's height (up to a readable maximum)
			// and centre what is left, so no empty band sits under the bottom string.
			const float available = ImGui::GetContentRegionAvail().y - style.ScrollbarSize;
			const float fixedHeight = metrics.above + metrics.below;
			const float stretched = (available - fixedHeight) / static_cast<float>(stringCount - 1);
			metrics.spacing = std::max(metrics.spacing, std::min(stretched, metrics.maxSpacing));
			const float drawnHeight = fixedHeight + metrics.spacing * static_cast<float>(stringCount - 1);
			const float verticalOffset = std::max(0.0f, (available - drawnHeight) * 0.5f);
			const float contentHeight = std::max(drawnHeight, available);
			const float contentWidth = layoutCache.contentWidth + std::max(0.0f, ImGui::GetContentRegionAvail().x);
			ImGui::SetNextWindowContentSize(ImVec2(contentWidth, contentHeight));
			if (ImGui::BeginChild("TablatureCanvas", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs))
			{
				const float scroll = ImGui::GetScrollX();
				const float viewportWidth = std::max(0.0f, ImGui::GetWindowWidth() - style.ScrollbarSize);
				const bool hovered = ImGui::IsWindowHovered();
				const auto& input = ImGui::GetIO();
				const float wheel = input.MouseWheelH != 0.0f ? -input.MouseWheelH : input.MouseWheel;
				if (hovered && wheel != 0.0f)
				{
					viewState.followCurrent = false;
					ImGui::SetScrollX(std::max(0.0f, scroll - wheel * metrics.number * 5.0f));
				}
				if (viewState.hasScrollSample && hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left)
					&& std::fabs(scroll - viewState.lastScroll) > 1.0f)
					viewState.followCurrent = false;
				const CachedEvent* target = FindTarget(snapshot.currentTargetId);
				// The current note sits a quarter of the way in, so the notes just
				// played stay in view.
				const float playheadOffset = metrics.gutter + (viewportWidth - metrics.gutter) * 0.25f;
				if (viewState.followCurrent && target != nullptr)
				{
					// Inside a loop that fits the window, show the whole loop from its
					// first note at the left edge; it then stays put on every repeat.
					float followScroll = target->x - playheadOffset;
					if (LoopIsPartial(snapshot) && target->time >= snapshot.loopStart - 0.05f && target->time <= snapshot.loopEnd + 0.05f)
					{
						const float pad = metrics.number;
						const float loopLeft = TimeToX(snapshot.loopStart);
						const float loopRight = TimeToX(snapshot.loopEnd);
						if (loopRight - loopLeft + pad * 2.0f <= viewportWidth - metrics.gutter)
							followScroll = loopLeft - metrics.gutter - pad;
					}
					ImGui::SetScrollX(std::max(0.0f, followScroll));
				}
				bool anchored = false;
				if (viewState.pendingAnchor && !viewState.followCurrent && !layoutCache.events.empty())
				{
					anchored = true;
					ImGui::SetScrollX(std::max(0.0f, TimeToX(viewState.anchorTime) - metrics.gutter));
				}
				viewState.pendingAnchor = false;
				// `scroll` still belongs to the old layout on the frame an anchor is applied.
				if (!anchored)
				{
					const std::size_t first = VisibleStart(scroll + metrics.gutter);
					if (first < layoutCache.events.size()) viewState.anchorTime = layoutCache.events[first].time;
				}
				viewState.lastScroll = scroll;
				viewState.hasScrollSample = true;

				const ImVec2 origin = ImGui::GetCursorScreenPos();
				// Click and drag slides the notes sideways with the mouse. The button
				// also stops a drag from moving the window.
				ImGui::InvisibleButton("TablatureDrag", ImVec2(std::max(1.0f, contentWidth), std::max(1.0f, contentHeight)));
				if (ImGui::IsItemHovered() || ImGui::IsItemActive())
					ImGui::SetMouseCursor(ImGui::IsItemActive() ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_Hand);
				if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && input.MouseDelta.x != 0.0f)
				{
					viewState.followCurrent = false;
					viewState.pendingAnchor = false;
					ImGui::SetScrollX(std::max(0.0f, scroll - input.MouseDelta.x));
				}
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const ImU32 lineColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
				const ImU32 stringColor = WithAlpha(ImGui::GetColorU32(ImGuiCol_Text), 0x70);
				const ImU32 normal = ImGui::GetColorU32(ImGuiCol_Text);
				const ImU32 played = ImGui::GetColorU32(ImGuiCol_TextDisabled);
				const ImU32 accent = HighlightColor();
				const ImU32 onAccent = OnAccent(accent);
				const ImU32 knockout = WithAlpha(ImGui::GetColorU32(ImGuiCol_WindowBg), 0xFF);
				const float staffTop = metrics.above + verticalOffset;
				const float staffBottom = staffTop + metrics.spacing * static_cast<float>(stringCount - 1);
				const float visibleLeft = scroll + metrics.gutter;
				const float visibleRight = scroll + viewportWidth;
				const ImVec2 windowPosition = ImGui::GetWindowPos();
				ImGui::PushClipRect(ImVec2(windowPosition.x + metrics.gutter, windowPosition.y),
					ImVec2(windowPosition.x + viewportWidth, windowPosition.y + ImGui::GetWindowHeight() - style.ScrollbarSize), true);
				DrawLoopAndSections(snapshot, origin, staffTop, staffBottom, visibleLeft, visibleRight, lineColor, accent);
				for (int stringIndex = 0; stringIndex < stringCount; ++stringIndex)
				{
					const float y = origin.y + StringY(staffTop, stringCount, stringIndex);
					drawList->AddLine(ImVec2(origin.x + visibleLeft, y), ImVec2(origin.x + visibleRight, y), stringColor, 1.0f);
				}
				// No separate playhead line: the filled pill marks the current note, and
				// a line beside it read as a second bar line at the loop start.
				DrawNotes(snapshot, origin, staffTop, visibleLeft, visibleRight, target, normal, played, accent, onAccent, knockout);
				DrawAbove(snapshot, origin, staffTop, visibleLeft, visibleRight, target, normal, played, accent);
				DrawPalmMutes(origin, staffBottom, visibleLeft, visibleRight, target, normal, played);
				{
					// Notes scrolling under the tuning gutter fade out over a short
					// band instead of being cut through a digit at its edge; the
					// strings are drawn again over the fade so they run on unbroken.
					const float fadeLeft = windowPosition.x + metrics.gutter;
					const float fadeRight = fadeLeft + metrics.number * 1.2f;
					const ImU32 solid = knockout;
					const ImU32 clear = WithAlpha(knockout, 0x00);
					drawList->AddRectFilledMultiColor(ImVec2(fadeLeft, windowPosition.y), ImVec2(fadeRight, windowPosition.y + ImGui::GetWindowHeight()),
						solid, clear, clear, solid);
					for (int stringIndex = 0; stringIndex < stringCount; ++stringIndex)
					{
						const float y = origin.y + StringY(staffTop, stringCount, stringIndex);
						drawList->AddLine(ImVec2(fadeLeft, y), ImVec2(fadeRight, y), stringColor, 1.0f);
					}
				}
				ImGui::PopClipRect();

				// Tuning labels in the fixed gutter, which the clip rect above keeps clear.
				for (int stringIndex = 0; stringIndex < stringCount; ++stringIndex)
				{
					const std::string& label = snapshot.tuningLabels[stringIndex];
					if (label.empty()) continue;
					const float y = origin.y + StringY(staffTop, stringCount, stringIndex);
					Text(metrics.caption, ImVec2(windowPosition.x + (metrics.gutter - Measure(metrics.caption, label.c_str())) * 0.5f,
						y - metrics.caption * 0.5f), lineColor, label.c_str());
				}
			}
			ImGui::EndChild();
			ImGui::End();
		}
	}
}
