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
	// Semitones to add to a native onset to bring it into expectedMidi's tuning frame; 0 unless
	// DropPedal's input pitch shifter is active. See HostApi::GetInputOnsetShiftSemitones.
	int GetInputOnsetShiftSemitones();
	bool QueryRawToneComb(double frequencyHz, ResearchProtocol::RawToneComb& out);
	bool QueryRawNoteConfirmation(double frequencyHz, ResearchProtocol::RawNoteConfirmation& out, uint64_t minimumSampleIndex = 0, uint64_t maximumSampleIndex = 0);

	// Tier-0 raw-audio tone evidence for the Player 1 route (HostApi::QueryRawToneEvidence).
	// False when the host predates the entry or not enough audio has been observed.
	bool QueryRawToneEvidence(double frequencyHz, float windowSeconds,
		ResearchProtocol::RawToneEvidence& out);
	// Tier-1 ML pitch estimate from the 64-bit companion service (HostApi::QueryMlPitch).
	// midi is a float in the OBSERVED route frame (post input shifter, unlike the tier-0
	// query which corrects the frame host-side). False when the host predates the entry
	// or the companion has not produced an estimate.
	bool QueryMlPitch(float& outMidi, float& outConfidence, double& outAgeSeconds);
	// True when the companion updated its estimate within the last second of audio
	// (HostApi::IsMlPitchServiceAlive). False on hosts that predate the entry.
	bool IsMlPitchServiceAlive();

	uint64_t GetMlAudioSampleIndex();
	bool QueryMlNoteEvidence(int expectedMidi, float minConfidence,
		uint64_t minimumSampleIndex, ResearchProtocol::MlNoteEvidence& evidence);
	bool QueryMlChordEvidence(const int32_t* expectedMidiByString, float minConfidence,
		uint64_t minimumSampleIndex, ResearchProtocol::MlChordEvidence& evidence);
	void PublishExpectedAttackEvent(const ResearchProtocol::ExpectedAttackEvent& event);
	bool QueryRawAttacks(uint64_t afterSampleIndex, RawPitchVerifier::RawAttackBatch& out);
	// The raw route snapshot behind chord/dyad/unison/mute attack confirmation, bridged from the
	// host (HostApi::CaptureRawSnapshot). The reloadable probe has no tap of its own.
	bool CaptureRawSnapshot(RawPitchVerifier::AudioSnapshot& out);
	// True when the snapshot bridge is wired (host present and the v9 slot set). Distinct from
	// CaptureRawSnapshot returning data, which is also false before any audio has been captured.
	bool IsRawSnapshotBridgeAvailable();
	// Song speed in percent (100 = normal) through HostApi v10. False when no host is present.
	bool GetSongSpeedPercent(float& percent);
	void SetSongSpeedPercent(float percent);
	// The host's FLOW MODE switch (HostApi v11). False when no host is present.
	bool GetHostFlowModeEnabled(bool& enabled);
	// v12: the player's Riff Repeater speed as a real percent, -1 when the host has not seen it.
	float GetPlayerSpeedRealPercent();
	// v12: a queued arrow-key navigation (+1 next, -1 previous, 0 none); the read clears it.
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
