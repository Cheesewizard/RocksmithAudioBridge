#include "../stdafx.h"
#include "D3DHooks.hpp"
#include "NoteByNoteHighwayRenderer.hpp"
#include "../Mods/NoteByNoteNativeScoring.hpp"
#include "../Mods/ArrangementInstrument.hpp"
#include "../Lib/DirectX/d3dx9shader.h"
#include "../Mods/NoteByNoteProbe.hpp"
#include "../Mods/NoteByNoteMenu.hpp"
#include "../Research/ResearchBridge.hpp"

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace {
	constexpr unsigned int NOTE_BY_NOTE_CAPTURE_FRAME_COUNT = 2;
	constexpr unsigned int NOTE_BY_NOTE_CAPTURE_DRAW_LIMIT = 256;
	constexpr unsigned int NOTE_BY_NOTE_VERTEX_CONSTANT_COUNT = 8;
	constexpr unsigned int NOTE_BY_NOTE_PIXEL_CONSTANT_COUNT = 4;
	constexpr unsigned int NOTE_BY_NOTE_INSTANCE_LIMIT = 64;
	constexpr size_t NOTE_BY_NOTE_STREAM_BYTE_LIMIT = 64 * 1024;
	constexpr UINT NOTE_BY_NOTE_TRANSFORM_STRIDE = 48;

	std::atomic<bool> isNoteByNoteRenderSnapshotArmed = false;
	std::mutex noteByNoteRenderSnapshotMutex;
	bool isNoteByNoteRenderSnapshotActive = false;
	bool hasNoteByNoteDrawInFrame = false;
	unsigned int noteByNoteCapturedFrameCount = 0;
	unsigned int noteByNoteFrameDrawCount = 0;
	unsigned int noteByNoteTotalDrawCount = 0;
	uint64_t noteByNoteRenderFrame = 0;
	// The frame the TARGET's own fretboard marker quads were last drawn (TryFilterStaleMarkerQuads),
	// for the gem-behind-marker rule in DecideNoteByNoteInstances.
	uint64_t targetMarkerLitFrame = 0;
	int targetMarkerString = -1;
	int targetMarkerFret = -1;
	std::atomic<uint64_t> gemBehindMarkerHiddenCount{ 0 };

	// Per-frame draw-hook state. GameState::IsInSong() walks a pointer chain into game
	// memory, builds a std::string from the menu name and scans the song-mode list, far too
	// costly for every draw call. The answer cannot change mid-frame in
	// any way a draw hook cares about, so it is sampled once per frame at the EndScene
	// seam (SampleDrawHookFrameState) and the hooks read this flag. The one-frame lag on
	// song entry/exit is harmless: everything downstream is either research observation
	// or a hold-gated visual filter.
	std::atomic<bool> isInSongForDrawHooks{ false };

	// Frame-time telemetry for the bridge 'status' reply, so a frame-rate claim is
	// measured rather than read off the overlay. Written on the render thread once per
	// frame, read from the bridge thread. The window max is the worst frame of the last
	// FRAME_TIME_WINDOW_FRAMES frames, published when the window rolls over.
	constexpr unsigned int FRAME_TIME_WINDOW_FRAMES = 120;
	LARGE_INTEGER frameTimeCounterFrequency = {};
	LARGE_INTEGER frameTimeLastCounter = {};
	std::atomic<float> frameTimeAverageMilliseconds{ 0.0f };
	std::atomic<float> frameTimeWindowMaxMilliseconds{ 0.0f };
	std::atomic<unsigned long long> frameTimeFrameCount{ 0 };
	float frameTimeWindowMaxAccumulator = 0.0f;
	unsigned int frameTimeWindowFrames = 0;

	// Once per presented frame, from FinishNoteByNoteRenderFrame.
	void SampleDrawHookFrameState()
	{
		isInSongForDrawHooks.store(GameState::IsInSong(), std::memory_order_relaxed);

		LARGE_INTEGER now = {};
		QueryPerformanceCounter(&now);
		if (frameTimeCounterFrequency.QuadPart == 0)
		{
			QueryPerformanceFrequency(&frameTimeCounterFrequency);
			frameTimeLastCounter = now;
			return;
		}
		const float milliseconds = static_cast<float>(
			(now.QuadPart - frameTimeLastCounter.QuadPart) * 1000.0
			/ static_cast<double>(frameTimeCounterFrequency.QuadPart));
		frameTimeLastCounter = now;
		// Ignore the pathological gaps (alt-tab, loads) so the average stays a play figure.
		if (milliseconds <= 0.0f || milliseconds > 500.0f) return;

		const float previous = frameTimeAverageMilliseconds.load(std::memory_order_relaxed);
		const float average = previous <= 0.0f
			? milliseconds
			: previous + (milliseconds - previous) * (1.0f / 60.0f);
		frameTimeAverageMilliseconds.store(average, std::memory_order_relaxed);
		frameTimeFrameCount.fetch_add(1, std::memory_order_relaxed);

		if (milliseconds > frameTimeWindowMaxAccumulator) frameTimeWindowMaxAccumulator = milliseconds;
		if (++frameTimeWindowFrames >= FRAME_TIME_WINDOW_FRAMES)
		{
			frameTimeWindowMaxMilliseconds.store(
				frameTimeWindowMaxAccumulator, std::memory_order_relaxed);
			frameTimeWindowMaxAccumulator = 0.0f;
			frameTimeWindowFrames = 0;
		}
	}
	std::vector<std::pair<std::string, uintptr_t>> loggedNoteByNoteShaders;
	std::atomic<bool> isNoteByNoteNativeLifecycleTraceEnabled = false;
	std::atomic<bool> hasLoggedNoteByNoteNativeCallStack = false;
	std::atomic<bool> isPhysicalMarkerCaptureArmed = false;
	std::atomic<bool> isPhysicalMarkerCaptureActive = false;
	std::atomic<unsigned int> physicalMarkerArmGeneration{ 0 };
	std::atomic<unsigned long long> physicalMarkerCallbackCount{ 0 };
	std::atomic<unsigned long long> physicalMarkerArmCallbackBase{ 0 };
	std::atomic<unsigned long long> physicalMarkerMatchedDrawCount{ 0 };
	std::atomic<unsigned long long> physicalMarkerArmMatchedDrawBase{ 0 };
	std::atomic<unsigned long long> physicalMarkerPublishedDrawCount{ 0 };
	std::atomic<unsigned long long> physicalMarkerArmPublishedDrawBase{ 0 };

	struct NativeFrontAttack
	{
		bool isAvailable = false;
		float longitudinalPosition = 0.0f;
		std::vector<NoteByNoteProbe::NativeRenderedNote> notes;
		UINT instanceCount = 0;
	};

	NativeFrontAttack currentFrameFrontAttack = {};
	NativeFrontAttack previousFrameFrontAttack = {};
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

	uint64_t HashBytes(const uint8_t* data, size_t byteCount);

	bool HasLoggedShader(const char* shaderType, uintptr_t identity)
	{
		return std::find(
			loggedNoteByNoteShaders.begin(),
			loggedNoteByNoteShaders.end(),
			std::make_pair(std::string(shaderType), identity))
			!= loggedNoteByNoteShaders.end();
	}

	void LogShaderDisassembly(
		const char* shaderType,
		uintptr_t identity,
		const std::vector<DWORD>& bytecode,
		UINT byteCount)
	{
		if (identity == 0 || byteCount == 0 || HasLoggedShader(shaderType, identity)) return;

		loggedNoteByNoteShaders.emplace_back(shaderType, identity);
		ID3DXBuffer* disassembly = nullptr;
		const HRESULT result = D3DXDisassembleShader(
			bytecode.data(),
			FALSE,
			nullptr,
			&disassembly);
		if (result != D3D_OK || disassembly == nullptr)
		{
			LOG_ERROR("(NBN NATIVE RENDER) " << shaderType
				<< " shader disassembly failed identity=0x" << std::hex << identity
				<< std::dec << " result=" << result << "." << std::endl);
			return;
		}

		LOG_INFO("(NBN NATIVE RENDER) " << shaderType
			<< " shader identity=0x" << std::hex << identity
			<< " bytecodeHash=0x" << HashBytes(
				reinterpret_cast<const uint8_t*>(bytecode.data()),
				byteCount)
			<< std::dec << " bytes=" << byteCount << " disassembly:\n"
			<< static_cast<const char*>(disassembly->GetBufferPointer()) << std::endl);
		disassembly->Release();
	}

	uintptr_t GetCurrentVertexShaderIdentity(IDirect3DDevice9* device, UINT& byteCount)
	{
		IDirect3DVertexShader9* vertexShader = nullptr;
		byteCount = 0;
		if (device->GetVertexShader(&vertexShader) != D3D_OK || vertexShader == nullptr) return 0;

		vertexShader->GetFunction(nullptr, &byteCount);
		const auto identity = reinterpret_cast<uintptr_t>(vertexShader);
		vertexShader->Release();
		return identity;
	}

	uintptr_t GetCurrentPixelShaderIdentity(IDirect3DDevice9* device, UINT& byteCount)
	{
		IDirect3DPixelShader9* pixelShader = nullptr;
		byteCount = 0;
		if (device->GetPixelShader(&pixelShader) != D3D_OK || pixelShader == nullptr) return 0;

		pixelShader->GetFunction(nullptr, &byteCount);
		const auto identity = reinterpret_cast<uintptr_t>(pixelShader);
		pixelShader->Release();
		return identity;
	}

	void LogCurrentShaderDisassemblies(
		IDirect3DDevice9* device,
		uintptr_t vertexShaderIdentity,
		uintptr_t pixelShaderIdentity)
	{
		if (!HasLoggedShader("vertex", vertexShaderIdentity))
		{
			IDirect3DVertexShader9* vertexShader = nullptr;
			if (device->GetVertexShader(&vertexShader) == D3D_OK && vertexShader != nullptr)
			{
				UINT byteCount = 0;
				vertexShader->GetFunction(nullptr, &byteCount);
				std::vector<DWORD> bytecode((byteCount + sizeof(DWORD) - 1) / sizeof(DWORD));
				if (byteCount > 0
					&& vertexShader->GetFunction(bytecode.data(), &byteCount) == D3D_OK)
				{
					LogShaderDisassembly(
						"vertex",
						vertexShaderIdentity,
						bytecode,
						byteCount);
				}
				vertexShader->Release();
			}
		}

		if (!HasLoggedShader("pixel", pixelShaderIdentity))
		{
			IDirect3DPixelShader9* pixelShader = nullptr;
			if (device->GetPixelShader(&pixelShader) == D3D_OK && pixelShader != nullptr)
			{
				UINT byteCount = 0;
				pixelShader->GetFunction(nullptr, &byteCount);
				std::vector<DWORD> bytecode((byteCount + sizeof(DWORD) - 1) / sizeof(DWORD));
				if (byteCount > 0
					&& pixelShader->GetFunction(bytecode.data(), &byteCount) == D3D_OK)
				{
					LogShaderDisassembly(
						"pixel",
						pixelShaderIdentity,
						bytecode,
						byteCount);
				}
				pixelShader->Release();
			}
		}
	}

	uintptr_t GetCurrentTextureIdentity(IDirect3DDevice9* device, DWORD stage)
	{
		IDirect3DBaseTexture9* texture = nullptr;
		if (device->GetTexture(stage, &texture) != D3D_OK || texture == nullptr) return 0;

		const auto identity = reinterpret_cast<uintptr_t>(texture);
		texture->Release();
		return identity;
	}

	uintptr_t GetCurrentIndexBufferIdentity(IDirect3DDevice9* device)
	{
		IDirect3DIndexBuffer9* indexBuffer = nullptr;
		if (device->GetIndices(&indexBuffer) != D3D_OK || indexBuffer == nullptr) return 0;

		const auto identity = reinterpret_cast<uintptr_t>(indexBuffer);
		indexBuffer->Release();
		return identity;
	}

	uint64_t HashBytes(const uint8_t* data, size_t byteCount)
	{
		constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
		constexpr uint64_t FNV_PRIME = 1099511628211ULL;
		uint64_t hash = FNV_OFFSET_BASIS;
		for (size_t byteIndex = 0; byteIndex < byteCount; ++byteIndex)
		{
			hash ^= data[byteIndex];
			hash *= FNV_PRIME;
		}
		return hash;
	}

	std::string CaptureInstanceStream(
		IDirect3DDevice9* device,
		UINT streamIndex,
		UINT instanceCount)
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
			return "stream" + std::to_string(streamIndex) + "=unavailable";
		}

		UINT streamFrequency = 0;
		const HRESULT frequencyResult = device->GetStreamSourceFreq(
			streamIndex,
			&streamFrequency);
		D3DVERTEXBUFFER_DESC description = {};
		const HRESULT descriptionResult = vertexBuffer->GetDesc(&description);
		std::ostringstream result;
		result << "stream" << streamIndex << "={identity=0x" << std::hex
			<< reinterpret_cast<uintptr_t>(vertexBuffer) << std::dec
			<< ",offset=" << streamOffset
			<< ",stride=" << streamStride
			<< ",frequency=";
		if (frequencyResult == D3D_OK) result << "0x" << std::hex << streamFrequency << std::dec;
		else result << "result(" << frequencyResult << ")";

		if (frequencyResult != D3D_OK
			|| (streamFrequency & D3DSTREAMSOURCE_INSTANCEDATA) == 0
			|| descriptionResult != D3D_OK
			|| streamStride == 0)
		{
			result << ",data=unavailable}";
			vertexBuffer->Release();
			return result.str();
		}

		constexpr UINT FREQUENCY_VALUE_MASK = 0x3FFFFFFF;
		const UINT stepRate = streamFrequency & FREQUENCY_VALUE_MASK;
		if (stepRate == 0 || streamOffset >= description.Size)
		{
			result << ",data=invalid-range}";
			vertexBuffer->Release();
			return result.str();
		}

		const auto requiredRecordCount = (instanceCount + stepRate - 1) / stepRate;
		const auto readableRecordCount = (std::min)(
			NOTE_BY_NOTE_INSTANCE_LIMIT,
			(std::min)(
				requiredRecordCount,
				(description.Size - streamOffset) / streamStride));
		const UINT byteCount = readableRecordCount * streamStride;
		void* lockedData = nullptr;
		const HRESULT lockResult = vertexBuffer->Lock(
			streamOffset,
			byteCount,
			&lockedData,
			D3DLOCK_READONLY);
		if (lockResult != D3D_OK || lockedData == nullptr)
		{
			result << ",data=result(" << lockResult << ")}";
			vertexBuffer->Release();
			return result.str();
		}

		const auto* bytes = static_cast<const uint8_t*>(lockedData);
		result << ",stepRate=" << stepRate
			<< ",records=" << readableRecordCount
			<< ",hash=0x" << std::hex << HashBytes(bytes, byteCount) << std::dec
			<< ",values=[" << std::fixed << std::setprecision(6);
		for (UINT recordIndex = 0; recordIndex < readableRecordCount; ++recordIndex)
		{
			if (recordIndex > 0) result << ";";
			result << "i" << recordIndex << "=(";
			const UINT componentCount = streamStride / sizeof(float);
			for (UINT componentIndex = 0; componentIndex < componentCount; ++componentIndex)
			{
				if (componentIndex > 0) result << ",";
				uint32_t bits = 0;
				std::memcpy(
					&bits,
					bytes + recordIndex * streamStride + componentIndex * sizeof(float),
					sizeof(bits));
				float value = 0.0f;
				std::memcpy(&value, &bits, sizeof(value));
				if (std::isfinite(value)) result << value;
				else result << "bits:0x" << std::hex << bits << std::dec;
			}
			result << ")";
		}
		result << "]}";

		vertexBuffer->Unlock();
		vertexBuffer->Release();
		return result.str();
	}

	std::string CaptureNativeInstances(IDirect3DDevice9* device)
	{
		UINT indexedFrequency = 0;
		if (device->GetStreamSourceFreq(0, &indexedFrequency) != D3D_OK
			|| (indexedFrequency & D3DSTREAMSOURCE_INDEXEDDATA) == 0)
		{
			return "instances=not-instanced";
		}

		constexpr UINT FREQUENCY_VALUE_MASK = 0x3FFFFFFF;
		const UINT instanceCount = indexedFrequency & FREQUENCY_VALUE_MASK;
		std::ostringstream result;
		result << "instances={count=" << instanceCount << " "
			<< CaptureInstanceStream(device, 1, instanceCount) << " "
			<< CaptureInstanceStream(device, 2, instanceCount) << "}";
		return result.str();
	}

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

	void ObservePhysicalMarkerDraw(
		IDirect3DDevice9* device,
		D3DPRIMITIVETYPE primitiveType,
		INT baseVertexIndex,
		UINT minimumVertexIndex,
		UINT vertexCount,
		UINT startIndex,
		UINT primitiveCount,
		UINT stride,
		uintptr_t streamIdentity,
		uintptr_t nativeCaller,
		const void* userPointerVertexData = nullptr)
	{
		if (!isPhysicalMarkerCaptureArmed.load(std::memory_order_acquire)) return;

		physicalMarkerCallbackCount.fetch_add(1, std::memory_order_relaxed);
		if (!isPhysicalMarkerCaptureActive.load(std::memory_order_acquire)) return;

		float transform[4][4] = {};
		if (device->GetVertexShaderConstantF(0, &transform[0][0], 4) != D3D_OK)
		{
			std::memcpy(transform, D3DHooks::vertexShaderConstantShadow, sizeof(transform));
		}

		// Ownership must not be inferred from decodability. Every draw in the
		// bounded frame is published, decode success or not, so a marker outside the
		// shader-transform decoder still leaves a captured identity.
		int stringIndex = -1;
		int fret = -1;
		const char* decodeFailure = nullptr;
		const bool decoded = TryDecodePhysicalMarkerTransform(
			transform,
			stringIndex,
			fret,
			&decodeFailure);
		if (decoded)
		{
			physicalMarkerMatchedDrawCount.fetch_add(1, std::memory_order_relaxed);
		}

		const auto publishedDrawCount = physicalMarkerPublishedDrawCount.fetch_add(
			1,
			std::memory_order_relaxed) + 1;
		const auto armPublishedDrawBase = physicalMarkerArmPublishedDrawBase.load(
			std::memory_order_acquire);
		constexpr unsigned long long EVENT_LIMIT = 512;
		if (publishedDrawCount - armPublishedDrawBase > EVENT_LIMIT) return;

		UINT streamFrequency = 0;
		if (userPointerVertexData == nullptr) device->GetStreamSourceFreq(0, &streamFrequency);
		bool isInstanced = false;
		UINT transformStreamStride = 0;
		if ((streamFrequency & D3DSTREAMSOURCE_INDEXEDDATA) != 0)
		{
			isInstanced = true;
			IDirect3DVertexBuffer9* transformBuffer = nullptr;
			UINT transformOffset = 0;
			if (device->GetStreamSource(
				1,
				&transformBuffer,
				&transformOffset,
				&transformStreamStride) != D3D_OK)
			{
				transformStreamStride = 0;
			}
			if (transformBuffer != nullptr) transformBuffer->Release();
		}

		int targetString = -1;
		int targetFret = -1;
		NoteByNoteHighwayRenderer::TryGetSelectedTargetForDiagnostics(targetString, targetFret);

		UINT vertexShaderByteCount = 0;
		UINT pixelShaderByteCount = 0;
		ResearchBridge::PhysicalMarkerDrawEvent event;
		event.renderFrame = noteByNoteRenderFrame;
		event.nativeCaller = nativeCaller;
		event.streamIdentity = streamIdentity;
		event.textureIdentity = GetCurrentTextureIdentity(device, 0);
		event.vertexShaderIdentity = GetCurrentVertexShaderIdentity(
			device,
			vertexShaderByteCount);
		event.pixelShaderIdentity = GetCurrentPixelShaderIdentity(
			device,
			pixelShaderByteCount);
		event.primitiveType = static_cast<uint32_t>(primitiveType);
		event.baseVertexIndex = baseVertexIndex;
		event.minimumVertexIndex = minimumVertexIndex;
		event.vertexCount = vertexCount;
		event.startIndex = startIndex;
		event.primitiveCount = primitiveCount;
		event.stride = stride;
		event.stringIndex = stringIndex;
		event.fret = fret;
		event.targetString = targetString;
		event.targetFret = targetFret;
		event.decoded = decoded;
		event.decodeFailure = decodeFailure;
		event.instanced = isInstanced;
		event.streamFrequency = streamFrequency;
		event.transformStreamStride = transformStreamStride;
		// Per-instance translations for the 48-byte transform stream: each record is
		// three rows of the same convention as c0..c2, so the translations sit at
		// floats 3 (longitudinal), 7 (fret) and 11 (string height).
		if (isInstanced && transformStreamStride == 48)
		{
			constexpr UINT FREQUENCY_VALUE_MASK = 0x3FFFFFFF;
			const UINT instanceCount = streamFrequency & FREQUENCY_VALUE_MASK;
			event.instanceCount = instanceCount;
			std::vector<float> records;
			if (instanceCount > 0
				&& TryReadInstanceRecords(device, 1, instanceCount, 48, records))
			{
				const uint32_t decodeLimit = (std::min)(
					instanceCount,
					static_cast<UINT>(32));
				for (uint32_t index = 0; index < decodeLimit; ++index)
				{
					const float* record = records.data() + static_cast<size_t>(index) * 12;
					event.instanceTranslations[index][0] = record[3];
					event.instanceTranslations[index][1] = record[7];
					event.instanceTranslations[index][2] = record[11];
				}
				event.decodedInstanceCount = decodeLimit;
			}
		}
		event.userPointer = userPointerVertexData != nullptr;
		// Small non-instanced draws can carry world-space vertices under a pure
		// view-projection transform, so their stream-0 vertex data is the only place their
		// position exists. Sample it. Hook_DP reports no vertex count, so derive one from the
		// primitive topology; its startIndex parameter is the start vertex.
		UINT effectiveVertexCount = vertexCount;
		UINT vertexOffsetIndex = static_cast<UINT>(
			static_cast<int64_t>(baseVertexIndex) + minimumVertexIndex);
		if (vertexCount == 0 && userPointerVertexData == nullptr)
		{
			switch (primitiveType)
			{
				case D3DPT_POINTLIST: effectiveVertexCount = primitiveCount; break;
				case D3DPT_LINELIST: effectiveVertexCount = primitiveCount * 2; break;
				case D3DPT_LINESTRIP: effectiveVertexCount = primitiveCount + 1; break;
				case D3DPT_TRIANGLELIST: effectiveVertexCount = primitiveCount * 3; break;
				case D3DPT_TRIANGLESTRIP:
				case D3DPT_TRIANGLEFAN: effectiveVertexCount = primitiveCount + 2; break;
				default: effectiveVertexCount = 0; break;
			}
			vertexOffsetIndex = startIndex;
		}
		if (userPointerVertexData == nullptr
			&& !isInstanced
			&& effectiveVertexCount > 0
			&& effectiveVertexCount <= 8192
			&& stride >= sizeof(float))
		{
			IDirect3DVertexBuffer9* vertexBuffer = nullptr;
			UINT streamOffset = 0;
			UINT boundStride = 0;
			if (device->GetStreamSource(0, &vertexBuffer, &streamOffset, &boundStride) == D3D_OK
				&& vertexBuffer != nullptr)
			{
				const uint64_t drawOffset = static_cast<uint64_t>(streamOffset)
					+ static_cast<uint64_t>(vertexOffsetIndex) * boundStride;
				const uint64_t byteCount =
					static_cast<uint64_t>(effectiveVertexCount) * boundStride;
				D3DVERTEXBUFFER_DESC description = {};
				void* lockedData = nullptr;
				if (boundStride < sizeof(float))
				{
					event.vertexSampleStatus = -1;
				}
				else if (vertexBuffer->GetDesc(&description) != D3D_OK)
				{
					event.vertexSampleStatus = -2;
				}
				else if (drawOffset + byteCount > description.Size)
				{
					event.vertexSampleStatus = -3;
				}
				else
				{
					// Write-only buffers refuse D3DLOCK_READONLY, so fall back to a
					// plain lock; write-combined memory is still readable.
					HRESULT lockResult = vertexBuffer->Lock(
						static_cast<UINT>(drawOffset),
						static_cast<UINT>(byteCount),
						&lockedData,
						D3DLOCK_READONLY);
					if (lockResult != D3D_OK || lockedData == nullptr)
					{
						lockedData = nullptr;
						lockResult = vertexBuffer->Lock(
							static_cast<UINT>(drawOffset),
							static_cast<UINT>(byteCount),
							&lockedData,
							0);
					}
					if (lockResult == D3D_OK && lockedData != nullptr)
					{
						const uint32_t availableFloats = (std::min)(
							static_cast<uint32_t>(24),
							static_cast<uint32_t>(byteCount / sizeof(float)));
						std::memcpy(
							event.vertexSample,
							lockedData,
							static_cast<size_t>(availableFloats) * sizeof(float));
						event.vertexSampleCount = availableFloats;
						event.vertexSampleStatus = 0;
						// Positions are the leading three floats of each vertex in every
						// observed layout; the min/max bounds place a whole batch in world
						// space even when its own transform is the raw view-projection.
						if (boundStride >= 3 * sizeof(float))
						{
							const auto* bytes = static_cast<const uint8_t*>(lockedData);
							for (UINT index = 0; index < effectiveVertexCount; ++index)
							{
								float position[3];
								std::memcpy(
									position,
									bytes + static_cast<size_t>(index) * boundStride,
									sizeof(position));
								for (int axis = 0; axis < 3; ++axis)
								{
									if (!event.hasBounds || position[axis] < event.boundsMin[axis])
									{
										event.boundsMin[axis] = position[axis];
									}
									if (!event.hasBounds || position[axis] > event.boundsMax[axis])
									{
										event.boundsMax[axis] = position[axis];
									}
								}
								event.hasBounds = true;
							}
						}
						vertexBuffer->Unlock();
					}
					else
					{
						event.vertexSampleStatus = static_cast<int32_t>(lockResult);
					}
				}
			}
			if (vertexBuffer != nullptr) vertexBuffer->Release();
		}
		if (userPointerVertexData != nullptr && stride >= sizeof(float))
		{
			// The vertex data itself carries the positions for user-pointer draws, so a
			// few leading floats identify the quad even when the transform is a full
			// view-projection matrix.
			const uint32_t availableFloats = (std::min)(
				static_cast<uint32_t>(24),
				(vertexCount > 0 ? vertexCount : primitiveCount + 2)
					* (stride / static_cast<uint32_t>(sizeof(float))));
			std::memcpy(
				event.vertexSample,
				userPointerVertexData,
				static_cast<size_t>(availableFloats) * sizeof(float));
			event.vertexSampleCount = availableFloats;
		}
		std::memcpy(event.transform, transform, sizeof(transform));
		ResearchBridge::PublishPhysicalMarkerDraw(event);
	}

	// Full-draw feed for the probe's transition capture: every in-song draw from every
	// device entry point, so a capture can contain the stale-marker box's draw by
	// construction (the filtered note-head feed cannot). The probe returns after one
	// atomic when its capture is disarmed; the bridge skips the probe mutex entirely when
	// no consumer is loaded.
	void DispatchFullDrawObservation(
		IDirect3DDevice9* device,
		uint32_t drawSite,
		D3DPRIMITIVETYPE primitiveType,
		INT baseVertexIndex,
		UINT minimumVertexIndex,
		UINT vertexCount,
		UINT startIndex,
		UINT primitiveCount,
		UINT stride,
		uintptr_t streamIdentity,
		uintptr_t nativeCaller,
		const void* userPointerVertexData)
	{
		if (!ResearchBridge::IsFullDrawFeedEnabled()) return;

		ResearchProtocol::NativeDrawObservation draw = {};
		draw.renderFrame = noteByNoteRenderFrame;
		draw.songTime = SongTimer::SongTimer();
		draw.greyNoteCutoff = SongTimer::GetGreyNoteTimer();
		draw.primitiveType = static_cast<uint32_t>(primitiveType);
		draw.baseVertexIndex = baseVertexIndex;
		draw.minimumVertexIndex = minimumVertexIndex;
		draw.vertexCount = vertexCount;
		draw.startIndex = startIndex;
		draw.primitiveCount = primitiveCount;
		draw.stride = stride;
		draw.streamIdentity = streamIdentity;
		draw.nativeCaller = nativeCaller;
		draw.devicePointer = reinterpret_cast<uintptr_t>(device);
		draw.shaderConstantShadow =
			reinterpret_cast<uintptr_t>(&D3DHooks::vertexShaderConstantShadow[0][0]);
		draw.drawSite = drawSite;
		draw.userPointerData = reinterpret_cast<uintptr_t>(userPointerVertexData);
		ResearchBridge::DispatchFullDraw(draw);
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
	constexpr uintptr_t MARKER_SLICE_CALLER = 0x00BEC0DA;
	// The batched glyph flush that draws the fingering NUMERALS (otherwise a suppressed
	// upcoming chord leaves its floating finger number behind). One triangle-list draw carries ~21 glyph quads including
	// the kept members' numerals, so it filters per quad exactly like the marker
	// slices; glyphs that do not land on a marker coordinate (fret numbers below the
	// strings, highway labels) never decode and are always kept.
	constexpr uintptr_t GLYPH_FLUSH_CALLER = 0x00DFC98C;
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
		IDirect3DDevice9* device,
		D3DPRIMITIVETYPE primitiveType,
		INT baseVertexIndex,
		UINT minimumVertexIndex,
		UINT vertexCount,
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
		if (!GameState::Menus::IsInLASPlayingModes()) return false;

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
			result = oDrawIndexedPrimitive(
				device,
				primitiveType,
				baseVertexIndex,
				minimumVertexIndex,
				vertexCount,
				startIndex + block * 6,
				isMarkerSlice ? runBlocks * 6 - 2 : runBlocks * 2);
			block = runEnd + 1;
		}
		return true;
	}

	void FinishPhysicalMarkerCaptureFrame()
	{
		if (!isPhysicalMarkerCaptureArmed.load(std::memory_order_acquire)) return;

		if (!isPhysicalMarkerCaptureActive.exchange(true, std::memory_order_acq_rel)) return;

		isPhysicalMarkerCaptureActive.store(false, std::memory_order_release);
		isPhysicalMarkerCaptureArmed.store(false, std::memory_order_release);
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
			std::memcpy(constants, D3DHooks::vertexShaderConstantShadow, sizeof(constants));
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
			std::memcpy(transform, D3DHooks::vertexShaderConstantShadow, sizeof(transform));
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
			&& GameState::Menus::IsInLASPlayingModes();
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
		IDirect3DDevice9* device,
		const InstanceFilterDecision& decision,
		D3DPRIMITIVETYPE primitiveType,
		INT baseVertexIndex,
		UINT minimumVertexIndex,
		UINT vertexCount,
		UINT startIndex,
		UINT primitiveCount)
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
			oDrawIndexedPrimitive(
				device,
				primitiveType,
				baseVertexIndex,
				minimumVertexIndex,
				vertexCount,
				startIndex,
				primitiveCount);
		}

		// Restore exactly what Rocksmith bound, so the rest of the frame is unaffected.
		device->SetStreamSource(1, transformBuffer, transformOffset, transformStride);
		device->SetStreamSource(2, gradientBuffer, gradientOffset, gradientStride);
		device->SetStreamSourceFreq(0, originalFrequency);
		transformBuffer->Release();
		gradientBuffer->Release();
		return true;
	}

	// Highlighted note head reconnaissance.
	//
	// D3D.hpp catalogues {stride=32, primCount=104, vertexCount=187} as the Highlighted
	// Note Head, a family distinct from the ordinary note head. Rocksmith draws it for
	// lesson-selected notes. This logs whether the engine draws it during Note by Note play
	// and for which note; if it carries the same instanced streams, the target can be marked
	// by rebinding it to the target's instance, as the instance filter does.
	//
	// Hard-capped: locking instance buffers forces a CPU/GPU sync and can cause ASIO input
	// xruns, so after a small number of observations it stops locking anything at all.
	constexpr unsigned int HIGHLIGHTED_HEAD_OBSERVATION_LIMIT = 16;
	unsigned int highlightedHeadObservationCount = 0;

	void ObserveHighlightedNoteHead(IDirect3DDevice9* device)
	{
		if (highlightedHeadObservationCount >= HIGHLIGHTED_HEAD_OBSERVATION_LIMIT) return;

		int targetString = -1;
		int targetFret = -1;
		const bool hasTarget = NoteByNoteHighwayRenderer::TryGetSelectedTargetForDiagnostics(
			targetString,
			targetFret);

		UINT indexedFrequency = 0;
		if (device->GetStreamSourceFreq(0, &indexedFrequency) != D3D_OK
			|| (indexedFrequency & D3DSTREAMSOURCE_INDEXEDDATA) == 0)
		{
			++highlightedHeadObservationCount;
			LOG_INFO("(NBN HIGHLIGHT) Highlighted note head drawn without instanced streams;"
				<< " it cannot be retargeted by rebinding. target=" << targetString << ':'
				<< targetFret << " hasTarget=" << hasTarget << std::endl);
			return;
		}

		constexpr UINT FREQUENCY_VALUE_MASK = 0x3FFFFFFF;
		const UINT instanceCount = indexedFrequency & FREQUENCY_VALUE_MASK;
		std::vector<float> transforms;
		std::vector<float> gradients;
		if (instanceCount == 0
			|| instanceCount > NOTE_BY_NOTE_INSTANCE_LIMIT
			|| !TryReadInstanceRecords(device, 1, instanceCount, 48, transforms)
			|| !TryReadInstanceRecords(device, 2, instanceCount, 16, gradients)
			|| transforms.size() != static_cast<size_t>(instanceCount) * 12
			|| gradients.size() != static_cast<size_t>(instanceCount) * 4)
		{
			++highlightedHeadObservationCount;
			LOG_INFO("(NBN HIGHLIGHT) Highlighted note head drawn, instanceCount="
				<< instanceCount << ", instance data unreadable at the expected strides."
				<< std::endl);
			return;
		}

		++highlightedHeadObservationCount;
		std::ostringstream instances;
		for (UINT index = 0; index < instanceCount; ++index)
		{
			const size_t transformOffset = static_cast<size_t>(index) * 12;
			const size_t gradientOffset = static_cast<size_t>(index) * 4;
			if (index != 0) instances << ' ';
			instances << static_cast<int>(std::lround(
					(gradients[gradientOffset] * 32.0f - 1.0f) / 2.0f))
				<< ':' << DecodeRenderedFret(transforms[transformOffset + 7])
				<< "@" << std::fixed << std::setprecision(2)
				<< transforms[transformOffset + 3];
		}
		LOG_INFO("(NBN HIGHLIGHT) Highlighted note head: instances=" << instanceCount
			<< " [" << instances.str() << "] target=" << targetString << ':' << targetFret
			<< " hasTarget=" << hasTarget
			<< ". If an instance matches the target, the family can mark it directly;"
			<< " if it matches something else, it can be rebound to the target." << std::endl);
	}

	void LogNativeNoteHeadCallStackOnce(uintptr_t nativeCaller)
	{
		if (hasLoggedNoteByNoteNativeCallStack.exchange(true, std::memory_order_acq_rel)) return;

		constexpr USHORT MAX_STACK_FRAMES = 24;
		std::array<void*, MAX_STACK_FRAMES> stackFrames = {};
		const USHORT capturedFrameCount = CaptureStackBackTrace(
			0,
			MAX_STACK_FRAMES,
			stackFrames.data(),
			nullptr);
		std::ostringstream stream;
		for (USHORT frameIndex = 0; frameIndex < capturedFrameCount; ++frameIndex)
		{
			if (frameIndex != 0) stream << ',';
			stream << "0x" << std::hex
				<< reinterpret_cast<uintptr_t>(stackFrames[frameIndex]);
		}

		LOG_INFO("(NBN NATIVE NOTEWAY STACK) caller=0x" << std::hex << nativeCaller
			<< " executableBase=0x" << Offsets::baseHandle
			<< std::dec << " thread=" << GetCurrentThreadId()
			<< " frames=[" << stream.str() << "]." << std::endl);
	}

	void ObserveNativeFrontAttack(
		IDirect3DDevice9* device,
		UINT meshVertexCount,
		uintptr_t nativeCaller)
	{
		if (!isNoteByNoteNativeLifecycleTraceEnabled.load(std::memory_order_acquire)) return;
		LogNativeNoteHeadCallStackOnce(nativeCaller);

		UINT indexedFrequency = 0;
		if (device->GetStreamSourceFreq(0, &indexedFrequency) != D3D_OK
			|| (indexedFrequency & D3DSTREAMSOURCE_INDEXEDDATA) == 0)
		{
			return;
		}

		constexpr UINT FREQUENCY_VALUE_MASK = 0x3FFFFFFF;
		const UINT instanceCount = indexedFrequency & FREQUENCY_VALUE_MASK;
		std::vector<float> transforms;
		std::vector<float> gradients;
		if (!TryReadInstanceRecords(device, 1, instanceCount, 48, transforms)
			|| !TryReadInstanceRecords(device, 2, instanceCount, 16, gradients))
		{
			return;
		}
		if (transforms.size() != static_cast<size_t>(instanceCount) * 12
			|| gradients.size() != static_cast<size_t>(instanceCount) * 4)
		{
			return;
		}

		for (UINT instanceIndex = 0; instanceIndex < instanceCount; ++instanceIndex)
		{
			const size_t transformOffset = static_cast<size_t>(instanceIndex) * 12;
			const size_t gradientOffset = static_cast<size_t>(instanceIndex) * 4;
			const float longitudinalPosition = transforms[transformOffset + 3];
			if (currentFrameFrontAttack.isAvailable
				&& longitudinalPosition < currentFrameFrontAttack.longitudinalPosition)
			{
				continue;
			}
			if (!currentFrameFrontAttack.isAvailable
				|| longitudinalPosition > currentFrameFrontAttack.longitudinalPosition)
			{
				currentFrameFrontAttack = {};
				currentFrameFrontAttack.isAvailable = true;
				currentFrameFrontAttack.longitudinalPosition = longitudinalPosition;
				currentFrameFrontAttack.instanceCount = instanceCount;
			}

			NoteByNoteProbe::NativeRenderedNote note = {};
			note.fret = meshVertexCount == 50
				? 0
				: DecodeRenderedFret(transforms[transformOffset + 7]);
			note.stringIndex = static_cast<int>(std::lround(
				(gradients[gradientOffset] * 32.0f - 1.0f) / 2.0f));
			const auto duplicate = std::find_if(
				currentFrameFrontAttack.notes.begin(),
				currentFrameFrontAttack.notes.end(),
				[&note](const NoteByNoteProbe::NativeRenderedNote& candidate)
				{
					return candidate.stringIndex == note.stringIndex
						&& candidate.fret == note.fret;
				});
			if (duplicate == currentFrameFrontAttack.notes.end())
			{
				currentFrameFrontAttack.notes.push_back(note);
			}
		}
	}

	bool HasSameNativeAttackIdentity(
		const NativeFrontAttack& first,
		const NativeFrontAttack& second)
	{
		if (first.notes.size() != second.notes.size()) return false;

		for (const auto& firstNote : first.notes)
		{
			const auto matchingNote = std::find_if(
				second.notes.begin(),
				second.notes.end(),
				[&firstNote](const NoteByNoteProbe::NativeRenderedNote& secondNote)
				{
					return firstNote.stringIndex == secondNote.stringIndex
						&& firstNote.fret == secondNote.fret;
				});
			if (matchingNote == second.notes.end()) return false;
		}
		return true;
	}

	std::string NativeAttackNotesText(const NativeFrontAttack& attack)
	{
		std::ostringstream stream;
		for (size_t index = 0; index < attack.notes.size(); ++index)
		{
			if (index != 0) stream << ',';
			stream << attack.notes[index].stringIndex << ':' << attack.notes[index].fret;
		}
		return stream.str();
	}

	void FinishNativeFrontAttackFrame()
	{
		if (!isNoteByNoteNativeLifecycleTraceEnabled.load(std::memory_order_acquire))
		{
			currentFrameFrontAttack = {};
			previousFrameFrontAttack = {};
			return;
		}

		if (!currentFrameFrontAttack.isAvailable) return;

		const bool hasTransitioned = previousFrameFrontAttack.isAvailable
			&& (!HasSameNativeAttackIdentity(currentFrameFrontAttack, previousFrameFrontAttack)
				|| currentFrameFrontAttack.longitudinalPosition
					< previousFrameFrontAttack.longitudinalPosition);
		const float songTime = SongTimer::SongTimer();
		if (!previousFrameFrontAttack.isAvailable || hasTransitioned)
		{
			LOG_INFO("(NBN NATIVE LIFECYCLE) "
				<< (hasTransitioned ? "incoming-front-transition" : "incoming-front-acquired")
				<< " frame=" << noteByNoteRenderFrame
				<< " songTime=" << std::fixed << std::setprecision(6) << songTime
				<< " previous={available=" << std::boolalpha
				<< previousFrameFrontAttack.isAvailable
				<< ",longitudinal=" << previousFrameFrontAttack.longitudinalPosition
				<< ",notes=" << NativeAttackNotesText(previousFrameFrontAttack)
				<< ",instances=" << previousFrameFrontAttack.instanceCount
				<< "} current={longitudinal=" << currentFrameFrontAttack.longitudinalPosition
				<< ",notes=" << NativeAttackNotesText(currentFrameFrontAttack)
				<< ",instances=" << currentFrameFrontAttack.instanceCount
				<< "}." << std::endl);
		}

		NoteByNoteProbe::NativeRenderedAttack renderedAttack = {};
		renderedAttack.isTransition = hasTransitioned;
		renderedAttack.renderFrame = noteByNoteRenderFrame;
		renderedAttack.songTime = songTime;
		renderedAttack.longitudinalPosition = currentFrameFrontAttack.longitudinalPosition;
		renderedAttack.notes = currentFrameFrontAttack.notes;
		NoteByNoteProbe::HandleNativeRenderedAttack(renderedAttack);

		previousFrameFrontAttack = currentFrameFrontAttack;
		currentFrameFrontAttack = {};
	}

	unsigned int GetFloatComponentCount(BYTE declarationType)
	{
		switch (declarationType)
		{
			case D3DDECLTYPE_FLOAT1:
				return 1;
			case D3DDECLTYPE_FLOAT2:
				return 2;
			case D3DDECLTYPE_FLOAT3:
				return 3;
			case D3DDECLTYPE_FLOAT4:
				return 4;
			default:
				return 0;
		}
	}

	std::string CaptureVertexStreams(
		IDirect3DDevice9* device,
		INT baseVertexIndex,
		UINT minimumVertexIndex,
		UINT vertexCount)
	{
		IDirect3DVertexDeclaration9* vertexDeclaration = nullptr;
		if (device->GetVertexDeclaration(&vertexDeclaration) != D3D_OK
			|| vertexDeclaration == nullptr)
		{
			return "declaration=unavailable";
		}

		std::array<D3DVERTEXELEMENT9, MAXD3DDECLLENGTH> elements = {};
		UINT elementCount = static_cast<UINT>(elements.size());
		const HRESULT declarationResult = vertexDeclaration->GetDeclaration(
			elements.data(),
			&elementCount);
		vertexDeclaration->Release();
		if (declarationResult != D3D_OK)
		{
			return "declaration=result(" + std::to_string(declarationResult) + ")";
		}

		std::ostringstream result;
		result << "declaration=[";
		std::array<bool, 16> usedStreams = {};
		bool hasElement = false;
		bool hasPosition = false;
		D3DVERTEXELEMENT9 positionElement = {};
		for (UINT elementIndex = 0; elementIndex < elementCount; ++elementIndex)
		{
			const auto& element = elements[elementIndex];
			if (element.Stream == 0xFF) break;
			if (hasElement) result << ",";
			hasElement = true;
			result << "{stream=" << element.Stream
				<< ",offset=" << element.Offset
				<< ",type=" << static_cast<UINT>(element.Type)
				<< ",method=" << static_cast<UINT>(element.Method)
				<< ",usage=" << static_cast<UINT>(element.Usage)
				<< ",usageIndex=" << static_cast<UINT>(element.UsageIndex)
				<< "}";
			if (element.Stream < usedStreams.size()) usedStreams[element.Stream] = true;
			if (!hasPosition
				&& element.Usage == D3DDECLUSAGE_POSITION
				&& element.UsageIndex == 0)
			{
				hasPosition = true;
				positionElement = element;
			}
		}
		result << "]";

		for (UINT streamIndex = 0; streamIndex < usedStreams.size(); ++streamIndex)
		{
			if (!usedStreams[streamIndex]) continue;

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
				result << " stream" << streamIndex << "=unavailable";
				continue;
			}

			D3DVERTEXBUFFER_DESC description = {};
			const HRESULT descriptionResult = vertexBuffer->GetDesc(&description);
			UINT frequency = 0;
			const HRESULT frequencyResult = device->GetStreamSourceFreq(streamIndex, &frequency);
			const auto identity = reinterpret_cast<uintptr_t>(vertexBuffer);
			result << " stream" << streamIndex
				<< "={identity=0x" << std::hex << identity << std::dec
				<< ",offset=" << streamOffset
				<< ",stride=" << streamStride
				<< ",frequency=";
			if (frequencyResult == D3D_OK) result << "0x" << std::hex << frequency << std::dec;
			else result << "result(" << frequencyResult << ")";

			if (descriptionResult != D3D_OK)
			{
				result << ",description=result(" << descriptionResult << ")}";
				vertexBuffer->Release();
				continue;
			}

			result << ",size=" << description.Size
				<< ",usage=0x" << std::hex << description.Usage
				<< ",pool=" << std::dec << static_cast<UINT>(description.Pool);

			uint64_t firstVertex = 0;
			if (streamIndex == 0)
			{
				const int64_t signedFirstVertex = static_cast<int64_t>(baseVertexIndex)
					+ minimumVertexIndex;
				if (signedFirstVertex > 0) firstVertex = static_cast<uint64_t>(signedFirstVertex);
			}
			const uint64_t requestedStart = static_cast<uint64_t>(streamOffset)
				+ firstVertex * streamStride;
			const uint64_t requestedByteCount = streamIndex == 0
				? static_cast<uint64_t>(vertexCount) * streamStride
				: NOTE_BY_NOTE_STREAM_BYTE_LIMIT;
			if (requestedStart >= description.Size)
			{
				result << ",data=out-of-range}";
				vertexBuffer->Release();
				continue;
			}

			const auto availableByteCount = static_cast<size_t>(description.Size - requestedStart);
			const auto lockedByteCount = static_cast<UINT>((std::min)(
				availableByteCount,
				static_cast<size_t>((std::min)(requestedByteCount,
					static_cast<uint64_t>(NOTE_BY_NOTE_STREAM_BYTE_LIMIT)))));
			void* lockedData = nullptr;
			const HRESULT lockResult = vertexBuffer->Lock(
				static_cast<UINT>(requestedStart),
				lockedByteCount,
				&lockedData,
				D3DLOCK_READONLY);
			if (lockResult != D3D_OK || lockedData == nullptr)
			{
				result << ",data=result(" << lockResult << ")}";
				vertexBuffer->Release();
				continue;
			}

			const auto* bytes = static_cast<const uint8_t*>(lockedData);
			result << ",lockedBytes=" << lockedByteCount
				<< ",hash=0x" << std::hex << HashBytes(bytes, lockedByteCount) << std::dec;
			if (hasPosition
				&& positionElement.Stream == streamIndex
				&& streamStride > 0)
			{
				const unsigned int componentCount = GetFloatComponentCount(positionElement.Type);
				if (componentCount > 0
					&& positionElement.Offset + componentCount * sizeof(float) <= streamStride)
				{
					std::array<float, 4> minimum = {
						(std::numeric_limits<float>::max)(),
						(std::numeric_limits<float>::max)(),
						(std::numeric_limits<float>::max)(),
						(std::numeric_limits<float>::max)()
					};
					std::array<float, 4> maximum = {
						(std::numeric_limits<float>::lowest)(),
						(std::numeric_limits<float>::lowest)(),
						(std::numeric_limits<float>::lowest)(),
						(std::numeric_limits<float>::lowest)()
					};
					const auto readableVertexCount = (std::min)(
						static_cast<size_t>(vertexCount),
						static_cast<size_t>(lockedByteCount / streamStride));
					size_t finitePositionCount = 0;
					for (size_t vertexIndex = 0; vertexIndex < readableVertexCount; ++vertexIndex)
					{
						std::array<float, 4> position = {};
						std::memcpy(
							position.data(),
							bytes + vertexIndex * streamStride + positionElement.Offset,
							componentCount * sizeof(float));
						bool isFinite = true;
						for (unsigned int componentIndex = 0;
							componentIndex < componentCount;
							++componentIndex)
						{
							isFinite = isFinite && std::isfinite(position[componentIndex]);
						}
						if (!isFinite) continue;

						++finitePositionCount;
						for (unsigned int componentIndex = 0;
							componentIndex < componentCount;
							++componentIndex)
						{
							minimum[componentIndex] = (std::min)(
								minimum[componentIndex],
								position[componentIndex]);
							maximum[componentIndex] = (std::max)(
								maximum[componentIndex],
								position[componentIndex]);
						}
					}

					result << ",positions=" << finitePositionCount << "x" << componentCount;
					if (finitePositionCount > 0)
					{
						result << std::fixed << std::setprecision(5) << ",positionBounds=(";
						for (unsigned int componentIndex = 0;
							componentIndex < componentCount;
							++componentIndex)
						{
							if (componentIndex > 0) result << ";";
							result << minimum[componentIndex] << ".." << maximum[componentIndex];
						}
						result << ")";
					}
				}
			}

			vertexBuffer->Unlock();
			result << "}";
			vertexBuffer->Release();
		}

		return result.str();
	}

	void ObserveNoteByNoteDraw(
		IDirect3DDevice9* device,
		D3DPRIMITIVETYPE primitiveType,
		INT baseVertexIndex,
		UINT minimumVertexIndex,
		UINT vertexCount,
		UINT startIndex,
		UINT primitiveCount,
		UINT stride,
		UINT startRegister,
		UINT vectorCount,
		UINT declarationType,
		UINT declarationElementCount,
		uintptr_t streamIdentity,
		uintptr_t nativeCaller)
	{
		if (!isNoteByNoteRenderSnapshotArmed.load(std::memory_order_acquire)) return;

		std::lock_guard<std::mutex> lock(noteByNoteRenderSnapshotMutex);
		if (!isNoteByNoteRenderSnapshotActive
			|| noteByNoteTotalDrawCount >= NOTE_BY_NOTE_CAPTURE_DRAW_LIMIT)
		{
			return;
		}

		float vertexConstants[NOTE_BY_NOTE_VERTEX_CONSTANT_COUNT * 4] = {};
		float pixelConstants[NOTE_BY_NOTE_PIXEL_CONSTANT_COUNT * 4] = {};
		const HRESULT constantsResult = device->GetVertexShaderConstantF(
			0,
			vertexConstants,
			NOTE_BY_NOTE_VERTEX_CONSTANT_COUNT);
		const HRESULT pixelConstantsResult = device->GetPixelShaderConstantF(
			0,
			pixelConstants,
			NOTE_BY_NOTE_PIXEL_CONSTANT_COUNT);
		UINT vertexShaderByteCount = 0;
		UINT pixelShaderByteCount = 0;
		const auto vertexShaderIdentity = GetCurrentVertexShaderIdentity(
			device,
			vertexShaderByteCount);
		const auto pixelShaderIdentity = GetCurrentPixelShaderIdentity(
			device,
			pixelShaderByteCount);
		LogCurrentShaderDisassemblies(
			device,
			vertexShaderIdentity,
			pixelShaderIdentity);
		const auto texture0Identity = GetCurrentTextureIdentity(device, 0);
		const auto texture1Identity = GetCurrentTextureIdentity(device, 1);
		const auto indexBufferIdentity = GetCurrentIndexBufferIdentity(device);
		const auto vertexStreams = CaptureVertexStreams(
			device,
			baseVertexIndex,
			minimumVertexIndex,
			vertexCount);
		const auto nativeInstances = CaptureNativeInstances(device);

		hasNoteByNoteDrawInFrame = true;
		++noteByNoteFrameDrawCount;
		++noteByNoteTotalDrawCount;

		std::ostringstream constants;
		constants << std::fixed << std::setprecision(5);
		if (constantsResult == D3D_OK)
		{
			for (unsigned int registerIndex = 0;
				registerIndex < NOTE_BY_NOTE_VERTEX_CONSTANT_COUNT;
				++registerIndex)
			{
				const auto valueIndex = registerIndex * 4;
				constants << " c" << registerIndex << "=("
					<< vertexConstants[valueIndex] << ","
					<< vertexConstants[valueIndex + 1] << ","
					<< vertexConstants[valueIndex + 2] << ","
					<< vertexConstants[valueIndex + 3] << ")";
			}
		}
		else
		{
			constants << " unavailable(result=" << std::dec << constantsResult << ")";
		}
		constants << " pixel=";
		if (pixelConstantsResult == D3D_OK)
		{
			for (unsigned int registerIndex = 0;
				registerIndex < NOTE_BY_NOTE_PIXEL_CONSTANT_COUNT;
				++registerIndex)
			{
				const auto valueIndex = registerIndex * 4;
				constants << " p" << registerIndex << "=("
					<< pixelConstants[valueIndex] << ","
					<< pixelConstants[valueIndex + 1] << ","
					<< pixelConstants[valueIndex + 2] << ","
					<< pixelConstants[valueIndex + 3] << ")";
			}
		}
		else
		{
			constants << "unavailable(result=" << std::dec << pixelConstantsResult << ")";
		}

		LOG_INFO("(NBN NATIVE RENDER) draw frame=" << noteByNoteRenderFrame
			<< " ordinal=" << noteByNoteFrameDrawCount
			<< " totalOrdinal=" << noteByNoteTotalDrawCount
			<< " songTime=" << std::fixed << std::setprecision(5) << SongTimer::SongTimer()
			<< " greyNoteCutoff=" << SongTimer::GetGreyNoteTimer()
			<< " caller=0x" << std::hex << nativeCaller
			<< " stream=0x" << streamIdentity
			<< " indexBuffer=0x" << indexBufferIdentity
			<< " texture0=0x" << texture0Identity
			<< " texture1=0x" << texture1Identity
			<< " vertexShader=0x" << vertexShaderIdentity
			<< " vertexShaderBytes=0x" << vertexShaderByteCount
			<< " pixelShader=0x" << pixelShaderIdentity
			<< " pixelShaderBytes=0x" << pixelShaderByteCount
			<< std::dec
			<< " mesh={stride=" << stride
			<< ",primitiveType=" << static_cast<UINT>(primitiveType)
			<< ",baseVertexIndex=" << baseVertexIndex
			<< ",minimumVertexIndex=" << minimumVertexIndex
			<< ",vertexCount=" << vertexCount
			<< ",startIndex=" << startIndex
			<< ",primitiveCount=" << primitiveCount
			<< ",startRegister=" << startRegister
			<< ",vectorCount=" << vectorCount
			<< ",declarationType=" << declarationType
			<< ",declarationElements=" << declarationElementCount
			<< "} constants=" << constants.str()
			<< " " << vertexStreams
			<< " " << nativeInstances << std::endl);
	}
}

void D3DHooks::ArmNoteByNoteRenderSnapshot()
{
	std::lock_guard<std::mutex> lock(noteByNoteRenderSnapshotMutex);
	isNoteByNoteRenderSnapshotActive = false;
	hasNoteByNoteDrawInFrame = false;
	noteByNoteCapturedFrameCount = 0;
	noteByNoteFrameDrawCount = 0;
	noteByNoteTotalDrawCount = 0;
	isNoteByNoteRenderSnapshotArmed.store(true, std::memory_order_release);
	LOG_INFO("(NBN NATIVE RENDER) Armed for the next "
		<< NOTE_BY_NOTE_CAPTURE_FRAME_COUNT
		<< " complete native note-head frames." << std::endl);
}

bool D3DHooks::ShouldSuppressDrawBySignature(
	IDirect3DDevice9* device,
	UINT stride,
	UINT vertexCount,
	UINT primitiveCount)
{
	return ShouldSuppressPhysicalGuideDraw(device, stride, vertexCount, primitiveCount);
}

void D3DHooks::SetPhysicalGuideSuppression(
	bool shouldEnable,
	uint32_t stride,
	uint32_t vertexCount,
	uint32_t primitiveCount)
{
	physicalGuideSuppressStride.store(stride, std::memory_order_release);
	physicalGuideSuppressVertexCount.store(vertexCount, std::memory_order_release);
	physicalGuideSuppressPrimitiveCount.store(primitiveCount, std::memory_order_release);
	isPhysicalGuideSuppressionEnabled.store(shouldEnable, std::memory_order_release);
}

void D3DHooks::ArmPhysicalMarkerDrawCapture()
{
	physicalMarkerArmCallbackBase.store(
		physicalMarkerCallbackCount.load(std::memory_order_acquire),
		std::memory_order_release);
	physicalMarkerArmPublishedDrawBase.store(
		physicalMarkerPublishedDrawCount.load(std::memory_order_acquire),
		std::memory_order_release);
	physicalMarkerArmMatchedDrawBase.store(
		physicalMarkerMatchedDrawCount.load(std::memory_order_acquire),
		std::memory_order_release);
	physicalMarkerArmGeneration.fetch_add(1, std::memory_order_acq_rel);
	isPhysicalMarkerCaptureActive.store(false, std::memory_order_release);
	isPhysicalMarkerCaptureArmed.store(true, std::memory_order_release);
}

D3DHooks::FrameTimeStats D3DHooks::GetFrameTimeStats()
{
	FrameTimeStats stats;
	stats.averageMilliseconds = frameTimeAverageMilliseconds.load(std::memory_order_relaxed);
	stats.windowMaxMilliseconds = frameTimeWindowMaxMilliseconds.load(std::memory_order_relaxed);
	stats.frameCount = frameTimeFrameCount.load(std::memory_order_relaxed);
	return stats;
}

void D3DHooks::SetStaleMarkerFilterEnabled(bool shouldEnable)
{
	isStaleMarkerFilterEnabled.store(shouldEnable, std::memory_order_release);
	LOG_INFO("(NBN STALE MARKER) Quad filter "
		<< (shouldEnable ? "enabled" : "disabled") << "." << std::endl);
}

D3DHooks::StaleMarkerFilterState D3DHooks::GetStaleMarkerFilterState()
{
	StaleMarkerFilterState state;
	state.enabled = isStaleMarkerFilterEnabled.load(std::memory_order_acquire);
	state.consideredQuads = staleMarkerQuadsConsidered.load(std::memory_order_relaxed);
	state.droppedQuads = staleMarkerQuadsDropped.load(std::memory_order_relaxed);
	state.filteredDraws = staleMarkerDrawsFiltered.load(std::memory_order_relaxed);
	state.lockWaits = staleMarkerLockWaits.load(std::memory_order_relaxed);
	state.lockNoWaits = staleMarkerLockNoWaits.load(std::memory_order_relaxed);
	return state;
}

void D3DHooks::SetFingerNumeralsEnabled(bool shouldEnable)
{
	isFingerNumeralsEnabled.store(shouldEnable, std::memory_order_release);
	LOG_INFO("(NBN FINGER NUMERALS) Host-drawn numerals "
		<< (shouldEnable ? "enabled" : "disabled") << "." << std::endl);
}

bool D3DHooks::AreFingerNumeralsEnabled()
{
	return isFingerNumeralsEnabled.load(std::memory_order_acquire);
}

bool D3DHooks::HasRestingFingerRing(int stringIndex, int fret)
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

int D3DHooks::GetFreshFingerAnchors(
	int (&strings)[8],
	int (&frets)[8],
	float (&screenX)[8],
	float (&screenY)[8])
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

D3DHooks::PhysicalMarkerCaptureState D3DHooks::GetPhysicalMarkerCaptureState()
{
	PhysicalMarkerCaptureState state;
	state.armGeneration = physicalMarkerArmGeneration.load(std::memory_order_acquire);
	state.callbackCount = physicalMarkerCallbackCount.load(std::memory_order_acquire);
	state.armCallbackBase = physicalMarkerArmCallbackBase.load(std::memory_order_acquire);
	state.matchedDrawCount = physicalMarkerMatchedDrawCount.load(std::memory_order_acquire);
	state.armMatchedDrawBase = physicalMarkerArmMatchedDrawBase.load(std::memory_order_acquire);
	state.publishedDrawCount = physicalMarkerPublishedDrawCount.load(std::memory_order_acquire);
	state.armPublishedDrawBase = physicalMarkerArmPublishedDrawBase.load(std::memory_order_acquire);
	state.guideSuppressionEnabled =
		isPhysicalGuideSuppressionEnabled.load(std::memory_order_acquire);
	state.guideConsideredDrawCount =
		physicalGuideConsideredDrawCount.load(std::memory_order_acquire);
	state.guideSuppressedDrawCount =
		physicalGuideSuppressedDrawCount.load(std::memory_order_acquire);
	state.isArmed = isPhysicalMarkerCaptureArmed.load(std::memory_order_acquire);
	state.isActive = isPhysicalMarkerCaptureActive.load(std::memory_order_acquire);
	return state;
}

void D3DHooks::SetNoteByNoteNativeLifecycleTraceEnabled(bool shouldEnable)
{
	if (shouldEnable)
	{
		hasLoggedNoteByNoteNativeCallStack.store(false, std::memory_order_release);
	}
	isNoteByNoteNativeLifecycleTraceEnabled.store(shouldEnable, std::memory_order_release);
	LOG_INFO("(NBN NATIVE LIFECYCLE) Trace "
		<< (shouldEnable ? "enabled" : "disabled") << "." << std::endl);
}

void D3DHooks::FinishNoteByNoteRenderFrame()
{
	SampleDrawHookFrameState();
	// The native Riff Repeater NOTE BY NOTE row: read while focused, repaired while not.
	NoteByNoteMenu::Poll();
	FinishPhysicalMarkerCaptureFrame();
	FinishNativeFrontAttackFrame();

	// Per-frame boundary for the probe's render snapshot. Sent every frame with the current
	// frame index, matching the value the draw forwards above carry; the probe acts on it only
	// while its own capture is armed. This runs regardless of the host-side snapshot flag below,
	// which only drives the older in-DLL capture.
	ResearchBridge::DispatchRenderFrameComplete(noteByNoteRenderFrame);

	// Cross-thread heap corruption check, render-thread arm: validate the heaps every
	// frame for the first ~900 frames after each Note by Note enable, so a corruption
	// arising on or near this thread names a render seam instead of the next scoring
	// seam. Uses the HOST's copy of HeapCheckpoint.
	// Debug only: HeapValidate over every process heap costs milliseconds per frame,
	// which would hide the real NBN-on frame cost for the first 15s after each enable.
#if defined(_DEBUG)
	{
		static bool wasAutomaticEnabled = false;
		static uint32_t framesSinceEnable = 0;
		const bool automaticEnabled = NoteByNoteProbe::IsAutomaticEnabled();
		if (automaticEnabled && !wasAutomaticEnabled) framesSinceEnable = 0;
		wasAutomaticEnabled = automaticEnabled;
		if (automaticEnabled && framesSinceEnable < 900)
		{
			++framesSinceEnable;
			// Host-local validation (the scoring TU's checkpoint lives in the
			// probe binary): latches on first detection like the probe's.
			static bool renderHeapCheckTripped = false;
			if (!renderHeapCheckTripped)
			{
				HANDLE heaps[64] = {};
				const DWORD heapCount = GetProcessHeaps(64, heaps);
				const DWORD checked = heapCount < 64 ? heapCount : 64;
				for (DWORD index = 0; index < checked; ++index)
				{
					if (HeapValidate(heaps[index], 0, nullptr) != FALSE) continue;
					renderHeapCheckTripped = true;
					LOG_ERROR("(NBN HEAP CHECK) HEAP CORRUPTION DETECTED at"
						<< " seam=render-frame-" << framesSinceEnable
						<< " heap=" << index << "/" << checked
						<< ". Render-thread lattice: the corrupting write happened"
						<< " before this frame boundary." << std::endl);
					break;
				}
			}
		}
	}
#endif

	if (!isNoteByNoteRenderSnapshotArmed.load(std::memory_order_acquire))
	{
		++noteByNoteRenderFrame;
		return;
	}

	std::lock_guard<std::mutex> lock(noteByNoteRenderSnapshotMutex);
	if (!isNoteByNoteRenderSnapshotActive)
	{
		isNoteByNoteRenderSnapshotActive = true;
		LOG_INFO("(NBN NATIVE RENDER) Capture starts at native frame="
			<< noteByNoteRenderFrame + 1 << "." << std::endl);
		++noteByNoteRenderFrame;
		return;
	}

	if (!hasNoteByNoteDrawInFrame)
	{
		++noteByNoteRenderFrame;
		return;
	}

	LOG_INFO("(NBN NATIVE RENDER) frame-complete frame=" << noteByNoteRenderFrame
		<< " noteHeadDraws=" << noteByNoteFrameDrawCount
		<< " totalDraws=" << noteByNoteTotalDrawCount << std::endl);
	hasNoteByNoteDrawInFrame = false;
	noteByNoteFrameDrawCount = 0;
	++noteByNoteCapturedFrameCount;
	++noteByNoteRenderFrame;

	if (noteByNoteCapturedFrameCount < NOTE_BY_NOTE_CAPTURE_FRAME_COUNT
		&& noteByNoteTotalDrawCount < NOTE_BY_NOTE_CAPTURE_DRAW_LIMIT)
	{
		return;
	}

	isNoteByNoteRenderSnapshotActive = false;
	isNoteByNoteRenderSnapshotArmed.store(false, std::memory_order_release);
	LOG_INFO("(NBN NATIVE RENDER) Capture complete. frames="
		<< noteByNoteCapturedFrameCount
		<< " noteHeadDraws=" << noteByNoteTotalDrawCount << "." << std::endl);
}

/// <summary>
/// IDirect3DDevice9::DrawPrimitive Middleware. Mainly used for Note Tails
/// </summary>
/// <param name="pDevice"> - Device Pointer</param>
/// <param name="PrimType"> - Member of the D3DPRIMITIVETYPE enumerated type, describing the type of primitive to render.</param>
/// <param name="StartIndex"> - Index of the first vertex to load.</param>
/// <param name="PrimCount"> - Number of primitives to render.</param>
/// <returns>If the method succeeds, the return value is D3D_OK. If the method fails, the return value can be D3DERR_INVALIDCALL.</returns>
HRESULT APIENTRY D3DHooks::Hook_DP(IDirect3DDevice9* pDevice, D3DPRIMITIVETYPE PrimType, UINT StartIndex, UINT PrimCount) { // Mainly used for Note Tails
	if (pDevice->GetStreamSource(0, &Stream_Data, &Offset, &Stride) == D3D_OK)
		Stream_Data->Release();
	if (isInSongForDrawHooks.load(std::memory_order_relaxed))
	{
		ObservePhysicalMarkerDraw(
			pDevice,
			PrimType,
			0,
			0,
			0,
			StartIndex,
			PrimCount,
			Stride,
			reinterpret_cast<uintptr_t>(Stream_Data),
			reinterpret_cast<uintptr_t>(_ReturnAddress()));
		DispatchFullDrawObservation(
			pDevice,
			1,
			PrimType,
			0,
			0,
			0,
			StartIndex,
			PrimCount,
			Stride,
			reinterpret_cast<uintptr_t>(Stream_Data),
			reinterpret_cast<uintptr_t>(_ReturnAddress()),
			nullptr);
	}
	// Note-tails for Extended Range / Custom Colors
	if (ERMode::AttemptedERInThisSong && ERMode::UseEROrColorsInThisSong && NOTE_TAILS) {
		GameState::ToggleCB(ERMode::UseERExclusivelyInThisSong);

		switch (Settings::GetModSetting("SeparateNoteColors")) {
			case 0: // Use same color scheme on notes as we do on strings
				pDevice->SetTexture(1, customStringColorTexture);
				break;
			case 1: // Default Colors, so don't do anything.
				break;
			case 2: // Use Custom Note Color Scheme
				pDevice->SetTexture(1, customNoteColorTexture);
				break;
			default:
				break;
		}
	}

	// Note-tails for Twitch mod - Remove Notes.
	if (Settings::IsTwitchSettingEnabled("RemoveNotes") && NOTE_TAILS)
		return REMOVE_TEXTURE;

	// Note-tails for Twitch mod - Transparent Notes.
	if (Settings::IsTwitchSettingEnabled("TransparentNotes") && NOTE_TAILS)
		pDevice->SetTexture(1, nonexistentTexture);

	// Note-tails for Twitch mod - Solid Colored notes.
	if (Settings::IsTwitchSettingEnabled("SolidNotes") && NOTE_TAILS) {
		if (Settings::ReturnSettingValue("SolidNoteColor") == "random")
			pDevice->SetTexture(1, randomTextures[currentRandomTexture]);
		else
			pDevice->SetTexture(1, twitchUserDefinedTexture);
	}
	
	// Note-tails for Rainbow Notes.
	if (ERMode::RainbowNotesEnabled && ERMode::customNoteColorH > 0 && NOTE_TAILS)
		pDevice->SetTexture(1, rainbowTextures[ERMode::customNoteColorH]);

	// Call the original DrawPrimitive. Non-indexed draws carry no vertex count, so the
	// suppression signature for this path is (stride, 0, primitiveCount).
	if (D3DHooks::ShouldSuppressDrawBySignature(pDevice, Stride, 0, PrimCount))
	{
		return D3D_OK;
	}
	return oDrawPrimitive(pDevice, PrimType, StartIndex, PrimCount);
}

/// <summary>
/// IDirect3DDevice9::DrawPrimitiveUP Middleware. Feeds user-pointer draws to the
/// physical-marker capture and suppresses the neck note marker during holds.
/// </summary>
HRESULT APIENTRY D3DHooks::Hook_DPUP(IDirect3DDevice9* pDevice, D3DPRIMITIVETYPE PrimType, UINT PrimCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
	if (isInSongForDrawHooks.load(std::memory_order_relaxed))
	{
		ObservePhysicalMarkerDraw(
			pDevice,
			PrimType,
			-1,
			0,
			0,
			0,
			PrimCount,
			VertexStreamZeroStride,
			reinterpret_cast<uintptr_t>(pVertexStreamZeroData),
			reinterpret_cast<uintptr_t>(_ReturnAddress()),
			pVertexStreamZeroData);
		DispatchFullDrawObservation(
			pDevice,
			2,
			PrimType,
			0,
			0,
			0,
			0,
			PrimCount,
			VertexStreamZeroStride,
			0,
			reinterpret_cast<uintptr_t>(_ReturnAddress()),
			pVertexStreamZeroData);

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
						D3DHooks::vertexShaderConstantShadow,
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
						return D3D_OK;
					}
				}
			}
		}
	}
	return oDrawPrimitiveUP(pDevice, PrimType, PrimCount, pVertexStreamZeroData, VertexStreamZeroStride);
}

/// <summary>
/// IDirect3DDevice9::DrawIndexedPrimitiveUP Middleware. Observe-only, as above.
/// </summary>
HRESULT APIENTRY D3DHooks::Hook_DIPUP(IDirect3DDevice9* pDevice, D3DPRIMITIVETYPE PrimType, UINT MinVertexIndex, UINT NumVertices, UINT PrimCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
	if (isInSongForDrawHooks.load(std::memory_order_relaxed))
	{
		ObservePhysicalMarkerDraw(
			pDevice,
			PrimType,
			-1,
			MinVertexIndex,
			NumVertices,
			0,
			PrimCount,
			VertexStreamZeroStride,
			reinterpret_cast<uintptr_t>(pVertexStreamZeroData),
			reinterpret_cast<uintptr_t>(_ReturnAddress()),
			pVertexStreamZeroData);
		DispatchFullDrawObservation(
			pDevice,
			3,
			PrimType,
			0,
			MinVertexIndex,
			NumVertices,
			0,
			PrimCount,
			VertexStreamZeroStride,
			0,
			reinterpret_cast<uintptr_t>(_ReturnAddress()),
			pVertexStreamZeroData);
	}
	return oDrawIndexedPrimitiveUP(pDevice, PrimType, MinVertexIndex, NumVertices, PrimCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
}

/// <summary>
/// IDirect3DDevice9::SetVertexDeclaration Middleware.
/// </summary>
/// <param name="pDevice"> - Device Pointer</param>
/// <param name="pdecl"> - Pointer to an IDirect3DVertexDeclaration9 object, which contains the vertex declaration.</param>
/// <returns>If the method succeeds, the return value is D3D_OK. The return value can be D3DERR_INVALIDCALL.</returns>
HRESULT APIENTRY D3DHooks::Hook_SetVertexDeclaration(LPDIRECT3DDEVICE9 pDevice, IDirect3DVertexDeclaration9* pdecl) {
	if (pdecl != NULL)
		pdecl->GetDeclaration(decl, &NumElements);

	// Call the original SetVertexDeclaration.
	return oSetVertexDeclaration(pDevice, pdecl);
}

/// <summary>
/// IDirect3DDevice9::SetVertexShaderConstantF Middleware.
/// </summary>
/// <param name="pDevice"> - Device Pointer</param>
/// <param name="i_StartRegister"> - Register number that will contain the first constant value.</param>
/// <param name="pConstantData"> - Pointer to an array of constants.</param>
/// <param name="Vector4fCount"> - Number of four float vectors in the array of constants.</param>
/// <returns>If the method succeeds, the return value is D3D_OK. If the method fails, the return value can be D3DERR_INVALIDCALL.</returns>
// Shadow of vertex-shader constant registers c0..c7, maintained below. The research
// probe's screen-map capture reads the draw matrices from here when the device rejects
// GetVertexShaderConstantF (a pure device would), so the capture works either way.
float D3DHooks::vertexShaderConstantShadow[8][4] = {};

HRESULT APIENTRY D3DHooks::Hook_SetVertexShaderConstantF(LPDIRECT3DDEVICE9 pDevice, UINT i_StartRegister, const float* pConstantData, UINT Vector4fCount) {
	if (pConstantData != NULL) {
		StartRegister = i_StartRegister;
		VectorCount = Vector4fCount;

		if (i_StartRegister < 8) {
			const UINT copyCount = Vector4fCount < 8 - i_StartRegister ? Vector4fCount : 8 - i_StartRegister;
			memcpy(D3DHooks::vertexShaderConstantShadow[i_StartRegister], pConstantData, copyCount * 4 * sizeof(float));
		}
	}

	// Call the original SetVertexShaderConstantF
	return oSetVertexShaderConstantF(pDevice, i_StartRegister, pConstantData, Vector4fCount);
}

/// <summary>
/// IDirect3DDevice9::SetVertexShader Middleware.
/// </summary>
/// <param name="pDevice"> - Device Pointer</param>
/// <param name="veShader"> - Vertex shader interface.</param>
/// <returns>If the method succeeds, the return value is D3D_OK. If the method fails, the return value can be D3DERR_INVALIDCALL.</returns>
HRESULT APIENTRY D3DHooks::Hook_SetVertexShader(LPDIRECT3DDEVICE9 pDevice, IDirect3DVertexShader9* veShader) {
	if (veShader != NULL) {
		vShader = veShader;
		vShader->GetFunction(NULL, &vSize);
	}
	
	// Call the original SetVertexShader.
	return oSetVertexShader(pDevice, veShader);
}

/// <summary>
/// IDirect3DDevice9::SetPixelShader Middleware.
/// </summary>
/// <param name="pDevice"> - Device Pointer</param>
/// <param name="piShader"> - Pixel shader interface.</param>
/// <returns>If the method succeeds, the return value is D3D_OK. If the method fails, the return value can be D3DERR_INVALIDCALL.</returns>
HRESULT APIENTRY D3DHooks::Hook_SetPixelShader(LPDIRECT3DDEVICE9 pDevice, IDirect3DPixelShader9* piShader) {
	if (piShader != NULL) {
		pShader = piShader;
		pShader->GetFunction(NULL, &pSize);
	}

	// Call the original SetPixelShader.
	return oSetPixelShader(pDevice, piShader);
}

/// <summary>
/// IDirect3DDevice9::SetStreamSource Middleware.
/// </summary>
/// <param name="pDevice"> - Device Pointer</param>
/// <param name="StreamNumber"> - Specifies the data stream, in the range from 0 to the maximum number of streams -1.</param>
/// <param name="pStreamData"> - Pointer to an IDirect3DVertexBuffer9 interface, representing the vertex buffer to bind to the specified data stream.</param>
/// <param name="OffsetInBytes"> - Offset from the beginning of the stream to the beginning of the vertex data, in bytes.</param>
/// <param name="i_Stride"> - Stride of the component, in bytes.</param>
/// <returns>If the method succeeds, the return value is D3D_OK. If the method fails, the return value can be D3DERR_INVALIDCALL.</returns>
HRESULT APIENTRY D3DHooks::Hook_SetStreamSource(LPDIRECT3DDEVICE9 pDevice, UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT i_Stride) {
	D3DVERTEXBUFFER_DESC desc;

	// Remove Line Markers mod.
	if (i_Stride == 32 && NumElements == 8 && VectorCount == 4 && decl->Type == 2) { 
		pStreamData->GetDesc(&desc);
		vertexBufferSize = desc.Size;
	}

	// Call original SetStreamSource.
	return oSetStreamSource(pDevice, StreamNumber, pStreamData, OffsetInBytes, i_Stride);
}

/// <summary>
/// IDirect3DDevice9::Reset Middleware. Required so Alt+Tab won't break the game (ImGUI & UI Text).
/// </summary>
/// <param name="pDevice"> - Device Pointer</param>
/// <param name="pPresentationParameters"> - Pointer to a D3DPRESENT_PARAMETERS structure, describing the new presentation parameters. This value cannot be NULL.</param>
/// <returns>Possible return values include: D3D_OK, D3DERR_DEVICELOST, D3DERR_DEVICEREMOVED, D3DERR_DRIVERINTERNALERROR, or D3DERR_OUTOFVIDEOMEMORY.</returns>
HRESULT APIENTRY D3DHooks::Hook_Reset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters) {
	// Lost Device - release device-dependent resources before Reset.
	NoteByNoteHighwayRenderer::ClearSelectedTarget();
	ImGui_ImplDX9_InvalidateDeviceObjects();
	GameOverlay::OnLostDevice();

	// Reset Device. Call original Reset.
	HRESULT ResetReturn = oReset(pDevice, pPresentationParameters);

	// Device-reset diagnostics. A Reset that keeps returning D3DERR_INVALIDCALL
	// (0x8876086C) is D3D9 saying a D3DPOOL_DEFAULT resource is still referenced or a buffer
	// is still locked; the game then retries every frame with a frozen picture (a hang after
	// unpause). Logged with the code and the count.
	{
		static unsigned int resetFailureCount = 0;
		if (FAILED(ResetReturn))
		{
			++resetFailureCount;
			if (resetFailureCount == 1 || resetFailureCount % 300 == 0)
			{
				LOG_ERROR("(D3D RESET) IDirect3DDevice9::Reset failed hr=0x" << std::hex
					<< static_cast<unsigned long>(ResetReturn) << std::dec
					<< " (0x8876086C = INVALIDCALL: a default-pool resource or locked buffer"
					<< " is still alive; 0x88760868 = DEVICELOST: not resettable yet)"
					<< " failures=" << resetFailureCount
					<< " backBuffer=" << pPresentationParameters->BackBufferWidth << 'x'
					<< pPresentationParameters->BackBufferHeight
					<< " windowed=" << pPresentationParameters->Windowed << std::endl);
			}
		}
		else if (resetFailureCount != 0)
		{
			LOG_INFO("(D3D RESET) Reset succeeded after " << resetFailureCount
				<< " failures." << std::endl);
			resetFailureCount = 0;
		}
	}

	// Only recreate device objects once the device is actually back. If Reset failed
	// (e.g. still D3DERR_DEVICELOST mid-Alt+Tab out of exclusive fullscreen), the game
	// retries Reset next frame; recreating against a lost device leaves a broken frame.
	if (SUCCEEDED(ResetReturn)) {
		ImGui_ImplDX9_CreateDeviceObjects();
		GameOverlay::OnResetDevice();
	}

	return ResetReturn;
}

/// <summary>
/// IDirect3DDevice9::DrawIndexedPrimitive Middleware. This is where most of our texture modifying mods are located.
/// </summary>
/// <param name="pDevice"> - Device Pointer</param>
/// <param name="PrimType"> - Member of the D3DPRIMITIVETYPE enumerated type, describing the type of primitive to render.</param>
/// <param name="BaseVertexIndex"> - Offset from the start of the vertex buffer to the first vertex.</param>
/// <param name="MinVertexIndex"> - Minimum vertex index for vertices used during this call. This is a zero based index relative to BaseVertexIndex.</param>
/// <param name="NumVertices"> - Number of vertices used during this call.</param>
/// <param name="StartIndex"> - Index of the first index to use when accesssing the vertex buffer.</param>
/// <param name="PrimCount"> - Number of primitives to render.</param>
/// <returns>If the method succeeds, the return value is D3D_OK. If the method fails, the return value can be the following: D3DERR_INVALIDCALL.</returns>
HRESULT APIENTRY D3DHooks::Hook_DIP(IDirect3DDevice9* pDevice, D3DPRIMITIVETYPE PrimType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimCount) { // Draw things on screen
	static bool calculatedCRC = false, calculatedHeadstocks = false, calculatedSkyline = false;

	if (pDevice->GetStreamSource(0, &Stream_Data, &Offset, &Stride) == D3D_OK)
		Stream_Data->Release();

	// This could potentially lead to game locking up (because DIP is called multiple times per frame) if that value is not filled, but generally it should work 
	if (Settings::ReturnSettingValue("ExtendedRangeEnabled").length() < 2) { // Due to some weird reasons, sometimes settings decide to go missing - this may solve the problem
		Settings::UpdateSettings();
		D3D::GenerateTextures(pDevice, D3D::Strings);
		D3D::GenerateTextures(pDevice, D3D::Notes);
		D3D::GenerateTextures(pDevice, D3D::Rainbow);
		LOG_INFO("Reloaded settings" << std::endl);
	}

	Mesh current(Stride, PrimCount, NumVertices);
	ThiccMesh currentThicc(Stride, PrimCount, NumVertices, StartIndex, StartRegister, PrimType, decl->Type, VectorCount, NumElements);
	// Sampled once per frame at the EndScene seam (isInSongForDrawHooks): a per-draw
	// GameState::IsInSong() walk costs noticeable frame rate.
	const bool isInSong = isInSongForDrawHooks.load(std::memory_order_relaxed);
	if (isInSong)
	{
		ObservePhysicalMarkerDraw(
			pDevice,
			PrimType,
			BaseVertexIndex,
			MinVertexIndex,
			NumVertices,
			StartIndex,
			PrimCount,
			Stride,
			reinterpret_cast<uintptr_t>(Stream_Data),
			reinterpret_cast<uintptr_t>(_ReturnAddress()));
	}
	const bool isNativeNoteAsset = isInSong
		&& (IsToBeRemoved(sevenstring, current)
			|| IsExtraRemoved(noteModifiers, currentThicc));
	const bool isNativeAttackHead = Stride == 36
		&& ((PrimCount == 40 && (NumVertices == 50 || NumVertices == 76 || NumVertices == 78))
			|| (PrimCount == 27 && NumVertices == 57));
	// Idle-path budget. With Note by Note holding
	// nothing and no research consumer listening, everything from here to the texture mods
	// must cost a few flag reads per draw. The two expensive per-draw classes are device
	// queries (GetStreamSource / GetStreamSourceFreq, each a trip through the D3D9 runtime)
	// and game-memory walks (SongTimer), and neither is issued unless something will
	// consume the answer on THIS draw:
	//   - the instance filter, which only acts while a presentation is selected (a hold),
	//   - the research draw feeds, which only exist while a consumer asked for them, and
	//   - the in-DLL render snapshot, which is armed per capture.
	// The visual filters further down (stale-marker doubles, physical-guide suppression,
	// instance filter) keep gating on hold/target state, never on the enable toggle.
	const bool drawFeedWanted = isInSong && ResearchBridge::IsDrawFeedWanted();
	const bool renderSnapshotArmed = isInSong
		&& isNoteByNoteRenderSnapshotArmed.load(std::memory_order_acquire);
	int openTargetString = -1;
	int openTargetFret = -1;
	const bool needsOpenFrontFilter = isInSong && isNativeAttackHead && NumVertices == 50
		&& NoteByNoteHighwayRenderer::GetHighwayMode()
			== NoteByNoteHighwayRenderer::HighwayMode::Off
		&& NoteByNoteHighwayRenderer::TryGetSelectedTargetForDiagnostics(
			openTargetString, openTargetFret);
	const bool needsInstanceStreams = isInSong
		&& (isNativeNoteAsset || isNativeAttackHead)
		&& (needsOpenFrontFilter || NoteByNoteHighwayRenderer::HasSelectedPresentation());
	const bool hasInstancedTransformStream =
		(needsInstanceStreams || drawFeedWanted || renderSnapshotArmed)
		&& HasInstancedTransformStream(pDevice);
	ResearchProtocol::NativeDrawObservation noteByNoteDraw = {};
	if (drawFeedWanted)
	{
		noteByNoteDraw.renderFrame = noteByNoteRenderFrame;
		noteByNoteDraw.songTime = SongTimer::SongTimer();
		noteByNoteDraw.greyNoteCutoff = SongTimer::GetGreyNoteTimer();
		noteByNoteDraw.primitiveType = static_cast<uint32_t>(PrimType);
		noteByNoteDraw.baseVertexIndex = BaseVertexIndex;
		noteByNoteDraw.minimumVertexIndex = MinVertexIndex;
		noteByNoteDraw.vertexCount = NumVertices;
		noteByNoteDraw.startIndex = StartIndex;
		noteByNoteDraw.primitiveCount = PrimCount;
		noteByNoteDraw.stride = Stride;
		noteByNoteDraw.startRegister = StartRegister;
		noteByNoteDraw.vectorCount = VectorCount;
		noteByNoteDraw.declarationType = decl->Type;
		noteByNoteDraw.declarationElementCount = NumElements;
		noteByNoteDraw.streamIdentity = reinterpret_cast<uintptr_t>(Stream_Data);
		noteByNoteDraw.nativeCaller = reinterpret_cast<uintptr_t>(_ReturnAddress());
		noteByNoteDraw.devicePointer = reinterpret_cast<uintptr_t>(pDevice);
		noteByNoteDraw.shaderConstantShadow =
			reinterpret_cast<uintptr_t>(&D3DHooks::vertexShaderConstantShadow[0][0]);

		// Full-draw feed: unlike the filtered dispatch below, every in-song draw goes to
		// the probe's transition capture. drawSite stays 0 (DrawIndexedPrimitive).
		if (ResearchBridge::IsFullDrawFeedEnabled())
		{
			ResearchBridge::DispatchFullDraw(noteByNoteDraw);
		}
	}
	if (isInSong && isNativeAttackHead)
	{
		ObserveNativeFrontAttack(
			pDevice,
			NumVertices,
			reinterpret_cast<uintptr_t>(_ReturnAddress()));
	}
	if (isInSong
		&& (renderSnapshotArmed || drawFeedWanted)
		&& (isNativeNoteAsset
			|| NOTE_STEMS
			|| OPEN_NOTE_ACCENTS
			|| hasInstancedTransformStream))
	{
		if (renderSnapshotArmed) ObserveNoteByNoteDraw(
			pDevice,
			PrimType,
			BaseVertexIndex,
			MinVertexIndex,
			NumVertices,
			StartIndex,
			PrimCount,
			Stride,
			StartRegister,
			VectorCount,
			decl->Type,
			NumElements,
			reinterpret_cast<uintptr_t>(Stream_Data),
			reinterpret_cast<uintptr_t>(_ReturnAddress()));

		// Forward the same filtered note-head draw to the reloadable probe, which owns the
		// render snapshot. _ReturnAddress() is Hook_DIP's own caller regardless of where in
		// the function it is read, so the native draw issuer is captured here just as above.
		// Only while the native draw feed is wanted: the struct above is empty otherwise.
		if (drawFeedWanted) ResearchBridge::DispatchNativeDraw(noteByNoteDraw);
	}
	// Reconnaissance for marking the target with Rocksmith's own highlighted note head.
	// Hard-capped inside, so this stops touching instance buffers after a few observations.
	if (isInSong && Stride == 32 && PrimCount == 104 && NumVertices == 187)
	{
		ObserveHighlightedNoteHead(pDevice);
	}

	// Quad-level filtering of the stale fretboard marker slices. Runs after the research
	// forwards so captures always see the unfiltered stream.
	if (isInSong)
	{
		HRESULT staleMarkerResult = D3D_OK;
		if (TryFilterStaleMarkerQuads(
			pDevice,
			PrimType,
			BaseVertexIndex,
			MinVertexIndex,
			NumVertices,
			StartIndex,
			PrimCount,
			Stride,
			reinterpret_cast<uintptr_t>(_ReturnAddress()),
			staleMarkerResult))
		{
			return staleMarkerResult;
		}

		// Decode-aware signature suppression on the INDEXED path: the fingering panels,
		// finger rings and marker mini-quads all draw through DrawIndexedPrimitive. The
		// decode gate (affine transform, fretboard longitudinal limit, exact
		// fret/string landing) keeps this narrow; the keep set spares the target,
		// its legato group and the held chord's members.
		if (D3DHooks::ShouldSuppressDrawBySignature(pDevice, Stride, NumVertices, PrimCount))
		{
			return D3D_OK;
		}
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
	if (isInSong
		&& hasInstancedTransformStream
		&& (isNativeNoteAsset || isNativeAttackHead))
	{
		const auto decision = DecideNoteByNoteInstances(pDevice, NumVertices);
		if (decision.mode == InstanceFilterMode::DrawNothing)
		{
			return D3D_OK;
		}
		if (decision.mode == InstanceFilterMode::DrawSubset
			&& DrawNoteByNoteSubset(
				pDevice,
				decision,
				PrimType,
				BaseVertexIndex,
				MinVertexIndex,
				NumVertices,
				StartIndex,
				PrimCount))
		{
			return D3D_OK;
		}
	}

	if (setAllToNoteGradientTexture) {
		pDevice->SetTexture(currStride, gradientTextureSeven);
		return SHOW_TEXTURE;
	}
	// Debugging of DIP.
	if (debug) {
		if (GetAsyncKeyState(VK_PRIOR) & 1 && currIdx < std::size(allMeshes) - 1)// Page up
			currIdx++;
		if (GetAsyncKeyState(VK_NEXT) & 1 && currIdx > 0) // Page down
			currIdx--;

		if (GetAsyncKeyState(VK_END) & 1) { // Toggle logging
			LOG_INFO("Logging is ");
			startLogging = !startLogging;
			if (!startLogging)
				LOG_NOHEAD("no longer ");
			LOG_NOHEAD("armed!" << std::endl);
		}

		if (GetAsyncKeyState(VK_F8) & 1) { // Save logged meshes to file
			for (const auto& mesh : allMeshes) {
				//Log(mesh.ToString().c_str());
			}
		}

		if (GetAsyncKeyState(VK_F7) & 1) { // Save only removed 
			for (const auto& mesh : removedMeshes) {
				//Log(mesh.ToString().c_str());
			}
		}

		if (GetAsyncKeyState(VK_CONTROL) & 1)
			//Log("{ %d, %d, %d, %d, %d, %d, %d, %d, %d }, ", Stride, PrimCount, NumVertices, StartIndex, StartRegister, PrimType, decl->Type, VectorCount, NumElements);

			if (startLogging) {
				if (std::find(allMeshes.begin(), allMeshes.end(), currentThicc) == allMeshes.end()) // Make sure we don't log what we'd already logged
					allMeshes.push_back(currentThicc);
				if (NOTE_STEMS) // Criteria for search
					LOG_INFO("{ " << Stride << ", "
									  << PrimCount << ", "
									  << NumVertices << ", "
									  << StartIndex << ", "
									  << StartRegister << ", "
									  << (UINT)PrimType << ", "
									  << (UINT)decl->Type << ", "
									  << VectorCount << ", "
									  << NumElements << " },"
									  << std::endl); // Thicc Mesh -> Log
				
				//_LOG("{ "<< Stride << ", " << PrimCount << ", " << NumVertices << " }," std::endl; // Mesh -> Console
				//_LOG(std::hex << crc << std::endl);
			}

		if (std::size(allMeshes) > 0 && allMeshes.at(currIdx) == currentThicc) {
			currStride = Stride;
			currNumVertices = NumVertices;
			currPrimCount = PrimCount;
			currStartIndex = StartIndex;
			currStartRegister = StartRegister;
			currPrimType = PrimType;
			currDeclType = decl->Type;
			currVectorCount = VectorCount;
			currNumElements = NumElements;
			//pDevice->SetTexture(1, Yellow);
			return REMOVE_TEXTURE;
		}

		if (IsExtraRemoved(removedMeshes, currentThicc))
			return REMOVE_TEXTURE;
	}

	// Mods

    bool RemoveFingerprints = Settings::ReturnSettingValue("RemoveFingerprints") == (std::string)"on";
	if (RemoveFingerprints && IsExtraRemoved(fingerprintMeshes, currentThicc)) {
		for (DWORD stage = 0; stage < 2; stage++) {
			LPDIRECT3DBASETEXTURE9 pTuningTexBase = nullptr;
			pDevice->GetTexture(stage, &pTuningTexBase);
			if (pTuningTexBase) {
				DWORD tuningCRC = 0;
				if (D3D::CRCForTexture((LPDIRECT3DTEXTURE9)pTuningTexBase, pDevice, tuningCRC)) {
					if (tuningCRC == crcFingerprintNumber || tuningCRC == crcFingerprintIcon) { pTuningTexBase->Release(); return REMOVE_TEXTURE; }
				}
				pTuningTexBase->Release();
			}
		}
	}

	// Change Noteway Color | This NEEDS to be above Extended Range / Custom Colors or it won't work.
	if (IsToBeRemoved(noteHighway, current) && Settings::ReturnSettingValue("CustomHighwayColors") == (std::string)"on") {
		pDevice->GetTexture(1, &pBaseNotewayTexture);
		pCurrNotewayTexture = (IDirect3DTexture9*)pBaseNotewayTexture;

		if (pBaseNotewayTexture) {
			if (D3D::CRCForTexture(pCurrNotewayTexture, pDevice, crc)) {

				// Noteway Texture
				if (crc == crcNoteLanes && Settings::ReturnNotewayColor("CustomHighwayNumbered") != (std::string)"" && Settings::ReturnNotewayColor("CustomHighwayUnNumbered") != (std::string)"")
					pDevice->SetTexture(1, notewayTexture);

				// Fret Number texture
				else if (crc == crcNotewayFretNumbers && Settings::ReturnNotewayColor("CustomFretNubmers") != (std::string)"")
					pDevice->SetTexture(1, fretNumTexture);

				// Gutter texture
				else if (crc == crcNotewayGutters && Settings::ReturnNotewayColor("CustomHighwayGutter") != (std::string)"")
					pDevice->SetTexture(1, gutterTexture);
			}
		}
	}

	//if (IsExtraRemoved(chordPanel, currentThicc))
	//{
	//	pDevice->GetTexture(0, &pBaseChordPanelTexture);
	//	pCurrentChordPanelTexture = (IDirect3DTexture9*)pBaseChordPanelTexture;

	//	if (pBaseChordPanelTexture)
	//	{
	//		if (D3D::CRCForTexture(pCurrentChordPanelTexture, pDevice, crc))
	//		{
	//			if (crc == crcChordPanelFHM1 || crc == crcChordPanelFHM2 || crc == crcChordPanelFHM3)
	//			{
	//				pDevice->SetTexture(0, customChordPanelFHMTexture);
	//				return SHOW_TEXTURE;
	//			}
	//			//else
	//			//{
	//			//	_LOG("Chord panel texture CRC: 0x" << std::hex << crc << std::endl);
	//			//}
	//		}
	//	}
	//}


	// // Custom Loft Gameplay Wall / Narnia / Portal / Venue wall
	//if (IsExtraRemoved(greenScreenWallMesh, currentThicc)) {
	//		//// Save Loft Texture To File
	//	//DumpTextureStages(pDevice, "greenscreenwall");

	//	// Use Custom Texture (File names can be found in venues/loft01.psarc/assets/generic/env/the_loft/
	//	// Files sent in currently require the name "stage#.png" where # is the number attached to the texture variable. Ex: customGreenScreenWall_Stage3 would need a file named "stage3.png"

	//	// Background tile displays as follows:
	//	// Top 512 of Background Tile are shown at the bottom of the screen, repeated 1-1/2 times.
	//	// Bottom 512 of Background Tile are shown at the top of the screen, repeated 2-1/2 times.

	//	// Example: What is shown - https://cdn.discordapp.com/attachments/711634485388771439/813523398587711488/unknown.png vs What is sent - https://cdn.discordapp.com/attachments/711634485388771439/813523420947152916/stage0.png
	//	// Example: Full wall (which you will never see for more than a second or so) - https://cdn.discordapp.com/attachments/711634485388771439/813524110390460436/unknown.png

	//	pDevice->SetTexture(0, customGreenScreenWall_Stage0);   // Background Tile | loft_concrete_wall_b.dds | 1024x1024 | Can be modified. Used for the background.
	//	//pDevice->SetTexture(1, customGreenScreenWall_Stage1); // Noise | noise03.dds | 256x256 | Doesn't have any effect
	//	//pDevice->SetTexture(2, customGreenScreenWall_Stage2); // Caustic (Indirect) | caustic_indirect01.dds | 256x256 | Doesn't have any effect
	//	//pDevice->SetTexture(3, customGreenScreenWall_Stage3); // Narnia / Venue Fade In Mask | fade_shape.dds | 512x512 | Can be modified. If you use a single colored square, you can make an almost "movie like" flashback.
	//	//pDevice->SetTexture(4, customGreenScreenWall_Stage4); // White square (Unknown) | 1024x1024 | Doesn't have any effect
	//	//pDevice->SetTexture(5, customGreenScreenWall_Stage5); // Pipes and wall trim | portal_wall_ao.dds | 1024x1024 | Can be modified.
	//	//pDevice->SetTexture(6, customGreenScreenWall_Stage6); // N Mask of Background tile | loft_concrete_wall_b_n.dds | 1024x1024 | Don't modify
	//}

	// Rainbow Notes | This part NEEDS to be above Extended Range / Custom Colors or it won't work.
	if (ERMode::RainbowNotesEnabled && ERMode::customNoteColorH > 0) { 

		if (ERMode::customNoteColorH > 179)
			ERMode::customNoteColorH -= 180;

		RainbowNotes = true;

		// Colors for note stems (part below the note), bends, slides, and accents
		if (NOTE_STEMS || OPEN_NOTE_ACCENTS) {
			pDevice->GetTexture(1, &pBaseRainbowTexture);
			pCurrRainbowTexture = (IDirect3DTexture9*)pBaseRainbowTexture;

			if (!pBaseRainbowTexture)
				return SHOW_TEXTURE;

			if (D3D::CRCForTexture(pCurrRainbowTexture, pDevice, crc)) {

				// Same checksum for stems and accents, because they use the same texture. Bends and slides use the same texture.
				if (crc == crcStemsAccents || crc == crcBendSlideIndicators)
					pDevice->SetTexture(1, rainbowTextures[ERMode::customNoteColorH]);
			}
		}

		// As of right now, this requires rainbow strings to be toggled on
		if (PrideMode && NOTE_TAILS) 
			pDevice->SetTexture(1, rainbowTextures[ERMode::customNoteColorH]);
	}

	// User has updated their settings, and we need to recreate our textures
	if (RecreateTextures && RecreateTextureTimer) {
		// Generate textures to be called later
		D3D::GenerateTextures(pDevice, D3D::Random_Solid);
		D3D::GenerateTextures(pDevice, D3D::Strings);
		D3D::GenerateTextures(pDevice, D3D::Notes);
		D3D::GenerateTextures(pDevice, D3D::Noteway);
		D3D::GenerateTextures(pDevice, D3D::Gutter);
		D3D::GenerateTextures(pDevice, D3D::FretNums);
		D3D::GenerateTextures(pDevice, D3D::Rainbow);

		RecreateTextures = false;
		RecreateTextureTimer = false;
	}
		
	//if (Settings::ReturnSettingValue("DiscoModeEnabled") == "on") {
	//	 //Need Lovro's Help With This :(
	//	if (DiscoModeInitialSetting.find(pDevice) == DiscoModeInitialSetting.end()) { // We haven't saved this pDevice's initial values yet
	//		DWORD initialAlphaValue = (DWORD)pDevice, initialSeparateValue = (DWORD)pDevice;
	//		pDevice->GetRenderState(D3DRS_ALPHABLENDENABLE, (DWORD*)initialAlphaValue);
	//		pDevice->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, (DWORD*)initialSeparateValue);
	//		
	//		DiscoModeInitialSetting.insert({ pDevice, std::make_pair(initialAlphaValue, initialSeparateValue) });
	//	}
	//	else { // We've seen this pDevice value before.
	//		if (DiscoModeEnabled) { // Key was pressed to have Disco Mode on
	//			pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); // Make AMPS Semi-Transparent <- Is the one that makes things glitchy.
	//			pDevice->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE); // Sticky Colors
	//		}

	//		else { // Disco mode was turned off, we need to revert the settings so there is no trace of disco mode.
	//			for (auto pDeviceList : DiscoModeInitialSetting) {
	//				pDeviceList.first->SetRenderState(D3DRS_ALPHABLENDENABLE, *(DWORD*)pDeviceList.second.first); // Needs to have *(DWORD*) since it only sets DWORD not DWORD*
	//				pDeviceList.first->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, *(DWORD*)pDeviceList.second.second); // Needs to have *(DWORD*) since it only sets DWORD not DWORD*
	//			}
	//		}
	//	}
	//}

	/*if (IsExtraRemoved(lyrics, currentThicc)) { // Move Lyrics to different file. Current State: CRC never updates, BUT does pass CRCForTexture().
		pDevice->GetTexture(0, &pBaseTexture);

		D3DXSaveTextureToFile(L"lyrics_temp.png", D3DXIFF_PNG, pBaseTexture, NULL);

		D3DXCreateTextureFromFile(pDevice, L"lyrics_temp.png", &pCurrTexture);

		if (CRCForTexture(pCurrTexture, pDevice, crc)) {
			if (crc != crcLyrics) {
				D3DXSaveTextureToFile(L"lyrics.png", D3DXIFF_PNG, pBaseTexture, NULL);
				crcLyrics = crc;
				_LOG("new lyric posted to lyrics.png" << std::endl);
			}
			_LOG(std::hex << crcLyrics << " = " << std::hex << crc << std::endl);
		}
	}*/

	// Extended Range / Custom Colors (includes separate note colors)
	if (ERMode::AttemptedERInThisSong && ERMode::UseEROrColorsInThisSong) {
		GameState::ToggleCB(ERMode::UseERExclusivelyInThisSong);

		// Settings::GetModSetting("SeparateNoteColors") == 1 -> Default Colors, so don't do anything.

		// Use same color scheme on notes as we do on strings (0) || Use Custom Note Color Scheme (2)
		if (Settings::GetModSetting("SeparateNoteColorsMode") == 0 || (Settings::ReturnSettingValue("SeparateNoteColors") == "on" && Settings::GetModSetting("SeparateNoteColorsMode") == 2)) { 

			// Color notes like string colors
			LPDIRECT3DTEXTURE9 textureToUseOnNotes = customStringColorTexture; 

			// Custom colored notes
			if (Settings::GetModSetting("SeparateNoteColorsMode") == 2)
				textureToUseOnNotes = customNoteColorTexture; 

			// Change all pieces of note head's textures
			if (IsToBeRemoved(sevenstring, current) || IsExtraRemoved(noteModifiers, currentThicc))  
				pDevice->SetTexture(1, textureToUseOnNotes);

			// Colors for note stems (part below the note), bends, slides, and accents
			else if (NOTE_STEMS || OPEN_NOTE_ACCENTS) { 
				pDevice->GetTexture(1, &pBaseTexture);
				pCurrTexture = (IDirect3DTexture9*)pBaseTexture;

				if (!pBaseTexture)
					return SHOW_TEXTURE;

				if (D3D::CRCForTexture(pCurrTexture, pDevice, crc)) {

					// Same checksum for stems and accents, because they use the same texture. Bends and slides use the same texture.
					if (crc == crcStemsAccents || crc == crcBendSlideIndicators)  
						pDevice->SetTexture(1, textureToUseOnNotes);
				}

				return SHOW_TEXTURE;
			}
		}
	}

	// Twitch wants notes to be removed.
	if (Settings::IsTwitchSettingEnabled("RemoveNotes"))
		// Note textures, outside of note stems and open note accents.
		if (IsToBeRemoved(sevenstring, current) || IsExtraRemoved(noteModifiers, currentThicc))
			return REMOVE_TEXTURE;

		// Colors for note stems (part below the note), bends, slides, and accents
		else if (NOTE_STEMS || OPEN_NOTE_ACCENTS) { 
			pDevice->GetTexture(1, &pBaseTexture);
			pCurrTexture = (IDirect3DTexture9*)pBaseTexture;

			if (!pBaseTexture)
				return REMOVE_TEXTURE;

			if (D3D::CRCForTexture(pCurrTexture, pDevice, crc)) {

				// Same checksum for stems and accents, because they use the same texture. Bends and slides use the same texture.
				if (crc == crcStemsAccents || crc == crcBendSlideIndicators)  
					return REMOVE_TEXTURE;
			}

			return REMOVE_TEXTURE;
		}

	// Twitch wants transparent notes.
	if (Settings::IsTwitchSettingEnabled("TransparentNotes"))
		// Note textures, outside of note stems and open note accents.
		if (IsToBeRemoved(sevenstring, current) || IsExtraRemoved(noteModifiers, currentThicc) || NOTE_STEMS || OPEN_NOTE_ACCENTS)
			pDevice->SetTexture(1, nonexistentTexture);

		// Colors for note stems (part below the note), bends, slides, and accents
		else if (NOTE_STEMS || OPEN_NOTE_ACCENTS) { 
			pDevice->GetTexture(1, &pBaseTexture);
			pCurrTexture = (IDirect3DTexture9*)pBaseTexture;

			if (!pBaseTexture)
				return SHOW_TEXTURE;

			if (D3D::CRCForTexture(pCurrTexture, pDevice, crc)) {

				// Same checksum for stems and accents, because they use the same texture. Bends and slides use the same texture.
				if (crc == crcStemsAccents || crc == crcBendSlideIndicators)  
					pDevice->SetTexture(1, nonexistentTexture);
			}

			return SHOW_TEXTURE;
		}

	// Twitch wants solid note colors
	if (Settings::IsTwitchSettingEnabled("SolidNotes")) {
		// Note textures, outside of note stems and open note accents.
		if (IsToBeRemoved(sevenstring, current) || IsExtraRemoved(noteModifiers, currentThicc)) {

			// Random Colors
			if (Settings::ReturnSettingValue("SolidNoteColor") == "random") 
				pDevice->SetTexture(1, randomTextures[currentRandomTexture]);
			// They set the color they want in the GUI | TODO: Colors are changed on chord boxes
			else 
				pDevice->SetTexture(1, twitchUserDefinedTexture);
		}

		// Colors for note stems (part below the note), bends, slides, and accents
		else if (NOTE_STEMS || OPEN_NOTE_ACCENTS) { 
			pDevice->GetTexture(1, &pBaseTexture);
			pCurrTexture = (IDirect3DTexture9*)pBaseTexture;

			if (!pBaseTexture)
				return SHOW_TEXTURE;

			if (D3D::CRCForTexture(pCurrTexture, pDevice, crc)) {

				// Same checksum for stems and accents, because they use the same texture. Bends and slides use the same texture.
				if (crc == crcStemsAccents || crc == crcBendSlideIndicators) {  

					// Random Colors
					if (Settings::ReturnSettingValue("SolidNoteColor") == "random") 
						pDevice->SetTexture(1, randomTextures[currentRandomTexture]);
					else
						pDevice->SetTexture(1, twitchUserDefinedTexture);
				}
			}

			return SHOW_TEXTURE;
		}
	}

	// Twitch wants us to reset your note streak.
	if (Settings::IsTwitchSettingEnabled("FYourFC")) {
		uintptr_t currentNoteStreak = 0;

		// Runs on every draw call while the effect is on, so the links are not checked one by one (each check is a
		// VirtualQuery call); the guards turn a link freed as the song ends into a skipped write.
		if (GameState::Menus::IsInLearnASongModes())
			currentNoteStreak = MemUtil::FindDMAAddyGuarded(Offsets::baseHandle + Offsets::ptr_currentNoteStreak, Offsets::ptr_currentNoteStreakLASOffsets, false);
		else if (GameState::Menus::IsInScoreAttackModes())
			currentNoteStreak = MemUtil::FindDMAAddyGuarded(Offsets::baseHandle + Offsets::ptr_currentNoteStreak, Offsets::ptr_currentNoteStreakSAOffsets, false);

		MemUtil::TryWrite<BYTE>(currentNoteStreak, 0);
	}

	// Twitch wants to see the user play in Drunk Mode.
	if (Settings::IsTwitchSettingEnabled("DrunkMode")) {
		std::uniform_real_distribution<> keepValueWithin(-1.5, 1.5);
		MemUtil::SetStaticValue(Offsets::ptr_drunkShit.Get(), (float)keepValueWithin(rng), sizeof(float));
	}

	// Greenscreen Wall
	if ((Settings::ReturnSettingValue("GreenScreenWallEnabled") == "on" || GreenScreenWall) && IsExtraRemoved(greenScreenWallMesh, currentThicc))
		return REMOVE_TEXTURE;

	// Thicc Mesh Mods that are as simple as doing a simple check against the params of this function.
	if (GameState::IsInSong()) {
		if (Settings::ReturnSettingValue("FretlessModeEnabled") == "on" && IsExtraRemoved(fretless, currentThicc))
			return REMOVE_TEXTURE;
		if (Settings::ReturnSettingValue("RemoveInlaysEnabled") == "on" && IsExtraRemoved(inlays, currentThicc))
			return REMOVE_TEXTURE;
		if (Settings::ReturnSettingValue("RemoveLaneMarkersEnabled") == "on" && IsExtraRemoved(laneMarkers, currentThicc))
			return REMOVE_TEXTURE;
		if (RemoveLyrics && Settings::ReturnSettingValue("RemoveLyrics") == "on" && IsExtraRemoved(lyrics, currentThicc))
			return REMOVE_TEXTURE;
	}

	// Remove Headstock Artifacts
	else if (GameState::Menus::IsInTuningMenus() && Settings::ReturnSettingValue("RemoveHeadstockEnabled") == "on" && RemoveHeadstockInThisMenu)
	{
		// This is called to remove those pesky tuning letters that share the same texture values as fret numbers and chord fingerings
		if (IsExtraRemoved(tuningLetters, currentThicc)) 
			return REMOVE_TEXTURE;

		// This is called to remove the tuner's highlights
		if (IsExtraRemoved(tunerHighlight, currentThicc))
			return REMOVE_TEXTURE;

		// Lefties need their own little place in life...
		if (IsExtraRemoved(leftyFix, currentThicc)) 
			return REMOVE_TEXTURE;
	}

	// Skyline Removal
	if (toggleSkyline && POSSIBLE_SKYLINE) {

		// If the user is in "Song" mode for Toggle Skyline and is NOT in a song -> draw the UI.
		// This means we show the skyline in the learn a song - Song Details page.
		if (DrawSkylineInMenu) { 
			SkylineOff = false;
			return SHOW_TEXTURE;
		}

		pDevice->GetTexture(1, &pBaseTextures[1]);
		pCurrTextures[1] = (IDirect3DTexture9*)pBaseTextures[1];

		// There's only two textures in Stage 1 for meshes with Stride = 16, so we could as well skip CRC calcuation and just check if !pBaseTextures[1] and return REMOVE_TEXTURE directly
		if (pBaseTextures[1]) {  
			if (D3D::CRCForTexture(pCurrTextures[1], pDevice, crc)) {

				// Purple rectangles + orange line beneath them
				if (crc == crcSkylinePurple || crc == crcSkylineOrange) { 
					SkylineOff = true;
					return REMOVE_TEXTURE;
				}
			}
		}

		pDevice->GetTexture(0, &pBaseTextures[0]);
		pCurrTextures[0] = (IDirect3DTexture9*)pBaseTextures[0];

		if (pBaseTextures[0]) {
			if (D3D::CRCForTexture(pCurrTextures[0], pDevice, crc)) {

				// There's a few more of textures used in Stage 0, so doing the same is no-go; Shadow-ish thing in the background + backgrounds of rectangles.
				if (crc == crcSkylineBackground || crc == crcSkylineShadow) {  
					SkylineOff = true;
					return REMOVE_TEXTURE;
				}
			}
		}
	}

	// Headstock Removal
	else if (Settings::ReturnSettingValue("RemoveHeadstockEnabled") == "on") {
		if (POSSIBLE_HEADSTOCKS) { // If we call GetTexture without any filtering, it causes a lockup when ALT-TAB-ing/changing fullscreen to windowed and vice versa
			if (!RemoveHeadstockInThisMenu) // This user has RemoveHeadstock only on during the song. So if we aren't in the song, we need to draw the headstock texture.
				return SHOW_TEXTURE;

			pDevice->GetTexture(1, &pBaseTextures[1]);
			pCurrTextures[1] = (IDirect3DTexture9*)pBaseTextures[1];

			// Need to reset cache, and this is a headstock texture.
			if (resetHeadstockCache && IsExtraRemoved(headstockThicc, currentThicc)) {
				if (!pBaseTextures[1]) //if there's no texture for Stage 1
					return REMOVE_TEXTURE;

				// Take a CRC of the texture, and check it against our preset CRCs.
				if (D3D::CRCForTexture(pCurrTextures[1], pDevice, crc)) {
					if (crc == crcHeadstock0 || crc == crcHeadstock1 || crc == crcHeadstock2 || crc == crcHeadstock3 || crc == crcHeadstock4)
						AddToTextureList(headstockTexturePointers, pCurrTextures[1]);
				}

				int headstockCRCLimit = 3;

				// If the user is in multiplayer, we have to make sure our CRC limit is double or some bugs appear.
				if (GameState::Menus::IsInMultiplayerTunerMenus())
					headstockCRCLimit = 6;

				// We've calculated all CRCs that we can, within our limit.
				if (headstockTexturePointers.size() == headstockCRCLimit) {
					calculatedHeadstocks = true;
					resetHeadstockCache = false;
				}

				return REMOVE_TEXTURE;
			}

			// We've already cached the headstocks we're using, so find the one we are working with and remove it.
			if (calculatedHeadstocks)
				if (std::find(std::begin(headstockTexturePointers), std::end(headstockTexturePointers), pCurrTextures[1]) != std::end(headstockTexturePointers))
					return REMOVE_TEXTURE;
		}
	}

	// Rainbow Notes || This part NEEDS to be below Extended Range / Custom Colors or it won't work.
	if (RainbowNotes) { 

		// Rainbow Note Heads
		if (IsToBeRemoved(sevenstring, current) || IsExtraRemoved(noteModifiers, currentThicc)) 
			pDevice->SetTexture(1, rainbowTextures[ERMode::customNoteColorH]);

		RainbowNotes = false;
	}

	
	return SHOW_TEXTURE; // KEEP THIS LINE. This translates to "Display Graphics".
}

std::string D3DHooks::ConvertFloatTimeToStringTime(float timeInSeconds) 
{
	int seconds = 0, minutes = 0, hours = 0;

	seconds = (int)timeInSeconds % 60;
	minutes = (int)(timeInSeconds / 60) % 60;
	hours = timeInSeconds / 3600;

	char buffer[64];

	if (hours > 0)
	{
		sprintf_s(buffer, "%02dh:%02dm:%02ds", hours, minutes, seconds);
	}
	else
	{
		sprintf_s(buffer, "%02dm:%02ds", minutes, seconds);
	}

	return std::string(buffer);
}

void D3DHooks::RegenerateTwitchNoteColors(IDirect3DDevice9* pDevice) {
	if (regenerateUserDefinedTexture) {
		RSColor userDefColor = Settings::ConvertHexToColor(Settings::ReturnSettingValue("SolidNoteColor"));

		ColorList customColorList(16, userDefColor);
		D3D::GenerateTexture(pDevice, &twitchUserDefinedTexture, customColorList);

		ERMode::customSolidColor.clear();
		for (int str = 0; str < 6;str++)
			ERMode::customSolidColor.push_back(userDefColor);

		regenerateUserDefinedTexture = false;
	}
}
