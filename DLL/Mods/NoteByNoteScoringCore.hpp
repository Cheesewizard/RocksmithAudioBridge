#pragma once

namespace NoteByNoteProbe { struct NativeRenderedAttack; }

// The Note by Note scoring engine's own entry points, in a namespace distinct from the
// host-facing NoteByNoteNativeScoring:: facade so both can link into one module (the in-process
// build, where the facade and the scoring engine are compiled together). The host facade
// forwards to these through the probe dispatch; the reloadable research probe calls them
// directly. Keeping the two namespaces apart is what lets a single DLL host both.
namespace NoteByNoteScoringCore
{
	void Initialize();
	bool IsAvailable();
	void ObserveRenderedAttack(const NoteByNoteProbe::NativeRenderedAttack& attack);
	void Stop();
	void RequestReArm();
}
