#pragma once

#include "ResearchProtocol.hpp"

#include <string>

namespace NoteByNoteProbe
{
	struct NativeRenderedAttack;
	struct NativeExpectedAttackEvent;
}

namespace ResearchBridge
{
	struct PhysicalMarkerDrawEvent
	{
		uint64_t renderFrame = 0;
		uintptr_t nativeCaller = 0;
		uintptr_t streamIdentity = 0;
		uintptr_t textureIdentity = 0;
		uintptr_t vertexShaderIdentity = 0;
		uintptr_t pixelShaderIdentity = 0;
		uint32_t primitiveType = 0;
		int32_t baseVertexIndex = 0;
		uint32_t minimumVertexIndex = 0;
		uint32_t vertexCount = 0;
		uint32_t startIndex = 0;
		uint32_t primitiveCount = 0;
		uint32_t stride = 0;
		int32_t stringIndex = -1;
		int32_t fret = -1;
		int32_t targetString = -1;
		int32_t targetFret = -1;
		bool decoded = false;
		const char* decodeFailure = nullptr;
		bool instanced = false;
		uint32_t streamFrequency = 0;
		uint32_t transformStreamStride = 0;
		uint32_t instanceCount = 0;
		uint32_t decodedInstanceCount = 0;
		float instanceTranslations[32][3] = {};
		bool userPointer = false;
		uint32_t vertexSampleCount = 0;
		int32_t vertexSampleStatus = 1;
		float vertexSample[24] = {};
		bool hasBounds = false;
		float boundsMin[3] = {};
		float boundsMax[3] = {};
		float transform[4][4] = {};
	};

	void ArmRenderSnapshot();
	void ArmNoteDrawListSnapshot();
	void ArmScreenMapSnapshot();
	bool TryHandleProbeCommand(const std::string& requestJson, std::string& response);
	bool ReloadDeployedProbe(std::string& error);
	std::string GetDeployedProbeReloadStatus();
	bool DescribeSelectedChordFingering(uintptr_t record, char* buffer, size_t bufferLength, int& lowestPlayedString);
	bool IsProbeReloadAvailable();
	struct ProbeIdentity
	{
		bool loaded = false;
		bool inProcess = false;
		std::string name;
		std::string buildId;
		bool isDebugBuild = false;
		bool hostIsDebugBuild = false;
		uint64_t loadedHash = 0;
		std::string loadedHashText;
		std::string loadedAt;
		bool deployedPresent = false;
		uint64_t deployedHash = 0;
		std::string deployedHashText;
		bool deployedIsDebugBuild = false;
		std::string deployedModified;
	};
	void GetProbeIdentity(ProbeIdentity& identity);
	void ProcessNoteDrawList(
		void* renderCtx,
		void* unusedEdx,
		int* noteArray,
		ResearchProtocol::NoteHeadDraw original);
	void DispatchHitDecision(
		void* owner,
		void* unusedEdx,
		void* note,
		ResearchProtocol::HitDecision original,
		bool& result);
	void DispatchGenericHook(uint32_t slotId, const ResearchProtocol::HookContext* context);
	void DispatchNeckPlacementStep(uint32_t site, void* stepContext);
	void DispatchNativeDraw(const ResearchProtocol::NativeDrawObservation& draw);
	void DispatchFullDraw(const ResearchProtocol::NativeDrawObservation& draw);
	bool IsFullDrawFeedEnabled();
	bool IsNativeDrawFeedEnabled();
	bool IsDrawFeedWanted();
	void DispatchRenderFrameComplete(uint64_t renderFrame);
	void DispatchRenderedAttack(const NoteByNoteProbe::NativeRenderedAttack& attack);
	void DispatchScoringUpdate(
		void* owner,
		float updateTime,
		ResearchProtocol::ScoringUpdate original);
	void DispatchStop();
	void DispatchRequestReArm();
	void Initialize();
	bool IsProbeLoaded();
	void PublishPhysicalMarkerDraw(const PhysicalMarkerDrawEvent& event);
	void PublishNoteByNoteEvent(const NoteByNoteProbe::NativeExpectedAttackEvent& event);
	void Shutdown();
	bool TryGetNoteByNoteState(ResearchProtocol::NoteByNoteState& state);
}
