#include "HighwayInterceptors.hpp"

#include "HighwaySeam.hpp"
#include "NoteByNoteHighwayRenderer.hpp"
#include "../Detection/ArrangementInstrument.hpp"
#include "../Host/NoteByNoteMode.hpp"
#include "../../Core/GameVersion.hpp"

#include "D3D/D3D.hpp"
#include "D3D/D3DHelper.hpp"
#include "Framework/Framework.hpp"
#include "Log.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

// The Note by Note half of 4.x's DLL/D3D/D3DHooks.cpp, as framework draw interceptors. The logic, thresholds and
// comments are 4.x's; only the plumbing differs:
//   - 4.x ran these inline in Hook_DIP / Hook_DPUP. Here they are one Indexed and one PrimitiveUP interceptor at
//     kFilterPriority, after the colour mods, so the instance-subset and quad-run redraws carry custom note colours
//     (4.x drew them before its colour code, so they kept stock colours).
//   - The native caller (_ReturnAddress() in 4.x's hook) is ctx.call.returnAddress; oDrawIndexedPrimitive is
//     ctx.DrawOriginal, and a re-issued draw returns Hide.
//   - D3DHooks::vertexShaderConstantShadow is filled by a VertexShaderConstants observer (Observe::Always).
//   - FinishNoteByNoteRenderFrame's frame count is FinishRenderFrame, from the mod's frame callback.
//   - GameState on the render thread: in-song is ctx.inSong (upstream's per-draw GameState::IsInSong; 4.x sampled the
//     same once per frame); the Learn-A-Song-playing check reads NoteByNoteProbe::IsInLasPlayingFast, published by the
//     MainThread tick.
// Left out, all research-only: the render snapshot (ArmNoteByNoteRenderSnapshot), the physical-marker capture and the
// full/native draw feeds for the research bridge (stubs in this build), the highlighted note-head reconnaissance log,
// the native front-attack lifecycle trace (off unless a bridge command enabled it), shader disassembly logging, the
// frame-time telemetry, the Debug-only numeral decode trace and heap check. The DrawPrimitive signature check is left
// out too: it passed vertexCount 0, which no suppression signature matches.
namespace
{
	using Framework::DrawContext;
	using Framework::DrawOutcome;
	using Framework::DrawResult;

	// After the texture interceptors (colour mods 10..40), so what these redraw keeps the custom colours.
	constexpr int kFilterPriority = 100;

	constexpr unsigned int NOTE_BY_NOTE_INSTANCE_LIMIT = 64;
	constexpr UINT NOTE_BY_NOTE_TRANSFORM_STRIDE = 48;

	// Counts presented scenes (4.x: EndScene calls, FinishNoteByNoteRenderFrame). Render thread only.
	uint64_t noteByNoteRenderFrame = 0;
	// The frame the TARGET's own fretboard marker quads were last drawn (TryFilterStaleMarkerQuads),
	// for the gem-behind-marker rule in DecideNoteByNoteInstances.
	uint64_t targetMarkerLitFrame = 0;
	int targetMarkerString = -1;
	int targetMarkerFret = -1;
	std::atomic<uint64_t> gemBehindMarkerHiddenCount{ 0 };

	// Shadow of vertex-shader constant registers c0..c7 (4.x's D3DHooks::vertexShaderConstantShadow), read when the
	// device rejects GetVertexShaderConstantF (a pure device would).
	float vertexShaderConstantShadow[8][4] = {};

	// One indexed draw of a range of the game's call (4.x: oDrawIndexedPrimitive with startIndex/primitiveCount).
	HRESULT IssueIndexedRange(const DrawContext& draw, UINT startIndex, UINT primitiveCount)
	{
		Framework::DrawCall range = draw.call;
		range.startIndex = startIndex;
		range.primitiveCount = primitiveCount;
		return draw.DrawOriginal(range);
	}

	constexpr std::array<float, 24> RENDERED_FRET_CENTERS = {
		-50.469322397f,
		-44.428611770f,
		-38.738738817f,
		-33.162784301f,
		-27.754626393f,
		-22.552128018f,
		-17.477779710f,
		-12.512395748f,
		-7.722923673f,
		-3.060635789f,
		1.496101972f,
		5.931809946f,
		10.242484268f,
		14.448217957f,
		18.593132232f,
		22.626917848f,
		26.565256712f,
		30.427782892f,
		34.216594898f,
		37.947872265f,
		41.559112282f,
		45.059581500f,
		48.496791942f,
		51.933174267f
	};
	constexpr std::array<float, 6> RENDERED_STRING_HEIGHTS = {
		4.018f,
		2.410f,
		0.803f,
		-0.803f,
		-2.410f,
		-4.018f
	};

	bool TryReadInstanceRecords(
		IDirect3DDevice9* device,
		UINT streamIndex,
		UINT recordCount,
		UINT expectedStride,
		std::vector<float>& values)
	{
		IDirect3DVertexBuffer9* vertexBuffer = nullptr;
		UINT streamOffset = 0;
		UINT streamStride = 0;
		if (device->GetStreamSource(
			streamIndex,
			&vertexBuffer,
			&streamOffset,
			&streamStride) != D3D_OK
			|| vertexBuffer == nullptr)
		{
			return false;
		}

		if (streamStride != expectedStride)
		{
			vertexBuffer->Release();
			return false;
		}

		D3DVERTEXBUFFER_DESC description = {};
		const uint64_t byteCount = static_cast<uint64_t>(recordCount) * streamStride;
		if (vertexBuffer->GetDesc(&description) != D3D_OK
			|| static_cast<uint64_t>(streamOffset) + byteCount > description.Size)
		{
			vertexBuffer->Release();
			return false;
		}

		void* lockedData = nullptr;
		const HRESULT lockResult = vertexBuffer->Lock(
			streamOffset,
			static_cast<UINT>(byteCount),
			&lockedData,
			D3DLOCK_READONLY);
		if (lockResult != D3D_OK || lockedData == nullptr)
		{
			vertexBuffer->Release();
			return false;
		}

		values.resize(static_cast<size_t>(byteCount) / sizeof(float));
		std::memcpy(values.data(), lockedData, static_cast<size_t>(byteCount));
		vertexBuffer->Unlock();
		vertexBuffer->Release();
		return true;
	}

	int DecodeRenderedFret(float fretPosition)
	{
		size_t closestIndex = 0;
		float closestDistance = (std::numeric_limits<float>::max)();
		for (size_t fretIndex = 0; fretIndex < RENDERED_FRET_CENTERS.size(); ++fretIndex)
		{
			const float rightHandedDistance = std::abs(
				fretPosition - RENDERED_FRET_CENTERS[fretIndex]);
			const float leftHandedDistance = std::abs(
				fretPosition + RENDERED_FRET_CENTERS[fretIndex]);
			const float distance = (std::min)(rightHandedDistance, leftHandedDistance);
			if (distance >= closestDistance) continue;

			closestIndex = fretIndex;
			closestDistance = distance;
		}
		return static_cast<int>(closestIndex) + 1;
	}

	bool TryDecodePhysicalMarkerTransform(
		const float transform[4][4],
		int& stringIndex,
		int& fret,
		const char** failureStage = nullptr,
		bool enforceLongitudinalLimit = true)
	{
		constexpr float AFFINE_EPSILON = 0.01f;
		constexpr float LONGITUDINAL_LIMIT = 5.0f;
		constexpr float FRET_EPSILON = 0.5f;
		constexpr float STRING_EPSILON = 0.4f;

		if (std::abs(transform[3][0]) > AFFINE_EPSILON
			|| std::abs(transform[3][1]) > AFFINE_EPSILON
			|| std::abs(transform[3][2]) > AFFINE_EPSILON
			|| std::abs(transform[3][3] - 1.0f) > AFFINE_EPSILON)
		{
			if (failureStage != nullptr) *failureStage = "not-affine";
			return false;
		}
		if (enforceLongitudinalLimit && std::abs(transform[0][3]) > LONGITUDINAL_LIMIT)
		{
			if (failureStage != nullptr) *failureStage = "longitudinal";
			return false;
		}

		float closestFretDistance = (std::numeric_limits<float>::max)();
		int closestFret = -1;
		for (size_t fretIndex = 0; fretIndex < RENDERED_FRET_CENTERS.size(); ++fretIndex)
		{
			const auto centre = RENDERED_FRET_CENTERS[fretIndex];
			const auto distance = (std::min)(
				std::abs(transform[1][3] - centre),
				std::abs(transform[1][3] + centre));
			if (distance >= closestFretDistance) continue;

			closestFretDistance = distance;
			closestFret = static_cast<int>(fretIndex) + 1;
		}
		if (closestFretDistance > FRET_EPSILON)
		{
			if (failureStage != nullptr) *failureStage = "fret";
			return false;
		}

		// Bass neck: the note-head panels that preview upcoming notes on the neck (stride 32 /
		// 187 verts / 104 prims) sit at the bass heights +-1.339 / +-4.018. On the guitar table
		// the A and D strings miss by 0.536 (> 0.4) and G decodes as guitar string 5.
		// Transforms are exact (no animation drift here), so 0.6 (under half the 2.679 bass
		// spacing) is enough.
		bool isBass = false;
		ArrangementInstrument::TryReadIsBass(isBass);
		constexpr std::array<float, 4> BASS_HEIGHTS = { 4.018f, 1.339f, -1.339f, -4.018f };
		constexpr float BASS_STRING_EPSILON = 0.6f;
		const float* heights = isBass ? BASS_HEIGHTS.data() : RENDERED_STRING_HEIGHTS.data();
		const size_t heightCount = isBass ? BASS_HEIGHTS.size() : RENDERED_STRING_HEIGHTS.size();
		const float stringEpsilon = isBass ? BASS_STRING_EPSILON : STRING_EPSILON;

		float closestStringDistance = (std::numeric_limits<float>::max)();
		int closestString = -1;
		for (size_t index = 0; index < heightCount; ++index)
		{
			const auto distance = std::abs(transform[2][3] - heights[index]);
			if (distance >= closestStringDistance) continue;

			closestStringDistance = distance;
			closestString = static_cast<int>(index);
		}
		if (closestStringDistance > stringEpsilon)
		{
			if (failureStage != nullptr) *failureStage = "string";
			return false;
		}

		stringIndex = closestString;
		fret = closestFret;
		return true;
	}

	// Stale fretboard marker quad filter. The marker boxes are DrawIndexedPrimitive
	// tri-strips from 0xBEC0DA (inside FUN_00BEBCD0), stride 12, drawn from a per-frame
	// rebuilt dynamic buffer. Each quad is a 6-index degenerate-bridged block whose vertex
	// positions live in (x, stringHeight, fretCentre) space, at exactly the
	// RENDERED_FRET_CENTERS / RENDERED_STRING_HEIGHTS values. No record, placement or
	// context write feeds this stream; the quads are re-emitted every frame from a source
	// object that survives target changes. So the filter acts at the draw itself: decode
	// each quad block, keep the current target's quads (and any block that does not decode
	// onto the tables), and re-issue only the kept runs, the same subset strategy the
	// instance filter uses for note heads. Active only while Note by Note holds a target.
	std::atomic<bool> isStaleMarkerFilterEnabled{ true };
	std::atomic<unsigned long long> staleMarkerQuadsConsidered{ 0 };
	std::atomic<unsigned long long> staleMarkerQuadsDropped{ 0 };
	std::atomic<unsigned long long> staleMarkerDrawsFiltered{ 0 };
	const uintptr_t MARKER_SLICE_CALLER = AudioBridge::GameVersion::Address(0x00BEC0DA, 0x007EB22A);
	// The batched glyph flush that draws the fingering NUMERALS (otherwise a suppressed
	// upcoming chord leaves its floating finger number behind). One triangle-list draw carries ~21 glyph quads including
	// the kept members' numerals, so it filters per quad exactly like the marker
	// slices; glyphs that do not land on a marker coordinate (fret numbers below the
	// strings, highway labels) never decode and are always kept.
	const uintptr_t GLYPH_FLUSH_CALLER = AudioBridge::GameVersion::Address(0x00DFC98C, 0x009FB9CC);
	constexpr float MARKER_QUAD_FRET_EPSILON = 1.2f;
	constexpr float MARKER_QUAD_STRING_EPSILON = 0.6f;
	// Bass neck. RENDERED_STRING_HEIGHTS is the guitar neck; on a bass arrangement the four
	// strings spread over the same width 2.679 apart, so the A and D strings (+-1.339) land
	// 0.536 from guitar strings 2 and 3 and would decode as the wrong string. The marker
	// animates over roughly A 0.80..1.41, D -1.29..-1.93; 0.9 covers that drift and stays
	// under half the spacing.
	constexpr std::array<float, 4> RENDERED_BASS_STRING_HEIGHTS = { 4.018f, 1.339f, -1.339f, -4.018f };
	constexpr float MARKER_QUAD_BASS_STRING_EPSILON = 0.9f;
	// Lock flags for the read-back. A plain READONLY lock on the game's DYNAMIC marker
	// buffers makes the runtime wait for the GPU to finish with them (several FPS lost).
	// NOOVERWRITE is the caller's promise not to touch anything the GPU is still reading,
	// which a read never does, so the runtime hands the current chunk back without the
	// wait. Only valid on D3DUSAGE_DYNAMIC buffers; anything else keeps the plain lock.
	// staleMarkerLockWaits counts the plain (waiting) locks so the bridge shows whether the
	// fast path is actually being taken.
	std::atomic<unsigned long long> staleMarkerLockWaits{ 0 };
	std::atomic<unsigned long long> staleMarkerLockNoWaits{ 0 };
	DWORD StaleMarkerLockFlags(DWORD usage)
	{
		if ((usage & D3DUSAGE_DYNAMIC) != 0)
		{
			staleMarkerLockNoWaits.fetch_add(1, std::memory_order_relaxed);
			return D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE;
		}
		staleMarkerLockWaits.fetch_add(1, std::memory_order_relaxed);
		return D3DLOCK_READONLY;
	}

	// Coordinates whose MARKER quads this filter dropped in the current or previous
	// render frame. The glyph flush may only drop a numeral that ACCOMPANIES a dropped
	// marker box: a stale marker and its floating finger number come as a pair, but a
	// LIVE note (a hammer-on's numeral, an upcoming note's number) also legitimately sits
	// on marker coordinates and must stay. Render-thread only, like the anchor table. The one-frame
	// grace covers a flush that lands on the far side of the frame boundary from its
	// marker slices.
	constexpr size_t STALE_COORD_CAPACITY = 64;
	int staleCoordStrings[STALE_COORD_CAPACITY] = {};
	int staleCoordFrets[STALE_COORD_CAPACITY] = {};
	uint64_t staleCoordFrames[STALE_COORD_CAPACITY] = {};
	size_t staleCoordCursor = 0;

	// Defined with the host-numeral bookkeeping below.
	void RecordRestingFingerRing(int stringIndex, int fret);

	void RecordDroppedMarkerCoordinate(int stringIndex, int fretIndex)
	{
		for (size_t index = 0; index < STALE_COORD_CAPACITY; ++index)
		{
			if (staleCoordStrings[index] == stringIndex
				&& staleCoordFrets[index] == fretIndex
				&& staleCoordFrames[index] + 1 >= noteByNoteRenderFrame)
			{
				staleCoordFrames[index] = noteByNoteRenderFrame;
				return;
			}
		}
		staleCoordStrings[staleCoordCursor] = stringIndex;
		staleCoordFrets[staleCoordCursor] = fretIndex;
		staleCoordFrames[staleCoordCursor] = noteByNoteRenderFrame;
		staleCoordCursor = (staleCoordCursor + 1) % STALE_COORD_CAPACITY;
	}

	bool WasMarkerCoordinateDroppedRecently(int stringIndex, int fretIndex)
	{
		for (size_t index = 0; index < STALE_COORD_CAPACITY; ++index)
		{
			if (staleCoordStrings[index] == stringIndex
				&& staleCoordFrets[index] == fretIndex
				&& staleCoordFrames[index] != 0
				&& staleCoordFrames[index] + 1 >= noteByNoteRenderFrame)
			{
				return true;
			}
		}
		return false;
	}

	bool TryFilterStaleMarkerQuads(
		const DrawContext& draw,
		IDirect3DDevice9* device,
		D3DPRIMITIVETYPE primitiveType,
		INT baseVertexIndex,
		UINT startIndex,
		UINT primitiveCount,
		UINT stride,
		uintptr_t nativeCaller,
		HRESULT& result)
	{
		if (!isStaleMarkerFilterEnabled.load(std::memory_order_acquire)) return false;
		// Two filterable draw shapes share the block machinery: the marker slices
		// (degenerate-bridged tri-strips, 6 indices per quad, run primCount
		// 6*n - 2) and the glyph flush (plain tri-list, 6 indices per quad, run
		// primCount 2*n).
		const bool isMarkerSlice = nativeCaller == MARKER_SLICE_CALLER
			&& stride == 12
			&& primitiveType == D3DPT_TRIANGLESTRIP
			&& primitiveCount >= 4
			&& primitiveCount <= 64
			&& (primitiveCount + 2) % 6 == 0;
		const bool isGlyphFlush = nativeCaller == GLYPH_FLUSH_CALLER
			&& stride == 24
			&& primitiveType == D3DPT_TRIANGLELIST
			&& primitiveCount >= 2
			&& primitiveCount <= 128
			&& primitiveCount % 2 == 0;
		if (!isMarkerSlice && !isGlyphFlush) return false;

		// The glyph flush at 0x00DFC98C (and this filter machinery) also carries MENU LABELS
		// and the SPEED/DIFFICULTY/MISSED HUD text. While a pause/practice/song-select/
		// calibration overlay owns the frame, NBN still holds its target so the keep-set is
		// non-empty and this filter would run over the menu/HUD text quads, dropping the ones
		// that are not on a marker coordinate (garbled labels such as "S EED / DIFFICULT").
		// Only filter during active Learn-A-Song play (currentMenu "LearnASong_Game" /
		// "NonStopPlay_Game"), where the flush is the gameplay fingering numerals. Every menu,
		// pause, and overlay state is excluded, so their text draws untouched. Checked after
		// the signature so the menu-name scan only runs for the two draw shapes it guards.
		if (!NoteByNoteProbe::IsInLasPlayingFast()) return false;

		// The keep set: single target plus legato group during note holds, fretted chord
		// members during chord holds. Empty set = filter dormant, everything draws.
		int keepStrings[NoteByNoteHighwayRenderer::MaxMarkerKeep];
		int keepFrets[NoteByNoteHighwayRenderer::MaxMarkerKeep];
		int keepCount = 0;
		if (!NoteByNoteHighwayRenderer::TryGetMarkerKeepCoordinates(
			keepStrings,
			keepFrets,
			keepCount)
			|| keepCount == 0)
		{
			return false;
		}
		// Read at the draw, not cached: four pointer loads, and only while a target is held.
		bool isBass = false;
		ArrangementInstrument::TryReadIsBass(isBass);

		const UINT indexCount = isMarkerSlice
			? primitiveCount + 2
			: primitiveCount * 3;
		const UINT blockCount = indexCount / 6;
		uint32_t indices[390] = {};
		if (indexCount > 390 || blockCount > 64) return false;

		IDirect3DIndexBuffer9* indexBuffer = nullptr;
		if (FAILED(device->GetIndices(&indexBuffer)) || indexBuffer == nullptr) return false;
		D3DINDEXBUFFER_DESC indexDescription = {};
		bool hasIndices = false;
		if (SUCCEEDED(indexBuffer->GetDesc(&indexDescription)))
		{
			const UINT indexSize = indexDescription.Format == D3DFMT_INDEX32 ? 4 : 2;
			const uint64_t offset = static_cast<uint64_t>(startIndex) * indexSize;
			const uint64_t byteCount = static_cast<uint64_t>(indexCount) * indexSize;
			void* data = nullptr;
			if (offset + byteCount <= indexDescription.Size
				&& SUCCEEDED(indexBuffer->Lock(
					static_cast<UINT>(offset),
					static_cast<UINT>(byteCount),
					&data,
					StaleMarkerLockFlags(indexDescription.Usage)))
				&& data != nullptr)
			{
				for (UINT index = 0; index < indexCount; ++index)
				{
					indices[index] = indexSize == 4
						? static_cast<const uint32_t*>(data)[index]
						: static_cast<const uint16_t*>(data)[index];
				}
				hasIndices = true;
				indexBuffer->Unlock();
			}
		}
		indexBuffer->Release();
		if (!hasIndices) return false;

		IDirect3DVertexBuffer9* vertexBuffer = nullptr;
		UINT streamOffset = 0;
		UINT boundStride = 0;
		if (FAILED(device->GetStreamSource(0, &vertexBuffer, &streamOffset, &boundStride))
			|| vertexBuffer == nullptr)
		{
			return false;
		}
		bool keepBlock[64] = {};
		bool didDecode = false;
		D3DVERTEXBUFFER_DESC vertexDescription = {};
		void* data = nullptr;
		if (boundStride >= 3 * sizeof(float)
			&& SUCCEEDED(vertexBuffer->GetDesc(&vertexDescription))
			&& SUCCEEDED(vertexBuffer->Lock(
				0, 0, &data, StaleMarkerLockFlags(vertexDescription.Usage)))
			&& data != nullptr)
		{
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (UINT block = 0; block < blockCount; ++block)
			{
				keepBlock[block] = true;
				float minHeight = 0.0f;
				float maxHeight = 0.0f;
				float minFret = 0.0f;
				float maxFret = 0.0f;
				float minAcross = 0.0f;
				float maxAcross = 0.0f;
				bool hasVertices = false;
				for (UINT corner = 0; corner < 6; ++corner)
				{
					const uint64_t vertexIndex =
						static_cast<uint64_t>(baseVertexIndex)
						+ indices[block * 6 + corner];
					const uint64_t byteOffset = static_cast<uint64_t>(streamOffset)
						+ vertexIndex * boundStride;
					if (byteOffset + 3 * sizeof(float) > vertexDescription.Size) continue;
					float position[3];
					std::memcpy(position, bytes + byteOffset, sizeof(position));
					if (!hasVertices || position[1] < minHeight) minHeight = position[1];
					if (!hasVertices || position[1] > maxHeight) maxHeight = position[1];
					if (!hasVertices || position[2] < minFret) minFret = position[2];
					if (!hasVertices || position[2] > maxFret) maxFret = position[2];
					if (!hasVertices || position[0] < minAcross) minAcross = position[0];
					if (!hasVertices || position[0] > maxAcross) maxAcross = position[0];
					hasVertices = true;
				}
				if (!hasVertices) continue;

				const float heightCentre = (minHeight + maxHeight) * 0.5f;
				const float fretCentre = (minFret + maxFret) * 0.5f;
				int stringIndex = -1;
				const float* stringHeights = isBass
					? RENDERED_BASS_STRING_HEIGHTS.data() : RENDERED_STRING_HEIGHTS.data();
				const size_t stringCount = isBass
					? RENDERED_BASS_STRING_HEIGHTS.size() : RENDERED_STRING_HEIGHTS.size();
				float stringDistance = isBass
					? MARKER_QUAD_BASS_STRING_EPSILON : MARKER_QUAD_STRING_EPSILON;
				for (size_t index = 0; index < stringCount; ++index)
				{
					const float distance =
						std::abs(heightCentre - stringHeights[index]);
					if (distance < stringDistance)
					{
						stringDistance = distance;
						stringIndex = static_cast<int>(index);
					}
				}
				int fretIndex = -1;
				float fretDistance = MARKER_QUAD_FRET_EPSILON;
				for (size_t index = 0; index < RENDERED_FRET_CENTERS.size(); ++index)
				{
					const float distance =
						std::abs(fretCentre - RENDERED_FRET_CENTERS[index]);
					if (distance < fretDistance)
					{
						fretDistance = distance;
						fretIndex = static_cast<int>(index);
					}
				}
				// Left-handed necks mirror the fret axis (fret centre -> -centre), the convention
				// DecodeRenderedFret and the panel decoder also use. Tried only when no
				// right-handed fret matched, so every right-handed decode is unchanged.
				if (fretIndex < 0)
				{
					fretDistance = MARKER_QUAD_FRET_EPSILON;
					for (size_t index = 0; index < RENDERED_FRET_CENTERS.size(); ++index)
					{
						const float distance =
							std::abs(fretCentre + RENDERED_FRET_CENTERS[index]);
						if (distance < fretDistance)
						{
							fretDistance = distance;
							fretIndex = static_cast<int>(index);
						}
					}
				}
				// A block that does not decode onto both tables is not provably a
				// marker quad; it is always kept.
				if (stringIndex < 0 || fretIndex < 0) continue;

				staleMarkerQuadsConsidered.fetch_add(1, std::memory_order_relaxed);
				didDecode = true;
				bool isKept = false;
				for (int keep = 0; keep < keepCount; ++keep)
				{
					if (stringIndex == keepStrings[keep]
						&& fretIndex + 1 == keepFrets[keep])
					{
						isKept = true;
						break;
					}
				}
				if (isKept)
				{
					if (isGlyphFlush)
					{
						RecordRestingFingerRing(stringIndex, fretIndex + 1);
					}
					else
					{
						int targetString = -1;
						int targetFret = -1;
						if (NoteByNoteHighwayRenderer::TryGetSelectedPresentation(targetString, targetFret)
							&& stringIndex == targetString && fretIndex + 1 == targetFret)
						{
							targetMarkerLitFrame = noteByNoteRenderFrame;
							targetMarkerString = targetString;
							targetMarkerFret = targetFret;
						}
					}
					continue;
				}
				if (isGlyphFlush)
				{
					// Numerals drop only in the company of their dropped marker box
					// (see the stale-coordinate record above); a live note's numeral
					// on a marker coordinate is kept.
					if (!WasMarkerCoordinateDroppedRecently(stringIndex, fretIndex))
					{
						continue;
					}
				}
				else
				{
					RecordDroppedMarkerCoordinate(stringIndex, fretIndex);
				}
				keepBlock[block] = false;
				staleMarkerQuadsDropped.fetch_add(1, std::memory_order_relaxed);
			}
			vertexBuffer->Unlock();
		}
		vertexBuffer->Release();
		if (!didDecode) return false;

		bool droppedAny = false;
		for (UINT block = 0; block < blockCount; ++block)
		{
			if (!keepBlock[block]) droppedAny = true;
		}
		if (!droppedAny) return false;

		staleMarkerDrawsFiltered.fetch_add(1, std::memory_order_relaxed);
		result = D3D_OK;
		UINT block = 0;
		while (block < blockCount)
		{
			if (!keepBlock[block])
			{
				++block;
				continue;
			}
			UINT runEnd = block;
			while (runEnd + 1 < blockCount && keepBlock[runEnd + 1]) ++runEnd;
			const UINT runBlocks = runEnd - block + 1;
			result = IssueIndexedRange(
				draw,
				startIndex + block * 6,
				isMarkerSlice ? runBlocks * 6 - 2 : runBlocks * 2);
			block = runEnd + 1;
		}
		return true;
	}

	// Draw suppression by runtime-selected mesh signature: the bridge can select a
	// stride/vertexCount/primitiveCount triple and toggle it live.
	std::atomic<bool> isPhysicalGuideSuppressionEnabled{ true };
	std::atomic<unsigned long long> physicalGuideConsideredDrawCount{ 0 };
	// The ghost fingering markers survive the native frozen mode, every note-head render
	// context and every neck-placement site: they are physical marker meshes with the
	// signature stride 32, 187 vertices, 104 primitives, shared with the target's own
	// marker. So suppression is DECODE-AWARE: a signature match during a hold decodes the
	// draw's own string/fret from the shader transform and suppresses only NON-TARGET
	// markers; the target's fingering and colours stay. guide-suppress-off disables it.
	std::atomic<unsigned long long> physicalGuideSuppressedDrawCount{ 0 };
	std::atomic<uint32_t> physicalGuideSuppressStride{ 32 };
	std::atomic<uint32_t> physicalGuideSuppressVertexCount{ 187 };
	std::atomic<uint32_t> physicalGuideSuppressPrimitiveCount{ 104 };

	// Host-drawn finger numerals: the game's own numeral glyphs are screen-space quads
	// that never repaint on frozen retargets, so the overlay draws the template's fingers
	// itself. Anchors are collected here at draw time: a kept member's fingering-panel
	// draw carries the marker's world position in c0..c3's translation (the axis-swap
	// world matrix) and the view-projection in c4..c7 (the same row convention note heads
	// carry in c0..c3). Render-thread only, like the suppressors around it.
	std::atomic<bool> isFingerNumeralsEnabled{ true };
	struct FingerAnchor
	{
		int stringIndex = -1;
		int fret = -1;
		float screenX = 0.0f;
		float screenY = 0.0f;
		uint64_t renderFrame = 0;
	};
	constexpr size_t FINGER_ANCHOR_CAPACITY = 8;
	// noteByNoteRenderFrame counts EndScene calls, and Rocksmith ends several scenes per
	// presented frame. With a one-scene window the anchors from the fretboard pass were already
	// stale by the final pass, the only one whose overlay text reaches the screen, so no host
	// numeral ever showed (#99: fixed test text drew, anchor-placed text did not). Anchors stay
	// fresh for a few scenes, well under a frame's worth.
	constexpr uint64_t FINGER_ANCHOR_FRESH_SCENES = 8;
	FingerAnchor fingerAnchors[FINGER_ANCHOR_CAPACITY];

	// Projection convention: at the FINGERING-PANEL draw (32/187/104) the fretboard
	// camera's view-projection sits in c4..c7 as rows (clip_i = dot(c(4+i), world)).
	// Ring/mini-quad shaders carry different c4..c7, and the note-head c0..c3 is the
	// HIGHWAY camera, not the fretboard camera, so anchors come from panel draws ONLY,
	// through that draw's own c4..c7.
	void RecordFingerAnchor(
		IDirect3DDevice9* device,
		int stringIndex,
		int fret)
	{
		if (!isFingerNumeralsEnabled.load(std::memory_order_acquire)) return;

		float constants[8][4] = {};
		if (device->GetVertexShaderConstantF(0, &constants[0][0], 8) != D3D_OK)
		{
			std::memcpy(constants, vertexShaderConstantShadow, sizeof(constants));
		}
		// World point = the marker transform's translation column
		// (longitudinal, fret position, string height).
		const float world[4] = {
			constants[0][3], constants[1][3], constants[2][3], 1.0f };
		float clip[4];
		for (int row = 0; row < 4; ++row)
		{
			clip[row] = constants[4 + row][0] * world[0]
				+ constants[4 + row][1] * world[1]
				+ constants[4 + row][2] * world[2]
				+ constants[4 + row][3] * world[3];
		}
		if (!std::isfinite(clip[3]) || std::abs(clip[3]) < 1e-6f) return;
		const float ndcX = clip[0] / clip[3];
		const float ndcY = clip[1] / clip[3];
		if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return;
		// A behind-the-camera or wildly off-screen result means c4..c7 did not hold
		// the view-projection for this draw; drop it rather than paint a stray.
		if (clip[3] < 0.0f || ndcX < -1.5f || ndcX > 1.5f || ndcY < -1.5f || ndcY > 1.5f)
		{
			return;
		}

		// Stored as [0..1] screen FRACTIONS, not viewport pixels: the 3D scene's
		// viewport can be smaller than the window/backbuffer the overlay's font draws
		// into. Fractions cancel the viewport out; the overlay multiplies by its own
		// window size.
		FingerAnchor anchor;
		anchor.stringIndex = stringIndex;
		anchor.fret = fret;
		anchor.screenX = ndcX * 0.5f + 0.5f;
		anchor.screenY = 0.5f - ndcY * 0.5f;
		anchor.renderFrame = noteByNoteRenderFrame;

		// Overwrite the slot for this coordinate, else claim the stalest slot.
		size_t slot = 0;
		uint64_t oldestFrame = (std::numeric_limits<uint64_t>::max)();
		for (size_t index = 0; index < FINGER_ANCHOR_CAPACITY; ++index)
		{
			if (fingerAnchors[index].stringIndex == stringIndex
				&& fingerAnchors[index].fret == fret)
			{
				slot = index;
				break;
			}
			if (fingerAnchors[index].renderFrame < oldestFrame)
			{
				oldestFrame = fingerAnchors[index].renderFrame;
				slot = index;
			}
		}
		fingerAnchors[slot] = anchor;
	}

	// Flow freeze doubles. Every keep rule matches by string/fret only, and a flow freeze-back
	// leaves a second copy of the SAME note's marker standing a little up the neck (drawn while the
	// transport ran past the note), so both copies would pass as "the target". Per marker class and kept
	// coordinate, only the copy nearest the hit line (the greatest longitudinal position, the same
	// convention the instance filter uses) is drawn; the decision uses the previous frame's nearest,
	// since a farther copy can draw before the nearer one within a frame.
	struct NearestMarkerCopy
	{
		int markerClass = -1;
		int stringIndex = -1;
		int fret = -1;
		uint64_t frame = 0;
		float frameNearest = 0.0f;
		float previousNearest = 0.0f;
		bool hasPrevious = false;
	};
	constexpr size_t NEAREST_MARKER_CAPACITY = 32;
	NearestMarkerCopy nearestMarkerCopies[NEAREST_MARKER_CAPACITY];
	std::atomic<uint64_t> fartherMarkerCopySuppressedCount{ 0 };
	constexpr float FARTHER_COPY_MARGIN = 0.3f;

	bool IsFartherSameCoordinateCopy(int markerClass, int stringIndex, int fret, float longitudinal)
	{
		if (!std::isfinite(longitudinal)) return false;
		NearestMarkerCopy* entry = nullptr;
		size_t stalest = 0;
		for (size_t index = 0; index < NEAREST_MARKER_CAPACITY; ++index)
		{
			auto& candidate = nearestMarkerCopies[index];
			if (candidate.markerClass == markerClass && candidate.stringIndex == stringIndex
				&& candidate.fret == fret)
			{
				entry = &candidate;
				break;
			}
			if (candidate.frame < nearestMarkerCopies[stalest].frame) stalest = index;
		}
		if (entry == nullptr)
		{
			entry = &nearestMarkerCopies[stalest];
			*entry = {};
			entry->markerClass = markerClass;
			entry->stringIndex = stringIndex;
			entry->fret = fret;
		}
		const uint64_t frame = noteByNoteRenderFrame;
		if (entry->frame != frame)
		{
			// A coordinate not drawn last frame starts fresh rather than comparing to old history.
			entry->hasPrevious = entry->frame != 0 && entry->frame + 1 == frame;
			entry->previousNearest = entry->frameNearest;
			entry->frame = frame;
			entry->frameNearest = longitudinal;
		}
		else if (longitudinal > entry->frameNearest)
		{
			entry->frameNearest = longitudinal;
		}
		if (!entry->hasPrevious || longitudinal >= entry->previousNearest - FARTHER_COPY_MARGIN) return false;

		const auto total = fartherMarkerCopySuppressedCount.fetch_add(1, std::memory_order_relaxed) + 1;
		if (total <= 12 || total % 600 == 0)
		{
			LOG_INFO("(NBN MARKERS) Farther copy of the target marker hidden: class=" << markerClass
				<< " string=" << stringIndex << " fret=" << fret << " longitudinal=" << longitudinal
				<< " nearest=" << entry->previousNearest << " count=" << total << std::endl);
		}
		return true;
	}

	std::atomic<unsigned long long> strandedCompanionSuppressedCount{ 0 };

	// Members whose game finger NUMERAL drew this frame (recorded by the glyph flush filter when
	// it keeps a numeral quad on a member coordinate). The host numeral is drawn only for a
	// member without one, so a frozen chord shows one number per box. Keyed on the numeral, not
	// the ring: a capture of a frozen G5 showed G7's ring at rest with no "3" drawn, since the
	// game lays its numerals out once and does not repaint them for a frozen retarget.
	// Render-thread only, like the anchor table.
	struct RestingFingerRing
	{
		int stringIndex = -1;
		int fret = -1;
		uint64_t renderFrame = 0;
	};
	constexpr size_t RESTING_RING_CAPACITY = 8;
	RestingFingerRing restingFingerRings[RESTING_RING_CAPACITY];

	void RecordRestingFingerRing(int stringIndex, int fret)
	{
		size_t slot = 0;
		for (size_t index = 0; index < RESTING_RING_CAPACITY; ++index)
		{
			auto& ring = restingFingerRings[index];
			if (ring.stringIndex == stringIndex && ring.fret == fret)
			{
				slot = index;
				break;
			}
			if (ring.renderFrame < restingFingerRings[slot].renderFrame) slot = index;
		}
		restingFingerRings[slot] = { stringIndex, fret, noteByNoteRenderFrame };
	}

	// A kept member's fingering panel frozen part-way through its grow-in (a live capture read
	// scales 0.71-0.89 against 1.0 at rest) draws small and can sit under its neighbour's box.
	// The hold is the moment the chord should be fully shown, so a kept panel is drawn at full
	// size: the transform's 3x3 is rescaled to unit columns, translation untouched. Only kept
	// members during a hold reach this, and only a plausible grow-in scale is corrected.
	void NormalizeHeldPanelScale(IDirect3DDevice9* device, const float (&transform)[4][4])
	{
		const float scale = std::sqrt(transform[0][0] * transform[0][0]
			+ transform[1][0] * transform[1][0]
			+ transform[2][0] * transform[2][0]);
		if (!std::isfinite(scale) || scale < 0.25f || scale > 0.98f) return;
		float rescaled[4][4];
		std::memcpy(rescaled, transform, sizeof(rescaled));
		for (int row = 0; row < 3; ++row)
		{
			for (int column = 0; column < 3; ++column) rescaled[row][column] /= scale;
		}
		device->SetVertexShaderConstantF(0, &rescaled[0][0], 4);
	}

	bool ShouldSuppressPhysicalGuideDraw(
		IDirect3DDevice9* device,
		UINT stride,
		UINT vertexCount,
		UINT primitiveCount)
	{
		if (!isPhysicalGuideSuppressionEnabled.load(std::memory_order_acquire)) return false;
		// The fingering panel signature (stride 32, 187 vertices, 104
		// primitives, indexed). The stride-12/100-primitive class must not be
		// suppressed here: note-trail segments share that signature AND decode onto
		// string/fret coordinates because they lie along strings. The neck note marker
		// of that class draws through DrawPrimitiveUP instead; its suppression lives
		// in Hook_DPUP with the same decode gate.
		//
		// Two companion families draw with the panel (without them a suppressed
		// preview leaves its finger RINGS behind): the stride-12/8-vert/6-prim ring
		// and the stride-32/4-vert/2-prim marker mini-quad. Both are fretboard-anchored (the decode's longitudinal limit
		// rejects highway geometry), so the same decode + keep-set gate applies.
		const bool matchesRuntimeSignature =
			stride == physicalGuideSuppressStride.load(std::memory_order_acquire)
			&& vertexCount == physicalGuideSuppressVertexCount.load(std::memory_order_acquire)
			&& primitiveCount
				== physicalGuideSuppressPrimitiveCount.load(std::memory_order_acquire);
		const bool matchesRingSignature =
			stride == 12 && vertexCount == 8 && primitiveCount == 6;
		const bool matchesMiniQuadSignature =
			stride == 32 && vertexCount == 4 && primitiveCount == 2;
		if (!matchesRuntimeSignature && !matchesRingSignature && !matchesMiniQuadSignature)
		{
			return false;
		}

		// Glyph guard: HUD/menu text quads share the mini-quad signature (a single glyph
		// IS a stride-32 4-vertex 2-primitive quad), and screen-space text draws without a
		// vertex shader, so GetVertexShaderConstantF below reports the LAST shader draw's
		// constants, which can decode as a plausible marker and make the same letters
		// vanish every frame. A draw with no vertex shader bound cannot be a fingering
		// marker; never suppress it.
		IDirect3DVertexShader9* boundVertexShader = nullptr;
		if (device->GetVertexShader(&boundVertexShader) != D3D_OK) return false;
		if (boundVertexShader == nullptr) return false;
		boundVertexShader->Release();

		// The keep set, not a single target, so the suppressor also works during chord
		// holds (which have no single target). Note holds keep target + legato group;
		// chord holds keep the fretted chord members.
		int keepStrings[NoteByNoteHighwayRenderer::MaxMarkerKeep];
		int keepFrets[NoteByNoteHighwayRenderer::MaxMarkerKeep];
		int keepCount = 0;
		if (!NoteByNoteHighwayRenderer::TryGetMarkerKeepCoordinates(
			keepStrings,
			keepFrets,
			keepCount)
			|| keepCount == 0)
		{
			return false;
		}

		physicalGuideConsideredDrawCount.fetch_add(1, std::memory_order_relaxed);

		float transform[4][4] = {};
		if (device->GetVertexShaderConstantF(0, &transform[0][0], 4) != D3D_OK)
		{
			std::memcpy(transform, vertexShaderConstantShadow, sizeof(transform));
		}
		int stringIndex = -1;
		int fret = -1;
		// Finger rings and mini-quads stranded mid-animation. A live capture of a frozen C5
		// (D5 G5 B8) showed the G string's ring and mini-quad from the neighbouring G5 chord
		// (G7, finger 3) standing at longitudinal 5.68-5.73, just past the decoder's 5.0 limit,
		// while every at-rest ring sat at 0.48-0.73. Undecodable, they were never filtered, so
		// the frozen hold showed a floating "3" off the strings (and the same for a kept member
		// frozen mid-flight). A companion draw that decodes onto a string and fret with the limit
		// relaxed but stands in that band is not at rest on the neck: it is hidden whether or not
		// it is a member, and its coordinate is recorded so the glyph flush drops its numeral
		// with it. The cap keeps highway geometry (hundreds of units out) out of this rule.
		// A second capture (frozen G5) caught a ring part-way through that flight, still inside
		// the old 5.0 limit, showing its "3" beside a box that was not drawn. The band therefore
		// starts at 1.5: every at-rest ring and mini-quad measured sat at 0.48-0.73. The host
		// numeral (DisplayNoteByNoteStatus) covers a member whose ring this hides.
		constexpr float COMPANION_REST_LIMIT = 1.5f;
		constexpr float COMPANION_STRANDED_LIMIT = 12.0f;
		if ((matchesRingSignature || matchesMiniQuadSignature)
			&& transform[0][3] > COMPANION_REST_LIMIT
			&& transform[0][3] <= COMPANION_STRANDED_LIMIT
			&& TryDecodePhysicalMarkerTransform(transform, stringIndex, fret, nullptr, false))
		{
			RecordDroppedMarkerCoordinate(stringIndex, fret);
			const auto total = strandedCompanionSuppressedCount.fetch_add(1, std::memory_order_relaxed) + 1;
			if (total <= 6 || total % 600 == 0)
			{
				LOG_INFO("(NBN MARKERS) Stranded finger " << (matchesRingSignature ? "ring" : "mini-quad")
					<< " hidden: string=" << stringIndex << " fret=" << fret << " longitudinal="
					<< transform[0][3] << " count=" << total << std::endl);
			}
			physicalGuideSuppressedDrawCount.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		if (!TryDecodePhysicalMarkerTransform(transform, stringIndex, fret, nullptr))
		{
			// An undecodable draw with the marker signature is left alone: never
			// hide what cannot be identified.
			return false;
		}
		// Keep-set filtering for chord holds too: dropping the whole layer would take the
		// member BOXES with it and leave the fretboard empty, which reads worse than
		// panels whose numerals are coordinate-matched (and usually correct, since
		// coinciding coordinates tend to share fingers). The authoritative fingering
		// rides the overlay line either way. Repeated asks RE-SHOW the chord, never
		// blank it.
		for (int keep = 0; keep < keepCount; ++keep)
		{
			if (stringIndex == keepStrings[keep] && fret == keepFrets[keep])
			{
				// A kept member's own PANEL draw is the authoritative screen
				// anchor for the host-drawn finger numeral at that coordinate.
				// Panels only: the ring/mini-quad shaders carry different
				// c4..c7 and would poison the anchor table.
				const int markerClass = matchesRuntimeSignature ? 0 : (matchesRingSignature ? 1 : 2);
				if (IsFartherSameCoordinateCopy(markerClass, stringIndex, fret, transform[0][3]))
				{
					physicalGuideSuppressedDrawCount.fetch_add(1, std::memory_order_relaxed);
					return true;
				}
				if (matchesRuntimeSignature)
				{
					RecordFingerAnchor(device, stringIndex, fret);
					NormalizeHeldPanelScale(device, transform);
				}
				return false;
			}
		}

		physicalGuideSuppressedDrawCount.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool HasInstancedTransformStream(IDirect3DDevice9* device)
	{
		UINT indexedFrequency = 0;
		if (device->GetStreamSourceFreq(0, &indexedFrequency) != D3D_OK
			|| (indexedFrequency & D3DSTREAMSOURCE_INDEXEDDATA) == 0)
		{
			return false;
		}

		IDirect3DVertexBuffer9* transformBuffer = nullptr;
		UINT transformOffset = 0;
		UINT transformStride = 0;
		const HRESULT result = device->GetStreamSource(
			1,
			&transformBuffer,
			&transformOffset,
			&transformStride);
		if (transformBuffer != nullptr) transformBuffer->Release();
		return result == D3D_OK && transformStride == NOTE_BY_NOTE_TRANSFORM_STRIDE;
	}

	// Note by Note instance filter.
	//
	// Rocksmith draws note heads instanced: stream 0's frequency carries the instance
	// count, stream 1 holds one 48-byte transform per instance and stream 2 one 16-byte
	// gradient record. Hiding a note here means leaving its instance out of the draw, which
	// is non-destructive. The engine keeps building every note's state exactly as it would
	// normally, so nothing goes grey and nothing is left half-initialised.
	//
	// The kept instances are drawn by rebinding streams 1 and 2 at the offset of the
	// instance we want and issuing one draw per kept instance with an instance count of 1.
	// That needs no vertex buffers of our own, so there is nothing to release and recreate
	// around a device Reset.
	enum class InstanceFilterMode { DrawEverything, DrawNothing, DrawSubset };

	struct InstanceFilterDecision
	{
		InstanceFilterMode mode = InstanceFilterMode::DrawEverything;
		UINT instanceCount = 0;
		std::vector<UINT> keptInstances;
	};

	InstanceFilterDecision DecideNoteByNoteInstances(
		IDirect3DDevice9* device,
		UINT meshVertexCount)
	{
		InstanceFilterDecision decision;

		int targetString = -1;
		int targetFret = -1;
		const bool isOpenFrontFilter = meshVertexCount == 50
			&& NoteByNoteHighwayRenderer::GetHighwayMode()
				== NoteByNoteHighwayRenderer::HighwayMode::Off
			&& NoteByNoteHighwayRenderer::TryGetSelectedTargetForDiagnostics(targetString, targetFret)
			&& NoteByNoteProbe::IsInLasPlayingFast();
		if (!isOpenFrontFilter
			&& !NoteByNoteHighwayRenderer::TryGetSelectedPresentation(targetString, targetFret))
		{
			return decision;
		}

		UINT indexedFrequency = 0;
		if (device->GetStreamSourceFreq(0, &indexedFrequency) != D3D_OK
			|| (indexedFrequency & D3DSTREAMSOURCE_INDEXEDDATA) == 0)
		{
			return decision;
		}

		constexpr UINT FREQUENCY_VALUE_MASK = 0x3FFFFFFF;
		const UINT instanceCount = indexedFrequency & FREQUENCY_VALUE_MASK;
		if (instanceCount == 0 || instanceCount > NOTE_BY_NOTE_INSTANCE_LIMIT)
		{
			return decision;
		}

		std::vector<float> transforms;
		std::vector<float> gradients;
		if (!TryReadInstanceRecords(device, 1, instanceCount, 48, transforms)
			|| !TryReadInstanceRecords(device, 2, instanceCount, 16, gradients)
			|| transforms.size() != static_cast<size_t>(instanceCount) * 12
			|| gradients.size() != static_cast<size_t>(instanceCount) * 4)
		{
			// Unreadable instance data is left to Rocksmith rather than guessed at.
			return decision;
		}
		if (isOpenFrontFilter)
		{
			// The strike-line sweep spans the first 50 longitudinal units. The selected
			// open note may occupy it; later open notes stay visible farther up the highway.
			constexpr float FRONT_OPEN_LIMIT = -50.0f;
			constexpr float SAME_ATTACK_EPSILON = 0.05f;
			float selectedPosition = -(std::numeric_limits<float>::max)();
			if (targetFret == 0)
			{
				for (UINT index = 0; index < instanceCount; ++index)
				{
					const size_t transformOffset = static_cast<size_t>(index) * 12;
					const size_t gradientOffset = static_cast<size_t>(index) * 4;
					const float position = transforms[transformOffset + 3];
					const int stringIndex = static_cast<int>(std::lround(
						(gradients[gradientOffset] * 32.0f - 1.0f) / 2.0f));
					if (stringIndex == targetString && std::isfinite(position)
						&& position >= FRONT_OPEN_LIMIT && position > selectedPosition)
					{
						selectedPosition = position;
					}
				}
			}
			decision.instanceCount = instanceCount;
			for (UINT index = 0; index < instanceCount; ++index)
			{
				const size_t transformOffset = static_cast<size_t>(index) * 12;
				const size_t gradientOffset = static_cast<size_t>(index) * 4;
				const float position = transforms[transformOffset + 3];
				const int stringIndex = static_cast<int>(std::lround(
					(gradients[gradientOffset] * 32.0f - 1.0f) / 2.0f));
				if (!std::isfinite(position))
				{
					static bool hasLoggedInvalidOpenPosition = false;
					if (!hasLoggedInvalidOpenPosition)
					{
						hasLoggedInvalidOpenPosition = true;
						LOG_INFO("(NBN OPEN FRONT) Non-finite open note position; draw left unchanged."
							<< std::endl);
					}
					return InstanceFilterDecision{};
				}
				if (position < FRONT_OPEN_LIMIT
					|| (stringIndex == targetString
						&& std::abs(position - selectedPosition) <= SAME_ATTACK_EPSILON))
				{
					decision.keptInstances.push_back(index);
				}
			}
			if (decision.keptInstances.empty())
			{
				decision.mode = InstanceFilterMode::DrawNothing;
			}
			else if (decision.keptInstances.size() != instanceCount)
			{
				decision.mode = InstanceFilterMode::DrawSubset;
			}
			if (decision.mode != InstanceFilterMode::DrawEverything)
			{
				static bool hasLoggedOpenFrontFilter = false;
				if (!hasLoggedOpenFrontFilter)
				{
					hasLoggedOpenFrontFilter = true;
					LOG_INFO("(NBN OPEN FRONT) Kept " << decision.keptInstances.size()
						<< " of " << instanceCount << " open note heads for target "
						<< targetString << ':' << targetFret << std::endl);
				}
			}
			return decision;
		}

		// String and fret alone are not a unique identity: a phrase that repeats the same
		// fret puts several matching instances on the highway at once, and keeping them all
		// would leave copies of the target standing further up the neck. The target is the
		// match nearest the hit line, which is the greatest longitudinal position, since
		// future notes sit far more negative (measured around -130 against -1.3 for a note
		// at the line). Ties are kept together so a same-attack group stays intact.
		decision.instanceCount = instanceCount;
		// Tight on purpose: a flow freeze-back of ~0.16 s leaves the pre-rewind copy of the target
		// standing a fraction of a unit up the highway, and a wider epsilon would keep both copies.
		// Notes of one attack share one position exactly.
		constexpr float SAME_ATTACK_EPSILON = 0.05f;
		bool hasNearest = false;
		float nearestPosition = 0.0f;
		std::vector<std::pair<UINT, float>> matches;
		for (UINT instanceIndex = 0; instanceIndex < instanceCount; ++instanceIndex)
		{
			const size_t transformOffset = static_cast<size_t>(instanceIndex) * 12;
			const size_t gradientOffset = static_cast<size_t>(instanceIndex) * 4;
			const int instanceFret = meshVertexCount == 50
				? 0
				: DecodeRenderedFret(transforms[transformOffset + 7]);
			const int instanceString = static_cast<int>(std::lround(
				(gradients[gradientOffset] * 32.0f - 1.0f) / 2.0f));
			if (instanceString != targetString || instanceFret != targetFret) continue;

			const float longitudinalPosition = transforms[transformOffset + 3];
			if (!std::isfinite(longitudinalPosition)) continue;

			matches.emplace_back(instanceIndex, longitudinalPosition);
			if (!hasNearest || longitudinalPosition > nearestPosition)
			{
				hasNearest = true;
				nearestPosition = longitudinalPosition;
			}
		}
		// Gem behind the marker: every hold freezes
		// 53 ms (the engine compensation) before the note's time, so the target's highway gem rests
		// ~8 units short of the strike line, while on a relatched hold the game also lights the
		// target's fretboard marker ON the line - two boxes for one note. While that marker is lit
		// (this frame or the last; draw order varies), the resting gem is not drawn, so the one box
		// left sits on the string. A plain hold with no lit marker keeps its gem.
		const bool targetMarkerLit = hasNearest
			&& targetMarkerLitFrame != 0 && targetMarkerLitFrame + 1 >= noteByNoteRenderFrame
			&& targetMarkerString == targetString && targetMarkerFret == targetFret
			&& nearestPosition >= -20.0f && nearestPosition <= -1.0f;
		// The marker is lit on the target but the gem was not hidden, so its position falls outside
		// the window. Logged a few times per target to record the actual resting position.
		if (!targetMarkerLit && hasNearest && targetMarkerLitFrame != 0 && targetMarkerLitFrame + 1 >= noteByNoteRenderFrame
			&& targetMarkerString == targetString && targetMarkerFret == targetFret)
		{
			static int missLogTarget = -1;
			static int missLogCount = 0;
			const int key = targetString * 100 + targetFret;
			if (key != missLogTarget) { missLogTarget = key; missLogCount = 0; }
			if (missLogCount < 3)
			{
				++missLogCount;
				LOG_INFO("(NBN MARKERS) Target marker lit but its gem NOT hidden: target=" << targetString << ':'
					<< targetFret << " gemPosition=" << nearestPosition << " copies=" << matches.size()
					<< " (hide window -20..-1)" << std::endl);
			}
		}
		if (targetMarkerLit)
		{
			const auto total = gemBehindMarkerHiddenCount.fetch_add(1, std::memory_order_relaxed) + 1;
			if (total <= 6 || total % 3000 == 0)
			{
				LOG_INFO("(NBN MARKERS) Target gem resting behind its lit fretboard marker hidden: target="
					<< targetString << ':' << targetFret << " gemPosition=" << nearestPosition
					<< " count=" << total << std::endl);
			}
		}
		size_t closeCopies = 0;
		for (const auto& match : matches)
		{
			if (targetMarkerLit && std::abs(match.second - nearestPosition) <= SAME_ATTACK_EPSILON) continue;
			const float distance = std::abs(match.second - nearestPosition);
			if (distance > 0.0f && distance <= 0.5f) ++closeCopies;
			if (distance > SAME_ATTACK_EPSILON) continue;
			decision.keptInstances.push_back(match.first);
		}
		if (closeCopies != 0)
		{
			static uint64_t closeCopyLogs = 0;
			if (++closeCopyLogs <= 12 || closeCopyLogs % 600 == 0)
			{
				std::ostringstream positions;
				for (const auto& match : matches) positions << ' ' << match.second;
				LOG_INFO("(NBN MARKERS) Target note copies on the highway: target=" << targetString << ':'
					<< targetFret << " nearest=" << nearestPosition << " positions=" << positions.str()
					<< " (copies within 0.5 now hidden) count=" << closeCopyLogs << std::endl);
			}
		}

		if (decision.keptInstances.empty())
		{
			decision.mode = InstanceFilterMode::DrawNothing;
		}
		else if (decision.keptInstances.size() == instanceCount)
		{
			decision.mode = InstanceFilterMode::DrawEverything;
		}
		else
		{
			decision.mode = InstanceFilterMode::DrawSubset;
		}

		static bool hasLoggedInstanceFilter = false;
		if (!hasLoggedInstanceFilter && decision.mode != InstanceFilterMode::DrawEverything)
		{
			hasLoggedInstanceFilter = true;
			LOG_INFO("(NBN INSTANCE FILTER) Engaged at the D3D instance layer: kept "
				<< decision.keptInstances.size() << " of " << instanceCount
				<< " instances for target " << targetString << ':' << targetFret
				<< ". Per-note state is left fully maintained, so hidden notes keep their"
				<< " colour when they become visible again." << std::endl);
		}
		return decision;
	}

	// Draws only the kept instances by rebinding the instance streams at each kept record.
	// Returns false when the original bindings could not be read, in which case the caller
	// must fall back to Rocksmith's own draw rather than show nothing.
	bool DrawNoteByNoteSubset(
		const DrawContext& draw,
		IDirect3DDevice9* device,
		const InstanceFilterDecision& decision)
	{
		IDirect3DVertexBuffer9* transformBuffer = nullptr;
		IDirect3DVertexBuffer9* gradientBuffer = nullptr;
		UINT transformOffset = 0;
		UINT transformStride = 0;
		UINT gradientOffset = 0;
		UINT gradientStride = 0;
		UINT originalFrequency = 0;
		if (device->GetStreamSource(1, &transformBuffer, &transformOffset, &transformStride) != D3D_OK
			|| transformBuffer == nullptr)
		{
			return false;
		}
		if (device->GetStreamSource(2, &gradientBuffer, &gradientOffset, &gradientStride) != D3D_OK
			|| gradientBuffer == nullptr
			|| device->GetStreamSourceFreq(0, &originalFrequency) != D3D_OK)
		{
			transformBuffer->Release();
			if (gradientBuffer != nullptr) gradientBuffer->Release();
			return false;
		}

		device->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 1);
		for (const UINT instanceIndex : decision.keptInstances)
		{
			device->SetStreamSource(
				1,
				transformBuffer,
				transformOffset + instanceIndex * transformStride,
				transformStride);
			device->SetStreamSource(
				2,
				gradientBuffer,
				gradientOffset + instanceIndex * gradientStride,
				gradientStride);
			draw.DrawOriginal();
		}

		// Restore exactly what Rocksmith bound, so the rest of the frame is unaffected.
		device->SetStreamSource(1, transformBuffer, transformOffset, transformStride);
		device->SetStreamSource(2, gradientBuffer, gradientOffset, gradientStride);
		device->SetStreamSourceFreq(0, originalFrequency);
		transformBuffer->Release();
		gradientBuffer->Release();
		return true;
	}

	// 4.x's Hook_DIP, from the stale-marker filter to the instance filter, then the signature check its SHOW_TEXTURE
	// made on every draw it let through.
	DrawResult FilterIndexedDraw(DrawContext& ctx)
	{
		IDirect3DDevice9* const pDevice = ctx.device;
		const UINT Stride = ctx.mesh.Stride;
		const UINT PrimCount = ctx.mesh.PrimCount;
		const UINT NumVertices = ctx.mesh.NumVertices;
		const auto PrimType = static_cast<D3DPRIMITIVETYPE>(ctx.call.primitiveType);
		const bool isInSong = ctx.inSong;

		// Quad-level filtering of the stale fretboard marker slices.
		if (isInSong)
		{
			HRESULT staleMarkerResult = D3D_OK;
			if (TryFilterStaleMarkerQuads(
				ctx,
				pDevice,
				PrimType,
				ctx.call.baseVertexIndex,
				ctx.call.startIndex,
				PrimCount,
				Stride,
				reinterpret_cast<uintptr_t>(ctx.call.returnAddress),
				staleMarkerResult))
			{
				return { DrawOutcome::Hide };
			}
		}

		// Decode-aware signature suppression on the INDEXED path: the fingering panels,
		// finger rings and marker mini-quads all draw through DrawIndexedPrimitive. The
		// decode gate (affine transform, fretboard longitudinal limit, exact
		// fret/string landing) keeps this narrow; the keep set spares the target,
		// its legato group and the held chord's members.
		if (ShouldSuppressPhysicalGuideDraw(pDevice, Stride, NumVertices, PrimCount))
		{
			return { DrawOutcome::Hide };
		}
		if (!isInSong) return { DrawOutcome::Pass };

		const bool isNativeNoteAsset = IsToBeRemoved(sevenstring, ctx.mesh)
			|| IsExtraRemoved(noteModifiers, ctx.thicc);
		const bool isNativeAttackHead = Stride == 36
			&& ((PrimCount == 40 && (NumVertices == 50 || NumVertices == 76 || NumVertices == 78))
				|| (PrimCount == 27 && NumVertices == 57));
		// Idle-path budget: no device query unless the instance filter could act on this draw (a presentation is
		// selected, or the open-front filter applies).
		int openTargetString = -1;
		int openTargetFret = -1;
		const bool needsOpenFrontFilter = isNativeAttackHead && NumVertices == 50
			&& NoteByNoteHighwayRenderer::GetHighwayMode()
				== NoteByNoteHighwayRenderer::HighwayMode::Off
			&& NoteByNoteHighwayRenderer::TryGetSelectedTargetForDiagnostics(
				openTargetString, openTargetFret);
		const bool needsInstanceStreams = (isNativeNoteAsset || isNativeAttackHead)
			&& (needsOpenFrontFilter || NoteByNoteHighwayRenderer::HasSelectedPresentation());
		if (!needsInstanceStreams || !HasInstancedTransformStream(pDevice))
		{
			return { DrawOutcome::Pass };
		}

		// Note by Note owns note visibility here rather than in the native per-note calls.
		// Filtering the instance list is non-destructive: every note keeps its state, so a note
		// that becomes visible again is fully coloured instead of grey.
		//
		// Deciding requires locking Rocksmith's instance buffers, which forces a CPU/GPU sync,
		// so this is kept off every path it does not belong on. It is restricted to draws
		// already classified as note assets rather than every instanced draw, and the target
		// check inside runs before any lock, so nothing is locked at all while Note by Note is
		// idle. This matters beyond frame rate: a stall of a millisecond or two is invisible in
		// the frame counter but is enough to make the ASIO input callback miss its deadline,
		// which shows up as xruns and a stopped capture client rather than as dropped frames.
		const auto decision = DecideNoteByNoteInstances(pDevice, NumVertices);
		if (decision.mode == InstanceFilterMode::DrawNothing)
		{
			return { DrawOutcome::Hide };
		}
		if (decision.mode == InstanceFilterMode::DrawSubset
			&& DrawNoteByNoteSubset(ctx, pDevice, decision))
		{
			return { DrawOutcome::Hide };
		}
		return { DrawOutcome::Pass };
	}

	// 4.x's Hook_DPUP: suppresses the neck note marker during holds.
	DrawResult FilterPrimitiveUpDraw(DrawContext& ctx)
	{
		if (!ctx.inSong) return { DrawOutcome::Pass };
		IDirect3DDevice9* const pDevice = ctx.device;
		const UINT VertexStreamZeroStride = ctx.call.stream0Stride;
		const UINT PrimCount = ctx.call.primitiveCount;

		// The neck note marker (the fretboard "double" during holds): a
		// stride-12, 100-primitive user-pointer draw whose shader transform
		// decodes to the marker's string/fret with the longitudinal limit
		// relaxed (this UP path carries no other suppression). Same rules as the
		// signature suppressor: only during an active hold, only draws that
		// decode onto the real string/fret tables, and never the target's own.
		if (isPhysicalGuideSuppressionEnabled.load(std::memory_order_acquire)
			&& VertexStreamZeroStride == 12
			&& PrimCount == 100)
		{
			// Keep set instead of a single target, so this gate stays live during
			// chord holds too (see ShouldSuppressPhysicalGuideDraw).
			int keepStrings[NoteByNoteHighwayRenderer::MaxMarkerKeep];
			int keepFrets[NoteByNoteHighwayRenderer::MaxMarkerKeep];
			int keepCount = 0;
			if (NoteByNoteHighwayRenderer::TryGetMarkerKeepCoordinates(
				keepStrings,
				keepFrets,
				keepCount)
				&& keepCount > 0)
			{
				float transform[4][4] = {};
				if (pDevice->GetVertexShaderConstantF(0, &transform[0][0], 4) != D3D_OK)
				{
					std::memcpy(transform,
						vertexShaderConstantShadow,
						sizeof(transform));
				}
				int stringIndex = -1;
				int fret = -1;
				if (TryDecodePhysicalMarkerTransform(
						transform, stringIndex, fret, nullptr, false))
				{
					bool isKept = false;
					for (int keep = 0; keep < keepCount; ++keep)
					{
						if (stringIndex == keepStrings[keep]
							&& fret == keepFrets[keep])
						{
							isKept = true;
							break;
						}
					}
					if (isKept && IsFartherSameCoordinateCopy(3, stringIndex, fret, transform[0][3]))
					{
						isKept = false;
					}
					if (!isKept)
					{
						physicalGuideConsideredDrawCount.fetch_add(
							1, std::memory_order_relaxed);
						physicalGuideSuppressedDrawCount.fetch_add(
							1, std::memory_order_relaxed);
						return { DrawOutcome::Hide };
					}
				}
			}
		}
		return { DrawOutcome::Pass };
	}

	// 4.x's Hook_SetVertexShaderConstantF shadow of c0..c7.
	void ShadowVertexShaderConstants(Framework::DeviceEvent::VertexShaderConstants& e)
	{
		if (e.data == nullptr || e.startRegister >= 8) return;
		const UINT copyCount = e.vector4fCount < 8 - e.startRegister ? e.vector4fCount : 8 - e.startRegister;
		std::memcpy(vertexShaderConstantShadow[e.startRegister], e.data, copyCount * 4 * sizeof(float));
	}

	// The engine's view of the highway (Ui/HighwaySeam): the renderer, plus the finger-numeral anchors kept here.
	class Highway final : public NoteByNote::HighwaySeam::IHighway
	{
	public:
		void RefreshSelectedTarget(void* owner) override { NoteByNoteHighwayRenderer::RefreshSelectedTarget(owner); }
		bool InstallPresentationGate() override { return NoteByNoteHighwayRenderer::InstallPresentationGate(); }
		void ClearSelectedTarget() override { NoteByNoteHighwayRenderer::ClearSelectedTarget(); }
		void EvictRememberedDimCandidates() override { NoteByNoteHighwayRenderer::EvictRememberedDimCandidates(); }
		void SetNeckPlacementMode(int mode) override { NoteByNoteHighwayRenderer::SetNeckPlacementMode(mode); }
		void SetChordTargetShape(int chordId, const int (&frets)[6], const int (&fingers)[6], int anchorFret,
			int anchorWidth) override
		{
			NoteByNoteHighwayRenderer::SetChordTargetShape(chordId, frets, fingers, anchorFret, anchorWidth);
		}

		bool AreFingerNumeralsEnabled() override
		{
			return isFingerNumeralsEnabled.load(std::memory_order_acquire);
		}

		bool IsChordHoldFrozen() override { return NoteByNoteHighwayRenderer::IsChordHoldFrozen(); }

		bool HasRestingFingerRing(int stringIndex, int fret) override
		{
			for (const auto& ring : restingFingerRings)
			{
				if (ring.stringIndex == stringIndex && ring.fret == fret
					&& ring.renderFrame + FINGER_ANCHOR_FRESH_SCENES >= noteByNoteRenderFrame)
				{
					return true;
				}
			}
			return false;
		}

		int GetFreshFingerAnchors(int (&strings)[8], int (&frets)[8], float (&screenX)[8], float (&screenY)[8]) override
		{
			// Render-thread only, like the collection site. An anchor is fresh when its
			// draw happened this frame or the one before (the overlay runs after the
			// scene's draws, so "this frame" is the common case; one frame of tolerance
			// keeps a numeral from flickering when a panel draw slips an odd frame).
			int count = 0;
			for (const auto& anchor : fingerAnchors)
			{
				if (anchor.stringIndex < 0) continue;
				// While the song is frozen on a chord the neck does not move, so a recorded anchor stays
				// valid however many scenes ago it was drawn. Expiring it by scene count left the "3" on a
				// frozen G5 showing about half the time (the scene count per frame varies); the finger
				// lookup in the overlay still limits anchors to the held chord's own coordinates.
				if (!NoteByNoteHighwayRenderer::IsChordHoldFrozen()
					&& anchor.renderFrame + FINGER_ANCHOR_FRESH_SCENES < noteByNoteRenderFrame)
				{
					continue;
				}
				if (count >= 8) break;
				strings[count] = anchor.stringIndex;
				frets[count] = anchor.fret;
				screenX[count] = anchor.screenX;
				screenY[count] = anchor.screenY;
				++count;
			}
			return count;
		}

		bool TryGetChordFingerForCoordinate(int stringIndex, int fret, int& finger) override
		{
			return NoteByNoteHighwayRenderer::TryGetChordFingerForCoordinate(stringIndex, fret, finger);
		}
	};

	Highway highway;
}

namespace NoteByNote::HighwayInterceptors
{
	void Register(Framework::ModContext& c)
	{
		NoteByNote::HighwaySeam::Register(&highway);

		c.Draw().Register("NoteByNoteHighway", kFilterPriority, Framework::DrawPath::Indexed, FilterIndexedDraw);
		c.Draw().Register("NoteByNoteNeckMarker", kFilterPriority, Framework::DrawPath::PrimitiveUP, FilterPrimitiveUpDraw);
		// Always: a shadow that missed constants set while Note by Note was off would be stale when it comes back on.
		c.Draw().OnVertexShaderConstants(ShadowVertexShaderConstants, Framework::Observe::Always);
		// 4.x's Hook_Reset cleared the selected target before every Reset.
		c.Draw().RegisterBeforeReset([](IDirect3DDevice9*) { NoteByNoteHighwayRenderer::ClearSelectedTarget(); });
	}

	void FinishRenderFrame()
	{
		++noteByNoteRenderFrame;
	}
}
