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
	// In-process probe reload for the debug overlay (no research pipe or N key needed).
	// ReloadDeployedProbe swaps in the probe currently deployed at GetDefaultProbePath,
	// preserving the player's Note by Note enable state across the swap (the reload gate
	// requires NBN off, so it is disabled for the swap then restored). IsProbeReloadAvailable
	// is true only when a probe DLL is actually present, so the overlay button appears only on
	// builds that ship a probe.
	// Starts the swap on a worker thread (it waits for NBN to release a held note) and returns
	// false only when a reload is already running; poll GetDeployedProbeReloadStatus for the result.
	bool ReloadDeployedProbe(std::string& error);
	std::string GetDeployedProbeReloadStatus();
	// The loaded controller's selected-chord fingering (see ProbeApi::DescribeSelectedChordFingering).
	bool DescribeSelectedChordFingering(uintptr_t record, char* buffer, size_t bufferLength, int& lowestPlayedString);
	bool IsProbeReloadAvailable();
	// What the debug overlay shows about the running probe: the loaded one (name, compile stamp,
	// content hash, load time) next to the one deployed on disk, so a stale load is visible.
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
	// Full draw feed: every in-song draw from all four device entry points, gated by
	// IsFullDrawFeedEnabled host-side and by the probe supplying ObserveFullDraw. The
	// disarmed cost is one relaxed atomic read at the hook site.
	void DispatchFullDraw(const ResearchProtocol::NativeDrawObservation& draw);
	bool IsFullDrawFeedEnabled();
	// Native (filtered note-head) draw feed: same shape as the full feed. Off by default in
	// Release, on in Debug; armed automatically by the render-snapshot and screen-map
	// captures that consume it, and by set_native_draw_feed.
	bool IsNativeDrawFeedEnabled();
	// True when EITHER feed has an enabled flag AND a loaded consumer. The draw hook builds
	// the per-draw observation struct only when this is true, so the idle Release path
	// pays two relaxed atomic reads per draw and nothing else.
	bool IsDrawFeedWanted();
	void DispatchRenderFrameComplete(uint64_t renderFrame);
	void DispatchRenderedAttack(const NoteByNoteProbe::NativeRenderedAttack& attack);
	void DispatchScoringUpdate(
		void* owner,
		float updateTime,
		ResearchProtocol::ScoringUpdate original);
	void DispatchStop();
	// Forward a bootstrap re-arm request to the loaded probe (optional ProbeApi entry;
	// a no-op when the probe predates it). Called on every NBN enable so a same-owner
	// section change still re-arms.
	void DispatchRequestReArm();
	void Initialize();
	bool IsProbeLoaded();
	void PublishPhysicalMarkerDraw(const PhysicalMarkerDrawEvent& event);
	void PublishNoteByNoteEvent(const NoteByNoteProbe::NativeExpectedAttackEvent& event);
	void Shutdown();
	bool TryGetNoteByNoteState(ResearchProtocol::NoteByNoteState& state);

	// Fake-guitar autoplay: SetFakeGuitarAutoPlay toggles it (arming the synth when enabled);
	// PollFakeGuitarAutoPlay is called every host-loop tick and, while on, injects the frozen
	// Note by Note target (note / chord / bend) so a section walks hands-free.
	void SetFakeGuitarAutoPlay(bool enabled);
	bool IsFakeGuitarAutoPlay();
	void PollFakeGuitarAutoPlay();
}
