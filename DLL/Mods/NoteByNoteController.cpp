#include "../stdafx.h"
#include "../Research/ResearchBridge.hpp"
#include "NoteByNoteHostServices.hpp"
#include "NoteByNoteNativeScoring.hpp"
#include "NoteByNoteProbe.hpp"

extern "C" const ResearchProtocol::ProbeApi* __cdecl RSMP_GetResearchProbeApi(
	uint32_t version, const ResearchProtocol::HostApi* host);

namespace
{
	std::recursive_mutex probeMutex;
	struct Controller
	{
		bool isInitialized = false;
		ResearchProtocol::ProbeApi api = {};
	} loadedProbe;
}

void ResearchBridge::Initialize()
{
	const auto* api = RSMP_GetResearchProbeApi(ResearchProtocol::HOST_API_VERSION, &NoteByNoteHostServices::GetHostApi());
	if (api == nullptr || api->version != ResearchProtocol::PROBE_API_VERSION || api->structSize != sizeof(*api) || !api->Initialize())
	{
		LOG_ERROR("(NBN CONTROLLER) Built-in controller initialization failed." << std::endl);
		return;
	}
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	loadedProbe.api = *api;
	loadedProbe.isInitialized = true;
	const ResearchProtocol::HookRequest* requests = nullptr;
	uint32_t count = 0;
	api->GetRequestedHooks(&requests, &count);
	NoteByNoteNativeScoring::SetRequestedGenericHooks(requests, count);
	LOG_INFO("(NBN CONTROLLER) Built-in gameplay controller ready. Public build; developer endpoints excluded." << std::endl);
}

void ResearchBridge::Shutdown()
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.isInitialized) loadedProbe.api.Shutdown();
	loadedProbe = {};
}
bool ResearchBridge::IsProbeLoaded()
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	return loadedProbe.isInitialized;
}

void ResearchBridge::DispatchGenericHook(
	uint32_t slotId,
	const ResearchProtocol::HookContext* context)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (!loadedProbe.isInitialized || loadedProbe.api.ObserveGenericHook == nullptr) return;
	loadedProbe.api.ObserveGenericHook(slotId, context);
}

void ResearchBridge::DispatchNeckPlacementStep(uint32_t site, void* stepContext)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (!loadedProbe.isInitialized
		|| loadedProbe.api.ObserveNeckPlacementStep == nullptr)
	{
		return;
	}
	loadedProbe.api.ObserveNeckPlacementStep(site, stepContext);
}

void ResearchBridge::DispatchScoringUpdate(
	void* owner,
	float updateTime,
	ResearchProtocol::ScoringUpdate original)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (!loadedProbe.isInitialized)
	{
		original(owner, updateTime);
		return;
	}
	loadedProbe.api.ProcessScoringUpdate(owner, updateTime, original);
}

void ResearchBridge::DispatchHitDecision(
	void* owner,
	void* unusedEdx,
	void* note,
	ResearchProtocol::HitDecision original,
	bool& result)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	result = !loadedProbe.isInitialized
		? original(owner, unusedEdx, note)
		: loadedProbe.api.ProcessHitDecision(owner, unusedEdx, note, original);
}

void ResearchBridge::DispatchRenderedAttack(
	const NoteByNoteProbe::NativeRenderedAttack& attack)
{
	std::vector<ResearchProtocol::RenderedNote> notes;
	notes.reserve(attack.notes.size());
	for (const auto& source : attack.notes)
	{
		notes.push_back({ source.stringIndex, source.fret });
	}

	const ResearchProtocol::RenderedAttack researchAttack =
	{
		attack.isTransition ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0),
		attack.renderFrame,
		attack.songTime,
		attack.longitudinalPosition,
		notes.data(),
		notes.size()
	};

	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.isInitialized) loadedProbe.api.ObserveRenderedAttack(&researchAttack);
}

void ResearchBridge::DispatchStop()
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.isInitialized) loadedProbe.api.Stop();
}

void ResearchBridge::DispatchRequestReArm()
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.isInitialized && loadedProbe.api.RequestReArm != nullptr)
	{
		loadedProbe.api.RequestReArm();
	}
}

void ResearchBridge::ArmRenderSnapshot()
{
}

void ResearchBridge::DispatchNativeDraw(const ResearchProtocol::NativeDrawObservation& draw)
{
}

void ResearchBridge::DispatchFullDraw(const ResearchProtocol::NativeDrawObservation& draw)
{
}

bool ResearchBridge::IsFullDrawFeedEnabled()
{
	return false;
}

bool ResearchBridge::IsNativeDrawFeedEnabled()
{
	return false;
}

bool ResearchBridge::IsDrawFeedWanted()
{
	return false;
}

void ResearchBridge::DispatchRenderFrameComplete(uint64_t renderFrame)
{
}

void ResearchBridge::ArmNoteDrawListSnapshot()
{
}

void ResearchBridge::ArmScreenMapSnapshot()
{
}

bool ResearchBridge::TryHandleProbeCommand(const std::string& requestJson, std::string& response)
{
	return false;
}

void ResearchBridge::ProcessNoteDrawList(
	void* renderCtx,
	void* unusedEdx,
	int* noteArray,
	ResearchProtocol::NoteHeadDraw original)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (!loadedProbe.isInitialized)
	{
		original(renderCtx, unusedEdx, noteArray);
		return;
	}

	const bool wasPrepared = loadedProbe.api.PrepareNoteDrawList(renderCtx, noteArray) != 0;
	original(renderCtx, unusedEdx, noteArray);
	if (wasPrepared) loadedProbe.api.CompleteNoteDrawList();
}

bool ResearchBridge::TryGetNoteByNoteState(ResearchProtocol::NoteByNoteState& state)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (!loadedProbe.isInitialized) return false;
	state = {};
	if (loadedProbe.api.GetState(&state) != 0) return true;

	static std::atomic<bool> hasLoggedStateFailure{ false };
	if (!hasLoggedStateFailure.exchange(true))
	{
		LOG_ERROR("(NBN CONTROLLER) State retrieval failed for a "
			<< sizeof(ResearchProtocol::NoteByNoteState)
			<< "-byte host state. Presentation and detector HUD data are unavailable."
			<< std::endl);
	}
	return false;
}


bool ResearchBridge::DescribeSelectedChordFingering(uintptr_t record, char* buffer, size_t bufferLength,
	int& lowestPlayedString)
{
	lowestPlayedString = -1;
	if (buffer == nullptr || bufferLength == 0) return false;
	buffer[0] = '\0';
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (!loadedProbe.isInitialized || loadedProbe.api.DescribeSelectedChordFingering == nullptr) return false;
	int32_t lowest = -1;
	const bool described = loadedProbe.api.DescribeSelectedChordFingering(
		record, buffer, static_cast<uint32_t>(bufferLength), &lowest) != 0;
	lowestPlayedString = lowest;
	return described;
}

void ResearchBridge::PublishPhysicalMarkerDraw(const PhysicalMarkerDrawEvent&) {}
void ResearchBridge::PublishNoteByNoteEvent(const NoteByNoteProbe::NativeExpectedAttackEvent&) {}
