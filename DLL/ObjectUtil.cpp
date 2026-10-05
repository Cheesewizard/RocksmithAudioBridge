#include "stdafx.h"
#include "ObjectUtil.hpp"

namespace ObjectUtil {
	std::mutex ObjectScaleMutex;

	Object* GetRootObject() {
		auto root = (Object*)MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_rootObject, Offsets::ptr_rootObjectOffsets, true);

		// Disabled due to spam of the log.
		/*if (root == NULL)
			LOG_ERROR("(ObjUtil) Root object is NULL. Some Twitch mods may not work" << std::endl);*/

		return root;
	}

	void GetChildrenOfObject_SEH(Object* parent, std::vector<Object*>& children)
	{
		if (parent == nullptr) return;

		__try
		{
			Object** childrenArray = parent->children;
			if (childrenArray == nullptr) {
				return;
			}

			for (size_t i = 0; i < parent->childCount; i++)
			{
				if (childrenArray[i] != nullptr) {
					children.push_back(parent->children[i]);
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	std::vector<Object*> GetChildrenOfObject(Object* parent) {
		std::vector<Object*> children;

		if (parent == nullptr) return children;

		GetChildrenOfObject_SEH(parent, children);

		return children;
	}

	void UpdateScales() {
		ObjectUtil::Object* rootObject = ObjectUtil::GetRootObject();

		std::vector<ObjectUtil::Object*> children = ObjectUtil::GetChildrenOfObject(rootObject);

		for (auto child : children)
		{
			// The children snapshot goes stale mid-walk: the game frees objects on song
			// load from another thread, so a non-null child can dangle by the time we get
			// here (crashed at the className read below). Guard the child pointer itself,
			// not just its className.
			if (!child || MemUtil::IsBadReadPtr(child))
			{
				continue;
			}

			if (!child->className || MemUtil::IsBadReadPtr(child->className))
			{
				continue;
			}

			std::scoped_lock<std::mutex> lock(ObjectScaleMutex);

			std::string className = child->className;
			if (ObjectScaleMap.find(className) == ObjectScaleMap.end()) continue;

			child->scale = ObjectScaleMap[className];
		}
	}

	void SetObjectScales(const std::map<std::string, float>& scales)
	{
		std::scoped_lock<std::mutex> lock(ObjectScaleMutex);

		//Apply scales to map
		for (const auto& [key, value] : scales) {
			ObjectScaleMap[key] = value;
		}
	}

	std::map<std::string, float> ObjectScaleMap{};

    std::vector<std::string> AllNoteParts = { "ActorChordLetter", "MeshBracketLarge", "MeshNoteBendArrow", "MeshNoteBendPanel", "MeshNoteIconFretHandMute", "MeshNoteIconHammerOn", "MeshNoteIconHarmonic", "MeshNoteIconPalmMute",
		"MeshNoteIconPinchHarmonic", "MeshNoteIconPop", "MeshNoteIconPullOff", "MeshNoteIconSlap", "MeshNoteIconTap", "MeshNoteOpenAccent", "MeshNoteOpenArpeggio", "MeshNoteOpenLH", "MeshNoteOpenRH", "MeshNoteSingleAccent",
		"MeshNoteSingleArpeggio", "MeshNoteSingleLH", "MeshNoteSingleRH", "MeshStemBracket", "MeshStemDoubleStop", "MeshStemNote", "MeshStemNoteOpen", "MeshStrumBracketLarge", "MeshStrumBracketSmall", "MeshStrumLarge", "MeshStrumLargeAccent",
		"MeshStrumLargeArpeggio", "MeshStrumLargeDoubleStop", "MeshStrumLargeFretMute", "MeshStrumLargePalmMute", "MeshStrumSmall", "MeshStrumSmallAccent", "MeshStrumSmallDoubleStop", "MeshStrumSmallDoubleStopFretMute",
	    "MeshStrumSmallDoubleStopPalmMute", "MeshStrumSmallFretMute", "MeshStrumSmallPalmMute", "NoteTail", "NoteTailMesh", "MeshFingerprintZoneLeft", "MeshFingerprintZoneRight" };
}