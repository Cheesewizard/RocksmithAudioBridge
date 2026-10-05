#include "ResearchProbeRuntime.hpp"

#include <cmath>
#include <mutex>

namespace
{
	std::mutex hostApiMutex;
	const ResearchProtocol::HostApi* activeHostApi = nullptr;

	const ResearchProtocol::HostApi* GetHostApi()
	{
		std::lock_guard<std::mutex> lock(hostApiMutex);
		return activeHostApi;
	}
}

bool ResearchProbeRuntime::Initialize(const ResearchProtocol::HostApi* hostApi)
{
	if (hostApi == nullptr
		|| hostApi->version != ResearchProtocol::HOST_API_VERSION
		|| hostApi->structSize < sizeof(ResearchProtocol::HostApi)
		|| hostApi->IsNoteByNoteEnabled == nullptr
		|| hostApi->HandleControllerFault == nullptr
		|| hostApi->PublishExpectedAttackEvent == nullptr
		|| hostApi->QueryMlNoteEvidence == nullptr
		|| hostApi->QueryMlChordEvidence == nullptr
		|| hostApi->GetMlAudioSampleIndex == nullptr
		|| hostApi->Log == nullptr
		|| hostApi->QueryRawToneComb == nullptr
		|| hostApi->QueryRawNoteConfirmation == nullptr
		|| hostApi->QueryRawAttacks == nullptr
		|| hostApi->CaptureRawSnapshot == nullptr
		|| hostApi->GetSongSpeedPercent == nullptr
		|| hostApi->SetSongSpeedPercent == nullptr
		|| hostApi->IsFlowModeEnabled == nullptr
		|| hostApi->GetPlayerSpeedRealPercent == nullptr
		|| hostApi->ConsumeNoteNavigation == nullptr)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(hostApiMutex);
	activeHostApi = hostApi;
	return true;
}

void ResearchProbeRuntime::Shutdown()
{
	std::lock_guard<std::mutex> lock(hostApiMutex);
	activeHostApi = nullptr;
}

bool ResearchProbeRuntime::IsNoteByNoteEnabled()
{
	const auto hostApi = GetHostApi();
	return hostApi != nullptr && hostApi->IsNoteByNoteEnabled() != 0;
}

void ResearchProbeRuntime::HandleControllerFault(const std::string& reason)
{
	const auto hostApi = GetHostApi();
	if (hostApi != nullptr) hostApi->HandleControllerFault(reason.c_str());
}

void ResearchProbeRuntime::PublishExpectedAttackEvent(
	const ResearchProtocol::ExpectedAttackEvent& event)
{
	const auto hostApi = GetHostApi();
	if (hostApi != nullptr) hostApi->PublishExpectedAttackEvent(&event);
}

void ResearchProbeRuntime::Log(ResearchProtocol::LogLevel level, const std::string& message)
{
	const auto hostApi = GetHostApi();
	if (hostApi != nullptr) hostApi->Log(level, message.c_str());
}

int ResearchProbeRuntime::GetInputOnsetShiftSemitones()
{
	const auto hostApi = GetHostApi();
	// Initialize already required structSize >= sizeof(HostApi), so the entry is present when
	// a host is active; guard the pointer for an older host that left it null.
	if (hostApi == nullptr || hostApi->GetInputOnsetShiftSemitones == nullptr) return 0;
	return hostApi->GetInputOnsetShiftSemitones();
}

bool ResearchProbeRuntime::QueryRawNoteConfirmation(double frequencyHz, ResearchProtocol::RawNoteConfirmation& out, uint64_t minimumSampleIndex, uint64_t maximumSampleIndex)
{
	out = {};
	const auto hostApi = GetHostApi();
	return hostApi != nullptr && hostApi->QueryRawNoteConfirmation(frequencyHz, minimumSampleIndex, maximumSampleIndex, &out) != 0;
}

bool ResearchProbeRuntime::QueryRawToneComb(double frequencyHz, ResearchProtocol::RawToneComb& out)
{
	out = {};
	const auto hostApi = GetHostApi();
	if (hostApi == nullptr) return false;
	return hostApi->QueryRawToneComb(frequencyHz, &out) != 0;
}

bool ResearchProbeRuntime::QueryRawToneEvidence(double frequencyHz, float windowSeconds,
	ResearchProtocol::RawToneEvidence& out)
{
	const auto hostApi = GetHostApi();
	if (hostApi == nullptr || hostApi->QueryRawToneEvidence == nullptr) return false;
	out.structSize = sizeof(ResearchProtocol::RawToneEvidence);
	return hostApi->QueryRawToneEvidence(frequencyHz, windowSeconds, &out) != 0;
}

bool ResearchProbeRuntime::QueryMlPitch(float& outMidi, float& outConfidence,
	double& outAgeSeconds)
{
	const auto hostApi = GetHostApi();
	// Initialize's structSize gate guarantees the appended entries exist on any accepted
	// host; the null guard covers a same-size host that left them unwired.
	if (hostApi == nullptr || hostApi->QueryMlPitch == nullptr) return false;
	return hostApi->QueryMlPitch(&outMidi, &outConfidence, &outAgeSeconds) != 0;
}

bool ResearchProbeRuntime::IsMlPitchServiceAlive()
{
	const auto hostApi = GetHostApi();
	if (hostApi == nullptr || hostApi->IsMlPitchServiceAlive == nullptr) return false;
	return hostApi->IsMlPitchServiceAlive() != 0;
}

uint64_t ResearchProbeRuntime::GetMlAudioSampleIndex()
{
	const auto hostApi = GetHostApi();
	return hostApi == nullptr ? 0 : hostApi->GetMlAudioSampleIndex();
}

bool ResearchProbeRuntime::QueryMlNoteEvidence(int expectedMidi, float minConfidence,
	uint64_t minimumSampleIndex, ResearchProtocol::MlNoteEvidence& evidence)
{
	const auto hostApi = GetHostApi();
	evidence = {};
	if (hostApi == nullptr) return false;
	return hostApi->QueryMlNoteEvidence(expectedMidi, minConfidence,
		minimumSampleIndex, &evidence) != 0;
}

bool ResearchProbeRuntime::QueryMlChordEvidence(const int32_t* expectedMidiByString,
	float minConfidence, uint64_t minimumSampleIndex,
	ResearchProtocol::MlChordEvidence& evidence)
{
	const auto hostApi = GetHostApi();
	evidence = {};
	if (hostApi == nullptr) return false;
	return hostApi->QueryMlChordEvidence(expectedMidiByString, minConfidence,
		minimumSampleIndex, &evidence) != 0;
}

bool ResearchProbeRuntime::QueryRawAttacks(uint64_t afterSampleIndex, RawPitchVerifier::RawAttackBatch& out)
{
	out = {};
	const auto hostApi = GetHostApi();
	return hostApi != nullptr && hostApi->QueryRawAttacks(afterSampleIndex, &out) != 0;
}

bool ResearchProbeRuntime::CaptureRawSnapshot(RawPitchVerifier::AudioSnapshot& out)
{
	out = {};
	const auto hostApi = GetHostApi();
	return hostApi != nullptr && hostApi->CaptureRawSnapshot != nullptr
		&& hostApi->CaptureRawSnapshot(&out) != 0;
}

bool ResearchProbeRuntime::GetSongSpeedPercent(float& percent)
{
	const auto hostApi = GetHostApi();
	if (hostApi == nullptr) return false;
	percent = hostApi->GetSongSpeedPercent();
	return std::isfinite(percent) && percent > 0.0f;
}

void ResearchProbeRuntime::SetSongSpeedPercent(float percent)
{
	const auto hostApi = GetHostApi();
	if (hostApi != nullptr) hostApi->SetSongSpeedPercent(percent);
}

bool ResearchProbeRuntime::GetHostFlowModeEnabled(bool& enabled)
{
	const auto hostApi = GetHostApi();
	if (hostApi == nullptr) return false;
	enabled = hostApi->IsFlowModeEnabled() != 0;
	return true;
}

float ResearchProbeRuntime::GetPlayerSpeedRealPercent()
{
	const auto hostApi = GetHostApi();
	return hostApi != nullptr ? hostApi->GetPlayerSpeedRealPercent() : -1.0f;
}

int ResearchProbeRuntime::ConsumeNoteNavigation()
{
	const auto hostApi = GetHostApi();
	return hostApi != nullptr ? hostApi->ConsumeNoteNavigation() : 0;
}

bool ResearchProbeRuntime::IsRawSnapshotBridgeAvailable()
{
	const auto hostApi = GetHostApi();
	return hostApi != nullptr && hostApi->CaptureRawSnapshot != nullptr;
}
