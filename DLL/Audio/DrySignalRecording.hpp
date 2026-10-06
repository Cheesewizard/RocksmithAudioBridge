#pragma once

#include "CaptureProcessingGate.hpp"
#include "GameAudioRecorder.hpp"

namespace Audio::DrySignalRecording
{
	inline CaptureProcessingGate gate;
	inline RecordingSession* session = nullptr;
	inline std::atomic<uint64_t> lastPacketTick{ 0 };
	inline std::atomic<uint32_t> lastSampleRate{ 0 };

	inline bool IsReady()
	{
		const auto tick = lastPacketTick.load();
		return tick != 0 && GetTickCount64() - tick < 1000 && lastSampleRate.load() == 48000;
	}

	// Control callers serialize attachment and detachment; the input callback never waits.
	inline HRESULT Attach(RecordingSession& target)
	{
		if (session) return HRESULT_FROM_WIN32(ERROR_BUSY);
		if (!IsReady()) return AUDCLNT_E_DEVICE_INVALIDATED;
		session = &target;
		gate.Open();
		return S_OK;
	}

	inline void Detach()
	{
		if (!session) return;
		gate.CloseAndWait();
		session = nullptr;
	}

	inline void Observe(const float* mono, uint32_t frames, uint32_t sampleRate)
	{
		if (!mono || !frames) return;
		lastSampleRate.store(sampleRate);
		lastPacketTick.store(GetTickCount64());
		CaptureCallbackScope callback(gate);
		if (callback && session) session->SubmitDry(mono, frames, sampleRate);
	}
}
