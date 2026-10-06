#pragma once

#include "NoteByNoteTypes.hpp"

#include <sstream>
#include <string>

namespace ResearchProbeRuntime
{
	void HandleControllerFault(const std::string& reason);
	bool Initialize(const ResearchProtocol::HostApi* hostApi);
	bool IsNoteByNoteEnabled();
	void Log(ResearchProtocol::LogLevel level, const std::string& message);
	int GetInputOnsetShiftSemitones();
	bool QueryRawToneComb(double frequencyHz, ResearchProtocol::RawToneComb& out);
	bool QueryRawNoteConfirmation(double frequencyHz, ResearchProtocol::RawNoteConfirmation& out, uint64_t minimumSampleIndex = 0, uint64_t maximumSampleIndex = 0);
	bool QueryRawToneEvidence(double frequencyHz, float windowSeconds,
		ResearchProtocol::RawToneEvidence& out);
	bool QueryMlPitch(float& outMidi, float& outConfidence, double& outAgeSeconds);
	bool IsMlPitchServiceAlive();

	uint64_t GetMlAudioSampleIndex();
	bool QueryMlNoteEvidence(int expectedMidi, float minConfidence,
		uint64_t minimumSampleIndex, ResearchProtocol::MlNoteEvidence& evidence);
	bool QueryMlChordEvidence(const int32_t* expectedMidiByString, float minConfidence,
		uint64_t minimumSampleIndex, ResearchProtocol::MlChordEvidence& evidence);
	void PublishExpectedAttackEvent(const ResearchProtocol::ExpectedAttackEvent& event);
	bool QueryRawAttacks(uint64_t afterSampleIndex, RawPitchVerifier::RawAttackBatch& out);
	bool CaptureRawSnapshot(RawPitchVerifier::AudioSnapshot& out);
	bool IsRawSnapshotBridgeAvailable();
	bool GetSongSpeedPercent(float& percent);
	void SetSongSpeedPercent(float percent);
	bool GetHostFlowModeEnabled(bool& enabled);
	float GetPlayerSpeedRealPercent();
	int ConsumeNoteNavigation();
	void Shutdown();
}

#define RESEARCH_LOG(level, message) \
	do \
	{ \
		std::ostringstream researchLogStream; \
		researchLogStream << message; \
		ResearchProbeRuntime::Log(level, researchLogStream.str()); \
	} while (0)

#define LOG_DEBUG(message) RESEARCH_LOG(ResearchProtocol::LogLevel::Debug, message)
#define LOG_INFO(message) RESEARCH_LOG(ResearchProtocol::LogLevel::Info, message)
#define LOG_WARNING(message) RESEARCH_LOG(ResearchProtocol::LogLevel::Warning, message)
#define LOG_ERROR(message) RESEARCH_LOG(ResearchProtocol::LogLevel::Error, message)
