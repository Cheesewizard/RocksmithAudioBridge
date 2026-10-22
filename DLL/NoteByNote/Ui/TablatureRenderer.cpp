#include "stdafx.h"
#include "TablatureRenderer.hpp"

// Classic guitar tablature: six tightly spaced string lines, small fret numbers
// sitting on their string (the line is knocked out behind them), small h/p slurs
// just above the two notes they join, and horizontal spacing that follows song
// time so a fast run reads as a tight cluster and a rest as a gap. Bar lines
// (from the chart's beat grid), section lines and the game's loop range give the
// eye an anchor; notes already played are dimmed and the current note sits on a
// playhead a quarter of the way in, or the loop starts at the left edge when the
// whole loop fits. The staff stretches to the window's height so no empty band
// under the bottom string makes a note read as one string lower.

#include "Lib/ImGUI/imgui.h"

#include <algorithm>
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
				float labelWidth = 0.0f;
				char label[16] = {};
			};

			struct CachedEvent
			{
				std::uint64_t id = 0;
				float time = 0.0f;
				float x = 0.0f;
				float width = 0.0f;
				std::vector<CachedPosition> positions;
			};

			struct Metrics
			{
				float number = 13.0f;    // fret number font size
				float caption = 11.0f;     // h / p / bend / loop label font size
				float spacing = 14.0f;   // distance between string lines
				float above = 30.0f;     // room above the top string for slurs and bends
				float below = 18.0f;     // room below the bottom string
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
			};

			LayoutCache layoutCache;
			ViewState viewState;
			Metrics metrics;
			std::uint64_t validationGeneration = std::numeric_limits<std::uint64_t>::max();

			float ClampScale(float textScale)
			{
				return std::max(0.5f, std::min(textScale, 3.0f));
			}

			int ClampStringCount(int stringCount)
			{
				return std::max(4, std::min(stringCount, 6));
			}

			Metrics MakeMetrics(float scale)
			{
				const float base = ImGui::GetFont()->FontSize * scale;
				Metrics m;
				m.number = std::max(11.0f, base * 0.85f);
				m.caption = std::max(11.0f, base * 0.65f);
				m.spacing = m.number * 1.1f;   // the minimum; Draw stretches it to the window
				m.above = m.caption * 2.0f + m.number * 0.9f;
				m.below = m.caption + 8.0f * scale;
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
				bool hasPrevious = false;
				float previousX = 0.0f;
				float previousHalf = 0.0f;
				for (const std::size_t eventIndex : order)
				{
					const Event& event = snapshot.events[eventIndex];
					CachedEvent cached;
					cached.id = event.id;
					cached.time = event.time;
					bool bend = false;
					for (const Position& position : event.positions)
					{
						if (position.stringIndex < 0 || position.stringIndex >= stringCount) continue;
						CachedPosition entry;
						entry.stringIndex = position.stringIndex;
						entry.fret = position.fret;
						entry.techniques = position.techniques;
						entry.bendSemitones = position.bendSemitones;
						entry.slideTargetFret = position.slideTargetFret;
						if ((position.techniques & Muted) != 0) std::snprintf(entry.label, sizeof(entry.label), "x");
						else if ((position.techniques & Harmonic) != 0) std::snprintf(entry.label, sizeof(entry.label), "<%d>", std::max(0, position.fret));
						else std::snprintf(entry.label, sizeof(entry.label), "%d", std::max(0, position.fret));
						entry.labelWidth = Measure(metrics.number, entry.label);
						cached.width = std::max(cached.width, entry.labelWidth);
						bend = bend || (position.techniques & (Bend | Slide)) != 0;
						cached.positions.push_back(entry);
					}
					if (cached.positions.empty()) continue;
					// A bend arrow sits to the right of its number and needs the room.
					const float half = cached.width * 0.5f + (bend ? metrics.number * 0.7f : 0.0f);
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

			// Learn a Song reports the whole song as its range; only a real loop counts.
			bool LoopIsPartial(const Snapshot& snapshot)
			{
				const auto& events = layoutCache.events;
				return snapshot.loopValid && !events.empty()
					&& (snapshot.loopStart > events.front().time + 0.05f || snapshot.loopEnd < events.back().time - 0.05f);
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
						const float top = origin.y + staffTop - (metrics.number * 1.3f + metrics.caption + 6.0f);
						const float bottom = origin.y + staffBottom + metrics.number * 0.9f;
						drawList->AddRectFilled(ImVec2(origin.x + left, top), ImVec2(origin.x + right, bottom), WithAlpha(accent, 0x22));
						drawList->AddRectFilled(ImVec2(origin.x + left, top), ImVec2(origin.x + right, top + 3.0f), WithAlpha(accent, 0xB0));
						const float labelX = std::max(left, visibleLeft) + 4.0f;
						Text(metrics.caption, ImVec2(origin.x + labelX, bottom + 2.0f), WithAlpha(accent, 0xE0), "Loop");
					}
				}
				// Bar lines with the measure number above, thinned out where bars are
				// close together; section starts get a heavier line.
				const ImU32 barColor = WithAlpha(lineColor, 0xC0);
				float lastNumberRight = -FLT_MAX;
				for (const auto& bar : snapshot.bars)
				{
					const float x = TimeToX(bar.first);
					if (x < visibleLeft - 2.0f || x > visibleRight + 2.0f) continue;
					drawList->AddLine(ImVec2(origin.x + x, origin.y + staffTop), ImVec2(origin.x + x, origin.y + staffBottom), barColor, 1.0f);
					char number[16] = {};
					std::snprintf(number, sizeof(number), "%d", bar.second + 1);
					const float width = Measure(metrics.caption, number);
					if (x + 2.0f > lastNumberRight + metrics.caption)
					{
						Text(metrics.caption, ImVec2(origin.x + x + 2.0f, origin.y + staffTop - metrics.caption - 2.0f), lineColor, number);
						lastNumberRight = x + 2.0f + width;
					}
				}
				for (const float start : snapshot.sectionStarts)
				{
					const float x = TimeToX(start);
					if (x < visibleLeft - 2.0f || x > visibleRight + 2.0f) continue;
					drawList->AddLine(ImVec2(origin.x + x, origin.y + staffTop), ImVec2(origin.x + x, origin.y + staffBottom), lineColor, 2.5f);
				}
			}

			// A slide is the usual tab stroke between two numbers: "/" up to a higher
			// fret, "\" down to a lower one, ending at the next note on the string
			// when it is the slide's target. An unpitched slide (no target) is a short
			// falling stroke off the note.
			void DrawSlide(std::size_t eventIndex, const CachedPosition& position, float left, float y, ImU32 color, float originX)
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
								endX = std::max(startX + 4.0f, originX + next.x - candidate.labelWidth * 0.5f - 2.0f);
							found = true;
							break;
						}
					if (found) break;
				}
				drawList->AddLine(ImVec2(startX, up ? y + rise : y - rise), ImVec2(endX, up ? y - rise : y + rise), color, 1.3f);
			}

			void DrawNotes(const Snapshot& snapshot, const ImVec2& origin, float staffTop, float visibleLeft, float visibleRight,
				const CachedEvent* target, ImU32 normal, ImU32 played, ImU32 accent, ImU32 onAccent, ImU32 knockout)
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
					const bool isTarget = target == &event;
					const bool isPlayed = !isTarget && event.time < targetTime;
					const ImU32 color = isPlayed ? played : normal;
					const float x = origin.x + event.x;
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
								const float startX = origin.x + previousEvent->x + previous->labelWidth * 0.5f;
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
						if ((position.techniques & Slide) != 0) DrawSlide(eventIndex, position, left, y, markColor, origin.x);
						if ((position.techniques & Tap) != 0)
							Text(metrics.caption, ImVec2(x - Measure(metrics.caption, "T") * 0.5f, y - size * 0.6f - metrics.caption),
								markColor, "T");
						if ((position.techniques & Bend) != 0)
						{
							// A short rising arrow off the number, labelled 1/2, full or the semitone count.
							const float startX = left + position.labelWidth + 2.0f;
							const float tipX = startX + size * 0.5f;
							const float tipY = y - size * 1.2f;
							drawList->AddBezierQuadratic(ImVec2(startX, y), ImVec2(tipX, y), ImVec2(tipX, tipY + 3.0f), markColor, 1.2f);
							drawList->AddTriangleFilled(ImVec2(tipX, tipY), ImVec2(tipX - 3.0f, tipY + 5.0f), ImVec2(tipX + 3.0f, tipY + 5.0f), markColor);
							const int semitones = std::max(1, position.bendSemitones);
							char bendText[16] = {};
							if (semitones == 1) std::snprintf(bendText, sizeof(bendText), "1/2");
							else if (semitones == 2) std::snprintf(bendText, sizeof(bendText), "full");
							else std::snprintf(bendText, sizeof(bendText), "%d", semitones);
							Text(metrics.caption, ImVec2(tipX - Measure(metrics.caption, bendText) * 0.5f, tipY - metrics.caption - 1.0f), markColor, bendText);
						}
					}
				}
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

		void Draw(const Snapshot& snapshot, float textScale, bool* open, bool* toggleLevels, bool docked, bool* toggleDock)
		{
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
				+ style.WindowPadding.y * 2.0f + 8.0f;
			const ImVec2 display = ImGui::GetIO().DisplaySize;
			static bool wasDocked = false;
			ImGuiWindowFlags windowFlags = 0;
			if (docked)
			{
				// Across the top: full width, the height stays resizable.
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
				ImGui::SetNextWindowSizeConstraints(ImVec2(display.x, initialHeight * 0.6f), ImVec2(display.x, display.y * 0.6f));
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
			if (!ImGui::Begin("Tablature", open, windowFlags))
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
			if (!snapshot.error.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", snapshot.error.c_str());

			if (viewState.targetId != snapshot.currentTargetId)
			{
				viewState.targetId = snapshot.currentTargetId;
				if (viewState.followCurrent) viewState.hasScrollSample = false;
			}
			if (toggleDock != nullptr)
			{
				if (ImGui::SmallButton(docked ? "Undock" : "Dock to top")) *toggleDock = true;
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
				ImGui::TextDisabled("Scroll to look ahead");
			}

			// Stretch the strings to the window's height (up to a readable maximum)
			// and centre what is left, so no empty band sits under the bottom string.
			const float available = ImGui::GetContentRegionAvail().y - style.ScrollbarSize;
			const float fixedHeight = metrics.above + metrics.below;
			const float stretched = (available - fixedHeight) / static_cast<float>(stringCount - 1);
			metrics.spacing = std::max(metrics.spacing, std::min(stretched, metrics.number * 2.4f));
			const float drawnHeight = fixedHeight + metrics.spacing * static_cast<float>(stringCount - 1);
			const float verticalOffset = std::max(0.0f, (available - drawnHeight) * 0.5f);
			const float contentHeight = std::max(drawnHeight, available);
			const float contentWidth = layoutCache.contentWidth + std::max(0.0f, ImGui::GetContentRegionAvail().x);
			ImGui::SetNextWindowContentSize(ImVec2(contentWidth, contentHeight));
			if (ImGui::BeginChild("TablatureCanvas", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
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
				ImGui::Dummy(ImVec2(contentWidth, contentHeight));
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const ImU32 lineColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
				const ImU32 stringColor = WithAlpha(ImGui::GetColorU32(ImGuiCol_Text), 0x70);
				const ImU32 normal = ImGui::GetColorU32(ImGuiCol_Text);
				const ImU32 played = ImGui::GetColorU32(ImGuiCol_TextDisabled);
				const ImU32 accent = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
				const ImU32 onAccent = IM_COL32(255, 255, 255, 255);
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
