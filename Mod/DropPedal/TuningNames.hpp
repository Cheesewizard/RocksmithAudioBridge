#pragma once

#include <string>

// Names a guitar tuning shape the way 4.x's Drop Pedal readout did (DLL/DropPedal/DropPedal.cpp NameChartShape with
// DLL/SongTuning.cpp TryGetTuningNameForOffsets): conventional names from the game's tuning list (Drop D, Eb Drop Db,
// Open G), else a computed "Drop <x>", else "<x> custom". Upstream's SongTuning has no reverse lookup, so it lives here.
namespace AudioBridge::DropPedal::TuningNames
{
	enum class Shape
	{
		Uniform,   // every string at the reference
		Drop,      // low string two below the rest
		Custom
	};

	// The game's list, RSMods\CustomMods\tuning.database.json next to the exe, read once. False when the shape isn't
	// listed (or the file is missing). Any thread.
	bool TryNameForOffsets(const int (&offsets)[6], std::string& name);

	// `reference` is the offset from E the shape is built on.
	std::string NameShape(Shape shape, const int (&offsets)[6], int reference);
}
