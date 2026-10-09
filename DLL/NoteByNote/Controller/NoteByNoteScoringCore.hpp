#pragma once

namespace NoteByNoteProbe { struct NativeRenderedAttack; }
namespace NoteByNoteScoringCore
{
	void Initialize();
	bool IsAvailable();
	void ObserveRenderedAttack(const NoteByNoteProbe::NativeRenderedAttack& attack);
	void Stop();
	void RequestReArm();
}
