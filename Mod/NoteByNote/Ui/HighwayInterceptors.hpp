#pragma once

namespace Framework { struct ModContext; }

// Note by Note's highway drawing on the framework's draw hooks: 4.x's D3DHooks Note by Note code (stale fretboard
// marker quads, decode-aware marker suppression, the note-head instance filter, host finger-numeral anchors) as draw
// interceptors, and 4.x's NoteByNoteHighwayRenderer registered as the engine's IHighway (Ui/HighwaySeam).
namespace NoteByNote::HighwayInterceptors
{
	// MainThread, from NoteByNoteMod::OnInitialize, before the engine installs its detours (they install the
	// renderer's presentation gate through the seam). Registers the seam implementation, the Indexed and PrimitiveUP
	// interceptors, the vertex-shader constant shadow and the before-reset target clear.
	void Register(Framework::ModContext& c);

	// Render thread, once per EndScene from the mod's frame callback (4.x's FinishNoteByNoteRenderFrame).
	void FinishRenderFrame();
}
