#pragma once

#include "AudioBridge/Input/CaptureProcessingGate.hpp"
#include "AudioBridge/Input/InputCaptureHook.hpp"
#include "GameAudioRecorder.hpp"

namespace Audio::DrySignalRecording
{
	// One slot per input route: route 0 is Player 1, route 1 is Player 2. Each player's dry input is
	// recorded to its own WAV next to the shared wet mix (the game mixes both players' tones into one
	// output before we see it, so there is no per-player wet).
	constexpr size_t ROUTES = AsioHook::INPUT_ROUTE_COUNT;

	struct Route
	{
		CaptureProcessingGate gate;
		RecordingSession* session = nullptr;
		std::atomic<uint64_t> lastPacketTick{ 0 };
		std::atomic<uint32_t> lastSampleRate{ 0 };
	};
	inline Route routes[ROUTES];

	inline bool IsReady(size_t route = 0)
	{
		if (route >= ROUTES) return false;
		const auto tick = routes[route].lastPacketTick.load();
		return tick != 0 && GetTickCount64() - tick < 1000 && routes[route].lastSampleRate.load() == 48000;
	}

	// Control callers serialize attachment and detachment; the input callback never waits.
	inline HRESULT Attach(size_t route, RecordingSession& target)
	{
		if (route >= ROUTES) return E_INVALIDARG;
		Route& slot = routes[route];
		if (slot.session) return HRESULT_FROM_WIN32(ERROR_BUSY);
		if (!IsReady(route)) return AUDCLNT_E_DEVICE_INVALIDATED;
		slot.session = &target;
		slot.gate.Open();
		return S_OK;
	}

	inline void Detach(size_t route)
	{
		if (route >= ROUTES) return;
		Route& slot = routes[route];
		if (!slot.session) return;
		slot.gate.CloseAndWait();
		slot.session = nullptr;
	}

	inline void Observe(size_t route, const float* mono, uint32_t frames, uint32_t sampleRate)
	{
		if (route >= ROUTES || !mono || !frames) return;
		Route& slot = routes[route];
		slot.lastSampleRate.store(sampleRate);
		slot.lastPacketTick.store(GetTickCount64());
		CaptureCallbackScope callback(slot.gate);
		if (callback && slot.session) slot.session->SubmitDry(route, mono, frames, sampleRate);
	}
}
