#pragma once

namespace NoteByNote::Ui
{
	bool IsTablaturePreviewVisible();
	// Call once per frame before ImGui::NewFrame.
	bool TablaturePreviewWantsCursor();
	void DrawTablaturePreview();
}
