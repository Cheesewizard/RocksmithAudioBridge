#include "NoteByNoteHostServices.hpp"
#include "NoteByNoteNativeScoring.hpp"
#include "MlChordConfirmation.hpp"
#include "ArrangementInstrument.hpp"

#include <iomanip>

#include "../Log.hpp"
#include "NoteByNoteProbe.hpp"
#include "DropPedal/DropPedal.hpp"
#include "../Audio/RawPitchVerifier.hpp"
#include "../Audio/MlAudioExporter.hpp"
#include "../Audio/MlStringFretReader.hpp"

#include <atomic>
#include <cmath>
#include <string>
#include <sstream>
#include <windows.h>
namespace RiffRepeater
{
	float GetSpeed(bool realSpeed);
	void SetSpeed(float newSpeed, bool isRealSpeed);
	void RequestGameSpeed(float realPercent);
	float GetPlayerRealSpeed();
}

namespace NoteByNoteHostServices
{
	namespace
	{
		std::atomic<LogTelemetrySink> logTelemetrySink{ nullptr };

		uint8_t __cdecl HostIsNoteByNoteEnabled()
		{
			return NoteByNoteProbe::IsFreezeActive() ? 1 : 0;
		}

		int __cdecl HostGetInputOnsetShiftSemitones()
		{
			if (DropPedal::GetPitchMode() != DropPedal::PitchMode::DropPedal) return 0;
			int chartMatchShift = 0;
			if (DropPedal::TryGetChartMatchShiftSemitones(chartMatchShift)) return chartMatchShift;
			return DropPedal::GetShiftSemitones();
		}

		void __cdecl HostHandleControllerFault(const char* reason)
		{
			NoteByNoteProbe::HandleNativeControllerFault(
				reason == nullptr ? "The research probe reported an unspecified fault" : reason);
		}

		void __cdecl HostPublishExpectedAttackEvent(
			const ResearchProtocol::ExpectedAttackEvent* source)
		{
			if (source == nullptr) return;

			NoteByNoteProbe::NativeExpectedAttackEvent event;
			event.kind = static_cast<NoteByNoteProbe::NativeExpectedAttackEventKind>(source->kind);
			event.ownerAddress = source->ownerAddress;
			event.epoch = source->epoch;
			event.updateTime = source->updateTime;
			event.isAfterUpdate = source->isAfterUpdate != 0;
			event.isNotePresent = source->isNotePresent != 0;
			event.note.nativeNoteAddress = source->note.nativeNoteAddress;
			event.note.recordAddress = source->note.recordAddress;
			event.note.noteMask = source->note.noteMask;
			event.note.noteFlags = source->note.noteFlags;
			event.note.noteHash = source->note.noteHash;
			event.note.authoredTime = source->note.authoredTime;
			event.note.nativeEventTime = source->note.nativeEventTime;
			event.note.stringIndex = source->note.stringIndex;
			event.note.fret = source->note.fret;
			event.note.chordId = source->note.chordId;
			event.note.chordNotesId = source->note.chordNotesId;
			event.note.phraseIterationId = source->note.phraseIterationId;
			event.note.rangeA4 = source->note.rangeA4;
			event.note.rangeA8 = source->note.rangeA8;
			event.note.stateC0 = source->note.stateC0;
			event.note.stateC1 = source->note.stateC1;
			event.note.stateC2 = source->note.stateC2;
			event.note.stateC3 = source->note.stateC3;
			event.note.bendSemitones = source->note.bendSemitones;
			NoteByNoteProbe::HandleNativeExpectedAttackEvent(event);
		}

		void __cdecl HostLog(ResearchProtocol::LogLevel level, const char* message)
		{
			const auto text = message == nullptr ? std::string() : std::string(message);
			switch (level)
			{
				case ResearchProtocol::LogLevel::Debug:
					Logger::GetInstance().Log(text, LogLevel::Debug);
					break;
				case ResearchProtocol::LogLevel::Info:
					Logger::GetInstance().Log(text, LogLevel::Info);
					break;
				case ResearchProtocol::LogLevel::Warning:
					Logger::GetInstance().Log(text, LogLevel::Warning);
					break;
				case ResearchProtocol::LogLevel::Error:
					Logger::GetInstance().Log(text, LogLevel::Error);
					break;
			}
			const auto sink = logTelemetrySink.load(std::memory_order_acquire);
			if (sink != nullptr) sink(level, text.c_str());
		}

		uint8_t __cdecl HostQueryRawToneEvidence(
			double frequencyHz,
			float windowSeconds,
			ResearchProtocol::RawToneEvidence* out)
		{
			if (out == nullptr || out->structSize < sizeof(ResearchProtocol::RawToneEvidence))
			{
				return 0;
			}
			RawPitchVerifier::ToneEvidence evidence;
			if (!RawPitchVerifier::QueryToneEvidence(frequencyHz, windowSeconds, evidence)) return 0;
			out->sampleRate = evidence.sampleRate;
			out->windowSampleCount = evidence.windowSampleCount;
			out->totalRms = evidence.totalRms;
			out->targetPower = evidence.targetPower;
			out->minusOnePower = evidence.minusOnePower;
			out->plusOnePower = evidence.plusOnePower;
			out->minusTwoPower = evidence.minusTwoPower;
			out->plusTwoPower = evidence.plusTwoPower;
			return 1;
		}

		uint8_t __cdecl HostQueryMlPitch(float* outMidi, float* outConfidence, double* outAgeSeconds)
		{
			if (outMidi == nullptr || outConfidence == nullptr || outAgeSeconds == nullptr) return 0;
			return MlAudioExporter::QueryResult(*outMidi, *outConfidence, *outAgeSeconds) ? 1 : 0;
		}

		uint8_t __cdecl HostIsMlPitchServiceAlive()
		{
			return MlAudioExporter::IsServiceAlive() ? 1 : 0;
		}
		using ResearchProtocol::MlNoteVerdict;
		MlNoteVerdict EvaluateMlNote(const MlStringFretReader::StringFret& sample,
			int expectedMidi, float minConfidence,
			int& observedMidi, float& confidence, float& outTargetConfidence)
		{
			observedMidi = -1;
			confidence = 0.0f;
			outTargetConfidence = -1.0f;
			if (expectedMidi < 0 || !std::isfinite(minConfidence)
				|| minConfidence <= 0.0f || minConfidence > 1.0f) return MlNoteVerdict::Unknown;
			const int* openMidi = ArrangementInstrument::OpenStringMidi(
				static_cast<ArrangementInstrument::Kind>(sample.instrument));
			float targetConfidence = -1.0f;
			float otherConfidence = -1.0f;
			int otherMidi = -1;
			for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
			{
				const int fret = sample.fret[stringIndex];
				const float value = sample.conf[stringIndex];
				if (openMidi[stringIndex] < 0 || fret < 0 || fret > 19 || !std::isfinite(value)
					|| value < 0.0f || value > 1.0f) continue;
				const int pitch = openMidi[stringIndex] + fret;
				if (pitch == expectedMidi)
				{
					if (value > targetConfidence) targetConfidence = value;
				}
				else if (value > otherConfidence)
				{
					otherConfidence = value;
					otherMidi = pitch;
				}
			}
			outTargetConfidence = targetConfidence;
			if (targetConfidence >= minConfidence && targetConfidence >= otherConfidence)
			{
				observedMidi = expectedMidi;
				confidence = targetConfidence;
				return MlNoteVerdict::Confirmed;
			}
			if (otherConfidence >= minConfidence && otherConfidence > targetConfidence)
			{
				observedMidi = otherMidi;
				confidence = otherConfidence;
				return MlNoteVerdict::Conflicting;
			}
			return MlNoteVerdict::Unknown;
		}

		uint64_t __cdecl HostGetMlAudioSampleIndex()
		{
			uint64_t sampleIndex = 0;
			uint32_t sampleRate = 0;
			MlAudioExporter::QueryAudioPosition(sampleIndex, sampleRate);
			return sampleIndex;
		}

		uint8_t __cdecl HostQueryMlNoteEvidence(int expectedMidi, float minConfidence,
			uint64_t minimumSampleIndex, ResearchProtocol::MlNoteEvidence* evidence)
		{
			if (evidence == nullptr) return 0;
			*evidence = {};
			MlStringFretReader::StringFret sample;
			const char* readReason = nullptr;
			const bool haveSample = MlStringFretReader::TryGet(sample, 0.6, &readReason);
			const int appliedShift = DropPedal::GetAppliedInputShiftSemitones();
			static uint64_t lastLogTick = 0;
			const uint64_t now = GetTickCount64();
			if (now - lastLogTick >= 100)
			{
				lastLogTick = now;
				std::ostringstream pitches;
				const int* OPEN_MIDI = ArrangementInstrument::OpenStringMidi(
					static_cast<ArrangementInstrument::Kind>(sample.instrument));
				for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
				{
					if (stringIndex != 0) pitches << ',';
					pitches << stringIndex << ':'
						<< (sample.fret[stringIndex] >= 0 && OPEN_MIDI[stringIndex] >= 0
							? OPEN_MIDI[stringIndex] + sample.fret[stringIndex] : -1)
						<< ':' << sample.conf[stringIndex];
				}
				LOG_INFO("(NBN FRETNET READ) exp=" << expectedMidi
					<< " physicalTarget=" << expectedMidi - appliedShift
					<< " read=" << readReason << " sample=" << sample.analyzedSampleIndex
					<< " minimumSample=" << minimumSampleIndex << " age=" << sample.ageSeconds
					<< " shift=" << sample.shift << " appliedShift=" << appliedShift
					<< " threshold=" << minConfidence << " instrument=" << sample.instrument
					<< " arrangement=" << MlAudioExporter::GetArrangementInstrument()
					<< " string:midi:conf=" << pitches.str() << std::endl);
			}
			if (!haveSample) return 0;
			if (sample.instrument != MlAudioExporter::GetArrangementInstrument()) return 0;
			evidence->analyzedSampleIndex = sample.analyzedSampleIndex;
			evidence->sampleRate = sample.sampleRate;
			evidence->ageSeconds = sample.ageSeconds;
			if (sample.analyzedSampleIndex <= minimumSampleIndex || sample.shift != appliedShift)
			{
				evidence->verdict = MlNoteVerdict::Pending;
				return 1;
			}
			evidence->verdict = EvaluateMlNote(sample, expectedMidi, minConfidence,
				evidence->observedMidi, evidence->confidence, evidence->targetConfidence);
			return 1;
		}

		uint8_t __cdecl HostQueryMlChordEvidence(const int32_t* expectedMidiByString,
			float minConfidence, uint64_t minimumSampleIndex,
			ResearchProtocol::MlChordEvidence* evidence)
		{
			if (expectedMidiByString == nullptr || evidence == nullptr) return 0;
			*evidence = {};
			MlStringFretReader::StringFret sample;
			const char* readReason = nullptr;
			if (!MlStringFretReader::TryGet(sample, 0.6, &readReason)) return 0;
			if (sample.instrument != MlAudioExporter::GetArrangementInstrument()) return 0;

			evidence->analyzedSampleIndex = sample.analyzedSampleIndex;
			evidence->sampleRate = sample.sampleRate;
			evidence->ageSeconds = sample.ageSeconds;
			const int appliedShift = DropPedal::GetAppliedInputShiftSemitones();
			if (sample.analyzedSampleIndex <= minimumSampleIndex || sample.shift != appliedShift)
			{
				evidence->verdict = MlNoteVerdict::Pending;
				return 1;
			}

			const int* OPEN_MIDI = ArrangementInstrument::OpenStringMidi(
				static_cast<ArrangementInstrument::Kind>(sample.instrument));
			int observedMidiByString[6] = { -1, -1, -1, -1, -1, -1 };
			for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
			{
				if (OPEN_MIDI[stringIndex] >= 0 && sample.fret[stringIndex] >= 0 && sample.fret[stringIndex] <= 19)
				{
					observedMidiByString[stringIndex] = OPEN_MIDI[stringIndex]
						+ sample.fret[stringIndex];
				}
			}

			const bool confirmed = NoteByNote::ConfirmMlChordStrings(
				expectedMidiByString,
				observedMidiByString,
				sample.conf,
				minConfidence,
				evidence->requiredStringMask,
				evidence->matchedStringMask);
			evidence->verdict = confirmed ? MlNoteVerdict::Confirmed : MlNoteVerdict::Conflicting;
			return 1;
		}

		uint8_t __cdecl HostQueryRawToneComb(double frequencyHz, ResearchProtocol::RawToneComb* out)
		{
			if (out == nullptr) return 0;
			*out = {};
			static thread_local RawPitchVerifier::AudioSnapshot snapshot;
			if (!RawPitchVerifier::CaptureSnapshot(snapshot)) return 0;
			constexpr float WINDOWS[] = { 0.05f, 0.10f, 0.15f };
			for (int window = 0; window < 3; ++window)
			{
				for (int bin = 0; bin < 17; ++bin)
				{
					const double frequency = frequencyHz * std::pow(2.0, (-50.0 + bin * 12.5) / 1200.0);
					if (!RawPitchVerifier::MeasureSnapshot(snapshot, frequency, WINDOWS[window],
						out->powers[window][bin], out->rms[window], out->sampleCounts[window])) return 0;
				}
			}
			out->endSampleIndex = snapshot.endSampleIndex;
			out->sampleRate = snapshot.sampleRate;
			return 1;
		}

		uint8_t __cdecl HostQueryRawNoteConfirmation(double frequencyHz, uint64_t minimumSampleIndex, uint64_t maximumSampleIndex, ResearchProtocol::RawNoteConfirmation* out)
		{
			if (out == nullptr) return 0;
			*out = {};
			RawPitchVerifier::NoteConfirmation evidence;
			const bool available = RawPitchVerifier::QueryNoteConfirmation(frequencyHz, evidence, minimumSampleIndex, maximumSampleIndex);
			out->endSampleIndex = evidence.endSampleIndex;
			out->sampleRate = evidence.sampleRate;
			out->targetPower = evidence.targetPower;
			out->attackChange = evidence.attackChange;
			out->attackPower = evidence.attackPower;
			out->attackMinusPower = evidence.attackMinusPower;
			out->attackPlusPower = evidence.attackPlusPower;
			out->neighbourPower = evidence.neighbourPower;
			out->confirmed = evidence.confirmed ? 1 : 0;
			return available ? 1 : 0;
		}

		uint8_t __cdecl HostQueryRawAttacks(uint64_t afterSampleIndex, RawPitchVerifier::RawAttackBatch* out)
		{
			return out != nullptr && RawPitchVerifier::QueryAttacks(afterSampleIndex, *out) ? 1 : 0;
		}

		uint8_t __cdecl HostCaptureRawSnapshot(RawPitchVerifier::AudioSnapshot* out)
		{
			return out != nullptr && RawPitchVerifier::CaptureSnapshot(*out) ? 1 : 0;
		}

		float __cdecl HostGetSongSpeedPercent()
		{
			return RiffRepeater::GetSpeed(true);
		}

		void __cdecl HostSetSongSpeedPercent(float percent)
		{
			RiffRepeater::RequestGameSpeed(percent);
		}

		uint8_t __cdecl HostIsFlowModeEnabled()
		{
			return NoteByNoteNativeScoring::GetFlowUntilMissEnabled() ? 1 : 0;
		}

		float __cdecl HostGetPlayerSpeedRealPercent()
		{
			return RiffRepeater::GetPlayerRealSpeed();
		}

		std::atomic<int> pendingNoteNavigation{ 0 };

		int32_t __cdecl HostConsumeNoteNavigation()
		{
			return pendingNoteNavigation.exchange(0);
		}

		const ResearchProtocol::HostApi hostApi =
		{
			ResearchProtocol::HOST_API_VERSION,
			sizeof(ResearchProtocol::HostApi),
			&HostIsNoteByNoteEnabled,
			&HostHandleControllerFault,
			&HostPublishExpectedAttackEvent,
			&HostLog,
			&HostGetInputOnsetShiftSemitones,
			&HostQueryRawToneEvidence,
			&HostQueryMlPitch,
			&HostIsMlPitchServiceAlive,
			&HostGetMlAudioSampleIndex,
			&HostQueryMlNoteEvidence,
			&HostQueryMlChordEvidence,
			&HostQueryRawToneComb,
			&HostQueryRawNoteConfirmation,
			&HostQueryRawAttacks,
			&HostCaptureRawSnapshot,
			&HostGetSongSpeedPercent,
			&HostSetSongSpeedPercent,
			&HostIsFlowModeEnabled,
			&HostGetPlayerSpeedRealPercent,
			&HostConsumeNoteNavigation
		};
	}

	const ResearchProtocol::HostApi& GetHostApi()
	{
		return hostApi;
	}

	void QueueNoteNavigation(int direction)
	{
		pendingNoteNavigation.store(direction > 0 ? 1 : direction < 0 ? -1 : 0);
	}

	void SetLogTelemetrySink(LogTelemetrySink sink)
	{
		logTelemetrySink.store(sink, std::memory_order_release);
	}
}
