#pragma once

// Where the Note by Note engine calls 4.x's highway renderer (DLL/NoteByNote/Ui/NoteByNoteHighwayRenderer). The port
// (Ui/NoteByNoteHighwayRenderer, with 4.x's D3DHooks half in Ui/HighwayInterceptors) registers its implementation from
// NoteByNoteMod::OnInitialize, before the engine's detours can call in. Until something registers every call is inert,
// which is how 4.x behaves with its highway filtering off.
//
// The HUD (Ui/NoteByNoteHud) asks it for the chord finger numerals 4.x painted over frozen chord holds (4.x's
// D3DHooks finger anchors and NoteByNoteHighwayRenderer chord fingers).
namespace NoteByNote::HighwaySeam
{
	class IHighway
	{
	public:
		virtual ~IHighway() = default;

		// Game thread, in the render-preparation detour, before the game prepares the highway.
		virtual void RefreshSelectedTarget(void* owner) = 0;
		// The renderer's own draw hooks; true when installed. Called once, from the engine's detour install.
		virtual bool InstallPresentationGate() = 0;
		virtual void ClearSelectedTarget() = 0;
		virtual void EvictRememberedDimCandidates() = 0;
		virtual void SetNeckPlacementMode(int mode) = 0;
		// frets/fingers per string (low E first), -1 / 0 for unplayed; anchor -1 when unreadable.
		virtual void SetChordTargetShape(int chordId, const int (&frets)[6], const int (&fingers)[6], int anchorFret,
			int anchorWidth) = 0;

		// Render thread, for the HUD's finger numerals. D3DHooks::AreFingerNumeralsEnabled.
		virtual bool AreFingerNumeralsEnabled() = 0;
		// True only while the song is frozen on the chord (native hold owned), not while the selected chord is still
		// scrolling in. NoteByNoteHighwayRenderer::IsChordHoldFrozen.
		virtual bool IsChordHoldFrozen() = 0;
		// Screen anchors (0..1 fractions of the screen) collected from the kept members' marker draws this frame;
		// returns how many. D3DHooks::GetFreshFingerAnchors.
		virtual int GetFreshFingerAnchors(int (&strings)[8], int (&frets)[8], float (&screenX)[8], float (&screenY)[8]) = 0;
		// True when the game's finger ring for a kept chord member stood at rest this frame or the last.
		virtual bool HasRestingFingerRing(int stringIndex, int fret) = 0;
		// The template finger (1..4) for one fretted chord member, valid only while a chord hold drives the keep set.
		virtual bool TryGetChordFingerForCoordinate(int stringIndex, int fret, int& finger) = 0;
	};

	// Any thread; null removes it. The implementation must outlive the game (a static or a mod member).
	void Register(IHighway* highway);

	void RefreshSelectedTarget(void* owner);
	bool InstallPresentationGate();
	void ClearSelectedTarget();
	void EvictRememberedDimCandidates();
	void SetNeckPlacementMode(int mode);
	void SetChordTargetShape(int chordId, const int (&frets)[6], const int (&fingers)[6], int anchorFret, int anchorWidth);

	bool AreFingerNumeralsEnabled();
	bool IsChordHoldFrozen();
	int GetFreshFingerAnchors(int (&strings)[8], int (&frets)[8], float (&screenX)[8], float (&screenY)[8]);
	bool HasRestingFingerRing(int stringIndex, int fret);
	bool TryGetChordFingerForCoordinate(int stringIndex, int fret, int& finger);
}
