#include "HighwaySeam.hpp"

#include <atomic>

namespace NoteByNote::HighwaySeam
{
	namespace
	{
		std::atomic<IHighway*> registered{ nullptr };

		IHighway* Get()
		{
			return registered.load(std::memory_order_acquire);
		}
	}

	void Register(IHighway* highway)
	{
		registered.store(highway, std::memory_order_release);
	}

	void RefreshSelectedTarget(void* owner)
	{
		if (IHighway* highway = Get()) highway->RefreshSelectedTarget(owner);
	}

	bool InstallPresentationGate()
	{
		IHighway* highway = Get();
		return highway != nullptr && highway->InstallPresentationGate();
	}

	void ClearSelectedTarget()
	{
		if (IHighway* highway = Get()) highway->ClearSelectedTarget();
	}

	void EvictRememberedDimCandidates()
	{
		if (IHighway* highway = Get()) highway->EvictRememberedDimCandidates();
	}

	void SetNeckPlacementMode(int mode)
	{
		if (IHighway* highway = Get()) highway->SetNeckPlacementMode(mode);
	}

	void SetChordTargetShape(int chordId, const int (&frets)[6], const int (&fingers)[6], int anchorFret, int anchorWidth)
	{
		if (IHighway* highway = Get()) highway->SetChordTargetShape(chordId, frets, fingers, anchorFret, anchorWidth);
	}

	bool AreFingerNumeralsEnabled()
	{
		IHighway* highway = Get();
		return highway != nullptr && highway->AreFingerNumeralsEnabled();
	}

	bool IsChordHoldFrozen()
	{
		IHighway* highway = Get();
		return highway != nullptr && highway->IsChordHoldFrozen();
	}

	int GetFreshFingerAnchors(int (&strings)[8], int (&frets)[8], float (&screenX)[8], float (&screenY)[8])
	{
		IHighway* highway = Get();
		return highway != nullptr ? highway->GetFreshFingerAnchors(strings, frets, screenX, screenY) : 0;
	}

	bool HasRestingFingerRing(int stringIndex, int fret)
	{
		IHighway* highway = Get();
		return highway != nullptr && highway->HasRestingFingerRing(stringIndex, fret);
	}

	bool TryGetChordFingerForCoordinate(int stringIndex, int fret, int& finger)
	{
		IHighway* highway = Get();
		return highway != nullptr && highway->TryGetChordFingerForCoordinate(stringIndex, fret, finger);
	}
}
