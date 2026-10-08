#include "../stdafx.h"
#include "NoteByNoteMenu.hpp"

#include "NoteByNoteProbe.hpp"
#include "NoteByNoteNativeScoring.hpp"
#include "FlowSpeedCap.hpp"
#include "RiffRepeater.hpp"
#include "../GameState.hpp"
#include "../OverlayToggles.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
	constexpr uintptr_t RIFF_REPEATER_INPUT_DISPATCHER = 0x00630800;
	constexpr uintptr_t RIFF_REPEATER_ADJUST_SLIDER = 0x00633150;
	constexpr uintptr_t RIFF_REPEATER_STAGE_BUILDER = 0x006397D0;
	constexpr uintptr_t VIEW_CONTAINER_OFFSET = 0x1C4;      // controller+0x1C4: the row container
	constexpr uintptr_t CONTAINER_ENUMERATE_SLOT = 0x74;    // (old container class only) fills {begin,end,cap}
	constexpr uintptr_t CONTAINER_GET_BY_NAME_SLOT = 0x18;  // (const char* name) -> row node (AdjustSlider 0x633150)
	constexpr uintptr_t NODE_RESOLVE_SLOT = 0x5C;           // () -> resolved row object (smart-ptr get)
	constexpr uintptr_t CHILD_GET_MEMBER_SLOT = 0x14;       // (const char* name) -> member or null
	constexpr uintptr_t MEMBER_AS_NUMBER_SLOT = 0x50;       // () -> holder or null; double at holder+8
	constexpr uintptr_t NUMBER_HOLDER_VALUE_OFFSET = 0x8;
	const byte STAGE_BUILDER_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0, 0x81, 0xEC, 0xB4, 0x00, 0x00, 0x00, 0xA1 };
	using StageBuilder = void(__stdcall*)(void* controller, int* sortOrderCounter);
	StageBuilder originalStageBuilder = nullptr;
	constexpr uintptr_t RIFF_REPEATER_SETTINGS_BUILDER = 0x006358E0;
	const byte SETTINGS_BUILDER_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0, 0x81, 0xEC, 0xB4, 0x08, 0x00, 0x00, 0xA1 };
	using SettingsBuilder = void(__fastcall*)(void* controller, void* unusedEdx, int* sortOrderCounter);
	SettingsBuilder originalSettingsBuilder = nullptr;
	void* volatile settingsBuiltController = nullptr;
	constexpr const char* NOTE_BY_NOTE_SLIDER_ID = "NoteByNote";
	constexpr const char* FLOW_SLIDER_ID = "NoteByNoteFlow";
	constexpr unsigned int POLL_FRAME_INTERVAL = 3;
	constexpr int UNWRITTEN = -12345;
	const byte DISPATCHER_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x44, 0xA1 };
	const byte ADJUST_SLIDER_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x70, 0xA1 };

	void* originalDispatcher = nullptr;
	void* volatile dispatcherController = nullptr;
	volatile unsigned long dispatcherGeneration = 0;
	constexpr uintptr_t CONTROLLER_NAME_OFFSET = 0x78;
	constexpr const char* ADVANCED_SETTINGS_CONTROLLER_NAME = "RiffRepeater_AdvancedSettings";
	uintptr_t knownControllerVtable = 0;
	unsigned int frameCounter = 0;
	unsigned long adoptedGeneration = 0;
	void* controller = nullptr;
	std::string controllerMenu;
	bool hasFocusedValue = false;
	int lastFocusedValue = -1;
	bool hasFlowFocusedValue = false;
	int lastFlowFocusedValue = -1;
	int appliedFlowDisable = -1;
	int flowBuiltDisabled = -1;
	unsigned long lastFlowReadGeneration = 0;
	bool loggedFault = false;
	int menuSettleFrames = 0;

	std::mutex diagnosticsMutex;
	NoteByNoteMenu::Diagnostics diagnostics;
	unsigned long SafeReadDword(uintptr_t address, bool& ok)
	{
		ok = false;
		__try
		{
			unsigned long value = *reinterpret_cast<volatile unsigned long*>(address);
			ok = true;
			return value;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}
	void SampleContainerVtable(void* controller, unsigned long& containerOut,
		unsigned long& vtableOut, unsigned long& slot74Out)
	{
		containerOut = 0; vtableOut = 0; slot74Out = 0;
		if (controller == nullptr) return;
		bool ok = false;
		const uintptr_t container = SafeReadDword(reinterpret_cast<uintptr_t>(controller) + VIEW_CONTAINER_OFFSET, ok);
		if (!ok) return;
		containerOut = static_cast<unsigned long>(container);
		if (container == 0) return;
		const uintptr_t vtable = SafeReadDword(container, ok);
		if (!ok) return;
		vtableOut = static_cast<unsigned long>(vtable);
		if (vtable == 0) return;
		slot74Out = SafeReadDword(vtable + CONTAINER_ENUMERATE_SLOT, ok);
	}

	bool MatchesBytes(uintptr_t address, const byte* expected, size_t count)
	{
		__try
		{
			return std::memcmp(reinterpret_cast<const void*>(address), expected, count) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
	bool AdjustSliderUnguarded(void* target, int delta, int* value, const char* sliderId)
	{
		const uintptr_t function = RIFF_REPEATER_ADJUST_SLIDER;
		__asm
		{
			mov ecx, value
			push delta
			push sliderId
			push target
			call dword ptr[function]
		}
		return *value != UNWRITTEN;
	}
	constexpr uintptr_t FOCUSED_COMPONENT_OFFSET = 0x5C;
	bool AdjustSliderGuarded(void* target, int delta, int& value, bool& faulted, bool blindToFocus,
		const char* sliderId = NOTE_BY_NOTE_SLIDER_ID)
	{
		value = UNWRITTEN;
		faulted = false;
		void** focusedSlot = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(target) + FOCUSED_COMPONENT_OFFSET);
		void* savedFocused = nullptr;
		bool blanked = false;
		__try
		{
			if (blindToFocus)
			{
				savedFocused = *focusedSlot;
				*focusedSlot = nullptr;
				blanked = true;
			}
			const bool written = AdjustSliderUnguarded(target, delta, &value, sliderId);
			if (blanked) *focusedSlot = savedFocused;
			return written;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (blanked) *focusedSlot = savedFocused;
			faulted = true;
			return false;
		}
	}

	__declspec(naked) void DispatcherDetour()
	{
		__asm
		{
			mov dispatcherController, ecx
			inc dispatcherGeneration
			jmp dword ptr[originalDispatcher]
		}
	}

	struct ChildRange { void** begin; void** end; void** capacity; };
	template <typename Fn>
	Fn VirtualSlot(void* object, uintptr_t slotOffset)
	{
		const uintptr_t vtable = *reinterpret_cast<uintptr_t*>(object);
		return *reinterpret_cast<Fn*>(vtable + slotOffset);
	}

	void EnumerateChildren(void* container, ChildRange* range)
	{
		using Fn = void(__fastcall*)(void* self, void* edx, ChildRange* out);
		VirtualSlot<Fn>(container, CONTAINER_ENUMERATE_SLOT)(container, nullptr, range);
	}

	void* GetMember(void* child, const char* name)
	{
		using Fn = void*(__fastcall*)(void* self, void* edx, const char* name);
		return VirtualSlot<Fn>(child, CHILD_GET_MEMBER_SLOT)(child, nullptr, name);
	}

	void* MemberAsNumber(void* member)
	{
		using Fn = void*(__fastcall*)(void* self, void* edx);
		return VirtualSlot<Fn>(member, MEMBER_AS_NUMBER_SLOT)(member, nullptr);
	}
	void* GetChildByName(void* container, const char* name)
	{
		using Fn = void*(__fastcall*)(void* self, void* edx, const char* name);
		return VirtualSlot<Fn>(container, CONTAINER_GET_BY_NAME_SLOT)(container, nullptr, name);
	}
	void* ResolveNode(void* node)
	{
		using Fn = void*(__fastcall*)(void* self, void* edx);
		return VirtualSlot<Fn>(node, NODE_RESOLVE_SLOT)(node, nullptr);
	}
	double* FindNumberMember(void* child, const char* name)
	{
		if (child == nullptr) return nullptr;
		void* member = GetMember(child, name);
		if (member == nullptr) return nullptr;
		if (MemberAsNumber(member) == nullptr) return nullptr;
		void* holder = MemberAsNumber(member);
		if (holder == nullptr) return nullptr;
		return reinterpret_cast<double*>(reinterpret_cast<uintptr_t>(holder) + NUMBER_HOLDER_VALUE_OFFSET);
	}

	double* FindSortOrder(void* child)
	{
		return FindNumberMember(child, "SortOrder");
	}
	constexpr uintptr_t MEMBER_AS_STRING_SLOT = 0x60;
	void* MemberAsString(void* member)
	{
		using Fn = void*(__fastcall*)(void* self, void* edx);
		return VirtualSlot<Fn>(member, MEMBER_AS_STRING_SLOT)(member, nullptr);
	}
	const char* FindStringMember(void* child, const char* name)
	{
		if (child == nullptr) return nullptr;
		void* member = GetMember(child, name);
		if (member == nullptr) return nullptr;
		if (MemberAsString(member) == nullptr) return nullptr;
		void* holder = MemberAsString(member);
		if (holder == nullptr) return nullptr;
		char* text = *reinterpret_cast<char**>(reinterpret_cast<uintptr_t>(holder) + 8);
		if (text == nullptr) return nullptr;
		if (*reinterpret_cast<char**>(text + 0x14) != text + 0x10) text = *reinterpret_cast<char**>(text);
		return text;
	}
	enum : uintptr_t
	{
		INTERN_NAME_FN = 0x0087C470,
		MAKE_NUMBER_FN = 0x0087F930,
		SET_MEMBER_FN = 0x008816C0,
		RELEASE_NAME_FN = 0x0087C3D0,
	};

	void InternName(const char* name, uint32_t* outHandle)
	{
		using Fn = void(__fastcall*)(const char* name, void* edx, uint32_t* out);
		reinterpret_cast<Fn>(INTERN_NAME_FN)(name, nullptr, outHandle);
	}

	void MakeNumberValue(double value, void** outValue)
	{
		const uintptr_t fn = MAKE_NUMBER_FN;
		__asm
		{
			mov edi, outValue
			sub esp, 8
			fld value
			fstp qword ptr [esp]
			call dword ptr [fn]
			add esp, 8
		}
	}

	void SetMember(void* object, uint32_t* nameHandle, void** valuePtr)
	{
		using Fn = void(__stdcall*)(void* object, uint32_t* nameHandle, void** valuePtr);
		reinterpret_cast<Fn>(SET_MEMBER_FN)(object, nameHandle, valuePtr);
	}

	void ReleaseName(uint32_t handle)
	{
		const uintptr_t fn = RELEASE_NAME_FN;
		__asm
		{
			mov esi, handle
			call dword ptr [fn]
		}
	}

	void ValueAddRef(void* valueObject)
	{
		using Fn = void(__fastcall*)(void* self, void* edx);
		VirtualSlot<Fn>(valueObject, 0x0)(valueObject, nullptr);
	}

	void ValueRelease(void* valueObject)
	{
		using Fn = void(__fastcall*)(void* self, void* edx);
		VirtualSlot<Fn>(valueObject, 0x4)(valueObject, nullptr);
	}
	bool SetNumberMember(void* object, const char* name, double value)
	{
		uint32_t handle = 0;
		InternName(name, &handle);
		void* valueObject = nullptr;
		MakeNumberValue(value, &valueObject);
		if (valueObject == nullptr)
		{
			if (handle != 0) ReleaseName(handle);
			return false;
		}
		ValueAddRef(valueObject);
		void* valuePtr = valueObject;
		SetMember(object, &handle, &valuePtr);
		ValueRelease(valueObject);
		ValueRelease(valueObject);
		if (handle != 0) ReleaseName(handle);
		return true;
	}
	constexpr uintptr_t SET_BOOL_MEMBER_FN = 0x00882660;

	void CallSetBoolMember(void* object, uint32_t* nameHandle, bool value, void** outValue)
	{
		const uintptr_t fn = SET_BOOL_MEMBER_FN;
		const int flag = value ? 1 : 0;
		__asm
		{
			push edi
			mov edi, outValue
			push flag
			push nameHandle
			push object
			call dword ptr [fn]
			pop edi
		}
	}
	void SetBoolMember(void* object, const char* name, bool value)
	{
		uint32_t handle = 0;
		InternName(name, &handle);
		void* outValue = nullptr;
		CallSetBoolMember(object, &handle, value, &outValue);
		if (outValue != nullptr) ValueRelease(outValue);
		if (handle != 0) ReleaseName(handle);
	}
	constexpr uintptr_t RS_SLIDER_VTABLE = 0x011AD72C;
	constexpr uintptr_t COMPONENT_DATA_OFFSET = 0x8;
	constexpr uintptr_t COMPONENT_DIRTY_OFFSET = 0xD;
	constexpr uintptr_t CONTROLLER_COMPONENT_MAP_OFFSET = 0x60;
	constexpr uintptr_t COMPONENT_MAP_ROOT_OFFSET = 0x4;
	constexpr uintptr_t COMPONENT_NODE_LEFT_OFFSET = 0x8;
	constexpr uintptr_t COMPONENT_NODE_RIGHT_OFFSET = 0xC;
	constexpr uintptr_t COMPONENT_NODE_COMPONENT_OFFSET = 0x28;
	constexpr int COMPONENT_MAP_MAX_NODES = 128;
	void* FindRealizedSlider(void* controller, const char* sliderId)
	{
		const uintptr_t head = reinterpret_cast<uintptr_t>(controller) + CONTROLLER_COMPONENT_MAP_OFFSET;
		uintptr_t pending[COMPONENT_MAP_MAX_NODES];
		int pendingCount = 0;
		int visited = 0;
		pending[pendingCount++] = *reinterpret_cast<uintptr_t*>(head + COMPONENT_MAP_ROOT_OFFSET);
		while (pendingCount > 0 && visited < COMPONENT_MAP_MAX_NODES)
		{
			const uintptr_t node = pending[--pendingCount];
			if (node == 0 || node == head) continue;
			++visited;
			void* component = *reinterpret_cast<void**>(node + COMPONENT_NODE_COMPONENT_OFFSET);
			if (component != nullptr && *reinterpret_cast<uintptr_t*>(component) == RS_SLIDER_VTABLE)
			{
				void* data = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(component) + COMPONENT_DATA_OFFSET);
				const char* id = data != nullptr ? FindStringMember(data, "ID") : nullptr;
				if (id != nullptr && std::strcmp(id, sliderId) == 0) return component;
			}
			if (pendingCount + 2 <= COMPONENT_MAP_MAX_NODES)
			{
				pending[pendingCount++] = *reinterpret_cast<uintptr_t*>(node + COMPONENT_NODE_LEFT_OFFSET);
				pending[pendingCount++] = *reinterpret_cast<uintptr_t*>(node + COMPONENT_NODE_RIGHT_OFFSET);
			}
		}
		return nullptr;
	}
	constexpr uintptr_t SLIDER_VALUE_OFFSET = 0x88;
	constexpr uintptr_t SLIDER_SET_VALUE_FN = 0x005C3FB0;
	constexpr uintptr_t SLIDER_REFRESH_FN = 0x005C3CD0;

	void CallSliderSetValue(void* slider, float value)
	{
		const uintptr_t fn = SLIDER_SET_VALUE_FN;
		__asm
		{
			push edi
			mov edi, slider
			push 0
			push value
			call dword ptr [fn]
			pop edi
		}
	}
	bool SetRealizedSliderValue(void* slider, int value)
	{
		const float current = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(slider) + SLIDER_VALUE_OFFSET);
		if (static_cast<int>(current + 0.5f) == value) return false;
		CallSliderSetValue(slider, static_cast<float>(value));
		using RefreshFn = void(__stdcall*)(void* slider);
		reinterpret_cast<RefreshFn>(SLIDER_REFRESH_FN)(slider);
		return true;
	}
	int SyncRealizedFlowRow(void* controller, bool writeAccess, bool disable, int shownValue)
	{
		__try
		{
			void* slider = FindRealizedSlider(controller, FLOW_SLIDER_ID);
			if (slider == nullptr) return 0;
			int result = 4;   // found
			if (writeAccess)
			{
				void* data = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(slider) + COMPONENT_DATA_OFFSET);
				SetBoolMember(data, "DisableAccess", disable);
				*reinterpret_cast<volatile uint8_t*>(reinterpret_cast<uintptr_t>(slider) + COMPONENT_DIRTY_OFFSET) = 1;
				result |= 1;
			}
			void* focused = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(controller) + FOCUSED_COMPONENT_OFFSET);
			if (focused != slider && SetRealizedSliderValue(slider, shownValue)) result |= 2;
			return result;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}
	struct BuilderScan
	{
		int childCount;
		int rowFrom;
		int rowTo;
		int preset;           // bitmask 1 = InitialValue written, 2 = Value written; 0 = no member; -1 = row not found
		bool faulted;
		void* rowObject;      // our row's data object, found pre-builder, renumbered post-builder
		double* rowSortOrder; // holders, reported only (never written: they are shared flyweights)
		double* rowInitialValue;
		double* rowValue;
		void* flowRowObject;  // the FLOW MODE row, numbered right after ours
		char listing[1024];
	};

	void ScanBuilderRows(void* controller, bool presetOn, BuilderScan& scan)
	{
		scan.childCount = 0; scan.rowFrom = -1; scan.rowTo = -1; scan.preset = -1;
		scan.faulted = false; scan.rowObject = nullptr; scan.rowSortOrder = nullptr;
		scan.rowInitialValue = nullptr; scan.rowValue = nullptr; scan.flowRowObject = nullptr; scan.listing[0] = 0;
		const double flowValue = NoteByNoteProbe::IsAutomaticEnabled()
			&& NoteByNoteNativeScoring::GetFlowUntilMissEnabled() ? 1.0 : 0.0;
		size_t used = 0;
		__try
		{
			void* container = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(controller) + VIEW_CONTAINER_OFFSET);
			if (container == nullptr) return;
			ChildRange range = {};
			EnumerateChildren(container, &range);
			for (void** at = range.begin; at != nullptr && at != range.end && scan.childCount < 32; ++at)
			{
				++scan.childCount;
				void* child = *at;
				const char* id = FindStringMember(child, "ID");
				double* sortOrder = FindSortOrder(child);
				const int written = std::snprintf(scan.listing + used, sizeof(scan.listing) - used,
					"%s%s:%d", scan.childCount > 1 ? " " : "", id != nullptr ? id : "?",
					sortOrder != nullptr ? static_cast<int>(*sortOrder) : -1);
				if (written > 0 && used + written < sizeof(scan.listing)) used += written;
				if (id != nullptr && std::strcmp(id, FLOW_SLIDER_ID) == 0)
				{
					scan.flowRowObject = child;
					SetNumberMember(child, "InitialValue", flowValue);
					SetNumberMember(child, "Value", flowValue);
					flowBuiltDisabled = NoteByNoteProbe::IsAutomaticEnabled() ? 0 : 1;
					continue;
				}
				if (id == nullptr || std::strcmp(id, NOTE_BY_NOTE_SLIDER_ID) != 0) continue;
				scan.rowObject = child;
				scan.rowSortOrder = sortOrder;
				if (sortOrder != nullptr) scan.rowFrom = static_cast<int>(*sortOrder);
				scan.preset = 0;
				scan.rowInitialValue = FindNumberMember(child, "InitialValue");
				scan.rowValue = FindNumberMember(child, "Value");
				if (!presetOn) continue;
				if (SetNumberMember(child, "InitialValue", 1.0)) scan.preset |= 1;
				if (SetNumberMember(child, "Value", 1.0)) scan.preset |= 2;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			scan.faulted = true;
		}
	}
	bool RenumberScannedRow(BuilderScan& scan, int* counter)
	{
		__try
		{
			if (scan.rowObject == nullptr || counter == nullptr) return false;
			scan.rowTo = *counter;
			if (!SetNumberMember(scan.rowObject, "SortOrder", static_cast<double>(scan.rowTo)))
			{
				scan.rowTo = -1;
				return false;
			}
			*counter = scan.rowTo + 1;
			if (scan.flowRowObject != nullptr
				&& SetNumberMember(scan.flowRowObject, "SortOrder", static_cast<double>(*counter)))
				*counter += 1;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			scan.rowTo = -1;
			return false;
		}
	}
	void FinishFlowRow(BuilderScan& scan)
	{
		__try
		{
			if (scan.flowRowObject != nullptr)
				SetBoolMember(scan.flowRowObject, "DisableAccess", !NoteByNoteProbe::IsAutomaticEnabled());
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}
	volatile bool rowProbeRequested = false;
	bool ProbeRowsUnwound(void* target, uintptr_t& containerAddress, uintptr_t& rangeBegin,
		uintptr_t& rangeEnd, int& count, char* text, size_t textCapacity)
	{
		containerAddress = 0; rangeBegin = 0; rangeEnd = 0; count = 0; text[0] = 0;
		size_t used = 0;
		__try
		{
			void* container = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(target) + VIEW_CONTAINER_OFFSET);
			containerAddress = reinterpret_cast<uintptr_t>(container);
			if (container == nullptr) return true;
			ChildRange range = {};
			EnumerateChildren(container, &range);
			rangeBegin = reinterpret_cast<uintptr_t>(range.begin);
			rangeEnd = reinterpret_cast<uintptr_t>(range.end);
			for (void** at = range.begin; at != nullptr && at != range.end && count < 32; ++at)
			{
				++count;
				double* sortOrder = FindSortOrder(*at);
				const int written = sortOrder != nullptr
					? std::snprintf(text + used, textCapacity - used, "%s%p:%d", count > 1 ? " " : "", *at, static_cast<int>(*sortOrder))
					: std::snprintf(text + used, textCapacity - used, "%s%p:-", count > 1 ? " " : "", *at);
				if (written <= 0 || used + written >= textCapacity) break;
				used += written;
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void ProbeRows(void* target)
	{
		uintptr_t containerAddress = 0, rangeBegin = 0, rangeEnd = 0;
		int count = 0;
		char text[1024];
		const bool ok = ProbeRowsUnwound(target, containerAddress, rangeBegin, rangeEnd, count, text, sizeof(text));
		std::string listing = text;
		if (!ok) listing += " <fault>";
		unsigned long pContainer = 0, pVtable = 0, pSlot74 = 0;
		SampleContainerVtable(target, pContainer, pVtable, pSlot74);
		std::lock_guard<std::mutex> lock(diagnosticsMutex);
		diagnostics.probeContainer = containerAddress;
		diagnostics.probeRangeBegin = rangeBegin;
		diagnostics.probeRangeEnd = rangeEnd;
		diagnostics.probeChildCount = count;
		diagnostics.probeListing = listing;
		diagnostics.probeVtable = pVtable;
		diagnostics.probeSlot74 = pSlot74;
		++diagnostics.probes;
	}
	constexpr const char* RIFF_REPEATER_SETTINGS_CONTROLLER_NAME = "LearnASong_RiffRepeater";
	constexpr const char* SPEED_SLIDER_ID = "Speed";
	constexpr uintptr_t SLIDER_MAX_OFFSET = 0x9C;
	constexpr uintptr_t SLIDER_REFRESH_FROM_MODEL_FN = 0x005C3590;
	constexpr uintptr_t CONTROLLER_SPEED_PERCENT_OFFSET = 0x300;
	constexpr uintptr_t CONTROLLER_SECTION_START_INDEX_OFFSET = 0x310;
	constexpr uintptr_t CONTROLLER_SECTION_END_INDEX_OFFSET = 0x318;
	constexpr uintptr_t EVENT_BUS_GLOBAL = 0x0135F54C;
	constexpr size_t MENU_CAP_NOTE_LIMIT = 4096;

	bool ControllerNameIs(void* candidate, const char* name)
	{
		if (candidate == nullptr) return false;
		__try
		{
			const uintptr_t base = reinterpret_cast<uintptr_t>(candidate);
			const char* text = reinterpret_cast<const char*>(base + CONTROLLER_NAME_OFFSET);
			if (*reinterpret_cast<uintptr_t*>(base + CONTROLLER_NAME_OFFSET + 0x14) != base + CONTROLLER_NAME_OFFSET + 0x10)
				text = *reinterpret_cast<const char* const*>(base + CONTROLLER_NAME_OFFSET);
			const char* end = *reinterpret_cast<const char* const*>(base + CONTROLLER_NAME_OFFSET + 0x10);
			const size_t length = static_cast<size_t>(end - text);
			return length == std::strlen(name) && std::memcmp(text, name, length) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	float menuCapTimes[MENU_CAP_NOTE_LIMIT];
	int menuCapFirstSection = -1;   // the sections CollectMenuSectionNoteTimes resolved, for the log
	int menuCapLastSection = -1;
	constexpr int MENU_CAP_SECTION_LIMIT = 512;
	int menuCapSliceStart[MENU_CAP_SECTION_LIMIT + 1];
	int menuCapSliceCount = 0;
	int CollectMenuSectionNoteTimes(void* controller)
	{
		__try
		{
			const uintptr_t bus = *reinterpret_cast<uintptr_t*>(EVENT_BUS_GLOBAL);
			if (bus == 0) return -1;
			const uintptr_t holder = *reinterpret_cast<uintptr_t*>(bus + 0x10);
			if (holder == 0) return -1;
			const uintptr_t owner = *reinterpret_cast<uintptr_t*>(holder + 0x50);
			if (owner == 0) return -1;
			const uintptr_t song = *reinterpret_cast<uintptr_t*>(owner + 0x78);
			const uintptr_t levelMap = *reinterpret_cast<uintptr_t*>(owner + 0x7C);
			if (song == 0 || levelMap == 0) return -1;

			const uintptr_t sectionsBegin = *reinterpret_cast<uintptr_t*>(song + 0xF4);
			const uintptr_t sectionsEnd = *reinterpret_cast<uintptr_t*>(song + 0xF8);
			if (sectionsBegin == 0 || sectionsEnd <= sectionsBegin || (sectionsEnd - sectionsBegin) % 0x58 != 0) return -1;
			const int sectionCount = static_cast<int>((sectionsEnd - sectionsBegin) / 0x58);
			(void)controller;
			const float loopStart = *reinterpret_cast<float*>(owner + 0x3C0);
			const float loopEnd = *reinterpret_cast<float*>(owner + 0x3C8);
			int firstSection = -1;
			int lastSection = -1;
			for (int index = 0; index < sectionCount; ++index)
			{
				const float start = *reinterpret_cast<float*>(sectionsBegin + index * 0x58 + 0x24);
				const float end = *reinterpret_cast<float*>(sectionsBegin + index * 0x58 + 0x28);
				if (firstSection < 0 && loopStart >= start - 0.01f && loopStart < end - 0.01f) firstSection = index;
				if (loopEnd > start + 0.01f && loopEnd <= end + 0.01f) lastSection = index;
			}
			if (firstSection < 0 || lastSection < firstSection) return -1;
			menuCapFirstSection = firstSection;
			menuCapLastSection = lastSection;
			const int firstIteration = *reinterpret_cast<int*>(sectionsBegin + firstSection * 0x58 + 0x2C);
			const int lastIteration = *reinterpret_cast<int*>(sectionsBegin + lastSection * 0x58 + 0x30);
			if (lastSection - firstSection + 1 > MENU_CAP_SECTION_LIMIT) return -1;

			const uintptr_t perIterationBegin = *reinterpret_cast<uintptr_t*>(levelMap + 0x18);
			const uintptr_t perIterationEnd = *reinterpret_cast<uintptr_t*>(levelMap + 0x1C);
			const uintptr_t levelsBegin = *reinterpret_cast<uintptr_t*>(song + 0x40);
			const uintptr_t levelsEnd = *reinterpret_cast<uintptr_t*>(song + 0x44);
			if (perIterationBegin == 0 || perIterationEnd <= perIterationBegin
				|| levelsBegin == 0 || levelsEnd <= levelsBegin) return -1;
			const int iterationCount = static_cast<int>((perIterationEnd - perIterationBegin) / 0x40);
			const int levelCount = static_cast<int>((levelsEnd - levelsBegin) / 0x64);
			if (firstIteration < 0 || lastIteration < firstIteration || firstIteration >= iterationCount) return -1;

			int count = 0;
			menuCapSliceCount = 0;
			for (int section = firstSection; section <= lastSection; ++section)
			{
				menuCapSliceStart[menuCapSliceCount++] = count;
				const int sectionFirst = *reinterpret_cast<int*>(sectionsBegin + section * 0x58 + 0x2C);
				const int sectionLast = *reinterpret_cast<int*>(sectionsBegin + section * 0x58 + 0x30);
				for (int iteration = sectionFirst; iteration <= sectionLast && iteration < iterationCount; ++iteration)
				{
					if (iteration < 0) continue;
					const int level = *reinterpret_cast<int*>(perIterationBegin + iteration * 0x40 + 4);
					if (level < 0 || level >= levelCount) continue;
					const uintptr_t levelRecord = levelsBegin + static_cast<uintptr_t>(level) * 0x64;
					const uintptr_t notesBegin = *reinterpret_cast<uintptr_t*>(levelRecord + 0x30);
					const uintptr_t notesEnd = *reinterpret_cast<uintptr_t*>(levelRecord + 0x34);
					if (notesBegin == 0 || notesEnd <= notesBegin || (notesEnd - notesBegin) / 0x1C8 > 50000) continue;
					for (uintptr_t note = notesBegin; note + 0x1C8 <= notesEnd; note += 0x1C8)
					{
						if (*reinterpret_cast<int*>(note + 0x20) != iteration) continue;
						if (count >= static_cast<int>(MENU_CAP_NOTE_LIMIT)) break;
						menuCapTimes[count++] = *reinterpret_cast<float*>(note + 0xC);
					}
				}
			}
			menuCapSliceStart[menuCapSliceCount] = count;
			return count;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	struct SpeedClampResult
	{
		bool sliderFound = false;
		bool maxWritten = false;
		bool valueLowered = false;
		int previousValue = 0;
	};
	SpeedClampResult ApplySpeedSliderClamp(void* controller, int maxSlider)
	{
		SpeedClampResult result;
		__try
		{
			void* slider = FindRealizedSlider(controller, SPEED_SLIDER_ID);
			if (slider == nullptr) return result;
			result.sliderFound = true;
			const uintptr_t sliderBase = reinterpret_cast<uintptr_t>(slider);
			const float wantedMax = maxSlider > 0 ? static_cast<float>(maxSlider) : 100.0f;
			float* maxField = reinterpret_cast<float*>(sliderBase + SLIDER_MAX_OFFSET);
			if (*maxField != wantedMax)
			{
				*maxField = wantedMax;
				result.maxWritten = true;
			}
			if (maxSlider <= 0) return result;
			const float current = *reinterpret_cast<float*>(sliderBase + SLIDER_VALUE_OFFSET);
			result.previousValue = static_cast<int>(current + 0.5f);
			if (result.previousValue > maxSlider)
			{
				*reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(controller) + CONTROLLER_SPEED_PERCENT_OFFSET)
					= static_cast<float>(maxSlider);
				SetRealizedSliderValue(slider, maxSlider);
				result.valueLowered = true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		return result;
	}
	bool ApplySpeedSliderAccess(void* controller, bool disable, bool lockAt100)
	{
		__try
		{
			void* slider = FindRealizedSlider(controller, SPEED_SLIDER_ID);
			if (slider == nullptr) return false;
			const uintptr_t sliderBase = reinterpret_cast<uintptr_t>(slider);
			if (lockAt100)
			{
				*reinterpret_cast<float*>(sliderBase + SLIDER_MAX_OFFSET) = 100.0f;
				*reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(controller) + CONTROLLER_SPEED_PERCENT_OFFSET) = 100.0f;
				SetRealizedSliderValue(slider, 100);
			}
			void* data = *reinterpret_cast<void**>(sliderBase + COMPONENT_DATA_OFFSET);
			SetBoolMember(data, "DisableAccess", disable);
			using RefreshFromModelFn = void(__stdcall*)(void* component);
			reinterpret_cast<RefreshFromModelFn>(SLIDER_REFRESH_FROM_MODEL_FN)(slider);
			*reinterpret_cast<volatile uint8_t*>(sliderBase + COMPONENT_DIRTY_OFFSET) = 1;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
	void* speedLockController = nullptr;
	bool speedLockApplied = false;

	void* speedClampController = nullptr;
	void* speedClampPendingController = nullptr;   // a screen whose notes were not readable yet (logged once)
	int speedClampSlider = 0;        // 0 = no clamp in force
	float speedClampRealPercent = 100.0f;
	bool TryReadControllerSpeed(void* controller, float& slider)
	{
		__try
		{
			slider = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(controller) + CONTROLLER_SPEED_PERCENT_OFFSET);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void RecordPlayerSpeedSlider(void* controller)
	{
		float slider = 0.f;
		if (!TryReadControllerSpeed(controller, slider) || !(slider >= 1.f && slider <= 400.f)
			|| slider == RiffRepeater::playerSliderPercent.load()) return;
		RiffRepeater::playerSliderPercent.store(slider);
		LOG_INFO("(NBN FLOW SPEED) Player SPEED slider " << slider << " = "
			<< RiffRepeater::SliderToRealSpeed(slider) << "% real (Linear Riff Repeater "
			<< (RiffRepeater::currentlyEnabled_LinearRR ? "on" : "off") << ")." << std::endl);
	}

	void PollSettingsSpeedClamp(void* controller)
	{
		const bool noteByNote = NoteByNoteProbe::IsAutomaticEnabled();
		const bool flow = NoteByNoteNativeScoring::GetFlowUntilMissEnabled();
		if (noteByNote && !flow)
		{
			if ((!speedLockApplied || controller != speedLockController) && ApplySpeedSliderAccess(controller, true, true))
			{
				speedLockApplied = true;
				speedLockController = controller;
				LOG_INFO("(NBN FLOW SPEED) Settings SPEED locked at 100% and greyed (Note by Note on, FLOW MODE off)." << std::endl);
			}
			speedClampSlider = 0;
			speedClampController = nullptr;
			return;
		}
		if (speedLockApplied && controller == speedLockController)
		{
			ApplySpeedSliderAccess(controller, false, false);
			LOG_INFO("(NBN FLOW SPEED) Settings SPEED slider available again." << std::endl);
		}
		speedLockApplied = false;
		speedLockController = nullptr;
		const bool wanted = noteByNote && flow;
		if (!wanted)
		{
			if (speedClampSlider != 0 && controller == speedClampController)
			{
				ApplySpeedSliderClamp(controller, 0);
				LOG_INFO("(NBN FLOW SPEED) Settings SPEED slider back to its full range (flow off)." << std::endl);
			}
			speedClampSlider = 0;
			speedClampController = nullptr;
			return;
		}
		if (controller != speedClampController)
		{
			const int count = CollectMenuSectionNoteTimes(controller);
			if (count < 0)
			{
				speedClampSlider = 0;
				if (controller != speedClampPendingController)
				{
					speedClampPendingController = controller;
					LOG_INFO("(NBN FLOW SPEED) Settings screen: the selection's notes are not readable yet; retrying." << std::endl);
				}
				return;
			}
			speedClampController = controller;
			speedClampPendingController = nullptr;
			const bool bass = NoteByNoteNativeScoring::IsBassArrangementActive();
			float typicalGap = 0.0f;
			int limitingSection = menuCapFirstSection;
			int limitingNotes = count;
			speedClampRealPercent = 100.0f;
			for (int slice = 0; slice < menuCapSliceCount; ++slice)
			{
				const int start = menuCapSliceStart[slice];
				const int sliceNotes = menuCapSliceStart[slice + 1] - start;
				float sliceGap = 0.0f;
				const float cap = FlowSpeedCap::CapFromNoteTimes(menuCapTimes + start, static_cast<size_t>(sliceNotes), bass, &sliceGap);
				if (cap < speedClampRealPercent)
				{
					speedClampRealPercent = cap;
					typicalGap = sliceGap;
					limitingSection = menuCapFirstSection + slice;
					limitingNotes = sliceNotes;
				}
			}
			speedClampSlider = speedClampRealPercent < 99.5f
				? (std::max)(1, static_cast<int>(RiffRepeater::RealSpeedToSlider(speedClampRealPercent)))
				: 0;
			LOG_INFO("(NBN FLOW SPEED) Settings screen: sections " << menuCapFirstSection << ".." << menuCapLastSection
				<< " have " << count << " notes; densest is section " << limitingSection << " (" << limitingNotes
				<< " notes, typical tight gap " << typicalGap << " s) -> flow cap "
				<< speedClampRealPercent << "% (slider " << speedClampSlider << ")." << std::endl);
		}
		if (speedClampSlider == 0) return;
		const SpeedClampResult result = ApplySpeedSliderClamp(controller, speedClampSlider);
		if (result.valueLowered)
		{
			LOG_INFO("(NBN FLOW SPEED) Settings SPEED slider lowered " << result.previousValue << " -> "
				<< speedClampSlider << " (flow cap " << speedClampRealPercent << "%); it cannot be raised past it."
				<< std::endl);
		}
	}

	bool ControllerLooksLive(void* candidate)
	{
		if (candidate == nullptr || knownControllerVtable == 0) return false;
		__try
		{
			const uintptr_t base = reinterpret_cast<uintptr_t>(candidate);
			if (*reinterpret_cast<uintptr_t*>(base) != knownControllerVtable) return false;
			const char* text = reinterpret_cast<const char*>(base + CONTROLLER_NAME_OFFSET);
			if (*reinterpret_cast<uintptr_t*>(base + CONTROLLER_NAME_OFFSET + 0x14) != base + CONTROLLER_NAME_OFFSET + 0x10)
				text = *reinterpret_cast<const char* const*>(base + CONTROLLER_NAME_OFFSET);
			const char* end = *reinterpret_cast<const char* const*>(base + CONTROLLER_NAME_OFFSET + 0x10);
			const size_t length = static_cast<size_t>(end - text);
			if (length != std::strlen(ADVANCED_SETTINGS_CONTROLLER_NAME)) return false;
			return std::memcmp(text, ADVANCED_SETTINGS_CONTROLLER_NAME, length) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
	std::atomic<int> menuRowState{ 0 };

	void __fastcall SettingsBuilderDetour(void* controller, void* unusedEdx, int* counter)
	{
		originalSettingsBuilder(controller, unusedEdx, counter);
		settingsBuiltController = controller;
	}

	void __stdcall StageBuilderDetour(void* controller, int* counter)
	{
		knownControllerVtable = *reinterpret_cast<uintptr_t*>(controller);
		unsigned long preContainer = 0, preVtable = 0, preSlot74 = 0;
		SampleContainerVtable(controller, preContainer, preVtable, preSlot74);
		const bool enabledAtBuild = NoteByNoteProbe::IsAutomaticEnabled()
			&& OverlayToggles::Get("menu_preset");
		static BuilderScan scan;   // UI thread only; static keeps the 1 KB listing off the stack
		ScanBuilderRows(controller, enabledAtBuild, scan);

		originalStageBuilder(controller, counter);
		const bool moved = OverlayToggles::Get("menu_renumber") && RenumberScannedRow(scan, counter);
		FinishFlowRow(scan);
		unsigned long bContainer = 0, bVtable = 0, bSlot74 = 0;
		SampleContainerVtable(controller, bContainer, bVtable, bSlot74);
		{
			std::lock_guard<std::mutex> lock(diagnosticsMutex);
			diagnostics.preBuilderContainer = preContainer;
			diagnostics.preBuilderVtable = preVtable;
			diagnostics.preBuilderSlot74 = preSlot74;
			++diagnostics.stageBuilds;
			diagnostics.lastRowSortOrderFrom = scan.rowFrom;
			diagnostics.lastRowSortOrderTo = scan.rowTo;
			diagnostics.lastChildCount = scan.childCount;
			diagnostics.lastRowPreset = scan.preset;
			diagnostics.builderListing = scan.listing;
			if (scan.faulted) diagnostics.builderListing += " <fault>";
			diagnostics.lastBuilderController = reinterpret_cast<uintptr_t>(controller);
			diagnostics.lastBuilderCounter = counter != nullptr ? *counter : -1;
			diagnostics.builderContainer = bContainer;
			diagnostics.builderVtable = bVtable;
			diagnostics.builderSlot74 = bSlot74;
		}
		dispatcherController = controller;
		++dispatcherGeneration;
		if (!scan.faulted) menuRowState.store(scan.preset == -1 ? 2 : 1);
		LOG_INFO("(NBN MENU) Advanced Settings built: " << scan.childCount << " rows ["
			<< scan.listing << (scan.faulted ? " <fault>" : "") << "]; NOTE BY NOTE "
			<< (moved ? "renumbered " : "NOT renumbered (")
			<< scan.rowFrom << (moved ? " -> " : ")") << (moved ? std::to_string(scan.rowTo) : std::string())
			<< "; Note by Note is " << (enabledAtBuild ? "ON" : "OFF")
			<< (scan.preset == -1 ? ", row not found"
				: !enabledAtBuild ? ", row left at Off"
				: scan.preset == 0 ? ", row NOT preset (no InitialValue/Value member)"
				: ", row preset to On")
			<< (enabledAtBuild && scan.preset > 0
				? std::string(" (InitialValue ") + ((scan.preset & 1) ? "yes" : "no")
					+ ", Value " + ((scan.preset & 2) ? "yes" : "no") + ")"
				: std::string())
			<< ". Holders: SortOrder=0x" << std::hex << reinterpret_cast<uintptr_t>(scan.rowSortOrder)
			<< " InitialValue=0x" << reinterpret_cast<uintptr_t>(scan.rowInitialValue)
			<< " Value=0x" << reinterpret_cast<uintptr_t>(scan.rowValue) << std::dec << "." << std::endl);
	}

	void DropController(const char* reason)
	{
		if (controller != nullptr)
		{
			LOG_INFO("(NBN MENU) Riff Repeater controller dropped: " << reason << "." << std::endl);
		}
		controller = nullptr;
		controllerMenu.clear();
		hasFocusedValue = false;
		hasFlowFocusedValue = false;
		appliedFlowDisable = -1;
	}
	bool PollFlowRow(void* target, bool noteByNoteOn)
	{
		const int wantDisable = noteByNoteOn ? 0 : 1;
		const bool flowOn = NoteByNoteNativeScoring::GetFlowUntilMissEnabled();
		const int shownValue = noteByNoteOn && flowOn ? 1 : 0;
		const int sync = SyncRealizedFlowRow(target, appliedFlowDisable != wantDisable, wantDisable == 1, shownValue);
		if (sync < 0) return false;
		if (sync & 1)
		{
			LOG_INFO("(NBN MENU) FLOW MODE " << (wantDisable ? "greyed out" : "made available")
				<< " (Note by Note " << (noteByNoteOn ? "ON" : "OFF") << ")." << std::endl);
			appliedFlowDisable = wantDisable;
		}
		if (sync & 2)
		{
			LOG_INFO("(NBN MENU) FLOW MODE row set to " << (shownValue ? "On" : "Off") << "." << std::endl);
		}
		bool faulted = false;
		int value = 0;
		if (AdjustSliderGuarded(target, 0, value, faulted, false, FLOW_SLIDER_ID))
		{
			const unsigned long keyGeneration = dispatcherGeneration;
			const bool keyPressed = keyGeneration != lastFlowReadGeneration;
			lastFlowReadGeneration = keyGeneration;
			const bool userToggled = hasFlowFocusedValue && value != lastFlowFocusedValue && keyPressed
				&& noteByNoteOn;
			if (hasFlowFocusedValue && value != lastFlowFocusedValue && !keyPressed)
				LOG_INFO("(NBN MENU) FLOW MODE row value changed to " << value
					<< " without a key press; ignored (flow stays " << (flowOn ? "ON" : "OFF") << ")." << std::endl);
			if (userToggled)
			{
				NoteByNoteNativeScoring::SetFlowUntilMissEnabled(value == 1);
				LOG_INFO("(NBN MENU) FLOW MODE rocker moved to " << value << " -> flow "
					<< (value == 1 ? "ON" : "OFF") << (noteByNoteOn ? "." : " (takes effect when Note by Note is on).")
					<< std::endl);
			}
			hasFlowFocusedValue = true;
			lastFlowFocusedValue = value;
			return !faulted;
		}
		if (faulted) return false;
		hasFlowFocusedValue = false;
		int moved = 0;
		AdjustSliderGuarded(target, shownValue == 1 ? 1 : -1, moved, faulted, false, FLOW_SLIDER_ID);
		return !faulted;
	}
}

void NoteByNoteMenu::Initialize()
{
	if (!MatchesBytes(RIFF_REPEATER_INPUT_DISPATCHER, DISPATCHER_PROLOGUE, sizeof(DISPATCHER_PROLOGUE))
		|| !MatchesBytes(RIFF_REPEATER_ADJUST_SLIDER, ADJUST_SLIDER_PROLOGUE, sizeof(ADJUST_SLIDER_PROLOGUE)))
	{
		LOG_ERROR("(NBN MENU) The Riff Repeater functions do not match the expected game build;"
			<< " the NOTE BY NOTE rocker is not wired (N key and the bridge still work)."
			<< " Found dispatcher=" << MemUtil::DescribeCodeBytes(RIFF_REPEATER_INPUT_DISPATCHER, sizeof(DISPATCHER_PROLOGUE))
			<< ", adjust=" << MemUtil::DescribeCodeBytes(RIFF_REPEATER_ADJUST_SLIDER, sizeof(ADJUST_SLIDER_PROLOGUE))
			<< "." << std::endl);
		return;
	}

	originalDispatcher = DetourFunction(
		reinterpret_cast<byte*>(RIFF_REPEATER_INPUT_DISPATCHER),
		reinterpret_cast<byte*>(DispatcherDetour));
	if (originalDispatcher == nullptr)
	{
		LOG_ERROR("(NBN MENU) Could not hook the Riff Repeater input dispatcher." << std::endl);
		return;
	}

	if (MatchesBytes(RIFF_REPEATER_STAGE_BUILDER, STAGE_BUILDER_PROLOGUE, sizeof(STAGE_BUILDER_PROLOGUE)))
	{
		originalStageBuilder = reinterpret_cast<StageBuilder>(DetourFunction(
			reinterpret_cast<byte*>(RIFF_REPEATER_STAGE_BUILDER),
			reinterpret_cast<byte*>(StageBuilderDetour)));
		if (originalStageBuilder == nullptr)
		{
			LOG_ERROR("(NBN MENU) Could not hook the Advanced Settings stage builder;"
				<< " the NOTE BY NOTE row stays first in the list." << std::endl);
		}
	}
	else
	{
		LOG_ERROR("(NBN MENU) The Advanced Settings stage builder does not match the expected"
			<< " game build; the NOTE BY NOTE row stays first in the list." << std::endl);
	}

	if (MatchesBytes(RIFF_REPEATER_SETTINGS_BUILDER, SETTINGS_BUILDER_PROLOGUE, sizeof(SETTINGS_BUILDER_PROLOGUE)))
	{
		originalSettingsBuilder = reinterpret_cast<SettingsBuilder>(DetourFunction(
			reinterpret_cast<byte*>(RIFF_REPEATER_SETTINGS_BUILDER),
			reinterpret_cast<byte*>(SettingsBuilderDetour)));
		if (originalSettingsBuilder == nullptr)
			LOG_ERROR("(NBN MENU) Could not hook the Riff Repeater Settings builder; the flow speed"
				<< " clamp waits for a key press on that screen." << std::endl);
	}
	else
	{
		LOG_ERROR("(NBN MENU) The Riff Repeater Settings builder does not match the expected game"
			<< " build; the flow speed clamp waits for a key press on that screen." << std::endl);
	}

	{
		std::lock_guard<std::mutex> lock(diagnosticsMutex);
		diagnostics.hooked = true;
	}
	LOG_INFO("(NBN MENU) Riff Repeater rocker wired: dispatcher=0x" << std::hex
		<< RIFF_REPEATER_INPUT_DISPATCHER << " adjust=0x" << RIFF_REPEATER_ADJUST_SLIDER << std::dec
		<< "; the row is read while focused and repaired by name while not." << std::endl);
}

void NoteByNoteMenu::Poll()
{
	if (originalDispatcher == nullptr) return;

	if (!GameState::Menus::IsInRiffRepeaterMenus())
	{
		if (controller != nullptr) DropController("left the Riff Repeater menus");
		if (GameState::currentMenu.rfind("LearnASong_", 0) != 0) settingsBuiltController = nullptr;
		return;
	}
	{
		void* const built = settingsBuiltController;
		if (built != nullptr && GameState::currentMenu == RIFF_REPEATER_SETTINGS_CONTROLLER_NAME
			&& ControllerNameIs(built, RIFF_REPEATER_SETTINGS_CONTROLLER_NAME)
			&& (controller == nullptr || controller == built || !ControllerLooksLive(controller)))
		{
			if (++frameCounter % POLL_FRAME_INTERVAL == 0)
			{
				PollSettingsSpeedClamp(built);
				RecordPlayerSpeedSlider(built);
			}
		}
	}
	const unsigned long generation = dispatcherGeneration;
	if (generation != adoptedGeneration)
	{
		adoptedGeneration = generation;
		void* const captured = dispatcherController;
		if (captured != controller)
		{
			controller = captured;
			controllerMenu = GameState::currentMenu;
			hasFocusedValue = false;
			hasFlowFocusedValue = false;
			appliedFlowDisable = flowBuiltDisabled;
			menuSettleFrames = 30;
		}
	}
	if (menuSettleFrames > 0)
	{
		--menuSettleFrames;
		if (GameState::currentMenu != controllerMenu) controllerMenu = GameState::currentMenu;
	}
	else if (controller != nullptr && GameState::currentMenu != controllerMenu)
	{
		DropController("screen changed");
	}
	if (controller == nullptr) return;
	if (!ControllerLooksLive(controller))
	{
		if (ControllerNameIs(controller, RIFF_REPEATER_SETTINGS_CONTROLLER_NAME))
		{
			if (++frameCounter % POLL_FRAME_INTERVAL == 0)
			{
				PollSettingsSpeedClamp(controller);
				RecordPlayerSpeedSlider(controller);
			}
			return;
		}
		DropController("not a live Advanced Settings controller");
		return;
	}
	if (rowProbeRequested)
	{
		rowProbeRequested = false;
		ProbeRows(controller);
	}
	if (++frameCounter % POLL_FRAME_INTERVAL != 0) return;

	const bool enabled = NoteByNoteProbe::IsAutomaticEnabled();
	bool faulted = false;
	int value = 0;
	if (AdjustSliderGuarded(controller, 0, value, faulted, false))
	{
		{
			std::lock_guard<std::mutex> lock(diagnosticsMutex);
			++diagnostics.focusedReads;
			diagnostics.lastRowValue = value;
		}
		const bool userToggled = hasFocusedValue
			? value != lastFocusedValue
			: (value == 1) != enabled;
		if (userToggled)
		{
			const bool wantEnabled = value == 1;
			const bool accepted = NoteByNoteProbe::SetAutomaticEnabled(wantEnabled);
			{
				std::lock_guard<std::mutex> lock(diagnosticsMutex);
				++diagnostics.toggles;
				diagnostics.lastToggleAccepted = accepted;
			}
			LOG_INFO("(NBN MENU) Rocker moved to " << value << " -> Note by Note "
				<< (wantEnabled ? "ON" : "OFF") << (accepted ? " accepted" : " REJECTED")
				<< "; state is " << (NoteByNoteProbe::IsAutomaticEnabled() ? "ON" : "OFF")
				<< "." << std::endl);
		}
		hasFocusedValue = true;
		lastFocusedValue = value;
		if (!PollFlowRow(controller, NoteByNoteProbe::IsAutomaticEnabled())) DropController("faulted");
		return;
	}
	if (faulted)
	{
		if (!loggedFault)
		{
			loggedFault = true;
			LOG_ERROR("(NBN MENU) AdjustSlider faulted on controller 0x" << std::hex
				<< reinterpret_cast<uintptr_t>(controller) << std::dec
				<< " (menu " << controllerMenu << "); pointer dropped." << std::endl);
		}
		DropController("faulted");
		return;
	}
	hasFocusedValue = false;
	int moved = 0;
	if (AdjustSliderGuarded(controller, enabled ? 1 : -1, moved, faulted, false))
	{
		std::lock_guard<std::mutex> lock(diagnosticsMutex);
		++diagnostics.repairs;
		diagnostics.lastRowValue = moved;
	}
	else if (faulted)
	{
		DropController("faulted");
		return;
	}
	if (!PollFlowRow(controller, enabled)) DropController("faulted");
}

void NoteByNoteMenu::RequestRowProbe()
{
	rowProbeRequested = true;
}

bool NoteByNoteMenu::IsMenuRowMissing()
{
	return menuRowState.load() == 2;
}

NoteByNoteMenu::Diagnostics NoteByNoteMenu::GetDiagnostics()
{
	std::lock_guard<std::mutex> lock(diagnosticsMutex);
	auto copy = diagnostics;
	copy.controllerCaptured = controller != nullptr;
	return copy;
}
