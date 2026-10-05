#include "../../Mods/ResearchProbeRuntime.hpp"
#include "../../Mods/NoteByNoteScoringCore.hpp"

#include "../../Mods/NoteByNoteNativeScoring.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	constexpr char PROBE_NAME[] = "RSModsPlus Note by Note Controller";
	// __TIME__ is baked per translation unit: this stamp only moves when THIS file
	// recompiles, so a probe rebuilt from changes elsewhere (NoteByNoteNativeScoring.cpp)
	// still reports the old id. It does not prove probe freshness.
	const std::string PROBE_BUILD_ID = std::string(__DATE__) + " " + __TIME__;

	// Render snapshot, hot-reloadable half. The host forwards every note-head draw it filters
	// at DrawIndexedPrimitive and a per-frame boundary tick; this owns arm, the two-frame
	// capture window, and the log, so changing what is captured needs only a probe reload.
	//
	// caller is _ReturnAddress() from inside the host's D3D hook: the native function that
	// issued the draw.
	namespace RenderSnapshot
	{
		constexpr unsigned int CAPTURE_FRAME_COUNT = 2;
		constexpr unsigned int CAPTURE_DRAW_LIMIT = 256;
		constexpr unsigned int INSTANCE_LIMIT = 256;
		constexpr unsigned int TRANSFORM_STRIDE = 48;
		constexpr unsigned int GRADIENT_STRIDE = 16;
		constexpr unsigned int FREQUENCY_VALUE_MASK = 0x3fffffff;
		constexpr char CAPTURE_PATH[] = "RSModsResearch\\NoteByNoteRenderSnapshot.txt";

		std::mutex mutex;
		std::atomic<bool> armed{ false };
		bool active = false;
		bool hasDrawInFrame = false;
		unsigned int capturedFrameCount = 0;
		unsigned int frameDrawCount = 0;
		unsigned int totalDrawCount = 0;
		std::ostringstream captureOutput;

		std::string DescribeTransform(const ResearchProtocol::NativeDrawObservation& draw)
		{
			float transform[4][4] = {};
			bool hasTransform = false;
			if (draw.devicePointer != 0)
			{
				auto* device = reinterpret_cast<IDirect3DDevice9*>(draw.devicePointer);
				hasTransform = SUCCEEDED(device->GetVertexShaderConstantF(
					0, &transform[0][0], 4));
			}
			if (!hasTransform && draw.shaderConstantShadow != 0)
			{
				std::memcpy(transform,
					reinterpret_cast<const float*>(draw.shaderConstantShadow),
					sizeof(transform));
				hasTransform = true;
			}

			if (!hasTransform) return "unavailable";

			std::ostringstream description;
			description << std::fixed << std::setprecision(6);
			for (size_t row = 0; row < 4; ++row)
			{
				if (row != 0) description << ',';
				description << '(' << transform[row][0] << ',' << transform[row][1]
					<< ',' << transform[row][2] << ',' << transform[row][3] << ')';
			}
			return description.str();
		}

		bool ReadInstanceStream(
			IDirect3DDevice9* device,
			unsigned int streamIndex,
			unsigned int recordCount,
			unsigned int expectedStride,
			std::vector<float>& values)
		{
			IDirect3DVertexBuffer9* buffer = nullptr;
			unsigned int offset = 0;
			unsigned int stride = 0;
			if (FAILED(device->GetStreamSource(streamIndex, &buffer, &offset, &stride))
				|| buffer == nullptr)
			{
				return false;
			}

			const unsigned int byteCount = recordCount * expectedStride;
			D3DVERTEXBUFFER_DESC description = {};
			if (stride != expectedStride
				|| FAILED(buffer->GetDesc(&description))
				|| offset > description.Size
				|| byteCount > description.Size - offset)
			{
				buffer->Release();
				return false;
			}

			void* data = nullptr;
			if (FAILED(buffer->Lock(offset, byteCount, &data, D3DLOCK_READONLY)) || data == nullptr)
			{
				buffer->Release();
				return false;
			}

			values.resize(byteCount / sizeof(float));
			std::memcpy(values.data(), data, byteCount);
			buffer->Unlock();
			buffer->Release();
			return true;
		}

		std::string DescribeInstances(const ResearchProtocol::NativeDrawObservation& draw)
		{
			if (draw.devicePointer == 0) return "unavailable";

			auto* device = reinterpret_cast<IDirect3DDevice9*>(draw.devicePointer);
			unsigned int frequency = 0;
			if (FAILED(device->GetStreamSourceFreq(0, &frequency))
				|| (frequency & D3DSTREAMSOURCE_INDEXEDDATA) == 0)
			{
				return "not-instanced";
			}

			const unsigned int count = frequency & FREQUENCY_VALUE_MASK;
			std::vector<float> transforms;
			std::vector<float> gradients;
			if (count == 0 || count > INSTANCE_LIMIT
				|| !ReadInstanceStream(device, 1, count, TRANSFORM_STRIDE, transforms)
				|| !ReadInstanceStream(device, 2, count, GRADIENT_STRIDE, gradients))
			{
				return "count=" + std::to_string(count) + ",unreadable";
			}

			std::ostringstream description;
			description << "count=" << count << ",values=[" << std::fixed << std::setprecision(4);
			for (unsigned int index = 0; index < count; ++index)
			{
				if (index != 0) description << ';';
				const size_t transformOffset = static_cast<size_t>(index) * 12;
				const size_t gradientOffset = static_cast<size_t>(index) * 4;
				const int stringIndex = static_cast<int>(std::lround(
					(gradients[gradientOffset] * 32.0f - 1.0f) / 2.0f));
				description << index << "={string=" << stringIndex
					<< ",longitudinal=" << transforms[transformOffset + 3]
					<< ",fretCoordinate=" << transforms[transformOffset + 7]
					<< ",height=" << transforms[transformOffset + 11]
					<< ",gradient=(" << gradients[gradientOffset] << ','
					<< gradients[gradientOffset + 1] << ','
					<< gradients[gradientOffset + 2] << ','
					<< gradients[gradientOffset + 3] << ")}";
			}
			description << ']';
			return description.str();
		}

		bool SaveCapture()
		{
			std::ofstream output(CAPTURE_PATH, std::ios::binary | std::ios::trunc);
			if (!output.is_open()) return false;

			output << captureOutput.str();
			output.flush();
			return output.good();
		}

		void Arm()
		{
			std::lock_guard<std::mutex> lock(mutex);
			active = false;
			hasDrawInFrame = false;
			capturedFrameCount = 0;
			frameDrawCount = 0;
			totalDrawCount = 0;
			captureOutput.str("");
			captureOutput.clear();
			captureOutput << "probeBuild=" << PROBE_BUILD_ID << '\n';
			armed.store(true, std::memory_order_release);
			LOG_INFO("(NBN NATIVE RENDER) Armed for the next " << CAPTURE_FRAME_COUNT
				<< " complete native note-head frames." << std::endl);
		}

		void ObserveDraw(const ResearchProtocol::NativeDrawObservation* draw)
		{
			if (draw == nullptr || !armed.load(std::memory_order_acquire)) return;

			std::lock_guard<std::mutex> lock(mutex);
			if (!active || totalDrawCount >= CAPTURE_DRAW_LIMIT) return;

			hasDrawInFrame = true;
			++frameDrawCount;
			++totalDrawCount;

			captureOutput << "draw frame=" << draw->renderFrame
				<< " ordinal=" << frameDrawCount
				<< " totalOrdinal=" << totalDrawCount
				<< " songTime=" << std::fixed << std::setprecision(5) << draw->songTime
				<< " greyNoteCutoff=" << draw->greyNoteCutoff
				<< " caller=0x" << std::hex << draw->nativeCaller
				<< " stream=0x" << draw->streamIdentity << std::dec
				<< " mesh={stride=" << draw->stride
				<< ",primitiveType=" << draw->primitiveType
				<< ",baseVertexIndex=" << draw->baseVertexIndex
				<< ",minimumVertexIndex=" << draw->minimumVertexIndex
				<< ",vertexCount=" << draw->vertexCount
				<< ",startIndex=" << draw->startIndex
				<< ",primitiveCount=" << draw->primitiveCount
				<< ",startRegister=" << draw->startRegister
				<< ",vectorCount=" << draw->vectorCount
				<< ",declarationType=" << draw->declarationType
				<< ",declarationElements=" << draw->declarationElementCount
				<< "} transform={" << DescribeTransform(*draw)
				<< "} instances={" << DescribeInstances(*draw) << "}\n";
		}

		void NotifyFrameComplete(uint64_t renderFrame)
		{
			if (!armed.load(std::memory_order_acquire)) return;

			std::lock_guard<std::mutex> lock(mutex);
			if (!active)
			{
				active = true;
				LOG_INFO("(NBN NATIVE RENDER) Capture starts at native frame="
					<< renderFrame + 1 << "." << std::endl);
				return;
			}

			if (!hasDrawInFrame) return;

			captureOutput << "frame-complete frame=" << renderFrame
				<< " noteHeadDraws=" << frameDrawCount
				<< " totalDraws=" << totalDrawCount << '\n';
			hasDrawInFrame = false;
			frameDrawCount = 0;
			++capturedFrameCount;

			if (capturedFrameCount < CAPTURE_FRAME_COUNT
				&& totalDrawCount < CAPTURE_DRAW_LIMIT)
			{
				return;
			}

			active = false;
			armed.store(false, std::memory_order_release);
			captureOutput << "capture-complete frames=" << capturedFrameCount
				<< " noteHeadDraws=" << totalDrawCount << '\n';
			const bool saved = SaveCapture();
			LOG_INFO("(NBN NATIVE RENDER) Capture complete. frames=" << capturedFrameCount
				<< " noteHeadDraws=" << totalDrawCount << " saved=" << std::boolalpha
				<< saved << " path=" << CAPTURE_PATH << "." << std::endl);
		}
	}

	// Declared ahead of the lifecycle exports: stop and shutdown must put any
	// persistently relocated marker records back before the probe goes away.
	namespace StoppedPreviewFilter
	{
		void RestorePersistent(const char* reason);
	}

	uint8_t __cdecl InitializeProbe()
	{
		NoteByNoteScoringCore::Initialize();
		return NoteByNoteScoringCore::IsAvailable() ? 1 : 0;
	}

	void __cdecl ShutdownProbe()
	{
		StoppedPreviewFilter::RestorePersistent("probe-shutdown");
		NoteByNoteNativeScoring::Shutdown();
		ResearchProbeRuntime::Shutdown();
	}

	void __stdcall ProcessScoringUpdate(
		void* owner,
		float updateTime,
		ResearchProtocol::ScoringUpdate original)
	{
		NoteByNoteNativeScoring::ProcessScoringUpdate(owner, updateTime, original);
	}

	bool __fastcall ProcessHitDecision(
		void* owner,
		void* unusedEdx,
		void* note,
		ResearchProtocol::HitDecision original)
	{
		return NoteByNoteNativeScoring::ProcessHitDecision(owner, unusedEdx, note, original);
	}

	void __cdecl ObserveRenderedAttack(const ResearchProtocol::RenderedAttack* source)
	{
		if (source == nullptr) return;

		NoteByNoteProbe::NativeRenderedAttack attack;
		attack.isTransition = source->isTransition != 0;
		attack.renderFrame = source->renderFrame;
		attack.songTime = source->songTime;
		attack.longitudinalPosition = source->longitudinalPosition;
		attack.notes.reserve(source->noteCount);
		for (size_t index = 0; index < source->noteCount; ++index)
		{
			attack.notes.push_back({ source->notes[index].stringIndex, source->notes[index].fret });
		}
		NoteByNoteScoringCore::ObserveRenderedAttack(attack);
	}

	void __cdecl StopProbe()
	{
		StoppedPreviewFilter::RestorePersistent("probe-stop");
		NoteByNoteScoringCore::Stop();
	}

	void __cdecl RequestReArmProbe()
	{
		NoteByNoteScoringCore::RequestReArm();
	}

	uint8_t __cdecl GetState(ResearchProtocol::NoteByNoteState* state)
	{
		if (state == nullptr) return 0;

		const auto callerSize = state->structSize;
		if (callerSize != sizeof(ResearchProtocol::NoteByNoteState))
		{
			static std::atomic<bool> hasLoggedStateSizeMismatch{ false };
			if (!hasLoggedStateSizeMismatch.exchange(true))
			{
				std::ostringstream message;
				message << "(NBN CONTROLLER) State ABI mismatch: caller provides "
					<< callerSize << " bytes, controller requires "
					<< sizeof(ResearchProtocol::NoteByNoteState)
					<< ". Rebuild and deploy the host and controller together.";
				ResearchProbeRuntime::Log(ResearchProtocol::LogLevel::Error, message.str());
			}
			return 0;
		}

		*state = NoteByNoteNativeScoring::GetResearchState();
		return 1;
	}

	// Defined below, after NoteListSnapshot, whose readers they share; the D3D-level
	// observation wrappers next need to reach them.
	namespace ScreenMapSnapshot
	{
		void ObserveDraw(const ResearchProtocol::NativeDrawObservation* draw);
	}
	namespace StoppedPreviewFilter
	{
		void RestorePending();
		bool HideNonTarget(void* renderCtx, void* noteArray);
	}
	// Defined after StoppedPreviewFilter, whose fret/string tables its decode annotations
	// reuse; the frame-boundary wrapper above it needs to reach it.
	namespace TransitionCapture
	{
		void ObserveDraw(const ResearchProtocol::NativeDrawObservation* draw);
		void NotifyFrameComplete(uint64_t renderFrame);
	}

	void __cdecl ArmRenderSnapshot()
	{
		RenderSnapshot::Arm();
	}

	void __cdecl ObserveNativeDraw(const ResearchProtocol::NativeDrawObservation* draw)
	{
		RenderSnapshot::ObserveDraw(draw);
		ScreenMapSnapshot::ObserveDraw(draw);
	}

	void __cdecl NotifyRenderFrameComplete(uint64_t renderFrame)
	{
		RenderSnapshot::NotifyFrameComplete(renderFrame);
		TransitionCapture::NotifyFrameComplete(renderFrame);
	}

	// Native note-list snapshot. One-shot: the next FUN_00BEBCD0 passes after arming dump their
	// note arrays, reading each note record in-process (onset-time field, and the transform
	// block at note+0x80 that separates neck-placed notes from highway read-ahead). Read-only.
	namespace NoteListSnapshot
	{
		// FUN_00BEBCD0 fires several times per frame with different note arrays, so one arm
		// captures a run of passes to tell the neck-marker pass from the highway pass.
		constexpr int PASS_LIMIT = 140;

		std::atomic<int> passesRemaining{ 0 };
		std::atomic<uint64_t> passOrdinal{ 0 };
		std::atomic<bool> imageDumped{ false };

		// Full-image dump: walk this process's own committed pages with VirtualQuery and copy
		// the readable ones (zero-filling gaps) into a flat file mapped 1:1 to VA, so file
		// offset i is VA 0x400000+i. No cross-process handle or ReadProcessMemory, which avoids
		// the memory-scraper pattern antivirus flags. Runs once per probe load.
		void DumpProcessImage(const char* path)
		{
			const uintptr_t base = 0x400000;
			const uintptr_t endAddr = base + 0x1F7A000;
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				LOG_ERROR("(NBN DUMP) could not open " << path << " for writing." << std::endl);
				return;
			}

			static char zero[0x1000] = { 0 };
			const DWORD readMask = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ
				| PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
			uintptr_t addr = base;
			size_t total = 0;
			size_t live = 0;
			while (addr < endAddr)
			{
				MEMORY_BASIC_INFORMATION mbi;
				if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) break;
				uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
				if (regionEnd > endAddr) regionEnd = endAddr;
				const size_t chunk = regionEnd - addr;
				const bool readable = mbi.State == MEM_COMMIT
					&& (mbi.Protect & readMask) != 0
					&& (mbi.Protect & PAGE_GUARD) == 0;
				if (readable)
				{
					out.write(reinterpret_cast<const char*>(addr), chunk);
					live += chunk;
				}
				else
				{
					size_t rem = chunk;
					while (rem > 0)
					{
						const size_t c = rem < sizeof(zero) ? rem : sizeof(zero);
						out.write(zero, c);
						rem -= c;
					}
				}
				total += chunk;
				addr = regionEnd;
			}
			out.close();
			LOG_INFO("(NBN DUMP) wrote " << total << " bytes (" << live
				<< " live) mapped 1:1 from VA 0x400000 to " << path << "." << std::endl);
		}

		bool LooksReadable(uintptr_t p)
		{
			return p >= 0x10000 && p < 0x7FFF0000;
		}

		float ReadFloat(uintptr_t base, unsigned int offset)
		{
			return *reinterpret_cast<const float*>(base + offset);
		}

		int ReadInt(uintptr_t base, unsigned int offset)
		{
			return *reinterpret_cast<const int*>(base + offset);
		}

		void Arm()
		{
			// Developer aid: only when RSMODS_IMAGE_DUMP_PATH names the output file.
			char dumpPath[MAX_PATH] = {};
			const DWORD dumpPathLength = GetEnvironmentVariableA("RSMODS_IMAGE_DUMP_PATH", dumpPath, MAX_PATH);
			if (dumpPathLength > 0 && dumpPathLength < MAX_PATH && !imageDumped.exchange(true, std::memory_order_acq_rel))
			{
				DumpProcessImage(dumpPath);
			}
			passOrdinal.store(0, std::memory_order_release);
			passesRemaining.store(PASS_LIMIT, std::memory_order_release);
			LOG_INFO("(NBN NOTE LIST) Armed for the next " << PASS_LIMIT
				<< " note-head draw passes." << std::endl);
		}

		void Observe(void* renderCtx, void* noteArray)
		{
			if (passesRemaining.load(std::memory_order_acquire) <= 0) return;
			if (passesRemaining.fetch_sub(1, std::memory_order_acq_rel) <= 0) return;
			const uint64_t pass = passOrdinal.fetch_add(1, std::memory_order_acq_rel);

			const auto arr = reinterpret_cast<uintptr_t>(noteArray);
			if (!LooksReadable(arr))
			{
				LOG_INFO("(NBN NOTE LIST) pass=" << pass << " noteArray not readable: 0x"
					<< std::hex << arr << std::dec << "." << std::endl);
				return;
			}

			const auto base = static_cast<uintptr_t>(static_cast<unsigned int>(ReadInt(arr, 0)));
			const int count = ReadInt(arr, 4);
			if (!LooksReadable(base) || count <= 0 || count > 1024) return;

			// One line per drawn note: renderCtx (the surface), the note pointer, and the
			// position floats at note+0x2C..0x38 that FUN_00BEBCD0 feeds to the shader (the
			// identity matrix at +0x80 carries no position).
			const int limit = count < 8 ? count : 8;
			for (int i = 0; i < limit; ++i)
			{
				const auto note = static_cast<uintptr_t>(
					static_cast<unsigned int>(ReadInt(base, static_cast<unsigned int>(i) * 4)));
				if (!LooksReadable(note)) continue;

				LOG_INFO("(NBN NOTE LIST) ctx=0x" << std::hex
					<< reinterpret_cast<uintptr_t>(renderCtx)
					<< " note=0x" << note << std::dec << std::fixed << std::setprecision(3)
					<< " pos{2C=" << ReadFloat(note, 0x2C)
					<< ",30=" << ReadFloat(note, 0x30)
					<< ",34=" << ReadFloat(note, 0x34)
					<< ",38=" << ReadFloat(note, 0x38) << "}"
					<< " f0C=" << ReadFloat(note, 0x0C)
					<< " i08=0x" << std::hex << (unsigned)ReadInt(note, 0x08)
					<< " i14=0x" << (unsigned)ReadInt(note, 0x14) << std::dec << std::endl);
			}
		}
	}

	// Screen-map snapshot. The render note carries no fret, string or time, so identity comes
	// from position: this joins the note pointer seen at the FUN_00BEBCD0 detour with the
	// vertex-shader constants of the D3D draw that call issues, and projects the note's
	// position floats to screen coordinates.
	//
	// Correlation is by call order on the render thread: the host detour dispatches
	// (renderCtx, noteArray) here first, then runs the original draw, whose
	// DrawIndexedPrimitive arrives as ObserveNativeDraw. The draw covers one note per call,
	// so each stored note is consumed by exactly one draw; a draw with no stored note logs
	// unmatched rather than reusing a stale one.
	//
	// The projection is computed under both matrix conventions (c0..c3 as rows, and the
	// transpose) since the register layout is unknown; the raw constants c0..c7 are logged
	// alongside.
	namespace ScreenMapSnapshot
	{
		constexpr unsigned int DRAW_LIMIT = 160;

		std::atomic<int> drawsRemaining{ 0 };
		std::atomic<uint64_t> drawOrdinal{ 0 };
		bool viewportLogged = false;
		bool constantReadFailureLogged = false;

		// Render-thread only: the note identity from the current native draw call, consumed
		// by the D3D-level observation it produces.
		struct PendingNote
		{
			uintptr_t renderCtx = 0;
			uintptr_t note = 0;
			int count = 0;
			bool valid = false;
		};
		PendingNote pendingNote;

		void Arm()
		{
			viewportLogged = false;
			constantReadFailureLogged = false;
			drawOrdinal.store(0, std::memory_order_release);
			drawsRemaining.store(DRAW_LIMIT, std::memory_order_release);
			LOG_INFO("(NBN SCREEN MAP) Armed for the next " << DRAW_LIMIT
				<< " note-head draws." << std::endl);
		}

		void ObserveDrawList(void* renderCtx, void* noteArray)
		{
			if (drawsRemaining.load(std::memory_order_acquire) <= 0) return;

			pendingNote = {};
			const auto arr = reinterpret_cast<uintptr_t>(noteArray);
			if (!NoteListSnapshot::LooksReadable(arr)) return;
			const auto base = static_cast<uintptr_t>(
				static_cast<unsigned int>(NoteListSnapshot::ReadInt(arr, 0)));
			const int count = NoteListSnapshot::ReadInt(arr, 4);
			if (!NoteListSnapshot::LooksReadable(base) || count <= 0 || count > 1024) return;
			const auto note = static_cast<uintptr_t>(
				static_cast<unsigned int>(NoteListSnapshot::ReadInt(base, 0)));
			if (!NoteListSnapshot::LooksReadable(note)) return;

			pendingNote.renderCtx = reinterpret_cast<uintptr_t>(renderCtx);
			pendingNote.note = note;
			pendingNote.count = count;
			pendingNote.valid = true;
		}

		void ObserveDraw(const ResearchProtocol::NativeDrawObservation* draw)
		{
			if (draw == nullptr) return;
			if (drawsRemaining.load(std::memory_order_acquire) <= 0) return;
			if (drawsRemaining.fetch_sub(1, std::memory_order_acq_rel) <= 0) return;
			const uint64_t ordinal = drawOrdinal.fetch_add(1, std::memory_order_acq_rel);

			const PendingNote matched = pendingNote;
			pendingNote.valid = false;

			auto* device = reinterpret_cast<IDirect3DDevice9*>(draw->devicePointer);
			float constants[8][4] = {};
			bool hasConstants = false;
			D3DVIEWPORT9 viewport = {};
			bool hasViewport = false;
			if (device != nullptr)
			{
				hasConstants = SUCCEEDED(device->GetVertexShaderConstantF(0, &constants[0][0], 8));
				hasViewport = SUCCEEDED(device->GetViewport(&viewport));
			}
			if (!hasConstants && draw->shaderConstantShadow != 0)
			{
				// The host's SetVertexShaderConstantF hook shadows c0..c7 exactly for this
				// case (a pure device rejects the Get call).
				std::memcpy(constants,
					reinterpret_cast<const float*>(draw->shaderConstantShadow),
					sizeof(constants));
				hasConstants = true;
				if (!constantReadFailureLogged)
				{
					constantReadFailureLogged = true;
					LOG_INFO("(NBN SCREEN MAP) GetVertexShaderConstantF failed; using the "
						<< "host's constant shadow instead." << std::endl);
				}
			}

			if (hasViewport && !viewportLogged)
			{
				viewportLogged = true;
				LOG_INFO("(NBN SCREEN MAP) viewport x=" << viewport.X << " y=" << viewport.Y
					<< " w=" << viewport.Width << " h=" << viewport.Height << std::endl);
			}

			std::ostringstream line;
			line << "(NBN SCREEN MAP) draw=" << ordinal
				<< " ctx=0x" << std::hex << matched.renderCtx
				<< " note=0x" << matched.note << std::dec
				<< " count=" << matched.count
				<< (matched.valid ? "" : " UNMATCHED");

			if (matched.valid)
			{
				const float px = NoteListSnapshot::ReadFloat(matched.note, 0x2C);
				const float py = NoteListSnapshot::ReadFloat(matched.note, 0x30);
				const float pz = NoteListSnapshot::ReadFloat(matched.note, 0x34);
				line << std::fixed << std::setprecision(3)
					<< " pos{" << px << "," << py << "," << pz
					<< "," << NoteListSnapshot::ReadFloat(matched.note, 0x38) << "}";

				if (hasConstants && hasViewport)
				{
					// Candidate A: c0..c3 are the matrix rows, clip_i = dot(c_i, v).
					// Candidate B: the transpose, clip_i = c0[i]x + c1[i]y + c2[i]z + c3[i].
					const float v[4] = { px, py, pz, 1.0f };
					float clipA[4], clipB[4];
					for (int i = 0; i < 4; ++i)
					{
						clipA[i] = constants[i][0] * v[0] + constants[i][1] * v[1]
							+ constants[i][2] * v[2] + constants[i][3] * v[3];
						clipB[i] = constants[0][i] * v[0] + constants[1][i] * v[1]
							+ constants[2][i] * v[2] + constants[3][i] * v[3];
					}
					const auto project = [&viewport](const float clip[4], float& sx, float& sy)
					{
						if (clip[3] > -1e-6f && clip[3] < 1e-6f) { sx = sy = -99999.0f; return; }
						const float ndcX = clip[0] / clip[3];
						const float ndcY = clip[1] / clip[3];
						sx = viewport.X + (ndcX * 0.5f + 0.5f) * viewport.Width;
						sy = viewport.Y + (0.5f - ndcY * 0.5f) * viewport.Height;
					};
					float axs, ays, bxs, bys;
					project(clipA, axs, ays);
					project(clipB, bxs, bys);
					line << std::setprecision(1)
						<< " screenA{" << axs << "," << ays << ",w=" << std::setprecision(3)
						<< clipA[3] << std::setprecision(1) << "}"
						<< " screenB{" << bxs << "," << bys << ",w=" << std::setprecision(3)
						<< clipB[3] << "}";
				}
			}

			if (hasConstants)
			{
				line << std::setprecision(3) << " c0..c7{";
				for (int r = 0; r < 8; ++r)
				{
					line << (r == 0 ? "" : " ") << "[" << constants[r][0] << ","
						<< constants[r][1] << "," << constants[r][2] << ","
						<< constants[r][3] << "]";
				}
				line << "}";
			}

			// Identity discriminators geometry cannot give: the marker's color rides in the
			// pixel-shader constants (string colors: green, purple, orange...) and its
			// artwork in the bound texture. These tell the green fret marker apart from the
			// purple target cue when both project to the fretboard.
			if (device != nullptr)
			{
				float pixelConstants[4][4] = {};
				if (SUCCEEDED(device->GetPixelShaderConstantF(0, &pixelConstants[0][0], 4)))
				{
					line << std::setprecision(3) << " ps0..ps3{";
					for (int r = 0; r < 4; ++r)
					{
						line << (r == 0 ? "" : " ") << "[" << pixelConstants[r][0] << ","
							<< pixelConstants[r][1] << "," << pixelConstants[r][2] << ","
							<< pixelConstants[r][3] << "]";
					}
					line << "}";
				}
				IDirect3DBaseTexture9* texture = nullptr;
				if (SUCCEEDED(device->GetTexture(0, &texture)) && texture != nullptr)
				{
					line << " tex=0x" << std::hex
						<< reinterpret_cast<uintptr_t>(texture) << std::dec;
					texture->Release();
				}
			}

			LOG_INFO(line.str() << std::endl);

			if (drawsRemaining.load(std::memory_order_acquire) <= 0)
			{
				LOG_INFO("(NBN SCREEN MAP) Capture complete: " << ordinal + 1
					<< " draws." << std::endl);
			}
		}
	}

	void __cdecl ArmNoteDrawListSnapshot()
	{
		NoteListSnapshot::Arm();
	}

	void __cdecl ObserveNoteDrawList(void* renderCtx, void* noteArray)
	{
		NoteListSnapshot::Observe(renderCtx, noteArray);
		ScreenMapSnapshot::ObserveDrawList(renderCtx, noteArray);
	}

	// Stop_TMusic activates Rocksmith's stopped-transport preview sweep. The sweep's
	// dedicated marker triples are already fully classified at this native draw boundary:
	// their context vtable is 0x121B6F0, +0x30/+0x34 encode fret/string with a 0.5 offset,
	// and +0x2C/+0x38 carry the three marker-quad scale values. Highway notes use different
	// position signatures.
	//
	// Do not skip the native draw call. Put only the marker's string coordinate on
	// Rocksmith's -999.5 off-screen sentinel and let the complete draw run.
	// Two write-timing modes:
	//   - transient: restore the record the moment the native function returns.
	//   - persistent: the record stays relocated for the whole hold, so consumers
	//     outside the bracket read the sentinel too. Restores run at hold end, target
	//     change, toggle-off and probe stop, and only touch a record whose sentinel is
	//     still in place (a rewritten record was recycled by the game).
	//
	// The visible stale box is drawn as per-frame stride-12 quads from 0xBEC0DA whose
	// position exists only in vertex data; the host's stale-marker quad filter removes
	// it at the draw, so this filter does not handle that box.
	namespace StoppedPreviewFilter
	{
		constexpr uintptr_t PREVIEW_CONTEXT_VTABLE = 0x0121B6F0;
		constexpr float MARKER_OFFSET = 0.5f;
		constexpr float OFFSCREEN_STRING_POSITION = -999.5f;
		constexpr float MARKER_SCALE_MIN = 0.70f;
		constexpr float MARKER_SCALE_MAX = 1.05f;
		constexpr float MARKER_W_MIN = 0.80f;
		constexpr float MARKER_W_MAX = 0.95f;
		constexpr float FRET_EPSILON = 0.35f;
		constexpr float STRING_EPSILON = 0.20f;
		constexpr std::array<float, 24> FRET_CENTERS =
		{
			-50.469f, -44.429f, -38.739f, -33.163f, -27.755f, -22.552f,
			-17.478f, -12.512f, -7.723f, -3.061f, 1.496f, 5.932f,
			10.242f, 14.448f, 18.593f, 22.627f, 26.565f, 30.107f,
			33.896f, 37.366f, 40.977f, 44.478f, 47.915f, 51.351f,
		};
		constexpr std::array<float, 6> STRING_HEIGHTS =
		{
			4.018f, 2.410f, 0.803f, -0.803f, -2.410f, -4.018f,
		};
		// Bass neck: four strings over the same width as guitar, 2.679 apart (the outer
		// strings keep the guitar heights). The marker animates, so readings drift up to ~0.6
		// from the rest height; the wider epsilon covers that and stays under half the spacing.
		constexpr std::array<float, 4> BASS_STRING_HEIGHTS =
		{
			4.018f, 1.339f, -1.339f, -4.018f,
		};
		constexpr float BASS_STRING_EPSILON = 0.90f;

		std::atomic<uint32_t> hiddenCount{ 0 };
		// The fretboard marker draws in several passes per frame (count==1 single-record
		// passes and multi-record arrays with nonzero +0x14), so every record in every pass
		// is classified by its fret/string decode and hidden when it is not the target.
		// These slots serve only the transient mode; persistent mode keeps its own list.
		constexpr size_t PENDING_CAPACITY = 16;
		thread_local uintptr_t pendingNotes[PENDING_CAPACITY] = {};
		thread_local float pendingStringPositions[PENDING_CAPACITY] = {};
		thread_local size_t pendingCount = 0;

		bool TryWriteFloat(uintptr_t address, float value)
		{
			__try
			{
				*reinterpret_cast<float*>(address) = value;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool TryReadFloat(uintptr_t address, float* value)
		{
			__try
			{
				*value = *reinterpret_cast<const float*>(address);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool TryReadDword(uintptr_t address, uint32_t* value)
		{
			__try
			{
				*value = *reinterpret_cast<const uint32_t*>(address);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Persistent-mode state. All draw-path callers arrive serialized under the
		// bridge's probe mutex, but stop/shutdown/toggle restores can come from other
		// threads, so the tracked list carries its own lock. The list is keyed by
		// record address; the stored position is what the restore writes back.
		// Default OFF: persistent relocation writes corrupt the game heap
		// (0xc0000409/0xc0000374 on the main thread). The host's stale-marker quad filter
		// does not need it; probe_marker_persist_on enables it for study only.
		std::atomic<bool> isPersistentRelocationEnabled{ false };
		constexpr size_t PERSISTENT_CAPACITY = 128;
		// headerWord and fretWord tell a live record from a freed one. Heap free garbles
		// only the leading bytes of the block, so a sentinel at +0x34 survives the free and
		// a restore would write into freed heap. The restore also requires the dword at
		// +0x00 and the fret dword at +0x30 to match their tracked values; any mismatch
		// counts as recycled and is never written.
		struct PersistentEntry
		{
			uintptr_t note;
			float originalPosition;
			uint32_t headerWord;
			uint32_t fretWord;
		};
		std::mutex persistentMutex;
		PersistentEntry persistentEntries[PERSISTENT_CAPACITY] = {};
		size_t persistentCount = 0;
		int persistentTargetString = -1;
		int persistentTargetFret = -1;
		std::atomic<uint32_t> persistentHiddenCount{ 0 };
		std::atomic<bool> wasCapacityReported{ false };

		// Per-hold log budget and counters, reset at each hold boundary (every
		// RestorePersistent) so a global throttle cannot hide whole holds.
		constexpr int PER_HOLD_LOG_BUDGET = 6;
		int perHoldLogBudget = PER_HOLD_LOG_BUDGET;
		uint32_t perHoldRelocations = 0;
		uint32_t perHoldRewrites = 0;

		void RestorePersistent(const char* reason)
		{
			std::lock_guard<std::mutex> lock(persistentMutex);
			persistentTargetString = -1;
			persistentTargetFret = -1;
			const auto holdRelocations = perHoldRelocations;
			const auto holdRewrites = perHoldRewrites;
			perHoldLogBudget = PER_HOLD_LOG_BUDGET;
			perHoldRelocations = 0;
			perHoldRewrites = 0;
			if (persistentCount == 0)
			{
				// Logged because it means the sweep delivered no records this hold. Only
				// explicit calls (toggle-off / stop / shutdown) reach here; the draw-path
				// reasons require a nonempty list.
				LOG_INFO("(NBN STOPPED PREVIEW) restore pass with nothing tracked, "
					<< "reason=" << reason << "." << std::endl);
				return;
			}

			size_t restored = 0;
			size_t recycled = 0;
			for (size_t index = 0; index < persistentCount; ++index)
			{
				const auto& entry = persistentEntries[index];
				float current = 0.0f;
				uint32_t headerWord = 0;
				uint32_t fretWord = 0;
				if (!TryReadFloat(entry.note + 0x34, &current)
					|| std::fabs(current - OFFSCREEN_STRING_POSITION) > 0.01f
					|| !TryReadDword(entry.note + 0x00, &headerWord)
					|| headerWord != entry.headerWord
					|| !TryReadDword(entry.note + 0x30, &fretWord)
					|| fretWord != entry.fretWord)
				{
					// The sentinel is gone OR the header/fret words moved: the game
					// rewrote, freed or reallocated the record, so the memory may
					// belong to someone else (or the allocator) now. Never write it.
					++recycled;
					continue;
				}
				if (TryWriteFloat(entry.note + 0x34, entry.originalPosition))
				{
					++restored;
				}
			}
			LOG_INFO("(NBN STOPPED PREVIEW) restored " << restored
				<< " persistent marker(s), " << recycled << " recycled, holdRelocations="
				<< holdRelocations << " holdRewrites=" << holdRewrites << ", reason="
				<< reason << "." << std::endl);
			persistentCount = 0;
		}

		void HidePersistently(
			uintptr_t note,
			float originalPosition,
			const ResearchProtocol::NoteByNoteState& state,
			int stringIndex,
			int fret)
		{
			std::lock_guard<std::mutex> lock(persistentMutex);
			for (size_t index = 0; index < persistentCount; ++index)
			{
				if (persistentEntries[index].note != note) continue;
				// The game wrote the record back on-screen mid-hold. Re-relocate and
				// adopt the game's fresh position as the restore value.
				if (TryWriteFloat(note + 0x34, OFFSCREEN_STRING_POSITION))
				{
					persistentEntries[index].originalPosition = originalPosition;
					// The sweep just delivered this record, so it is live right now;
					// refresh the identity words the restore validates against.
					TryReadDword(note + 0x00, &persistentEntries[index].headerWord);
					TryReadDword(note + 0x30, &persistentEntries[index].fretWord);
					++perHoldRewrites;
					if (perHoldLogBudget > 0)
					{
						--perHoldLogBudget;
						LOG_INFO("(NBN STOPPED PREVIEW) re-relocated after game rewrite "
							<< stringIndex << ':' << fret
							<< " note=0x" << std::hex << note << std::dec
							<< " holdRewrites=" << perHoldRewrites << std::endl);
					}
				}
				return;
			}
			if (persistentCount >= PERSISTENT_CAPACITY)
			{
				if (!wasCapacityReported.exchange(true, std::memory_order_acq_rel))
				{
					LOG_ERROR("(NBN STOPPED PREVIEW) persistent list full at "
						<< PERSISTENT_CAPACITY
						<< " records; further markers stay visible." << std::endl);
				}
				return;
			}
			// Capture the identity words while the record is provably live (the sweep
			// delivered it this pass); the restore refuses to write unless they still
			// match. Read before the relocation so fretWord holds the on-screen value.
			uint32_t headerWord = 0;
			uint32_t fretWord = 0;
			if (!TryReadDword(note + 0x00, &headerWord)
				|| !TryReadDword(note + 0x30, &fretWord))
			{
				return;
			}
			if (!TryWriteFloat(note + 0x34, OFFSCREEN_STRING_POSITION))
			{
				LOG_ERROR("(NBN STOPPED PREVIEW) Could not relocate marker record 0x"
					<< std::hex << note << std::dec << "." << std::endl);
				return;
			}
			persistentEntries[persistentCount].note = note;
			persistentEntries[persistentCount].originalPosition = originalPosition;
			persistentEntries[persistentCount].headerWord = headerWord;
			persistentEntries[persistentCount].fretWord = fretWord;
			++persistentCount;
			persistentTargetString = state.visualString;
			persistentTargetFret = state.visualFret;

			++perHoldRelocations;
			const auto total = persistentHiddenCount.fetch_add(1, std::memory_order_acq_rel) + 1;
			// The per-hold budget keeps every hold's first relocations visible after the
			// global throttle has long since gone quiet.
			const bool hasHoldBudget = perHoldLogBudget > 0;
			if (hasHoldBudget) --perHoldLogBudget;
			if (hasHoldBudget || total <= 18 || total % 500 == 0)
			{
				LOG_INFO("(NBN STOPPED PREVIEW) relocated persistently "
					<< stringIndex << ':' << fret
					<< " target=" << state.visualString << ':' << state.visualFret
					<< " note=0x" << std::hex << note << std::dec
					<< " tracked=" << persistentCount
					<< " holdRelocations=" << perHoldRelocations
					<< " count=" << total << std::endl);
			}
		}

		void RestorePending()
		{
			for (size_t index = 0; index < pendingCount; ++index)
			{
				if (!TryWriteFloat(pendingNotes[index] + 0x34, pendingStringPositions[index]))
				{
					LOG_ERROR("(NBN STOPPED PREVIEW) Could not restore marker record 0x"
						<< std::hex << pendingNotes[index] << std::dec << "." << std::endl);
				}
				pendingNotes[index] = 0;
			}
			pendingCount = 0;
		}

		int DecodeClosest(float value, const float* values, size_t count, float epsilon)
		{
			int closest = -1;
			float closestDistance = epsilon;
			for (size_t index = 0; index < count; ++index)
			{
				const float distance = std::fabs(value - values[index]);
				if (distance >= closestDistance) continue;

				closest = static_cast<int>(index);
				closestDistance = distance;
			}
			return closest;
		}

		bool IsSelectedCoordinate(
			const ResearchProtocol::NoteByNoteState& state,
			int stringIndex,
			int fret)
		{
			if (stringIndex == state.visualString && fret == state.visualFret) return true;

			for (uint32_t index = 0; index < state.visualGroupCount
				&& index < ResearchProtocol::NoteByNoteState::MaxVisualGroup; ++index)
			{
				if (stringIndex == state.visualGroupStrings[index]
					&& fret == state.visualGroupFrets[index])
				{
					return true;
				}
			}
			return false;
		}

		bool HideNonTarget(void* renderCtx, void* noteArray)
		{
			RestorePending();
			const auto state = NoteByNoteNativeScoring::GetResearchState();
			const bool isPersistent =
				isPersistentRelocationEnabled.load(std::memory_order_relaxed);
			const bool isHoldActive = state.ownsNativeHold != 0 && state.visualChordId < 0;

			// Persistent restores ride the draw stream: this seam keeps firing after
			// release (every note-head pass lands here before the vtable gate), so the
			// first pass after the hold ends, the target moves, or the mode switches
			// off puts every relocated record back.
			const char* restoreReason = nullptr;
			{
				std::lock_guard<std::mutex> lock(persistentMutex);
				if (persistentCount > 0)
				{
					if (!isPersistent) restoreReason = "persist-disabled";
					else if (!isHoldActive) restoreReason = "hold-ended";
					else if (persistentTargetString != state.visualString
						|| persistentTargetFret != state.visualFret)
					{
						restoreReason = "target-changed";
					}
				}
			}
			if (restoreReason != nullptr) RestorePersistent(restoreReason);

			if (!isHoldActive) return false;
			if (!NoteListSnapshot::LooksReadable(reinterpret_cast<uintptr_t>(renderCtx))
				|| *reinterpret_cast<const uintptr_t*>(renderCtx) != PREVIEW_CONTEXT_VTABLE)
			{
				return false;
			}

			const auto arrayAddress = reinterpret_cast<uintptr_t>(noteArray);
			if (!NoteListSnapshot::LooksReadable(arrayAddress)) return false;
			const auto notes = static_cast<uintptr_t>(
				static_cast<unsigned int>(NoteListSnapshot::ReadInt(arrayAddress, 0)));
			const int count = NoteListSnapshot::ReadInt(arrayAddress, 4);
			if (!NoteListSnapshot::LooksReadable(notes) || count < 1 || count > 64) return false;

			// The fret/string decode is the whole classifier: a record whose +0x30/+0x34
			// land on the fret-center and string-height tables is a fretboard marker, and
			// nothing else in these arrays decodes near them.
			bool didHide = false;
			const bool isBass = NoteByNoteNativeScoring::IsBassArrangementActive();
			for (int index = 0; index < count && pendingCount < PENDING_CAPACITY; ++index)
			{
				const auto note = static_cast<uintptr_t>(static_cast<unsigned int>(
					NoteListSnapshot::ReadInt(notes + static_cast<uintptr_t>(index) * 4, 0)));
				if (!NoteListSnapshot::LooksReadable(note)) continue;

				const int fretIndex = DecodeClosest(
					NoteListSnapshot::ReadFloat(note, 0x30) - MARKER_OFFSET,
					FRET_CENTERS.data(),
					FRET_CENTERS.size(),
					FRET_EPSILON);
				const int stringIndex = isBass
					? DecodeClosest(
						NoteListSnapshot::ReadFloat(note, 0x34) - MARKER_OFFSET,
						BASS_STRING_HEIGHTS.data(),
						BASS_STRING_HEIGHTS.size(),
						BASS_STRING_EPSILON)
					: DecodeClosest(
						NoteListSnapshot::ReadFloat(note, 0x34) - MARKER_OFFSET,
						STRING_HEIGHTS.data(),
						STRING_HEIGHTS.size(),
						STRING_EPSILON);
				if (fretIndex < 0 || stringIndex < 0) continue;

				const int fret = fretIndex + 1;
				if (IsSelectedCoordinate(state, stringIndex, fret)) continue;

				const float originalPosition = NoteListSnapshot::ReadFloat(note, 0x34);
				if (isPersistent)
				{
					// A record already relocated does not decode onto a string height,
					// so only fresh or game-rewritten records reach this call.
					HidePersistently(note, originalPosition, state, stringIndex, fret);
					continue;
				}
				if (!TryWriteFloat(note + 0x34, OFFSCREEN_STRING_POSITION))
				{
					LOG_ERROR("(NBN STOPPED PREVIEW) Could not hide marker record 0x"
						<< std::hex << note << std::dec << "." << std::endl);
					continue;
				}
				pendingNotes[pendingCount] = note;
				pendingStringPositions[pendingCount] = originalPosition;
				++pendingCount;
				didHide = true;

				const auto total = hiddenCount.fetch_add(1, std::memory_order_acq_rel) + 1;
				if (total <= 18 || total % 500 == 0)
				{
					LOG_INFO("(NBN STOPPED PREVIEW) hidden transiently " << stringIndex << ':' << fret
						<< " target=" << state.visualString << ':' << state.visualFret
						<< " note=0x" << std::hex << note << std::dec
						<< " count=" << total << std::endl);
				}
			}
			return didHide;
		}
	}

	// Transition capture. Read-only capture of the draws around a hold latch on a new
	// target, where the stale fretboard box is painted.
	//
	// It rides the host's full-draw feed (every in-song draw from all four device entry
	// points, unfiltered). While armed it keeps a rolling light record of each frame's
	// draws (identity keys plus the c0..c3 shadow; no buffer locks, so no GPU sync on the
	// rolling path). At each frame boundary it reads the controller's state; when a hold
	// latches on a new target it freezes the pre-transition frames, captures the
	// transition frame plus two more with instance translations and bounded vertex
	// sampling, and dumps both sides with a set diff keyed on (site, caller, texture,
	// shaders, mesh signature). Decode annotations against the fret and string tables
	// mark which draws sit at a marker's neck position.
	namespace TransitionCapture
	{
		constexpr size_t PRE_FRAME_COUNT = 2;
		constexpr size_t POST_FRAME_COUNT = 3;
		constexpr size_t FRAME_DRAW_LIMIT = 3072;
		constexpr size_t INSTANCE_TRANSLATION_LIMIT = 8;
		constexpr size_t INSTANCE_READ_LIMIT = 64;
		constexpr size_t POST_VERTEX_LOCK_BUDGET = 96;
		constexpr size_t VERTEX_BOUND_LIMIT = 512;
		constexpr int PER_ARM_TRANSITION_DEFAULT = 1;
		constexpr char CAPTURE_PATH[] = "RSModsResearch\\NoteByNoteTransitionCapture.txt";

		// Full marker batches: bass draws 22-43 quads in one strip. 400 covers the host
		// filter's 64-block cap.
		constexpr size_t SLICE_VERTEX_LIMIT = 400;

		struct DrawRecord
		{
			uint32_t site = 0;
			uintptr_t caller = 0;
			uintptr_t stream = 0;
			uintptr_t texture = 0;
			uintptr_t vertexShader = 0;
			uintptr_t pixelShader = 0;
			uint32_t primitiveType = 0;
			uint32_t primitiveCount = 0;
			uint32_t vertexCount = 0;
			uint32_t minimumVertexIndex = 0;
			uint32_t stride = 0;
			int32_t baseVertexIndex = 0;
			uint32_t startIndex = 0;
			uint32_t instanceCount = 0;
			float transform[4][4] = {};
			uint8_t translationCount = 0;
			float translations[INSTANCE_TRANSLATION_LIMIT][3] = {};
			uint8_t hasBounds = 0;
			float boundsMin[3] = {};
			float boundsMax[3] = {};
			// Exact geometry of a marker-slice draw: the index-buffer entries at startIndex
			// resolved through the vertex buffer. The slice family draws under a bare
			// axis-swap transform with position baked into the vertices.
			uint16_t sliceVertexCount = 0;
			// Heap, not inline: only the few marker draws per frame fill it.
			std::vector<std::array<float, 3>> sliceVertices;
		};

		struct Frame
		{
			uint64_t frameIndex = 0;
			uint32_t overflowCount = 0;
			std::vector<DrawRecord> draws;
		};

		enum class Phase { WaitingForTransition, CapturingPost };

		// Every entry point (draw feed, frame boundary, probe commands) arrives serialized
		// under the bridge's probe mutex, so plain state is safe; the atomic is only the
		// disarmed fast path for the per-draw feed.
		std::atomic<bool> isArmed{ false };
		std::atomic<uint64_t> feedDrawCount{ 0 };
		Phase phase = Phase::WaitingForTransition;
		int transitionsRemaining = 0;
		int transitionOrdinal = 0;
		Frame preFrames[PRE_FRAME_COUNT];
		size_t preFrameOccupancy = 0;
		size_t preFrameNext = 0;
		Frame currentFrame;
		std::vector<Frame> postFrames;
		size_t postLockBudget = 0;
		int lastVisualString = -1;
		int lastVisualFret = -1;
		// Chord holds all read 255:255, so string/fret identity is blind to
		// chord->chord transitions. The record pointer changes on every new target
		// and is stable across re-latches of the same target, so it is the
		// primary identity; string/fret are a secondary check.
		uintptr_t lastVisualRecord = 0;
		int transitionFromString = -1;
		int transitionFromFret = -1;
		int transitionToString = -1;
		int transitionToFret = -1;
		uint64_t transitionFrame = 0;

		// SEH-isolated so a stale user pointer cannot take the render thread down; no
		// C++ objects may live in this frame.
		bool TryComputeUserPointerBounds(
			uintptr_t data,
			size_t vertexCount,
			size_t stride,
			float* boundsMin,
			float* boundsMax) noexcept
		{
			__try
			{
				for (size_t index = 0; index < vertexCount; ++index)
				{
					const float* position =
						reinterpret_cast<const float*>(data + index * stride);
					for (int axis = 0; axis < 3; ++axis)
					{
						if (index == 0 || position[axis] < boundsMin[axis])
						{
							boundsMin[axis] = position[axis];
						}
						if (index == 0 || position[axis] > boundsMax[axis])
						{
							boundsMax[axis] = position[axis];
						}
					}
				}
				return vertexCount > 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The marker-slice family: DrawIndexedPrimitive tri-strips, stride 12, issued from
		// 0xBEC0DA inside FUN_00BEBCD0, drawing per-frame slices of an append-only marker
		// quad buffer whose stale slices are the visible double. Textures and vertex
		// ranges shift per paint, so the stable key is site+caller+stride+type.
		constexpr uintptr_t MARKER_SLICE_CALLER = 0x00BEC0DA;

		bool IsMarkerSliceDraw(const ResearchProtocol::NativeDrawObservation& draw)
		{
			return draw.drawSite == 0
				&& draw.nativeCaller == MARKER_SLICE_CALLER
				&& draw.stride == 12
				&& draw.primitiveType == D3DPT_TRIANGLESTRIP
				&& draw.vertexCount > 0
				&& draw.vertexCount <= 4096;
		}

		// The batched glyph flush that draws fingering numerals. Sampled to show whether
		// its per-quad vertex data leads with a world position; the host's per-glyph
		// filter fails open until that layout is confirmed.
		constexpr uintptr_t GLYPH_FLUSH_CALLER = 0x00DFC98C;

		bool IsGlyphFlushDraw(const ResearchProtocol::NativeDrawObservation& draw)
		{
			return draw.drawSite == 0
				&& draw.nativeCaller == GLYPH_FLUSH_CALLER
				&& draw.stride == 24
				&& draw.primitiveType == D3DPT_TRIANGLELIST
				&& draw.primitiveCount >= 2
				&& draw.primitiveCount <= 128;
		}

		// Resolves one slice draw's exact geometry: the strip's index-buffer entries at
		// startIndex, resolved through vertex-buffer stream 0. Two small read-only
		// locks; runs only for the handful of family draws per frame while armed.
		void CollectSliceGeometry(
			const ResearchProtocol::NativeDrawObservation& draw,
			IDirect3DDevice9* device,
			DrawRecord& record)
		{
			if (device == nullptr) return;
			// Strips carry primCount+2 indices; the glyph triangle list carries
			// primCount*3. Either way the sample cap bounds the read.
			const uint32_t rawIndexCount = draw.primitiveType == D3DPT_TRIANGLESTRIP
				? draw.primitiveCount + 2
				: draw.primitiveCount * 3;
			const uint32_t indexCount = (std::min)(
				rawIndexCount,
				static_cast<uint32_t>(SLICE_VERTEX_LIMIT));

			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			if (FAILED(device->GetIndices(&indexBuffer)) || indexBuffer == nullptr) return;
			D3DINDEXBUFFER_DESC indexDescription = {};
			uint32_t indices[SLICE_VERTEX_LIMIT] = {};
			bool hasIndices = false;
			if (SUCCEEDED(indexBuffer->GetDesc(&indexDescription)))
			{
				const uint32_t indexSize =
					indexDescription.Format == D3DFMT_INDEX32 ? 4 : 2;
				const uint64_t offset =
					static_cast<uint64_t>(draw.startIndex) * indexSize;
				const uint64_t byteCount =
					static_cast<uint64_t>(indexCount) * indexSize;
				void* data = nullptr;
				if (offset + byteCount <= indexDescription.Size
					&& SUCCEEDED(indexBuffer->Lock(
						static_cast<UINT>(offset),
						static_cast<UINT>(byteCount),
						&data,
						D3DLOCK_READONLY))
					&& data != nullptr)
				{
					for (uint32_t index = 0; index < indexCount; ++index)
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
			if (!hasIndices) return;

			IDirect3DVertexBuffer9* vertexBuffer = nullptr;
			UINT streamOffset = 0;
			UINT boundStride = 0;
			if (FAILED(device->GetStreamSource(0, &vertexBuffer, &streamOffset, &boundStride))
				|| vertexBuffer == nullptr)
			{
				return;
			}
			D3DVERTEXBUFFER_DESC vertexDescription = {};
			void* data = nullptr;
			if (boundStride >= 3 * sizeof(float)
				&& SUCCEEDED(vertexBuffer->GetDesc(&vertexDescription))
				&& SUCCEEDED(vertexBuffer->Lock(0, 0, &data, D3DLOCK_READONLY))
				&& data != nullptr)
			{
				const auto* bytes = static_cast<const uint8_t*>(data);
				for (uint32_t index = 0; index < indexCount; ++index)
				{
					const uint64_t vertexIndex =
						static_cast<uint64_t>(draw.baseVertexIndex) + indices[index];
					const uint64_t byteOffset =
						static_cast<uint64_t>(streamOffset) + vertexIndex * boundStride;
					if (byteOffset + 3 * sizeof(float) > vertexDescription.Size) continue;
					std::array<float, 3> position{};
					std::memcpy(position.data(), bytes + byteOffset, 3 * sizeof(float));
					record.sliceVertices.push_back(position);
					++record.sliceVertexCount;
				}
				vertexBuffer->Unlock();
			}
			vertexBuffer->Release();
		}

		uint32_t DeriveVertexCount(const ResearchProtocol::NativeDrawObservation& draw)
		{
			if (draw.vertexCount != 0) return draw.vertexCount;
			switch (draw.primitiveType)
			{
				case D3DPT_POINTLIST: return draw.primitiveCount;
				case D3DPT_LINELIST: return draw.primitiveCount * 2;
				case D3DPT_LINESTRIP: return draw.primitiveCount + 1;
				case D3DPT_TRIANGLELIST: return draw.primitiveCount * 3;
				case D3DPT_TRIANGLESTRIP:
				case D3DPT_TRIANGLEFAN: return draw.primitiveCount + 2;
				default: return 0;
			}
		}

		// Post-frame detail: instance translations for instanced draws, position bounds
		// for user-pointer draws (free reads) and a lock-budgeted sample of small
		// non-instanced stream draws (world-space quads keep their position only in
		// vertex data).
		void CollectPostDetail(
			const ResearchProtocol::NativeDrawObservation& draw,
			IDirect3DDevice9* device,
			DrawRecord& record)
		{
			if (record.instanceCount > 0
				&& record.instanceCount <= INSTANCE_READ_LIMIT
				&& device != nullptr)
			{
				std::vector<float> values;
				if (RenderSnapshot::ReadInstanceStream(
					device,
					1,
					record.instanceCount,
					RenderSnapshot::TRANSFORM_STRIDE,
					values))
				{
					const size_t translationCount = (std::min)(
						static_cast<size_t>(record.instanceCount),
						INSTANCE_TRANSLATION_LIMIT);
					for (size_t index = 0; index < translationCount; ++index)
					{
						const float* instance = values.data() + index * 12;
						record.translations[index][0] = instance[3];
						record.translations[index][1] = instance[7];
						record.translations[index][2] = instance[11];
					}
					record.translationCount = static_cast<uint8_t>(translationCount);
				}
				return;
			}

			const uint32_t vertexCount = DeriveVertexCount(draw);
			if (vertexCount == 0 || vertexCount > VERTEX_BOUND_LIMIT
				|| draw.stride < 3 * sizeof(float))
			{
				return;
			}

			if (draw.userPointerData != 0)
			{
				record.hasBounds = TryComputeUserPointerBounds(
					draw.userPointerData,
					vertexCount,
					draw.stride,
					record.boundsMin,
					record.boundsMax) ? 1 : 0;
				return;
			}

			if (device == nullptr || record.instanceCount > 0 || postLockBudget == 0) return;
			--postLockBudget;

			IDirect3DVertexBuffer9* buffer = nullptr;
			UINT streamOffset = 0;
			UINT boundStride = 0;
			if (FAILED(device->GetStreamSource(0, &buffer, &streamOffset, &boundStride))
				|| buffer == nullptr)
			{
				return;
			}
			// DrawPrimitive's start vertex arrives in startIndex; the indexed path offsets
			// by base + minimum.
			const uint64_t startVertex = draw.drawSite == 1
				? draw.startIndex
				: static_cast<uint64_t>(
					static_cast<int64_t>(draw.baseVertexIndex) + draw.minimumVertexIndex);
			const uint64_t drawOffset = static_cast<uint64_t>(streamOffset)
				+ startVertex * boundStride;
			const uint64_t byteCount = static_cast<uint64_t>(vertexCount) * boundStride;
			D3DVERTEXBUFFER_DESC description = {};
			void* data = nullptr;
			if (boundStride >= 3 * sizeof(float)
				&& SUCCEEDED(buffer->GetDesc(&description))
				&& drawOffset + byteCount <= description.Size
				&& SUCCEEDED(buffer->Lock(
					static_cast<UINT>(drawOffset),
					static_cast<UINT>(byteCount),
					&data,
					D3DLOCK_READONLY))
				&& data != nullptr)
			{
				const auto* bytes = static_cast<const uint8_t*>(data);
				for (uint32_t index = 0; index < vertexCount; ++index)
				{
					float position[3];
					std::memcpy(
						position,
						bytes + static_cast<size_t>(index) * boundStride,
						sizeof(position));
					for (int axis = 0; axis < 3; ++axis)
					{
						if (index == 0 || position[axis] < record.boundsMin[axis])
						{
							record.boundsMin[axis] = position[axis];
						}
						if (index == 0 || position[axis] > record.boundsMax[axis])
						{
							record.boundsMax[axis] = position[axis];
						}
					}
				}
				record.hasBounds = 1;
				buffer->Unlock();
			}
			buffer->Release();
		}

		void ObserveDraw(const ResearchProtocol::NativeDrawObservation* draw)
		{
			if (draw == nullptr) return;
			feedDrawCount.fetch_add(1, std::memory_order_relaxed);
			if (!isArmed.load(std::memory_order_acquire)) return;

			if (currentFrame.draws.size() >= FRAME_DRAW_LIMIT)
			{
				++currentFrame.overflowCount;
				return;
			}

			DrawRecord record;
			record.site = draw->drawSite;
			record.caller = draw->nativeCaller;
			record.stream = draw->streamIdentity;
			record.primitiveType = draw->primitiveType;
			record.primitiveCount = draw->primitiveCount;
			record.vertexCount = draw->vertexCount;
			record.minimumVertexIndex = draw->minimumVertexIndex;
			record.stride = draw->stride;
			record.baseVertexIndex = draw->baseVertexIndex;
			record.startIndex = draw->startIndex;

			auto* device = reinterpret_cast<IDirect3DDevice9*>(draw->devicePointer);
			if (device != nullptr)
			{
				IDirect3DBaseTexture9* texture = nullptr;
				if (SUCCEEDED(device->GetTexture(0, &texture)) && texture != nullptr)
				{
					record.texture = reinterpret_cast<uintptr_t>(texture);
					texture->Release();
				}
				IDirect3DVertexShader9* vertexShader = nullptr;
				if (SUCCEEDED(device->GetVertexShader(&vertexShader))
					&& vertexShader != nullptr)
				{
					record.vertexShader = reinterpret_cast<uintptr_t>(vertexShader);
					vertexShader->Release();
				}
				IDirect3DPixelShader9* pixelShader = nullptr;
				if (SUCCEEDED(device->GetPixelShader(&pixelShader)) && pixelShader != nullptr)
				{
					record.pixelShader = reinterpret_cast<uintptr_t>(pixelShader);
					pixelShader->Release();
				}
				if (draw->drawSite <= 1)
				{
					UINT frequency = 0;
					if (SUCCEEDED(device->GetStreamSourceFreq(0, &frequency))
						&& (frequency & D3DSTREAMSOURCE_INDEXEDDATA) != 0)
					{
						record.instanceCount =
							frequency & RenderSnapshot::FREQUENCY_VALUE_MASK;
					}
				}
			}
			if (draw->shaderConstantShadow != 0)
			{
				std::memcpy(
					record.transform,
					reinterpret_cast<const float*>(draw->shaderConstantShadow),
					sizeof(record.transform));
			}

			// Slice geometry rides every armed frame, pre and post: the stale box's
			// slices draw in steady state too, and their vertices are the only
			// position record they have. The glyph flush is sampled alongside.
			if (IsMarkerSliceDraw(*draw) || IsGlyphFlushDraw(*draw))
			{
				CollectSliceGeometry(*draw, device, record);
			}
			if (phase == Phase::CapturingPost) CollectPostDetail(*draw, device, record);
			currentFrame.draws.push_back(record);
		}

		// The diff key deliberately excludes stream/texture pointers' low-churn cousins
		// (baseVertexIndex, startIndex): the same object drawn from a rebuilt buffer
		// section must still match its pre-frame self.
		std::string MakeDiffKey(const DrawRecord& record)
		{
			char key[160];
			std::snprintf(key, sizeof(key),
				"site=%u caller=0x%08llx tex=0x%08llx vs=0x%08llx ps=0x%08llx "
				"stride=%u type=%u prims=%u verts=%u",
				record.site,
				static_cast<unsigned long long>(record.caller),
				static_cast<unsigned long long>(record.texture),
				static_cast<unsigned long long>(record.vertexShader),
				static_cast<unsigned long long>(record.pixelShader),
				record.stride,
				record.primitiveType,
				record.primitiveCount,
				record.vertexCount);
			return key;
		}

		// Decode annotation: does this record sit at a fretboard marker position? Tries
		// the instance translations, then the c0..c2 shadow translations (floats 3/7/11
		// convention), then the vertex-bound midpoint, each with and without the marker's
		// +0.5 offset. Annotation only, never a gate; a miss means "position unknown",
		// not "not the box".
		bool TryDecodeMarkerPosition(
			float fretCoordinate,
			float stringHeight,
			int& stringIndex,
			int& fret)
		{
			for (int pass = 0; pass < 2; ++pass)
			{
				const float offset = pass == 0 ? StoppedPreviewFilter::MARKER_OFFSET : 0.0f;
				const int fretIndex = StoppedPreviewFilter::DecodeClosest(
					fretCoordinate - offset,
					StoppedPreviewFilter::FRET_CENTERS.data(),
					StoppedPreviewFilter::FRET_CENTERS.size(),
					StoppedPreviewFilter::FRET_EPSILON);
				const int heightIndex = StoppedPreviewFilter::DecodeClosest(
					stringHeight - offset,
					StoppedPreviewFilter::STRING_HEIGHTS.data(),
					StoppedPreviewFilter::STRING_HEIGHTS.size(),
					StoppedPreviewFilter::STRING_EPSILON);
				if (fretIndex >= 0 && heightIndex >= 0)
				{
					stringIndex = heightIndex;
					fret = fretIndex + 1;
					return true;
				}
			}
			return false;
		}

		std::string DescribeDecode(const DrawRecord& record)
		{
			int stringIndex = -1;
			int fret = -1;
			for (uint8_t index = 0; index < record.translationCount; ++index)
			{
				if (TryDecodeMarkerPosition(
					record.translations[index][1],
					record.translations[index][2],
					stringIndex,
					fret))
				{
					return " decode=" + std::to_string(stringIndex) + ':'
						+ std::to_string(fret) + "(instance " + std::to_string(index) + ")";
				}
			}
			if (TryDecodeMarkerPosition(
				record.transform[1][3],
				record.transform[2][3],
				stringIndex,
				fret))
			{
				return " decode=" + std::to_string(stringIndex) + ':'
					+ std::to_string(fret) + "(transform)";
			}
			if (record.hasBounds != 0)
			{
				const float midFret = (record.boundsMin[1] + record.boundsMax[1]) * 0.5f;
				const float midHeight = (record.boundsMin[2] + record.boundsMax[2]) * 0.5f;
				if (TryDecodeMarkerPosition(midFret, midHeight, stringIndex, fret))
				{
					return " decode=" + std::to_string(stringIndex) + ':'
						+ std::to_string(fret) + "(bounds)";
				}
			}
			return "";
		}

		void WriteDrawLine(std::ostream& output, const DrawRecord& record)
		{
			output << "d " << MakeDiffKey(record)
				<< " base=" << record.baseVertexIndex
				<< " minv=" << record.minimumVertexIndex
				<< " start=" << record.startIndex
				<< " stream=0x" << std::hex << record.stream << std::dec
				<< " inst=" << record.instanceCount
				<< std::fixed << std::setprecision(3);
			for (size_t row = 0; row < 4; ++row)
			{
				output << " c" << row << "=(" << record.transform[row][0] << ','
					<< record.transform[row][1] << ',' << record.transform[row][2] << ','
					<< record.transform[row][3] << ')';
			}
			for (uint8_t index = 0; index < record.translationCount; ++index)
			{
				output << " t" << static_cast<int>(index) << "=("
					<< record.translations[index][0] << ','
					<< record.translations[index][1] << ','
					<< record.translations[index][2] << ')';
			}
			if (record.hasBounds != 0)
			{
				output << " bounds=(" << record.boundsMin[0] << ',' << record.boundsMin[1]
					<< ',' << record.boundsMin[2] << ")..(" << record.boundsMax[0] << ','
					<< record.boundsMax[1] << ',' << record.boundsMax[2] << ')';
			}
			for (uint16_t index = 0; index < record.sliceVertexCount; ++index)
			{
				output << " sv" << static_cast<int>(index) << "=("
					<< record.sliceVertices[index][0] << ','
					<< record.sliceVertices[index][1] << ','
					<< record.sliceVertices[index][2] << ')';
			}
			output << DescribeDecode(record) << '\n';
		}

		struct KeyStatistics
		{
			uint32_t maximumDrawCount = 0;
			uint32_t maximumInstanceSum = 0;
			const DrawRecord* sample = nullptr;
		};

		void AccumulateFrame(
			const Frame& frame,
			std::map<std::string, KeyStatistics>& statistics)
		{
			std::map<std::string, KeyStatistics> frameStatistics;
			for (const auto& record : frame.draws)
			{
				auto& entry = frameStatistics[MakeDiffKey(record)];
				++entry.maximumDrawCount;
				entry.maximumInstanceSum += record.instanceCount;
				entry.sample = &record;
			}
			for (const auto& [key, frameEntry] : frameStatistics)
			{
				auto& entry = statistics[key];
				entry.maximumDrawCount =
					(std::max)(entry.maximumDrawCount, frameEntry.maximumDrawCount);
				entry.maximumInstanceSum =
					(std::max)(entry.maximumInstanceSum, frameEntry.maximumInstanceSum);
				if (entry.sample == nullptr) entry.sample = frameEntry.sample;
			}
		}

		void DumpCapture()
		{
			std::ofstream output(CAPTURE_PATH, std::ios::binary | std::ios::app);
			if (!output.is_open())
			{
				LOG_ERROR("(NBN TRANSITION) Could not open " << CAPTURE_PATH
					<< " for writing." << std::endl);
				return;
			}

			output << "probeBuild=" << PROBE_BUILD_ID << '\n'
				<< "transition ordinal=" << transitionOrdinal
				<< " from=" << transitionFromString << ':' << transitionFromFret
				<< " to=" << transitionToString << ':' << transitionToFret
				<< " frame=" << transitionFrame << '\n';

			std::map<std::string, KeyStatistics> preStatistics;
			for (size_t index = 0; index < preFrameOccupancy; ++index)
			{
				const auto& frame = preFrames[index];
				output << "pre-frame frame=" << frame.frameIndex
					<< " draws=" << frame.draws.size()
					<< " overflow=" << frame.overflowCount << '\n';
				for (const auto& record : frame.draws) WriteDrawLine(output, record);
				AccumulateFrame(frame, preStatistics);
			}

			std::map<std::string, KeyStatistics> postStatistics;
			for (const auto& frame : postFrames)
			{
				output << "post-frame frame=" << frame.frameIndex
					<< " draws=" << frame.draws.size()
					<< " overflow=" << frame.overflowCount << '\n';
				for (const auto& record : frame.draws) WriteDrawLine(output, record);
				AccumulateFrame(frame, postStatistics);
			}

			size_t appearedCount = 0;
			size_t grownCount = 0;
			for (const auto& [key, postEntry] : postStatistics)
			{
				const auto preEntry = preStatistics.find(key);
				const std::string decode = postEntry.sample != nullptr
					? DescribeDecode(*postEntry.sample)
					: std::string();
				if (preEntry == preStatistics.end())
				{
					++appearedCount;
					output << "diff appeared key={" << key << "} postDraws="
						<< postEntry.maximumDrawCount << " postInstances="
						<< postEntry.maximumInstanceSum << decode << '\n';
					continue;
				}
				if (postEntry.maximumDrawCount > preEntry->second.maximumDrawCount
					|| postEntry.maximumInstanceSum > preEntry->second.maximumInstanceSum)
				{
					++grownCount;
					output << "diff grew key={" << key << "} preDraws="
						<< preEntry->second.maximumDrawCount << " postDraws="
						<< postEntry.maximumDrawCount << " preInstances="
						<< preEntry->second.maximumInstanceSum << " postInstances="
						<< postEntry.maximumInstanceSum << decode << '\n';
				}
			}
			output << "capture-complete preFrames=" << preFrameOccupancy
				<< " postFrames=" << postFrames.size()
				<< " appeared=" << appearedCount
				<< " grew=" << grownCount << '\n';
			output.flush();

			LOG_INFO("(NBN TRANSITION) Capture " << transitionOrdinal << " complete: "
				<< transitionFromString << ':' << transitionFromFret << " -> "
				<< transitionToString << ':' << transitionToFret
				<< " frame=" << transitionFrame
				<< " appeared=" << appearedCount << " grew=" << grownCount
				<< " saved=" << CAPTURE_PATH << "." << std::endl);
		}

		void ResetFrames()
		{
			for (auto& frame : preFrames) frame = {};
			preFrameOccupancy = 0;
			preFrameNext = 0;
			currentFrame = {};
			postFrames.clear();
		}

		void Arm(int transitions)
		{
			ResetFrames();
			phase = Phase::WaitingForTransition;
			transitionsRemaining = transitions;
			transitionOrdinal = 0;
			// Prime the last-seen target from live state so a capture armed mid-hold
			// triggers on the next target, and a re-latch of the same target (which the
			// sweep ignores) never consumes the arm.
			const auto state = NoteByNoteNativeScoring::GetResearchState();
			const bool owns = state.ownsNativeHold != 0 && state.visualString >= 0;
			lastVisualString = owns ? state.visualString : -1;
			lastVisualFret = owns ? state.visualFret : -1;
			lastVisualRecord = owns ? state.visualRecord : 0;
			std::ofstream truncate(CAPTURE_PATH, std::ios::binary | std::ios::trunc);
			isArmed.store(true, std::memory_order_release);
			LOG_INFO("(NBN TRANSITION) Armed for " << transitions
				<< " target transition(s); rolling " << PRE_FRAME_COUNT
				<< " pre-frames, capturing " << POST_FRAME_COUNT
				<< " post-frames." << std::endl);
		}

		void Disarm(const char* reason)
		{
			isArmed.store(false, std::memory_order_release);
			transitionsRemaining = 0;
			ResetFrames();
			LOG_INFO("(NBN TRANSITION) Disarmed, reason=" << reason << "." << std::endl);
		}

		void NotifyFrameComplete(uint64_t renderFrame)
		{
			if (!isArmed.load(std::memory_order_acquire)) return;
			currentFrame.frameIndex = renderFrame;

			if (phase == Phase::WaitingForTransition)
			{
				const auto state = NoteByNoteNativeScoring::GetResearchState();
				const bool owns = state.ownsNativeHold != 0 && state.visualString >= 0;
				const bool triggered = owns
					&& (state.visualRecord != lastVisualRecord
						|| state.visualString != lastVisualString
						|| state.visualFret != lastVisualFret);
				if (triggered)
				{
					++transitionOrdinal;
					transitionFromString = lastVisualString;
					transitionFromFret = lastVisualFret;
					transitionToString = state.visualString;
					transitionToFret = state.visualFret;
					transitionFrame = renderFrame;
					lastVisualString = state.visualString;
					lastVisualFret = state.visualFret;
					lastVisualRecord = state.visualRecord;
					// The boundary frame may already carry the fresh paint (the latch
					// happened somewhere inside it), so it counts as the first post
					// frame rather than a pre frame.
					postFrames.clear();
					postFrames.push_back(std::move(currentFrame));
					currentFrame = {};
					phase = Phase::CapturingPost;
					postLockBudget = POST_VERTEX_LOCK_BUDGET;
					LOG_INFO("(NBN TRANSITION) Hold latched on "
						<< transitionToString << ':' << transitionToFret
						<< " record=0x" << std::hex << state.visualRecord << std::dec
						<< " at frame=" << renderFrame
						<< "; capturing post frames." << std::endl);
					return;
				}
				if (owns)
				{
					lastVisualString = state.visualString;
					lastVisualFret = state.visualFret;
					lastVisualRecord = state.visualRecord;
				}
				preFrames[preFrameNext] = std::move(currentFrame);
				preFrameNext = (preFrameNext + 1) % PRE_FRAME_COUNT;
				preFrameOccupancy = (std::min)(preFrameOccupancy + 1, PRE_FRAME_COUNT);
				currentFrame = {};
				return;
			}

			postFrames.push_back(std::move(currentFrame));
			currentFrame = {};
			postLockBudget = POST_VERTEX_LOCK_BUDGET;
			if (postFrames.size() < POST_FRAME_COUNT) return;

			DumpCapture();
			--transitionsRemaining;
			if (transitionsRemaining <= 0)
			{
				Disarm("capture-complete");
				return;
			}
			phase = Phase::WaitingForTransition;
			ResetFrames();
		}
	}

	void __cdecl ArmScreenMapSnapshot()
	{
		ScreenMapSnapshot::Arm();
	}

	uint8_t __cdecl PrepareNoteDrawList(void* renderCtx, void* noteArray)
	{
		return StoppedPreviewFilter::HideNonTarget(renderCtx, noteArray) ? 1 : 0;
	}

	// One-shot register probe for the finger-numeral projection. The fretboard is rendered
	// by its own camera, and neither the panel shader's c4..c7 nor the note-head c0..c3 is
	// its view-projection, so on arm this dumps c0..c23 plus the viewport at the next
	// fingering-panel draw (32/187/104) whose c0..c3 decodes as a fretboard-space world
	// transform.
	namespace AnchorProbe
	{
		std::atomic<bool> isArmed{ false };

		void ObserveDraw(const ResearchProtocol::NativeDrawObservation* draw)
		{
			if (!isArmed.load(std::memory_order_acquire)) return;
			if (draw == nullptr) return;
			if (draw->stride != 32 || draw->vertexCount != 187
				|| draw->primitiveCount != 104)
			{
				return;
			}
			auto* device = reinterpret_cast<IDirect3DDevice9*>(draw->devicePointer);
			if (device == nullptr) return;

			float constants[24][4] = {};
			const bool hasConstants =
				SUCCEEDED(device->GetVertexShaderConstantF(0, &constants[0][0], 24));
			if (!hasConstants && draw->shaderConstantShadow != 0)
			{
				std::memcpy(constants,
					reinterpret_cast<const float*>(draw->shaderConstantShadow),
					8 * 4 * sizeof(float));
			}
			// Only fretboard-space panels carry the fretboard camera: the world
			// transform's translation must sit on the fret/string tables
			// (|longitudinal| small, fret position in range).
			const float longitudinal = constants[0][3];
			const float fretPosition = constants[1][3];
			const float stringHeight = constants[2][3];
			if (std::abs(longitudinal) > 5.0f || std::abs(stringHeight) > 5.0f)
			{
				return;
			}
			if (!isArmed.exchange(false, std::memory_order_acq_rel)) return;

			D3DVIEWPORT9 viewport = {};
			const bool hasViewport = SUCCEEDED(device->GetViewport(&viewport));
			std::ostringstream line;
			line << "(NBN ANCHOR PROBE) panel draw world translation=("
				<< std::fixed << std::setprecision(3) << longitudinal << ", "
				<< fretPosition << ", " << stringHeight << ")";
			if (hasViewport)
			{
				line << " viewport=" << viewport.X << "," << viewport.Y
					<< "," << viewport.Width << "," << viewport.Height;
			}
			line << " constantsFrom=" << (hasConstants ? "device" : "shadow(c0..c7 only)");
			LOG_INFO(line.str() << std::endl);
			for (int row = 0; row < 24; ++row)
			{
				LOG_INFO("(NBN ANCHOR PROBE) c" << row << " = ["
					<< std::fixed << std::setprecision(6)
					<< constants[row][0] << ", " << constants[row][1] << ", "
					<< constants[row][2] << ", " << constants[row][3] << "]"
					<< std::endl);
			}
		}
	}

	void __cdecl ObserveFullDraw(const ResearchProtocol::NativeDrawObservation* draw)
	{
		AnchorProbe::ObserveDraw(draw);
		TransitionCapture::ObserveDraw(draw);
	}

	void __cdecl CompleteNoteDrawList()
	{
		StoppedPreviewFilter::RestorePending();
	}

	// probe_* commands forwarded by the bridge (it forwards only probe_* verbs). Answers
	// probe_ping so the passthrough can be verified end to end; anything else is reported
	// unknown.
	// Marker color-state override, toggled by probe_marker_dim_on / probe_marker_dim_off.
	// Default OFF: these contexts never apply under Note by Note in any phase, so the
	// ctx+0x24/+0x2C writes are inert during play and would only apply, wrongly, once
	// normal transport resumes. The fretboard marker is driven by the host's synthetic
	// updater call (host mode 3) instead.
	std::atomic<bool> isMarkerDimEnabled{ false };

	uint8_t __cdecl HandleProbeCommand(
		const char* requestJson,
		char* responseBuffer,
		uint32_t responseCapacity)
	{
		if (requestJson == nullptr || responseBuffer == nullptr || responseCapacity < 3) return 0;
		if (std::strstr(requestJson, "probe_ping") != nullptr)
		{
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"probe\":\"pong\",\"buildId\":\"%s\"}", PROBE_BUILD_ID.c_str());
			return 1;
		}
		if (std::strstr(requestJson, "probe_marker_dim_on") != nullptr
			|| std::strstr(requestJson, "probe_marker_dim_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_marker_dim_on") != nullptr;
			isMarkerDimEnabled.store(enable, std::memory_order_relaxed);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"markerDimEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Transition capture. Disarm and status are matched before arm so their
		// substrings cannot shadow it.
		if (std::strstr(requestJson, "probe_transition_capture_status") != nullptr)
		{
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"armed\":%s,\"phase\":\"%s\","
				"\"transitionsRemaining\":%d,\"transitionOrdinal\":%d,"
				"\"preFrames\":%u,\"currentDraws\":%u,\"postFrames\":%u,"
				"\"feedDraws\":%llu,\"lastTarget\":\"%d:%d\"}",
				TransitionCapture::isArmed.load(std::memory_order_acquire)
					? "true" : "false",
				TransitionCapture::phase
						== TransitionCapture::Phase::CapturingPost
					? "capturing-post" : "waiting-for-transition",
				TransitionCapture::transitionsRemaining,
				TransitionCapture::transitionOrdinal,
				static_cast<unsigned>(TransitionCapture::preFrameOccupancy),
				static_cast<unsigned>(TransitionCapture::currentFrame.draws.size()),
				static_cast<unsigned>(TransitionCapture::postFrames.size()),
				static_cast<unsigned long long>(
					TransitionCapture::feedDrawCount.load(std::memory_order_relaxed)),
				TransitionCapture::lastVisualString,
				TransitionCapture::lastVisualFret);
			return 1;
		}
		if (std::strstr(requestJson, "probe_transition_capture_disarm") != nullptr)
		{
			TransitionCapture::Disarm("command");
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"transitionCaptureArmed\":false}");
			return 1;
		}
		if (std::strstr(requestJson, "probe_transition_capture_arm") != nullptr)
		{
			int transitions = TransitionCapture::PER_ARM_TRANSITION_DEFAULT;
			if (const char* key = std::strstr(requestJson, "\"transitions\":"))
			{
				const long parsed = std::strtol(key + 14, nullptr, 10);
				if (parsed >= 1 && parsed <= 16) transitions = static_cast<int>(parsed);
			}
			TransitionCapture::Arm(transitions);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"transitionCaptureArmed\":true,\"transitions\":%d}",
				transitions);
			return 1;
		}
		// Persistent-relocation status: the live tracked count, target and per-hold counters.
		if (std::strstr(requestJson, "probe_marker_persist_status") != nullptr)
		{
			size_t trackedCount = 0;
			int targetString = -1;
			int targetFret = -1;
			uint32_t holdRelocations = 0;
			uint32_t holdRewrites = 0;
			{
				std::lock_guard<std::mutex> lock(StoppedPreviewFilter::persistentMutex);
				trackedCount = StoppedPreviewFilter::persistentCount;
				targetString = StoppedPreviewFilter::persistentTargetString;
				targetFret = StoppedPreviewFilter::persistentTargetFret;
				holdRelocations = StoppedPreviewFilter::perHoldRelocations;
				holdRewrites = StoppedPreviewFilter::perHoldRewrites;
			}
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"persistEnabled\":%s,\"trackedCount\":%u,"
				"\"target\":\"%d:%d\",\"holdRelocations\":%u,\"holdRewrites\":%u,"
				"\"persistentHiddenTotal\":%u,\"transientHiddenTotal\":%u}",
				StoppedPreviewFilter::isPersistentRelocationEnabled.load(
					std::memory_order_relaxed) ? "true" : "false",
				static_cast<unsigned>(trackedCount),
				targetString,
				targetFret,
				holdRelocations,
				holdRewrites,
				StoppedPreviewFilter::persistentHiddenCount.load(std::memory_order_relaxed),
				StoppedPreviewFilter::hiddenCount.load(std::memory_order_relaxed));
			return 1;
		}
		// Persistent marker relocation. Default OFF because it corrupts the game heap (see
		// the declaration comment); on is for study only. Off restores every tracked
		// record immediately and falls back to the transient hide-restore bracket.
		if (std::strstr(requestJson, "probe_marker_persist_on") != nullptr
			|| std::strstr(requestJson, "probe_marker_persist_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_marker_persist_on") != nullptr;
			StoppedPreviewFilter::isPersistentRelocationEnabled.store(
				enable, std::memory_order_relaxed);
			if (!enable) StoppedPreviewFilter::RestorePersistent("persist-toggled-off");
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"markerPersistEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Finger-numeral projection register probe: arm the one-shot c0..c23 dump
		// at the next fretboard-space fingering-panel draw.
		if (std::strstr(requestJson, "probe_anchor_probe") != nullptr)
		{
			AnchorProbe::isArmed.store(true, std::memory_order_release);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"anchorProbeArmed\":true}");
			return 1;
		}
		// Per-hold Play_FreezeNoteTrack prompt sound (see isFreezePromptSoundEnabled in
		// the scoring TU).
		if (std::strstr(requestJson, "probe_freeze_sound_on") != nullptr
			|| std::strstr(requestJson, "probe_freeze_sound_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_freeze_sound_on") != nullptr;
			NoteByNoteNativeScoring::SetFreezePromptSoundEnabled(enable);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"freezeSoundEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Flow-until-miss: on, on-time notes commit while the transport flows (no per-note
		// freeze/restart, which avoids audio dropouts in dense sections); off restores
		// freeze-per-note.
		if (std::strstr(requestJson, "probe_flow_on") != nullptr
			|| std::strstr(requestJson, "probe_flow_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_flow_on") != nullptr;
			NoteByNoteNativeScoring::SetFlowUntilMissEnabled(enable);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"flowUntilMissEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Flow late-grace tuning (seconds): how far past a note's time the transport flows
		// before the boundary freezes on it. Sent as {"command":"probe_flow_grace","grace":0.15}.
		if (std::strstr(requestJson, "probe_flow_grace") != nullptr)
		{
			float grace = NoteByNoteNativeScoring::GetFlowLateGraceSeconds();
			const char* graceField = std::strstr(requestJson, "\"grace\"");
			if (graceField != nullptr)
			{
				const char* colon = std::strchr(graceField, ':');
				if (colon != nullptr) grace = static_cast<float>(std::atof(colon + 1));
			}
			NoteByNoteNativeScoring::SetFlowLateGraceSeconds(grace);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"flowLateGraceSeconds\":%.3f}",
				NoteByNoteNativeScoring::GetFlowLateGraceSeconds());
			return 1;
		}
		// Chord holds: the authoritative controller runs inside this probe, so the
		// toggle rides the probe-command channel rather than a host symbol.
		if (std::strstr(requestJson, "probe_chord_holds_on") != nullptr
			|| std::strstr(requestJson, "probe_chord_holds_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_chord_holds_on") != nullptr;
			NoteByNoteNativeScoring::SetChordHoldsEnabled(enable);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"chordHoldsEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Verbose trace: toggles per-tick diagnostic logging at runtime so the logging cost
		// is paid only while diagnosing.
		if (std::strstr(requestJson, "probe_verbose_on") != nullptr
			|| std::strstr(requestJson, "probe_verbose_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_verbose_on") != nullptr;
			NoteByNoteNativeScoring::SetVerboseTrace(enable);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"verboseTrace\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Chord window slide: avoids out-of-window chord refusals.
		if (std::strstr(requestJson, "probe_chord_window_on") != nullptr
			|| std::strstr(requestJson, "probe_chord_window_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_chord_window_on") != nullptr;
			NoteByNoteNativeScoring::SetChordWindowSlideEnabled(enable);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"chordWindowSlideEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Repeat-strum holds: off falls back to playing through repeat strums.
		if (std::strstr(requestJson, "probe_repeat_holds_on") != nullptr
			|| std::strstr(requestJson, "probe_repeat_holds_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_repeat_holds_on") != nullptr;
			NoteByNoteNativeScoring::SetRepeatStrumHoldsEnabled(enable);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"repeatStrumHoldsEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// One StartAt-core call on the next idle scoring tick (see RequestNativeSeekTest).
		if (std::strstr(requestJson, "probe_native_seek_test") != nullptr)
		{
			NoteByNoteNativeScoring::RequestNativeSeekTest();
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"nativeSeekTestRequested\":true}");
			return 1;
		}
		// One hold enters the game's own frozen mode.
		if (std::strstr(requestJson, "probe_freeze_mode_test") != nullptr)
		{
			NoteByNoteNativeScoring::RequestFreezeModeTest();
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"freezeModeTestArmed\":true}");
			return 1;
		}
		// Single-note acceptance from the native sounding table with the lesson
		// duration gate. Default ON.
		if (std::strstr(requestJson, "probe_nd_accept_on") != nullptr
			|| std::strstr(requestJson, "probe_nd_accept_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_nd_accept_on") != nullptr;
			NoteByNoteNativeScoring::SetNdAcceptEnabled(enable);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"ndAcceptEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Detection strategy per technique (dev/test only, no GUI). Blend is the default;
		// native/ml force one engine. Commands: probe_detect_{chord,note,bend}_{blend,native,ml}.
		{
			using DetectionStrategy = NoteByNoteNativeScoring::DetectionStrategy;
			struct { const char* technique; void (*setter)(DetectionStrategy); } techniques[] = {
				{ "chord", &NoteByNoteNativeScoring::SetChordDetectionStrategy },
				{ "note",  &NoteByNoteNativeScoring::SetNoteDetectionStrategy },
				{ "bend",  &NoteByNoteNativeScoring::SetBendDetectionStrategy },
			};
			for (const auto& t : techniques)
			{
				char blendCmd[48], nativeCmd[48], mlCmd[48];
				std::snprintf(blendCmd, sizeof(blendCmd), "probe_detect_%s_blend", t.technique);
				std::snprintf(nativeCmd, sizeof(nativeCmd), "probe_detect_%s_native", t.technique);
				std::snprintf(mlCmd, sizeof(mlCmd), "probe_detect_%s_ml", t.technique);
				const char* chosen = nullptr;
				if (std::strstr(requestJson, blendCmd) != nullptr) { t.setter(DetectionStrategy::Blend); chosen = "blend"; }
				else if (std::strstr(requestJson, nativeCmd) != nullptr) { t.setter(DetectionStrategy::NativeOnly); chosen = "native"; }
				else if (std::strstr(requestJson, mlCmd) != nullptr) { t.setter(DetectionStrategy::MlOnly); chosen = "ml"; }
				if (chosen != nullptr)
				{
					std::snprintf(responseBuffer, responseCapacity,
						"{\"ok\":true,\"technique\":\"%s\",\"strategy\":\"%s\"}", t.technique, chosen);
					return 1;
				}
			}
		}
		// Native release (default ON): owned releases run the StartAt core and then the
		// coordinated PlayerSong restart for the music.
		if (std::strstr(requestJson, "probe_native_release_on") != nullptr
			|| std::strstr(requestJson, "probe_native_release_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_native_release_on") != nullptr;
			NoteByNoteNativeScoring::SetNativeReleaseEnabled(enable);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"nativeReleaseEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Native schedule shift: release-side frozen-span compensation via the game's own
		// scheduler service. Default OFF.
		if (std::strstr(requestJson, "probe_schedule_shift_on") != nullptr
			|| std::strstr(requestJson, "probe_schedule_shift_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_schedule_shift_on") != nullptr;
			NoteByNoteNativeScoring::SetScheduleShiftEnabled(enable);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"scheduleShiftEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Timed safety release: default OFF, since a timed release reads as a false accept
		// and masks stuck-hold refusals.
		if (std::strstr(requestJson, "probe_safety_release_on") != nullptr
			|| std::strstr(requestJson, "probe_safety_release_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_safety_release_on") != nullptr;
			NoteByNoteNativeScoring::SetSafetyReleaseEnabled(enable);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"safetyReleaseEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Native freeze announcement: the GE_FreezeOnTag frozen-on-tag byte.
		if (std::strstr(requestJson, "probe_freeze_flag_on") != nullptr
			|| std::strstr(requestJson, "probe_freeze_flag_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_freeze_flag_on") != nullptr;
			NoteByNoteNativeScoring::SetNativeFreezeFlagEnabled(enable);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"nativeFreezeFlagEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Native chord panel: GE_ShowChordDisplay-driven frozen panel.
		if (std::strstr(requestJson, "probe_chord_panel_on") != nullptr
			|| std::strstr(requestJson, "probe_chord_panel_off") != nullptr)
		{
			const bool enable = std::strstr(requestJson, "probe_chord_panel_on") != nullptr;
			NoteByNoteNativeScoring::SetNativeChordPanelEnabled(enable);
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"nativeChordPanelEnabled\":%s}", enable ? "true" : "false");
			return 1;
		}
		// Read-only in-process memory peek. Request:
		// {"command":"probe_peek","addr":<decimal VA>,"len":<bytes, max 256>}. Responds
		// with hex bytes. VirtualQuery-guarded; never writes.
		if (std::strstr(requestJson, "probe_peek") != nullptr)
		{
			const char* addrKey = std::strstr(requestJson, "\"addr\":");
			const char* lenKey = std::strstr(requestJson, "\"len\":");
			if (addrKey == nullptr || lenKey == nullptr)
			{
				std::snprintf(responseBuffer, responseCapacity,
					"{\"ok\":false,\"error\":\"addr and len are required\"}");
				return 1;
			}
			const auto addr = static_cast<uintptr_t>(_strtoui64(addrKey + 7, nullptr, 10));
			auto len = static_cast<size_t>(std::strtoul(lenKey + 6, nullptr, 10));
			if (len > 256) len = 256;

			MEMORY_BASIC_INFORMATION mbi = {};
			const bool readable = VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) != 0
				&& mbi.State == MEM_COMMIT
				&& (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ
					| PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0
				&& (mbi.Protect & PAGE_GUARD) == 0
				&& addr + len <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
			if (!readable)
			{
				std::snprintf(responseBuffer, responseCapacity,
					"{\"ok\":false,\"error\":\"address is not readable\"}");
				return 1;
			}

			std::string hex;
			hex.reserve(len * 2 + 16);
			char pair[3];
			for (size_t i = 0; i < len; ++i)
			{
				std::snprintf(pair, sizeof(pair), "%02x",
					*reinterpret_cast<const uint8_t*>(addr + i));
				hex += pair;
			}
			std::snprintf(responseBuffer, responseCapacity,
				"{\"ok\":true,\"addr\":%llu,\"bytes\":\"%s\"}",
				static_cast<unsigned long long>(addr), hex.c_str());
			return 1;
		}
		return 0;
	}

	// Generic observation hooks. Addresses this probe wants the host to observe live. Empty by
	// default: add rows and reload the probe with the game running, with no host change.
	// slotId is this probe's own tag, echoed back to ObserveGenericHook so one callback tells
	// its hooks apart. The host installs a pass-through detour that always runs the original,
	// so listing an address here can only observe it, never change behavior. A returned
	// pointer stays valid until Shutdown.
	const std::vector<ResearchProtocol::HookRequest>& GenericHookList()
	{
		static const std::vector<ResearchProtocol::HookRequest> hooks =
		{
			// { 0x007E2880u, 1u },  // example: the scoring update, tagged slot 1
		};
		return hooks;
	}

	void __cdecl GetRequestedHooks(
		const ResearchProtocol::HookRequest** requests,
		uint32_t* count)
	{
		const auto& hooks = GenericHookList();
		if (requests != nullptr) *requests = hooks.data();
		if (count != nullptr) *count = static_cast<uint32_t>(hooks.size());
	}

	// Called on every hit of a requested address, on whatever thread runs that function, with
	// the entry register file overlaid on the live stack. ecx is `this` (thiscall) or arg0
	// (fastcall); edx is fastcall arg1; stackArgs[0..] are the remaining stack arguments;
	// returnAddress is the native caller. A hook on a hot per-frame function logs every frame,
	// so rate-limit the address's logging here when chasing one.
	void __cdecl ObserveGenericHook(uint32_t slotId, const ResearchProtocol::HookContext* context)
	{
		if (context == nullptr) return;
		LOG_INFO("(GENERIC HOOK) slot=" << slotId
			<< " caller=0x" << std::hex << context->returnAddress
			<< " ecx=0x" << context->ecx
			<< " edx=0x" << context->edx
			<< " arg0=0x" << context->stackArgs[0]
			<< " arg1=0x" << context->stackArgs[1]
			<< std::dec);
	}

	// Marker-expiry policy seam (hot-reloadable). The host forwards neck-placement sites
	// 2, 4 and 5 here with the native step context. Element identity reads the same way
	// as the host's fade sampling: parent at ctx+0x8 (string at +0xC, fret at +0xD),
	// sub-element at ctx+0xC, state bytes at sub+0x50/0x51.
	//
	// Natural expiry steps the state bytes in stages starting with 0x50; zeroing both bytes
	// at once faults the scoring commit.
	struct NeckElementIdentity
	{
		uintptr_t parent = 0;
		uintptr_t sub = 0;
		int stringIndex = -1;
		int fret = -1;
	};

	bool TryReadNeckElementIdentity(uintptr_t ctx, NeckElementIdentity* out) noexcept
	{
		__try
		{
			const auto parent = *reinterpret_cast<volatile uintptr_t*>(ctx + 0x8);
			const auto sub = *reinterpret_cast<volatile uintptr_t*>(ctx + 0xC);
			if (parent < 0x10000 || (parent & 3) != 0) return false;
			if (sub < 0x10000 || (sub & 3) != 0) return false;
			out->parent = parent;
			out->sub = sub;
			out->stringIndex = *reinterpret_cast<volatile uint8_t*>(parent + 0xC);
			out->fret = *reinterpret_cast<volatile uint8_t*>(parent + 0xD);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Steps one stage of the natural expiry sequence (01 01 -> 00 01 -> 00 00): clears
	// byte 0x50 when set, otherwise byte 0x51. Returns which byte was cleared (0 or 1),
	// or -1 when the element was already fully dark or unreadable.
	int TryStepNeckElementExpiry(uintptr_t sub) noexcept
	{
		__try
		{
			auto flag0 = reinterpret_cast<volatile uint8_t*>(sub + 0x50);
			auto flag1 = reinterpret_cast<volatile uint8_t*>(sub + 0x51);
			if (*flag0 != 0)
			{
				*flag0 = 0;
				return 0;
			}
			if (*flag1 != 0)
			{
				*flag1 = 0;
				return 1;
			}
			return -1;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	constexpr size_t NECK_EXPIRY_CLEARED_CAPACITY = 64;
	uintptr_t neckExpiryClearedSubs[NECK_EXPIRY_CLEARED_CAPACITY] = {};
	size_t neckExpiryClearedCount = 0;
	int neckExpiryHoldString = -1;
	int neckExpiryHoldFret = -1;

	// Read-only color-step context dump: the shared visual+0x50 flag cannot separate the
	// fretboard marker from its highway note, but the color step is surface-aware. Dumps
	// the first 0x40 bytes of the step context for target and non-target elements.
	volatile long neckStepDumpBudget = 0;

	bool TryDumpStepContext(uintptr_t ctx, uint32_t* words, int wordCount) noexcept
	{
		__try
		{
			for (int index = 0; index < wordCount; ++index)
			{
				words[index] = *reinterpret_cast<volatile uint32_t*>(
					ctx + static_cast<uintptr_t>(index) * 4);
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Marker color repaint. The color step 0x7A8E90 derives a marker's lit/dim state from
	// ctx+0x24 (the state the builder baked in, 1 or 3), forced to 2 when the visual's live
	// window answer at (ctx+0xC)+0x50 is nonzero; NeckMarkerColorFetch treats exactly state
	// 2 as the dim bank (+0xA28) and everything else as lit (+0x9F8). The application is
	// one-shot per context (the step consumes ctx+0x2C), so an upcoming note whose approach
	// window opens during a frozen hold paints lit once and stays lit.
	//
	// Fretboard-family contexts (component 0x533365E4) never consume ctx+0x2C during a
	// frozen hold; the application only completes while transport runs, and a dim write
	// left pending through a hold would apply at release under the next target.
	//
	// So the policy repaints during transport, ahead of the reveal: every fretboard-family
	// context for a non-target element is kept dim (state 2) and re-armed (+0x2C nonzero)
	// when already applied, so the game repaints it dim on the next transport frame;
	// target/group elements get the inverse (state 1, re-armed when previously dimmed), so
	// each new target relights at its retarget. The +0x2C arming values are learned from
	// pending contexts (0.34375 lit / 0.46875 dim) rather than hardcoded; no re-arm happens
	// until the needed value has been seen. Chord targets (visualChordId >= 0) suspend all
	// writes. The whitelist covers both selected* and visual* identities.
	volatile long markerDimLogBudget = 60;
	volatile long markerDimPostLogBudget = 48;
	constexpr uint32_t FRETBOARD_MARKER_COMPONENT = 0x533365E4u;

	// Learned +0x2C arming values, as float bits; 0 means not yet observed this load.
	std::atomic<uint32_t> learnedLitColorBits{ 0 };
	std::atomic<uint32_t> learnedDimColorBits{ 0 };

	bool TryClassifyFretboardElement(uintptr_t element, int* placementFamily) noexcept
	{
		__try
		{
			*placementFamily = static_cast<int>(
				*reinterpret_cast<volatile uint32_t*>(element + 0x10));
			if (*placementFamily != 0) return false;
			const auto resource = *reinterpret_cast<volatile uintptr_t*>(element + 0x74);
			if (resource < 0x10000 || (resource & 3) != 0) return false;
			const auto definition = *reinterpret_cast<volatile uintptr_t*>(resource + 0x5C);
			if (definition < 0x10000 || (definition & 3) != 0) return false;
			const auto head = definition + 0xCC;
			auto node = *reinterpret_cast<volatile uintptr_t*>(head);
			for (int guard = 0; guard < 32 && node != head; ++guard)
			{
				if (node < 0x10000 || (node & 3) != 0) return false;
				if (*reinterpret_cast<volatile uint32_t*>(node + 0x8)
					== FRETBOARD_MARKER_COMPONENT)
				{
					return true;
				}
				node = *reinterpret_cast<volatile uintptr_t*>(node);
			}
			return false;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool IsWhitelistedCoordinate(
		const ResearchProtocol::NoteByNoteState& state,
		int stringIndex,
		int fret) noexcept
	{
		if (stringIndex == state.selectedString && fret == state.selectedFret) return true;
		return StoppedPreviewFilter::IsSelectedCoordinate(state, stringIndex, fret);
	}

	bool TryReadMarkerCtxComposition(
		uintptr_t ctx,
		uint32_t* colorState,
		float* pendingColor,
		bool* isMarkerPath) noexcept
	{
		__try
		{
			*isMarkerPath = *reinterpret_cast<volatile uint8_t*>(ctx + 0x28) != 0;
			*colorState = *reinterpret_cast<volatile uint32_t*>(ctx + 0x24);
			*pendingColor = *reinterpret_cast<volatile float*>(ctx + 0x2C);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Applies the repaint plan to one marker context. wantDim selects the desired painted
	// state; armColorBits (nonzero float bits) re-arms an already-applied context so the
	// game repaints it on the next transport frame. Returns a short action tag for the
	// log, or null when nothing changed.
	const char* TryRepaintMarkerCtx(
		uintptr_t ctx,
		bool wantDim,
		uint32_t armColorBits) noexcept
	{
		__try
		{
			auto statePtr = reinterpret_cast<volatile uint32_t*>(ctx + 0x24);
			auto colorPtr = reinterpret_cast<volatile uint32_t*>(ctx + 0x2C);
			const uint32_t current = *statePtr;
			if (wantDim)
			{
				// 1 and 3 are the baked lit states; anything else is already dim or a
				// layout this policy does not understand.
				if (current == 1 || current == 3)
				{
					*statePtr = 2;
					if (*colorPtr == 0)
					{
						if (armColorBits == 0) return "dim-state-only";
						*colorPtr = armColorBits;
						return "dim-rearmed";
					}
					return "dim-pending";
				}
				if (current == 2 && *colorPtr == 0 && armColorBits != 0)
				{
					// Dim state already set (a previous pass) but never applied and not
					// armed; arm it so the repaint actually happens.
					*colorPtr = armColorBits;
					return "dim-armed";
				}
				return nullptr;
			}
			// Relight: only undo state 2, which only this policy stores (builders bake 1 or 3;
			// a natively dimmed element gets state 2 forced at application through
			// visual+0x50, not stored here).
			if (current != 2) return nullptr;
			*statePtr = 1;
			if (*colorPtr == 0)
			{
				if (armColorBits == 0) return "lit-state-only";
				*colorPtr = armColorBits;
				return "lit-rearmed";
			}
			return "lit-pending";
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	void __cdecl ObserveNeckPlacementStep(uint32_t site, void* stepContext)
	{
		const bool isPreCall =
			site == (2u | ResearchProtocol::NECK_PLACEMENT_STEP_PRE);
		const bool isPostCall = site == 2u;
		if ((!isPreCall && !isPostCall) || stepContext == nullptr) return;
		if (!isMarkerDimEnabled.load(std::memory_order_relaxed)) return;

		const auto state = NoteByNoteNativeScoring::GetResearchState();
		// Active whenever the controller owns presentation with a published single-note
		// target, in EVERY phase: applications only complete while transport runs, so the
		// repaint has to be staged outside holds to land before the next reveal.
		if (state.isInitialized == 0 || state.visualString < 0) return;
		if (state.visualChordId >= 0) return;

		NeckElementIdentity identity;
		if (!TryReadNeckElementIdentity(
			reinterpret_cast<uintptr_t>(stepContext),
			&identity))
		{
			return;
		}

		int placementFamily = -1;
		if (!TryClassifyFretboardElement(identity.parent, &placementFamily)) return;

		uint32_t colorState = 0;
		float pendingColor = 0.0f;
		bool isMarkerPath = false;
		if (!TryReadMarkerCtxComposition(
			reinterpret_cast<uintptr_t>(stepContext),
			&colorState, &pendingColor, &isMarkerPath))
		{
			return;
		}
		if (!isMarkerPath) return;

		// Learn the arming values from live pending contexts (0.34375 lit / 0.46875 dim
		// observed); the repaint never invents a value.
		if (pendingColor != 0.0f)
		{
			uint32_t bits = 0;
			std::memcpy(&bits, &pendingColor, sizeof(bits));
			if (colorState == 1 || colorState == 3)
			{
				learnedLitColorBits.store(bits, std::memory_order_relaxed);
			}
			else if (colorState == 2)
			{
				learnedDimColorBits.store(bits, std::memory_order_relaxed);
			}
		}

		if (isPostCall)
		{
			// Composition diagnostic only; never writes.
			if (InterlockedDecrement(&markerDimPostLogBudget) >= 0)
			{
				LOG_INFO("(NBN MARKDIM POST) " << identity.stringIndex << ":"
					<< identity.fret
					<< " state=" << colorState
					<< " pendingColor=" << pendingColor
					<< " phase=" << static_cast<uint32_t>(state.gatePhase)
					<< " sel=" << state.selectedString << ":" << state.selectedFret
					<< " vis=" << state.visualString << ":" << state.visualFret
					<< " ctx=0x" << std::hex
					<< reinterpret_cast<uintptr_t>(stepContext)
					<< std::dec << std::endl);
			}
			return;
		}

		const bool isTarget = IsWhitelistedCoordinate(
			state, identity.stringIndex, identity.fret);
		const uint32_t armBits = isTarget
			? learnedLitColorBits.load(std::memory_order_relaxed)
			: learnedDimColorBits.load(std::memory_order_relaxed);

		const char* action = TryRepaintMarkerCtx(
			reinterpret_cast<uintptr_t>(stepContext),
			!isTarget,
			armBits);
		if (action != nullptr && InterlockedDecrement(&markerDimLogBudget) >= 0)
		{
			LOG_INFO("(NBN MARKDIM) " << action << " " << identity.stringIndex << ":"
				<< identity.fret
				<< " phase=" << static_cast<uint32_t>(state.gatePhase)
				<< " sel=" << state.selectedString << ":" << state.selectedFret
				<< " vis=" << state.visualString << ":" << state.visualFret
				<< " ctx=0x" << std::hex << reinterpret_cast<uintptr_t>(stepContext)
				<< std::dec << std::endl);
		}
	}

	uint8_t __cdecl DescribeSelectedChordFingering(uintptr_t record, char* buffer, uint32_t bufferLength,
		int32_t* lowestPlayedString)
	{
		int lowest = -1;
		const bool described = NoteByNoteNativeScoring::TryDescribeSelectedChordFingering(
			record, buffer, bufferLength, lowest);
		if (lowestPlayedString != nullptr) *lowestPlayedString = lowest;
		return described ? 1 : 0;
	}

	const ResearchProtocol::ProbeApi probeApi =
	{
		ResearchProtocol::PROBE_API_VERSION,
		sizeof(ResearchProtocol::ProbeApi),
		PROBE_NAME,
		PROBE_BUILD_ID.c_str(),
		&InitializeProbe,
		&ShutdownProbe,
		&ProcessScoringUpdate,
		&ProcessHitDecision,
		&ObserveRenderedAttack,
		&StopProbe,
		&GetState,
		#if defined(RSMODS_PUBLIC_RELEASE)
		nullptr,
		#else
		&ArmRenderSnapshot,
		#endif
		#if defined(RSMODS_PUBLIC_RELEASE)
		nullptr,
		#else
		&ObserveNativeDraw,
		#endif
		#if defined(RSMODS_PUBLIC_RELEASE)
		nullptr,
		#else
		&NotifyRenderFrameComplete,
		#endif
		#if defined(RSMODS_PUBLIC_RELEASE)
		nullptr,
		#else
		&ArmNoteDrawListSnapshot,
		#endif
		#if defined(RSMODS_PUBLIC_RELEASE)
		nullptr,
		#else
		&ObserveNoteDrawList,
		#endif
		#if defined(RSMODS_PUBLIC_RELEASE)
		nullptr,
		#else
		&ArmScreenMapSnapshot,
		#endif
		#if defined(RSMODS_PUBLIC_RELEASE)
		nullptr,
		#else
		&HandleProbeCommand,
		#endif
		&PrepareNoteDrawList,
		&CompleteNoteDrawList,
		&GetRequestedHooks,
		&ObserveGenericHook,
		&ObserveNeckPlacementStep,
		#if defined(RSMODS_PUBLIC_RELEASE)
		nullptr,
		#else
		&ObserveFullDraw,
		#endif
		&RequestReArmProbe,
		&DescribeSelectedChordFingering
	};
}

#if defined(RSMODS_PUBLIC_RELEASE)
#define RSMP_PROBE_EXPORT
#else
#define RSMP_PROBE_EXPORT __declspec(dllexport)
#endif
extern "C" RSMP_PROBE_EXPORT const ResearchProtocol::ProbeApi* __cdecl RSMP_GetResearchProbeApi(
	uint32_t hostApiVersion,
	const ResearchProtocol::HostApi* hostApi)
{
	if (hostApiVersion != ResearchProtocol::HOST_API_VERSION) return nullptr;
	if (!ResearchProbeRuntime::Initialize(hostApi)) return nullptr;
	return &probeApi;
}
