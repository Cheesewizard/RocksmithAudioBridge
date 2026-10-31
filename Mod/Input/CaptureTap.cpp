#include "CaptureTap.hpp"

#include "Framework/AudioInput.hpp"
#include "Log.hpp"
#include "MemUtil.hpp"

#include "CaptureProcessingGate.hpp"
#include "CaptureSamples.hpp"
#include "CaptureState.hpp"
#include "ComVTable.hpp"
#include "InputStageMeter.hpp"
#include "RsAsioConfig.hpp"
#include "../AudioBridge/Cable/CableInput.hpp"
#include "../Core/Hooking.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <audioclient.h>
#include <cstring>
#include <mutex>
#include <psapi.h>
#include <vector>

namespace AudioBridge::Input::CaptureTap
{
	namespace
	{
		constexpr std::size_t SLOT_CAPTURE_CLIENT_GET_BUFFER = 3;
		// The chain's maxFrames; a longer packet passes untouched.
		constexpr std::uint32_t MAX_BUFFER_FRAMES = 4096;
		constexpr std::uint64_t INSTALL_RETRY_MILLISECONDS = 250;
		// A bound stream that delivered a buffer this recently is alive and is never replaced by a newcomer.
		constexpr std::uint64_t STREAM_ALIVE_MILLISECONDS = 2000;

		// The call into PortAudio's WASAPI stream unmarshal, before and after RS_ASIO patches it (Sept 2022), and in
		// Learn & Play.
		constexpr std::array<std::uint8_t, 11> UNMARSHAL_CALL_PRE_PATCH =
		{
			0xe8, 0x59, 0xdf, 0xff, 0xff, 0x57, 0xe8, 0x33, 0xe0, 0xff, 0xff
		};
		constexpr std::array<std::uint8_t, 11> UNMARSHAL_CALL_POST_PATCH =
		{
			0xe8, 0x97, 0xe2, 0xff, 0xff, 0x57, 0xe8, 0x71, 0xe3, 0xff, 0xff
		};
		constexpr std::array<std::uint8_t, 11> UNMARSHAL_CALL_LEARN_AND_PLAY =
		{
			0xe8, 0xd6, 0xfd, 0xff, 0xff, 0x56, 0xe8, 0xb0, 0xfe, 0xff, 0xff
		};

		// The leading fields of PortAudio's PaWasapiSubStream / PaWasapiStream in the game's build.
		struct PaWasapiSubStreamPrefix
		{
			IUnknown* clientParent;
			IUnknown* clientStream;
			IUnknown* clientProc;
			WAVEFORMATEXTENSIBLE waveFormat;
			std::uint8_t remaining[0xdc - sizeof(WAVEFORMATEXTENSIBLE)];
		};

		struct PaWasapiStreamPrefix
		{
			std::uint8_t prefix[0x108];
			PaWasapiSubStreamPrefix input;
			IUnknown* captureClientParent;
			IUnknown* captureClientStream;
			IAudioCaptureClient* captureClient;
		};

		static_assert(sizeof(void*) == 4, "Rocksmith is a 32-bit game");
		static_assert(offsetof(PaWasapiStreamPrefix, input.waveFormat) == 0x114, "Unexpected PortAudio input format offset");
		static_assert(offsetof(PaWasapiStreamPrefix, captureClientParent) == 0x1f0, "Unexpected PortAudio capture parent offset");
		static_assert(offsetof(PaWasapiStreamPrefix, captureClient) == 0x1f8, "Unexpected PortAudio capture client offset");

		using UnmarshalStreamComPointers_t = HRESULT(__cdecl*)(void*);
		using CaptureGetBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IAudioCaptureClient*, BYTE**, UINT32*, DWORD*, UINT64*, UINT64*);

		enum class InstallResult
		{
			Installed,
			Retry,
			Failed
		};

		// One player's capture stream. format and the bridge state change only with the route's gate closed, so a
		// GetBuffer call that got in sees one consistent binding.
		struct CaptureRoute
		{
			std::atomic<bool> configured{ false };   // set once by Install
			std::atomic<IAudioCaptureClient*> client{ nullptr };
			PacketFormat format;
			std::atomic<ICaptureState*> bridgeState{ nullptr };   // owns a reference
			std::atomic<UINT64> bridgeGeneration{ 0 };
			std::atomic<std::uint64_t> lastBufferTick{ 0 };
			std::atomic<bool> needsPrepare{ false };
			std::atomic<bool> prepared{ false };
			Audio::CaptureProcessingGate gate;
			std::array<float, MAX_BUFFER_FRAMES> samples{};   // that stream's audio thread only
		};

		std::array<CaptureRoute, PLAYER_COUNT> routes;
		std::mutex registrationMutex;
		std::atomic<bool> tapActive{ false };

		UnmarshalStreamComPointers_t originalUnmarshalStreamComPointers = nullptr;
		CaptureGetBuffer_t originalCaptureGetBuffer = nullptr;
		void** captureClientVTable = nullptr;
		std::uint8_t* unmarshalTarget = nullptr;
		bool installStarted = false;
		bool installPending = false;
		std::uint64_t nextInstallAttemptTick = 0;
		bool loggedWaitingForRsAsio = false;
		bool loggedWaitingForRsAsioPatch = false;

		int FindRoute(IAudioCaptureClient* captureClient)
		{
			for (std::uint32_t index = 0; index < PLAYER_COUNT; ++index)
			{
				if (routes[index].client.load(std::memory_order_acquire) == captureClient) return static_cast<int>(index);
			}
			return -1;
		}

		HRESULT STDMETHODCALLTYPE Hook_CaptureGetBuffer(IAudioCaptureClient* self, BYTE** data, UINT32* frameCount,
			DWORD* flags, UINT64* devicePosition, UINT64* performanceCounterPosition)
		{
			// PortAudio passes null for the timestamp; substitute our own so the packet's device capture time is
			// available for the cable's measured input lag (4.x's capture hook did the same).
			UINT64 localQpc = 0;
			UINT64* qpcOut = performanceCounterPosition ? performanceCounterPosition : &localQpc;
			const HRESULT result = originalCaptureGetBuffer(self, data, frameCount, flags, devicePosition, qpcOut);
			if (FAILED(result) || result == AUDCLNT_S_BUFFER_EMPTY || !frameCount || *frameCount == 0) return result;

			const int index = FindRoute(self);
			if (index < 0) return result;
			CaptureRoute& route = routes[index];
			Audio::CaptureCallbackScope callback(route.gate);
			if (!callback) return result;

			// The bridge's placeholder while the real device is away, or a device that changed under the stream: the
			// game hears silence, and the stream doesn't count as alive.
			if (ICaptureState* state = route.bridgeState.load(std::memory_order_acquire))
			{
				if (!state->IsPhysicalPacket() || state->GetGeneration() != route.bridgeGeneration.load(std::memory_order_acquire))
				{
					route.lastBufferTick.store(0, std::memory_order_relaxed);
					StageMeter::NoteReplacedWithSilence(static_cast<std::uint32_t>(index));
					if (flags) *flags |= AUDCLNT_BUFFERFLAGS_SILENT;
					return result;
				}
			}
			route.lastBufferTick.store(GetTickCount64(), std::memory_order_relaxed);

			// Cable mode, Player 1: how old the packet is when the game gets it (CableInput's evidence line).
			if (index == 0 && !Audio::CableInput::IsAsioPath())
			{
				if (*qpcOut == 0 || (flags && (*flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)))
				{
					Audio::CableInput::ReportCaptureTimestampLag(-1.0);
				}
				else
				{
					static const LARGE_INTEGER frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
					LARGE_INTEGER counter{};
					QueryPerformanceCounter(&counter);
					const double now100ns = frequency.QuadPart > 0 ? counter.QuadPart * 10000000.0 / frequency.QuadPart : 0.0;
					const double delta100ns = now100ns - static_cast<double>(*qpcOut);
					Audio::CableInput::ReportMeasuredInputRaw(static_cast<std::int64_t>(delta100ns), *frameCount);
					Audio::CableInput::ReportCaptureTimestampLag(delta100ns / 10000.0);
				}
			}

			if (!tapActive.load(std::memory_order_acquire) || !route.prepared.load(std::memory_order_acquire)
				|| !data || !*data || *frameCount > MAX_BUFFER_FRAMES) return result;

			// A silent packet goes through the chain as zeros, so processors and observers keep a continuous timeline,
			// and the game keeps its silent packet.
			const bool silent = flags && (*flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
			StageMeter::BeginPacket(static_cast<std::uint32_t>(index), silent);
			float* samples = route.samples.data();
			if (silent) std::fill_n(samples, *frameCount, 0.0f);
			else ReadFirstChannel(*data, route.format, *frameCount, samples);

			if (Framework::AudioInput().ProcessTap(static_cast<std::uint32_t>(index), &samples, *frameCount) && !silent)
				WriteAllChannels(*data, route.format, *frameCount, samples);
			return result;
		}

		// Assigns a newly unmarshalled capture stream to a player. Runs on the thread the game opens streams on.
		void RegisterCaptureStream(PaWasapiStreamPrefix* stream)
		{
			if (!stream || !stream->input.clientParent || !stream->captureClient) return;
			std::lock_guard<std::mutex> guard(registrationMutex);
			if (FindRoute(stream->captureClient) >= 0) return;

			std::uint32_t index = PLAYER_COUNT;
			bool replacesCapture = false;
			for (std::uint32_t candidate = 0; candidate < PLAYER_COUNT; ++candidate)
			{
				if (routes[candidate].configured && !routes[candidate].client.load(std::memory_order_relaxed))
				{
					index = candidate;
					break;
				}
			}

			if (index == PLAYER_COUNT)
			{
				// Leaving a song and reloading makes the game abandon its capture stream and open a new one. With one
				// configured player the newest stream is the live one; with two, which player moved is unknown.
				const auto configuredCount = std::count_if(routes.begin(), routes.end(),
					[](const CaptureRoute& route) { return route.configured.load(std::memory_order_acquire); });
				if (configuredCount != 1)
				{
					LOG_WARNING("[InputTap] Ignoring an additional capture stream after every configured player was bound." << std::endl);
					return;
				}
				index = 0;
				while (!routes[index].configured) ++index;

				// The game and RS_ASIO open several capture streams, and one may carry the song instead of the guitar.
				// A binding still delivering buffers is never stolen; one quiet for two seconds yields.
				const std::uint64_t lastBuffer = routes[index].lastBufferTick.load(std::memory_order_relaxed);
				if (lastBuffer != 0 && GetTickCount64() - lastBuffer < STREAM_ALIVE_MILLISECONDS)
				{
					LOG_INFO("[InputTap] Ignoring a new capture stream while Player " << index + 1
						<< "'s bound stream is still delivering buffers." << std::endl);
					return;
				}
				replacesCapture = true;
			}

			const PacketFormat format = ReadPacketFormat(stream->input.waveFormat);
			if (!format.IsUsable())
			{
				const WAVEFORMATEX& waveFormat = stream->input.waveFormat.Format;
				LOG_ERROR("[InputTap] Player " << index + 1 << " negotiated an unsupported capture format: tag "
					<< waveFormat.wFormatTag << ", " << waveFormat.nSamplesPerSec << " Hz, " << waveFormat.nChannels
					<< " channel(s), " << waveFormat.wBitsPerSample << " bits." << std::endl);
				return;
			}

			void** vTable = Audio::ComVTable::GetVTable(stream->captureClient);
			if (!captureClientVTable)
			{
				originalCaptureGetBuffer = reinterpret_cast<CaptureGetBuffer_t>(vTable[SLOT_CAPTURE_CLIENT_GET_BUFFER]);
				void* replaced = Audio::ComVTable::PatchSlot(stream->captureClient, SLOT_CAPTURE_CLIENT_GET_BUFFER,
					reinterpret_cast<void*>(&Hook_CaptureGetBuffer));
				if (!replaced || replaced != reinterpret_cast<void*>(originalCaptureGetBuffer))
				{
					originalCaptureGetBuffer = nullptr;
					LOG_ERROR("[InputTap] Could not patch IAudioCaptureClient::GetBuffer." << std::endl);
					return;
				}
				captureClientVTable = vTable;
			}
			else if (captureClientVTable != vTable)
			{
				// With the Player 2 cable always listed, the game opens its stream just after Player 1's RS_ASIO stream,
				// before that one delivered a buffer. It's a different device, not a replacement.
				if (replacesCapture)
				{
					LOG_INFO("[InputTap] Ignoring a capture stream from a different implementation while Player "
						<< index + 1 << " stays bound." << std::endl);
					return;
				}
				LOG_ERROR("[InputTap] Player " << index + 1 << " uses a different capture implementation; its input is not tapped." << std::endl);
				return;
			}

			CaptureRoute& route = routes[index];
			route.gate.CloseAndWait();
			if (route.prepared.exchange(false, std::memory_order_acq_rel)) Framework::AudioInput().ReleaseTap(index);

			ICaptureState* state = nullptr;
			if (FAILED(stream->captureClient->QueryInterface(__uuidof(ICaptureState), reinterpret_cast<void**>(&state)))) state = nullptr;
			if (ICaptureState* previous = route.bridgeState.exchange(state, std::memory_order_acq_rel)) previous->Release();
			route.bridgeGeneration.store(state ? state->GetGeneration() : 0, std::memory_order_release);
			route.format = format;
			route.lastBufferTick.store(0, std::memory_order_relaxed);
			route.client.store(stream->captureClient, std::memory_order_release);
			route.needsPrepare.store(true, std::memory_order_release);
			route.gate.Open();

			if (replacesCapture)
				LOG_INFO("[InputTap] Player " << index + 1 << "'s capture stream went quiet and was replaced; rebinding to the newest stream." << std::endl);
			LOG_INFO("[InputTap] Player " << index + 1 << " bound to the game capture client " << stream->captureClient
				<< " (" << DescribePacketFormat(format) << (state ? ", Audio Bridge device" : "") << ")." << std::endl);
		}

		HRESULT __cdecl Hook_UnmarshalStreamComPointers(void* stream)
		{
			const HRESULT result = originalUnmarshalStreamComPointers(stream);
			if (SUCCEEDED(result) && stream) RegisterCaptureStream(reinterpret_cast<PaWasapiStreamPrefix*>(stream));
			return result;
		}

		bool IsRangeInsideModule(const void* address, std::size_t size, const MODULEINFO& moduleInfo)
		{
			const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(moduleInfo.lpBaseOfDll);
			const std::uintptr_t end = start + moduleInfo.SizeOfImage;
			const std::uintptr_t rangeStart = reinterpret_cast<std::uintptr_t>(address);
			return rangeStart >= start && rangeStart <= end && size <= end - rangeStart;
		}

		template<std::size_t SIZE>
		void FindPattern(const MODULEINFO& moduleInfo, const std::array<std::uint8_t, SIZE>& pattern, std::vector<std::uint8_t*>& matches)
		{
			auto* start = static_cast<std::uint8_t*>(moduleInfo.lpBaseOfDll);
			auto* end = start + moduleInfo.SizeOfImage;
			for (auto* cursor = std::search(start, end, pattern.begin(), pattern.end()); cursor != end;
				cursor = std::search(cursor + 1, end, pattern.begin(), pattern.end()))
			{
				matches.push_back(cursor);
			}
		}

		bool FindUnmarshalTarget(const MODULEINFO& gameInfo)
		{
			std::vector<std::uint8_t*> callSites;
			FindPattern(gameInfo, UNMARSHAL_CALL_PRE_PATCH, callSites);
			FindPattern(gameInfo, UNMARSHAL_CALL_POST_PATCH, callSites);
			FindPattern(gameInfo, UNMARSHAL_CALL_LEARN_AND_PLAY, callSites);

			std::vector<std::uint8_t*> targets;
			for (std::uint8_t* callSite : callSites)
			{
				std::int32_t relative = 0;
				std::memcpy(&relative, callSite + 1, sizeof(relative));
				std::uint8_t* target = callSite + 5 + relative;
				if (!IsRangeInsideModule(target, 6, gameInfo))
				{
					LOG_ERROR("[InputTap] A PortAudio unmarshal call targets outside the Rocksmith image; the input is not tapped." << std::endl);
					return false;
				}
				if (std::find(targets.begin(), targets.end(), target) == targets.end()) targets.push_back(target);
			}

			if (targets.size() != 1)
			{
				LOG_ERROR("[InputTap] Expected one PortAudio unmarshal target, found " << targets.size() << " across "
					<< callSites.size() << " call sites; the input is not tapped." << std::endl);
				return false;
			}
			unmarshalTarget = targets[0];
			return true;
		}

		// Cable mode: detour the unmarshal function itself, which covers every caller (the input stream reaches it
		// through a different caller than the two direct call sites).
		InstallResult DetourNativeUnmarshal()
		{
			if (IsRsAsioEnabled())
			{
				if (!loggedWaitingForRsAsio)
				{
					loggedWaitingForRsAsio = true;
					LOG_INFO("[InputTap] RS_ASIO.dll is enabled but not loaded yet; retrying." << std::endl);
				}
				return InstallResult::Retry;
			}

			const PBYTE trampoline = AudioBridge::Hooking::DetourFunction(unmarshalTarget, reinterpret_cast<PBYTE>(&Hook_UnmarshalStreamComPointers));
			if (trampoline == nullptr)
			{
				LOG_ERROR("[InputTap] Could not detour the PortAudio unmarshal function; the input is not tapped." << std::endl);
				return InstallResult::Failed;
			}
			originalUnmarshalStreamComPointers = reinterpret_cast<UnmarshalStreamComPointers_t>(trampoline);
			LOG_INFO("[InputTap] Real Tone Cable capture hook installed." << std::endl);
			return InstallResult::Installed;
		}

		// RS_ASIO patches the unmarshal function's entry into "push <its function>; ret". Chain in front of it, so
		// RS_ASIO keeps doing its own work and no second ASIO host is created.
		InstallResult ChainRsAsioUnmarshal(HMODULE rsAsioModule)
		{
			MODULEINFO rsAsioInfo{};
			if (!GetModuleInformation(GetCurrentProcess(), rsAsioModule, &rsAsioInfo, sizeof(rsAsioInfo)))
			{
				LOG_ERROR("[InputTap] Could not inspect RS_ASIO.dll, error " << GetLastError() << "." << std::endl);
				return InstallResult::Failed;
			}

			if (unmarshalTarget[0] != 0x68 || unmarshalTarget[5] != 0xc3)
			{
				if (!loggedWaitingForRsAsioPatch)
				{
					loggedWaitingForRsAsioPatch = true;
					LOG_INFO("[InputTap] RS_ASIO has not patched the PortAudio unmarshal function yet; retrying." << std::endl);
				}
				return InstallResult::Retry;
			}

			void* rsAsioUnmarshal = nullptr;
			std::memcpy(&rsAsioUnmarshal, unmarshalTarget + 1, sizeof(rsAsioUnmarshal));
			if (!IsRangeInsideModule(rsAsioUnmarshal, 1, rsAsioInfo))
			{
				LOG_ERROR("[InputTap] The patched unmarshal target is outside RS_ASIO.dll; the input is not tapped." << std::endl);
				return InstallResult::Failed;
			}

			originalUnmarshalStreamComPointers = reinterpret_cast<UnmarshalStreamComPointers_t>(rsAsioUnmarshal);
			void* replacement = reinterpret_cast<void*>(&Hook_UnmarshalStreamComPointers);
			if (!MemUtil::PatchAdr(unmarshalTarget + 1, &replacement, sizeof(replacement)))
			{
				originalUnmarshalStreamComPointers = nullptr;
				LOG_ERROR("[InputTap] Could not chain RS_ASIO's unmarshal patch." << std::endl);
				return InstallResult::Failed;
			}
			LOG_INFO("[InputTap] Chained onto RS_ASIO's capture path." << std::endl);
			return InstallResult::Installed;
		}

		InstallResult InstallUnmarshalHook()
		{
			if (!unmarshalTarget)
			{
				HMODULE gameModule = GetModuleHandleA(nullptr);
				MODULEINFO gameInfo{};
				if (!gameModule || !GetModuleInformation(GetCurrentProcess(), gameModule, &gameInfo, sizeof(gameInfo)))
				{
					LOG_ERROR("[InputTap] Could not inspect the Rocksmith executable, error " << GetLastError() << "." << std::endl);
					return InstallResult::Failed;
				}
				if (!FindUnmarshalTarget(gameInfo)) return InstallResult::Failed;
			}

			HMODULE rsAsioModule = GetModuleHandleA("RS_ASIO.dll");
			return rsAsioModule ? ChainRsAsioUnmarshal(rsAsioModule) : DetourNativeUnmarshal();
		}

		void AttemptInstall()
		{
			switch (InstallUnmarshalHook())
			{
			case InstallResult::Installed:
			case InstallResult::Failed:
				installPending = false;
				break;
			case InstallResult::Retry:
				nextInstallAttemptTick = GetTickCount64() + INSTALL_RETRY_MILLISECONDS;
				break;
			}
		}

		// Configures which players have an input. False when none does, or one is misconfigured.
		bool ConfigureRoutes()
		{
			if (!IsRsAsioEnabled())
			{
				ReportUnreadableRsAsioConfiguration();
				routes[0].configured = true;
				LOG_INFO("[InputTap] Real Tone Cable input for Player 1." << std::endl);
				return true;
			}

			const RsAsioInputs inputs = ReadRsAsioInputs();
			bool anyConfigured = false;
			for (std::uint32_t index = 0; index < PLAYER_COUNT; ++index)
			{
				if (!inputs.configured[index]) continue;
				if (inputs.channels[index] < 0)
				{
					LOG_ERROR("[InputTap] Player " << index + 1 << " has no valid Channel in RS_ASIO.ini; the input is not tapped." << std::endl);
					return false;
				}
				routes[index].configured = true;
				anyConfigured = true;
				LOG_INFO("[InputTap] Player " << index + 1 << " follows RS_ASIO's " << inputs.sources[index]
					<< ", ASIO channel " << inputs.channels[index] << "." << std::endl);
				if (inputs.inferred[index])
					LOG_WARNING("[InputTap] Player " << index + 1 << "'s input was inferred: no [Asio.Input.N] section named a driver." << std::endl);
			}

			if (!anyConfigured)
				LOG_ERROR("[InputTap] RS_ASIO is enabled but RS_ASIO.ini configures no input." << std::endl);
			return anyConfigured;
		}

		void ReleaseRoute(std::uint32_t index)
		{
			if (routes[index].prepared.exchange(false, std::memory_order_acq_rel)) Framework::AudioInput().ReleaseTap(index);
		}

		void PrepareRoute(std::uint32_t index)
		{
			CaptureRoute& route = routes[index];
			route.needsPrepare.store(false, std::memory_order_relaxed);
			const std::uint32_t latency = Framework::AudioInput().PrepareTap(index,
				{ route.format.sampleRate, 1, MAX_BUFFER_FRAMES });
			if (!Framework::AudioInput().TapLive(index))
			{
				LOG_ERROR("[InputTap] The input chain refused Player " << index + 1 << "'s stream ("
					<< DescribePacketFormat(route.format) << ")." << std::endl);
				return;
			}
			route.prepared.store(true, std::memory_order_release);
			LOG_INFO("[InputTap] Player " << index + 1 << "'s input runs through the chain, " << latency
				<< " frames of processing latency." << std::endl);
		}
	}

	void Install()
	{
		std::lock_guard<std::mutex> guard(registrationMutex);
		if (installStarted) return;
		installStarted = true;

		if (!ConfigureRoutes()) return;
		installPending = true;
		AttemptInstall();
	}

	void Tick()
	{
		std::lock_guard<std::mutex> guard(registrationMutex);
		if (installPending && GetTickCount64() >= nextInstallAttemptTick) AttemptInstall();
		if (!tapActive.load(std::memory_order_acquire)) return;

		for (std::uint32_t index = 0; index < PLAYER_COUNT; ++index)
		{
			CaptureRoute& route = routes[index];
			if (!route.client.load(std::memory_order_acquire)) continue;

			// The bridge swapped the device behind the game's capture client: start the chain fresh for it.
			ICaptureState* state = route.bridgeState.load(std::memory_order_acquire);
			if (state && state->GetGeneration() != route.bridgeGeneration.load(std::memory_order_acquire))
			{
				route.gate.CloseAndWait();
				route.bridgeGeneration.store(state->GetGeneration(), std::memory_order_release);
				route.needsPrepare.store(true, std::memory_order_release);
				route.gate.Open();
				LOG_INFO("[InputTap] Player " << index + 1 << "'s input device changed; preparing the chain again." << std::endl);
			}

			if (route.needsPrepare.load(std::memory_order_acquire)) PrepareRoute(index);
		}
	}

	void SetActive(bool active)
	{
		std::lock_guard<std::mutex> guard(registrationMutex);
		tapActive.store(active, std::memory_order_release);
		if (active)
		{
			for (CaptureRoute& route : routes)
			{
				if (route.client.load(std::memory_order_acquire)) route.needsPrepare.store(true, std::memory_order_release);
			}
			return;
		}
		for (std::uint32_t index = 0; index < PLAYER_COUNT; ++index) ReleaseRoute(index);
	}

	bool IsPlayerConfigured(std::uint32_t player)
	{
		return player < PLAYER_COUNT && routes[player].configured.load(std::memory_order_acquire);
	}

	bool IsPlayerLive(std::uint32_t player)
	{
		return player < PLAYER_COUNT && routes[player].prepared.load(std::memory_order_acquire);
	}

	// The hooks stay for the life of the process; with every gate closed they pass buffers through untouched.
	void Shutdown()
	{
		std::lock_guard<std::mutex> guard(registrationMutex);
		tapActive.store(false, std::memory_order_release);
		for (std::uint32_t index = 0; index < PLAYER_COUNT; ++index)
		{
			routes[index].gate.CloseAndWait();
			ReleaseRoute(index);
			if (ICaptureState* state = routes[index].bridgeState.exchange(nullptr, std::memory_order_acq_rel)) state->Release();
		}
	}
}
