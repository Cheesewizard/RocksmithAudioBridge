#pragma once

#include "NoteByNote/Protocol/TablaturePreview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace NoteByNote::Controller
{
	inline void GroupTablatureEvents(
		std::vector<ResearchProtocol::PreviewEvent>& events,
		std::unordered_map<uintptr_t, uint64_t>& recordIds,
		float simultaneousTolerance)
	{
		std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right)
		{
			if (left.time != right.time) return left.time < right.time;
			return left.id < right.id;
		});
		std::vector<ResearchProtocol::PreviewEvent> grouped;
		grouped.reserve(events.size());
		recordIds.clear();
		for (const auto& event : events)
		{
			bool canMerge = !grouped.empty()
				&& std::fabs(grouped.back().time - event.time) <= simultaneousTolerance;
			uint32_t newPositions = 0;
			if (canMerge)
			{
				for (uint32_t right = 0; right < event.positionCount; ++right)
				{
					bool isDuplicate = false;
					for (uint32_t left = 0; left < grouped.back().positionCount; ++left)
					{
						const auto& previous = grouped.back().positions[left];
						const auto& next = event.positions[right];
						if (previous.stringIndex != next.stringIndex) continue;
						if (previous.fret != next.fret) canMerge = false;
						isDuplicate = true;
						break;
					}
					if (!isDuplicate) ++newPositions;
				}
				canMerge = canMerge && grouped.back().positionCount + newPositions <= 6;
			}
			if (!canMerge)
			{
				grouped.push_back(event);
			}
			else
			{
				for (uint32_t right = 0; right < event.positionCount; ++right)
				{
					const auto& next = event.positions[right];
					auto& destination = grouped.back();
					uint32_t index = 0;
					while (index < destination.positionCount && destination.positions[index].stringIndex != next.stringIndex) ++index;
					if (index == destination.positionCount)
						destination.positions[destination.positionCount++] = next;
					else
						destination.positions[index].techniques |= next.techniques;
				}
			}
			recordIds[static_cast<uintptr_t>(event.id)] = grouped.back().id;
		}
		events = std::move(grouped);
	}

	// Events must be sorted by time. Returns the id of the event closest to
	// `time`, or 0 when none lies within `tolerance` seconds.
	inline uint64_t FindTablatureEventNearTime(
		const std::vector<ResearchProtocol::PreviewEvent>& events,
		float time,
		float tolerance)
	{
		const auto next = std::lower_bound(events.begin(), events.end(), time,
			[](const ResearchProtocol::PreviewEvent& event, float value) { return event.time < value; });
		const ResearchProtocol::PreviewEvent* best = nullptr;
		if (next != events.end()) best = &*next;
		if (next != events.begin())
		{
			const auto& previous = *(next - 1);
			if (best == nullptr || time - previous.time <= best->time - time) best = &previous;
		}
		if (best == nullptr || std::fabs(best->time - time) > tolerance) return 0;
		return best->id;
	}
}
