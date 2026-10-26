#include "stdafx.h"
#include "StompboxPage.hpp"
#include "OverlayUi.hpp"
#include "ToneDesignerUi.hpp"
#include "Lib/ImGUI/imgui_internal.h"   // ImRect, drop targets on the gaps, SetItemUsingMouseWheel
#include "ToneDesigner/StompboxLink.hpp"
#include "ToneDesigner/VstHostLink.hpp"
#include "ToneDesigner/SongLibrary.hpp"
#include "Shared/ShellOpenAsync.hpp"
#include "ExternalAmp/ExternalAmp.hpp"

#include <ShlObj.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Overlay
{
	namespace
	{
		using namespace StompboxLink;
		namespace Ink = ToneDesignerUi::Ink;
		using ToneDesignerUi::CaptionFont;
		using ToneDesignerUi::Chip;
		using ToneDesignerUi::DrawPill;
		using ToneDesignerUi::FoldLabel;
		using ToneDesignerUi::IconSquare;
		using ToneDesignerUi::Jack;
		using ToneDesignerUi::MutedCaption;
		using ToneDesignerUi::PedalLook;
		using ToneDesignerUi::PillLook;
		using ToneDesignerUi::PushPopupPadding;
		using ToneDesignerUi::Text;
		using ToneDesignerUi::TextWidth;
		using ToneDesignerUi::TintPill;

		// The panel's proportions, as on the VST rig page (OverlayShell.cpp, Panel).
		constexpr float MIN_BOARD_HEIGHT = 190.0f;
		constexpr float NATURAL_MAIN_HEIGHT = 300.0f;
		constexpr float DIVIDER_HEIGHT = 8.0f;
		constexpr float BASE_PANE_HEIGHT = 280.0f;
		constexpr float MIN_PANE_HEIGHT = 150.0f;
		constexpr float METER_COLUMN_WIDTH = 44.0f;
		constexpr float EDITOR_WIDTH = 420.0f;
		// The load meter's lines. The typical block's share of real time is the tone's own cost, judged against the
		// base game (480-frame blocks): kept under half, a tone has room on slower PCs than the one it was made on.
		// The slowest block is what this PC's block size risks: ASIO's 128 or 64 frames are a bonus, not the target.
		constexpr float SHARE_WARN_LOAD = 0.5f;
		constexpr float SHARE_HEAVY_LOAD = 0.7f;
		constexpr float PEAK_WARN_LOAD = 0.85f;
		constexpr ULONGLONG NOTICE_MILLISECONDS = 6000;
		constexpr ULONGLONG LATE_MILLISECONDS = 10000;
		constexpr char AMP_KIND[] = "NAM";
		constexpr char CABINET_KIND[] = "Cabinet";
		constexpr char PEDAL_PAYLOAD[] = "STOMPBOX_PEDAL";
		constexpr char NEW_PAYLOAD[] = "STOMPBOX_NEW";

		enum class BrowserTab { Effects, Amps, Cabinets };

		// What the browser offers: a pedal kind, and for amps and cabinets the file it loads.
		struct BrowserItem
		{
			std::string kind;
			std::string file;   // empty for an effect
		};

		// A browser item as drag-and-drop data (ImGui copies the bytes, so it holds no pointers).
		struct NewPedalPayload
		{
			char kind[64];
			char file[512];
		};

		struct PageState
		{
			std::string selectedPedal;          // pedal id on the board
			bool isEditorOpen = false;
			bool isSongsMode = false;           // the pane under the board shows the song's tones
			BrowserTab tab = BrowserTab::Effects;
			int selectedItem = -1;              // row in the browser's current list
			char search[64] = {};
			char toneSearch[64] = {};
			char toneName[64] = {};
			std::string selectedTone;           // saved tone row in the library
			bool isSaveAsWanted = false;        // opened from a menu: the bar opens the popup in its own ID stack
			std::string notice;                 // a refusal or hint under the board, for a few seconds
			ULONGLONG noticeAt = 0;
			std::vector<std::string> loadingPedals;   // a file was just chosen: spinner until the host confirms
			float paneExtra = 0.0f;             // what the divider was dragged by, for the session
			uint32_t lastLateBlocks = 0;
			ULONGLONG lateAt = 0;
		};

		PageState& State()
		{
			static PageState state;
			return state;
		}

		void Notice(const std::string& text)
		{
			State().notice = text;
			State().noticeAt = GetTickCount64();
		}

		bool IsAmpKind(const std::string& kind) { return kind == AMP_KIND || kind == "NAMMulti"; }

		bool IsGateKind(const std::string& kind) { return kind == "NoiseGate" || kind == "SimpleGate"; }

		bool IsDynamicsKind(const std::string& kind) { return IsGateKind(kind) || kind == "Compressor"; }

		// The same looks by role as the VST rig's pedals: amps brown and wider, dynamics teal, effects purple, the
		// cabinet slate.
		const PedalLook& LookFor(const std::string& kind)
		{
			if (IsAmpKind(kind)) return ToneDesignerUi::Looks::Amp;
			if (IsDynamicsKind(kind)) return ToneDesignerUi::Looks::Dynamics;
			if (kind == CABINET_KIND) return ToneDesignerUi::Looks::Other;
			return ToneDesignerUi::Looks::Effect;
		}

		bool ContainsText(const std::string& text, const char* search)
		{
			if (!search[0]) return true;
			return std::search(text.begin(), text.end(), search, search + strlen(search),
				[](char a, char b) { return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b)); }) != text.end();
		}

		const ParameterDefinition* FileParameter(const PedalDefinition* definition)
		{
			if (!definition) return nullptr;
			for (const ParameterDefinition& parameter : definition->parameters)
			{
				if (parameter.type == ParameterType::File) return &parameter;
			}
			return nullptr;
		}

		// "Amps/Fender/Deluxe Crunch.nam" -> "Deluxe Crunch".
		std::string ShortFileName(const std::string& file)
		{
			const size_t slash = file.find_last_of("/\\");
			std::string name = slash == std::string::npos ? file : file.substr(slash + 1);
			const size_t dot = name.find_last_of('.');
			return dot == std::string::npos ? name : name.substr(0, dot);
		}

		// The file an amp or cabinet plays, as its tile and editor show it; empty for an effect.
		std::string PedalFileLabel(const Pedal& pedal, const PedalDefinition* definition)
		{
			const ParameterDefinition* file = FileParameter(definition);
			if (!file) return {};
			const auto value = pedal.values.find(file->name);
			return value == pedal.values.end() || value->second.empty() ? std::string("No file") : ShortFileName(value->second);
		}

		std::vector<std::string> ChainIds()
		{
			std::vector<std::string> ids;
			for (const Pedal& pedal : GetSnapshot().chain) ids.push_back(pedal.id);
			return ids;
		}

		int IndexOfPedal(const std::string& id)
		{
			const auto& chain = GetSnapshot().chain;
			for (int i = 0; i < static_cast<int>(chain.size()); ++i)
			{
				if (chain[i].id == id) return i;
			}
			return -1;
		}

		// Where a pedal added without a drop position goes: a gate near the input, an amp before the first cabinet,
		// a cabinet after the last amp, anything else at the end. Only a default: the player can drag it anywhere.
		int DefaultInsertIndex(const std::string& kind)
		{
			const auto& chain = GetSnapshot().chain;
			const int count = static_cast<int>(chain.size());
			const auto lastOf = [&](auto predicate) { for (int i = count - 1; i >= 0; --i) if (predicate(chain[i].kind)) return i; return -1; };
			const auto firstOf = [&](auto predicate) { for (int i = 0; i < count; ++i) if (predicate(chain[i].kind)) return i; return -1; };
			if (IsGateKind(kind)) return lastOf(IsGateKind) + 1;
			if (IsAmpKind(kind))
			{
				const int cabinet = firstOf([](const std::string& k) { return k == CABINET_KIND; });
				return cabinet >= 0 ? cabinet : count;
			}
			if (kind == CABINET_KIND)
			{
				const int amp = lastOf(IsAmpKind);
				return amp >= 0 ? amp + 1 : count;
			}
			return count;
		}

		// Adds a pedal at `index` (-1: the default place), with the file an amp or cabinet item carries.
		void AddPedal(const BrowserItem& item, int index)
		{
			const PedalDefinition* definition = FindDefinition(item.kind);
			if (!definition)
			{
				Notice("The engine has no " + item.kind + " pedal.");
				return;
			}
			std::vector<std::string> ids = ChainIds();
			const int at = index < 0 ? DefaultInsertIndex(item.kind) : std::clamp(index, 0, static_cast<int>(ids.size()));
			const std::string id = NewPedalId(item.kind);
			ids.insert(ids.begin() + at, id);
			SetChain(ids);
			if (!item.file.empty())
			{
				if (const ParameterDefinition* file = FileParameter(definition))
				{
					SetParameter(id, file->name, item.file);
					State().loadingPedals.push_back(id);
				}
			}
			State().selectedPedal = id;
		}

		void RemovePedal(const std::string& id)
		{
			std::vector<std::string> ids = ChainIds();
			ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
			SetChain(ids);
			if (State().selectedPedal == id)
			{
				State().selectedPedal.clear();
				State().isEditorOpen = false;
			}
		}

		// Moves the pedal at `from` so that it lands at `to` in the chain as it is after the move.
		void MovePedal(int from, int to)
		{
			std::vector<std::string> ids = ChainIds();
			if (from < 0 || from >= static_cast<int>(ids.size())) return;
			const std::string id = ids[from];
			ids.erase(ids.begin() + from);
			ids.insert(ids.begin() + std::clamp(to, 0, static_cast<int>(ids.size())), id);
			SetChain(ids);
		}

		void ReplacePedal(const std::string& id, const BrowserItem& item)
		{
			const int index = IndexOfPedal(id);
			if (index < 0) return;
			RemovePedal(id);
			AddPedal(item, index);
		}

		void OpenEditor(const std::string& id)
		{
			State().selectedPedal = id;
			State().isEditorOpen = true;
			State().isSongsMode = false;
		}

		std::vector<BrowserItem> BrowserItems(BrowserTab tab, const char* search)
		{
			std::vector<BrowserItem> items;
			if (tab == BrowserTab::Effects)
			{
				for (const PedalDefinition& definition : GetSnapshot().definitions)
				{
					if (definition.isUserSelectable && !IsAmpKind(definition.name) && definition.name != CABINET_KIND && ContainsText(definition.name, search))
						items.push_back({ definition.name, {} });
				}
				return items;
			}
			const char* kind = tab == BrowserTab::Amps ? AMP_KIND : CABINET_KIND;
			if (const ParameterDefinition* file = FileParameter(FindDefinition(kind)))
			{
				for (const std::string& choice : file->choices)
				{
					if (ContainsText(choice, search)) items.push_back({ kind, choice });
				}
			}
			return items;
		}

		bool IsItemInChain(const BrowserItem& item)
		{
			for (const Pedal& pedal : GetSnapshot().chain)
			{
				if (pedal.kind != item.kind) continue;
				if (item.file.empty()) return true;
				const PedalDefinition* definition = FindDefinition(pedal.kind);
				const ParameterDefinition* file = FileParameter(definition);
				const auto value = file ? pedal.values.find(file->name) : pedal.values.end();
				if (value != pedal.values.end() && value->second == item.file) return true;
			}
			return false;
		}

		std::wstring EngineFolder(const char* subfolder)
		{
			PWSTR appData = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData))) return {};
			std::wstring folder = std::wstring(appData) + L"\\RocksmithAudioBridge\\Stompbox\\" + std::wstring(subfolder, subfolder + strlen(subfolder));
			CoTaskMemFree(appData);
			return folder;
		}

		void Spinner(ImDrawList* list, ImVec2 center, float radius, ImU32 color)
		{
			const float start = static_cast<float>(ImGui::GetTime()) * 6.0f;
			list->PathArcTo(center, radius, start, start + 4.2f, 20);
			list->PathStroke(color, false, 2.5f);
		}

		bool IsPedalLoading(const Pedal& pedal, const PedalDefinition* definition)
		{
			const PageState& state = State();
			const bool isChosen = std::find(state.loadingPedals.begin(), state.loadingPedals.end(), pedal.id) != state.loadingPedals.end();
			return isChosen || (GetSnapshot().isPresetLoading && FileParameter(definition));
		}

		// ---- Load -------------------------------------------------------------------------------------------

		// Late blocks (the amp link's count) in the last few seconds: the proof that a block did not make it.
		bool HasRecentLateBlocks()
		{
			PageState& state = State();
			const uint32_t late = ExternalAmp::GetSnapshot().link.lateBlocks;
			if (late > state.lastLateBlocks) state.lateAt = GetTickCount64();
			state.lastLateBlocks = late;
			return state.lateAt != 0 && GetTickCount64() - state.lateAt < LATE_MILLISECONDS;
		}

		ImU32 LoadHue(const Snapshot& s, bool isLate)
		{
			if (isLate || s.medianLoad >= SHARE_HEAVY_LOAD) return Ink::Bad;
			if (s.medianLoad >= SHARE_WARN_LOAD || s.peakLoad >= PEAK_WARN_LOAD) return Ink::Amber;
			return Ink::Good;
		}

		// What the load means, when it needs saying; null while the tone fits.
		const char* LoadWarning(const Snapshot& s, bool isLate)
		{
			if (!s.hasLoad) return nullptr;
			if (s.medianLoad >= SHARE_HEAVY_LOAD) return "This tone is heavy: on a slower PC it may crackle even at the game's normal settings. Remove a pedal or use a lighter NAM capture.";
			if (isLate || s.peakLoad >= PEAK_WARN_LOAD) return "Your block size is tight for this tone on this PC. It plays at a larger block size (128 or more), or with a lighter tone.";
			return nullptr;
		}

		std::string LoadText(const Snapshot& s)
		{
			if (!s.hasLoad) return "Load: play to measure";
			char text[64];
			sprintf_s(text, "Load %d%%  Slowest %d%%", static_cast<int>(std::lround(s.medianLoad * 100.0f)), static_cast<int>(std::lround(s.peakLoad * 100.0f)));
			return text;
		}

		// The bar's load pill at `at`, measured while the player plays.
		void LoadPill(ImVec2 at, float height, bool isLate)
		{
			const Snapshot& s = GetSnapshot();
			ImFont* caption = CaptionFont();
			const std::string text = LoadText(s);
			const ImU32 hue = s.hasLoad ? LoadHue(s, isLate) : Ink::Muted;
			const PillLook look = s.hasLoad ? TintPill(text.c_str(), hue) : ToneDesignerUi::NeutralPill(text.c_str());
			const float width = ToneDesignerUi::PillWidth(caption, text.c_str(), true);
			ImGui::SetCursorScreenPos(at);
			ImGui::InvisibleButton("##load", ImVec2(width, height));
			char hint[512];
			sprintf_s(hint, "Measured on this PC while you play, at %d samples a block. Load is the tone's share of the audio time, about the same at any block size: "
				"under 50%% leaves room for slower PCs at the game's normal settings. Slowest is the worst block of the last second at your block size: "
				"small blocks (ASIO's 128 or 64) are a bonus that a heavy tone can run out of.%s", s.blockFrames, isLate ? " Blocks were late in the last few seconds." : "");
			Hint(hint);
			DrawPill(ImGui::GetWindowDrawList(), caption, at, height, look, hue);
		}

		// ---- Bar --------------------------------------------------------------------------------------------

		void StepTone(int direction)
		{
			const Snapshot& s = GetSnapshot();
			if (s.presets.empty()) return;
			const auto current = std::find(s.presets.begin(), s.presets.end(), s.currentPreset);
			const int count = static_cast<int>(s.presets.size());
			const int index = current == s.presets.end() ? (direction > 0 ? 0 : count - 1)
				: (static_cast<int>(current - s.presets.begin()) + direction + count) % count;
			LoadPreset(s.presets[index]);
		}

		// Asks for Save as; the bar opens it (SaveAsPopup), since a menu or a popup has an ID stack of its own.
		void RequestSaveAs() { State().isSaveAsWanted = true; }

		void SaveAsPopup()
		{
			PageState& state = State();
			if (state.isSaveAsWanted)
			{
				state.isSaveAsWanted = false;
				strncpy_s(state.toneName, GetSnapshot().currentPreset.c_str(), _TRUNCATE);
				ImGui::OpenPopup("##saveTone");
			}
			PushPopupPadding();
			if (ImGui::BeginPopup("##saveTone"))
			{
				ImGui::TextDisabled("Save the board as");
				if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
				ImGui::SetNextItemWidth(240.0f);
				const bool isEntered = ImGui::InputText("##toneName", state.toneName, sizeof(state.toneName), ImGuiInputTextFlags_EnterReturnsTrue);
				if ((Button("Save", true) || isEntered) && state.toneName[0])
				{
					SavePreset(state.toneName);
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (Button("Cancel")) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			ImGui::PopStyleVar();
		}

		void FileMenu()
		{
			const Snapshot& s = GetSnapshot();
			if (!ImGui::BeginMenu("File")) return;
			if (ImGui::MenuItem("New tone", nullptr, false, !s.chain.empty())) SetChain({});
			if (ImGui::MenuItem("Save", nullptr, false, !s.currentPreset.empty())) SavePreset(s.currentPreset);
			if (ImGui::MenuItem("Save as...")) RequestSaveAs();
			ImGui::Separator();
			if (ImGui::MenuItem("Open amps folder")) ShellOpenAsync::Open(EngineFolder("NAM"));
			if (ImGui::MenuItem("Open cabinets folder")) ShellOpenAsync::Open(EngineFolder("Cabinets"));
			ImGui::EndMenu();
		}

		// The bar: File, the tone's name between arrows, Save, then the load pill and a spinner while it works.
		void Bar(float width, float height, bool isLate)
		{
			const Snapshot& s = GetSnapshot();
			ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_Header, ImGui::ColorConvertU32ToFloat4(Ink::Edge));
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::ColorConvertU32ToFloat4(Ink::Edge));
			// The bar's height and its labels' place come from FramePadding (as on the VST rig's bar); popped right
			// after, so the menus' popups keep normal item heights.
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9, (height - ImGui::GetFontSize()) * 0.5f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 0));
			ImGui::BeginChild("##stompboxBar", ImVec2(width, height), false,
				ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::PopStyleVar();
			const bool isBarOpen = ImGui::BeginMenuBar();
			ImGui::PopStyleVar();
			PushPopupPadding();
			if (isBarOpen)
			{
				FileMenu();
				const float menusEnd = ImGui::GetCursorPosX();
				ImDrawList* list = ImGui::GetWindowDrawList();
				const ImVec2 origin = ImGui::GetWindowPos();
				const float button = height - 12.0f;
				const float y = (height - button) * 0.5f;

				// Right: the load pill, and a spinner while a tone, a model or a command is on its way.
				ImFont* caption = CaptionFont();
				const float pillW = ToneDesignerUi::PillWidth(caption, LoadText(s).c_str(), true);
				const float pillX = width - 10.0f - pillW;
				LoadPill(ImVec2(origin.x + pillX, origin.y + y), button, isLate);
				if (s.isPresetLoading || s.isBusy) Spinner(list, ImVec2(origin.x + pillX - 16.0f, origin.y + height * 0.5f), 6.0f, Ink::AddText);

				// Middle: < tone name > Save, centred between the menus and the pill.
				const float saveW = TextWidth(ImGui::GetFont(), "Save") + 22.0f;
				const float left = menusEnd + 12.0f, right = pillX - 32.0f;
				const float nameW = std::clamp(right - left - 2.0f * (button + 6.0f) - saveW - 6.0f, 80.0f, 240.0f);
				const float groupW = nameW + 2.0f * (button + 6.0f) + saveW + 6.0f;
				float gx = left + std::max(0.0f, (right - left - groupW) * 0.5f);
				ImGui::SetCursorScreenPos(ImVec2(origin.x + gx, origin.y + y));
				if (IconSquare("##prevTone", Glyph::ChevronLeft, false, "Previous saved tone.", button)) StepTone(-1);
				gx += button + 6.0f;
				const ImVec2 box(origin.x + gx, origin.y + y);
				ImGui::SetCursorScreenPos(box);
				if (ImGui::InvisibleButton("##toneName", ImVec2(nameW, button))) ImGui::OpenPopup("##toneList");
				Hint("Your tones. Click to pick one; File has Save as.");
				list->AddRectFilled(box, ImVec2(box.x + nameW, box.y + button), Ink::Field, 6.0f);
				list->AddRect(box, ImVec2(box.x + nameW, box.y + button), Ink::FieldEdge, 6.0f);
				const char* name = s.isPresetLoading ? "Loading..." : s.currentPreset.empty() ? (s.chain.empty() ? "New tone" : "Unsaved tone") : s.currentPreset.c_str();
				const float textW = ImGui::CalcTextSize(name).x;
				list->PushClipRect(box, ImVec2(box.x + nameW - 6.0f, box.y + button), true);
				list->AddText(ImVec2(box.x + std::max(8.0f, (nameW - textW) * 0.5f), box.y + (button - ImGui::GetFontSize()) * 0.5f), IM_COL32_WHITE, name);
				list->PopClipRect();
				gx += nameW + 6.0f;
				ImGui::SetCursorScreenPos(ImVec2(origin.x + gx, origin.y + y));
				if (IconSquare("##nextTone", Glyph::ChevronRight, false, "Next saved tone.", button)) StepTone(+1);
				gx += button + 6.0f;
				ImGui::SetCursorScreenPos(ImVec2(origin.x + gx, origin.y + y));
				const ImVec2 saveAt = ImGui::GetCursorScreenPos();
				if (ImGui::InvisibleButton("##save", ImVec2(saveW, button)))
				{
					if (s.currentPreset.empty()) RequestSaveAs();
					else SavePreset(s.currentPreset);
				}
				const bool isSaveHovered = ImGui::IsItemHovered();
				Hint(s.currentPreset.empty() ? "Save the board as a tone, with every pedal's settings." : "Save the board over this tone. File, Save as keeps a copy under a new name.");
				list->AddRectFilled(saveAt, ImVec2(saveAt.x + saveW, saveAt.y + button), isSaveHovered ? Mix(Ink::Blue, IM_COL32_WHITE, 0.12f) : Ink::Blue, 6.0f);
				list->AddText(ImVec2(saveAt.x + 11.0f, saveAt.y + (button - ImGui::GetFontSize()) * 0.5f), IM_COL32_WHITE, "Save");

				if (ImGui::BeginPopup("##toneList"))
				{
					if (s.presets.empty()) ImGui::TextDisabled("No saved tones yet");
					for (const std::string& preset : s.presets)
					{
						if (ImGui::MenuItem(preset.c_str(), nullptr, preset == s.currentPreset) && preset != s.currentPreset) LoadPreset(preset);
					}
					ImGui::Separator();
					if (ImGui::MenuItem("Save as...")) RequestSaveAs();
					ImGui::EndPopup();
				}
				SaveAsPopup();
				ImGui::EndMenuBar();
			}
			ImGui::PopStyleVar();
			ImGui::EndChild();
			ImGui::PopStyleColor(3);
		}

		// ---- Library ----------------------------------------------------------------------------------------

		// One saved tone: click selects, double-click loads, right-click for the rest. The tone on the board has a
		// green dot, as on the VST rig.
		void ToneRow(const std::string& preset, float width)
		{
			const Snapshot& s = GetSnapshot();
			PageState& state = State();
			ImGui::PushID(preset.c_str());
			const bool isCurrent = preset == s.currentPreset;
			const float height = ImGui::GetFontSize() + 10.0f;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			if (ImGui::InvisibleButton("##tone", ImVec2(width, height))) state.selectedTone = preset;
			const bool isHovered = ImGui::IsItemHovered();
			if (isHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !isCurrent) LoadPreset(preset);
			Hint(isCurrent ? "The tone on the board now. Right-click for more." : "Double-click to load it. Right-click for more.");
			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 end(at.x + width, at.y + height);
			const bool isSelected = state.selectedTone == preset;
			ToneDesignerUi::RowBackground(list, at, end, isSelected, isHovered);
			if (isCurrent) list->AddCircleFilled(ImVec2(at.x + 9.0f, at.y + height * 0.5f), 3.5f, Ink::Good, 12);
			list->PushClipRect(at, end, true);
			list->AddText(ImVec2(at.x + 18.0f, at.y + 5.0f), isCurrent || isSelected ? IM_COL32_WHITE : IM_COL32(195, 200, 210, 255), preset.c_str());
			list->PopClipRect();

			PushPopupPadding();
			if (ImGui::BeginPopupContextItem("##toneMenu"))
			{
				state.selectedTone = preset;
				ImGui::TextDisabled("%s", preset.c_str());
				ImGui::Separator();
				if (ImGui::MenuItem("Load", "or double-click it", false, !isCurrent)) LoadPreset(preset);
				if (ImGui::MenuItem("Save the board over it")) SavePreset(preset);
				ImGui::Separator();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
				if (ImGui::MenuItem("Delete")) DeletePreset(preset);
				ImGui::PopStyleColor();
				ImGui::EndPopup();
			}
			ImGui::PopStyleVar();
			ImGui::PopID();
		}

		// How many of the chosen song's tones play a saved tone, for the library's Song tones line.
		std::string SongToneSummary(const Snapshot& s, const SongLibrary::Song* song)
		{
			if (s.songKey.empty()) return "Choose a song";
			const size_t assigned = s.songTones.size();
			if (!song || song->tones.empty()) return std::to_string(assigned) + " assigned";
			return std::to_string(assigned) + " of " + std::to_string(song->tones.size()) + " assigned";
		}

		// Left column: search, the saved tones, and the chosen song's tones (a click shows them in the pane).
		void LibraryColumn(float width, float height)
		{
			const Snapshot& s = GetSnapshot();
			PageState& state = State();
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 10));
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 2));
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
			ImGui::BeginChild("##stompboxLibrary", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
			ImGui::PopStyleColor();
			const float rowW = ImGui::GetContentRegionAvail().x;
			ImGui::SetNextItemWidth(rowW);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
			ImGui::InputTextWithHint("##toneSearch", "Search tones", state.toneSearch, sizeof(state.toneSearch));
			ImGui::PopStyleVar();

			FoldLabel("Tones");
			if (s.presets.empty()) MutedCaption("None yet. Save keeps the board with every pedal's settings.", rowW);
			for (const std::string& preset : s.presets)
			{
				if (ContainsText(preset, state.toneSearch)) ToneRow(preset, rowW);
			}

			FoldLabel("Song tones");
			const auto library = SongLibrary::Get();
			const SongLibrary::Song* song = s.songKey.empty() ? nullptr : library->Find(s.songKey);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
			ImGui::TextUnformatted(SongToneSummary(s, song).c_str());
			if (!s.songKey.empty())
			{
				if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				if (ImGui::IsItemClicked()) State().isSongsMode = true;
			}
			Hint(s.songKey.empty() ? "Choose a song in the game to give each of its tones a saved tone."
				: "Click to see the song's tones in the pane under the board and pick a saved tone for each.");
			ImGui::PopStyleColor();
			PushPopupPadding();
			if (ImGui::BeginPopupContextWindow("##libraryMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
			{
				ImGui::TextDisabled("Tones");
				ImGui::Separator();
				if (ImGui::MenuItem("New tone", nullptr, false, !s.chain.empty())) SetChain({});
				ImGui::EndPopup();
			}
			ImGui::PopStyleVar();
			ImGui::EndChild();
			ImGui::PopStyleVar(2);
		}

		// ---- Board ------------------------------------------------------------------------------------------

		void MeterColumn(ImDrawList* list, ImVec2 at, float width, const char* label, float peak)
		{
			const float height = ToneDesignerUi::MeterHeight();
			ToneDesignerUi::MeterSegments(list, at.x, at.y - height * 0.5f, width, label, peak);
		}

		// The Add menu and the board's right-click menu: every effect, then the amps and cabinets found.
		void AddMenuItems()
		{
			for (const BrowserTab tab : { BrowserTab::Effects, BrowserTab::Amps, BrowserTab::Cabinets })
			{
				const std::vector<BrowserItem> items = BrowserItems(tab, "");
				const char* label = tab == BrowserTab::Effects ? "Effects" : tab == BrowserTab::Amps ? "Amps" : "Cabinets";
				if (!ImGui::BeginMenu(label, !items.empty())) continue;
				for (const BrowserItem& item : items)
				{
					const std::string name = item.file.empty() ? item.kind : ShortFileName(item.file);
					if (ImGui::MenuItem(name.c_str())) AddPedal(item, -1);
				}
				ImGui::EndMenu();
			}
		}

		void PedalMenu(const Pedal& pedal)
		{
			PushPopupPadding();
			if (ImGui::BeginPopupContextItem("##pedalMenu"))
			{
				State().selectedPedal = pedal.id;
				ImGui::TextDisabled("%s", pedal.id.c_str());
				ImGui::Separator();
				if (ImGui::MenuItem("Edit", "or double-click it")) OpenEditor(pedal.id);
				if (ImGui::MenuItem(pedal.isEnabled ? "Bypass" : "Turn on", "or its footswitch")) SetEnabled(pedal.id, !pedal.isEnabled);
				ImGui::Separator();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
				if (ImGui::MenuItem("Remove")) RemovePedal(pedal.id);
				ImGui::PopStyleColor();
				ImGui::EndPopup();
			}
			ImGui::PopStyleVar();
		}

		// One pedal on the rack at `at`: click selects, the footswitch bypasses, double-click edits, drag moves it,
		// and a browser item dropped on it replaces it. Returns whether the mouse is on it.
		void DrawPedalSymbol(ImDrawList* list, const std::string& kind, ImVec2 center, ImU32 color)
		{
			if (IsAmpKind(kind))
			{
				list->AddRect(ImVec2(center.x - 26, center.y - 9), ImVec2(center.x + 26, center.y + 9), color, 3.0f, 0, 1.5f);
				for (int i = -2; i <= 2; ++i) list->AddCircle(ImVec2(center.x + i * 10.0f, center.y), 2.0f, color, 10);
				return;
			}
			if (kind == CABINET_KIND)
			{
				list->AddCircle(center, 13.0f, color, 20, 1.5f);
				list->AddCircle(center, 7.0f, color, 16, 1.5f);
				return;
			}
			if (kind == "EQ-7" || kind == "BEQ-7" || kind == "Tonestack")
			{
				for (int i = -3; i <= 3; ++i)
				{
					const float x = center.x + i * 7.0f;
					list->AddLine(ImVec2(x, center.y - 12), ImVec2(x, center.y + 12), color, 1.5f);
					list->AddCircleFilled(ImVec2(x, center.y + (i % 2 == 0 ? -4 : 4)), 2.0f, color);
				}
				return;
			}
			const ImVec2 wave[] = { ImVec2(center.x - 25, center.y), ImVec2(center.x - 15, center.y),
				ImVec2(center.x - 7, center.y - 10), ImVec2(center.x, center.y + 12),
				ImVec2(center.x + 7, center.y - 13), ImVec2(center.x + 15, center.y), ImVec2(center.x + 25, center.y) };
			list->AddPolyline(wave, IM_ARRAYSIZE(wave), color, false, 1.5f);
		}

		bool PedalTileAt(const Pedal& pedal, int index, ImVec2 at, ImDrawList* list)
		{
			PageState& state = State();
			const PedalDefinition* definition = FindDefinition(pedal.kind);
			const PedalLook& look = LookFor(pedal.kind);
			ImGui::PushID(pedal.id.c_str());
			ImGui::SetCursorScreenPos(at);
			const bool isPressed = ImGui::InvisibleButton("##pedal", ImVec2(look.width, ToneDesignerUi::PEDAL_HEIGHT));
			ImGui::SetItemUsingMouseWheel();   // the wheel scrolls the rack, not the page
			const bool isHovered = ImGui::IsItemHovered();
			const bool isOnSwitch = ToneDesignerUi::IsOnFootswitch(at, look, ImGui::GetIO().MousePos);
			if (isPressed)
			{
				if (isOnSwitch) SetEnabled(pedal.id, !pedal.isEnabled);
				else state.selectedPedal = pedal.id;
			}
			if (isHovered && !isOnSwitch && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenEditor(pedal.id);
			Hint(isOnSwitch ? (pedal.isEnabled ? "Footswitch: bypass it." : "Footswitch: turn it on.")
				: "Double-click to edit it. Right-click for more. Drag to move it.");
			if (ImGui::BeginDragDropSource())
			{
				ImGui::SetDragDropPayload(PEDAL_PAYLOAD, &index, sizeof(index));
				ImGui::TextUnformatted(pedal.id.c_str());
				ImGui::EndDragDropSource();
			}
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(NEW_PAYLOAD))
				{
					const auto& data = *static_cast<const NewPedalPayload*>(payload->Data);
					ReplacePedal(pedal.id, { data.kind, data.file });
				}
				ImGui::EndDragDropTarget();
			}
			PedalMenu(pedal);

			const std::string file = PedalFileLabel(pedal, definition);
			const std::string sub = !file.empty() ? file : (pedal.isEnabled ? std::string() : std::string("bypassed"));
			ToneDesignerUi::PedalTile tile;
			tile.name = pedal.kind.c_str();
			tile.sub = sub.c_str();
			tile.look = look;
			tile.look.knobs = 0;
			tile.isPlaying = pedal.isEnabled;
			tile.isSelected = state.selectedPedal == pedal.id;
			tile.isHovered = isHovered;
			tile.isOnSwitch = isOnSwitch;
			ToneDesignerUi::DrawPedalTile(list, at, tile);
			DrawPedalSymbol(list, pedal.kind, ImVec2(at.x + look.width * 0.5f, at.y + 76.0f), pedal.isEnabled ? Ink::Soft : Ink::Dim);
			if (IsPedalLoading(pedal, definition)) ToneDesignerUi::DrawTileBusy(list, at, look.width, "Loading");
			ImGui::PopID();
			return isHovered;
		}

		// Dropping into a gap inserts there: a dragged pedal moves to that place, a browser item goes in at that
		// slot. While something is dragged every gap shows a faint marker and the one under the mouse a bright bar.
		void GapDropTargets(const std::vector<ImRect>& gaps, ImDrawList* list)
		{
			const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
			if (!dragging || !(dragging->IsDataType(PEDAL_PAYLOAD) || dragging->IsDataType(NEW_PAYLOAD))) return;
			const int from = dragging->IsDataType(PEDAL_PAYLOAD) ? *static_cast<const int*>(dragging->Data) : -1;
			for (int g = 0; g < static_cast<int>(gaps.size()); ++g)
			{
				if (from >= 0 && (g == from || g == from + 1)) continue;   // a pedal's own two sides are no move
				const ImRect& gap = gaps[g];
				const float gx = gap.Min.x + 14.0f;
				list->AddLine(ImVec2(gx, gap.Min.y + 18.0f), ImVec2(gx, gap.Max.y - 18.0f), Ink::Jack, 2.0f);
				if (!ImGui::BeginDragDropTargetCustom(gap, ImGui::GetID(("##gap" + std::to_string(g)).c_str()))) continue;
				const ImGuiDragDropFlags peek = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(PEDAL_PAYLOAD, peek))
				{
					list->AddLine(ImVec2(gx, gap.Min.y), ImVec2(gx, gap.Max.y), Ink::Outline, 4.0f);
					if (payload->IsDelivery()) MovePedal(from, g > from ? g - 1 : g);
				}
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(NEW_PAYLOAD, peek))
				{
					list->AddLine(ImVec2(gx, gap.Min.y), ImVec2(gx, gap.Max.y), Ink::Outline, 4.0f);
					if (payload->IsDelivery())
					{
						const auto& data = *static_cast<const NewPedalPayload*>(payload->Data);
						AddPedal({ data.kind, data.file }, g);
					}
				}
				ImGui::EndDragDropTarget();
			}
		}

		// What to do, under the rack.
		const char* BoardHint(const Snapshot& s)
		{
			if (s.chain.empty()) return "The board is empty, so you hear your clean guitar. Drag an amp from Amps below, then a cabinet after it and pedals before it (gate, drive) or after it (delay, reverb).";
			return "Double-click a pedal to edit it. Click a footswitch to bypass. Drag a pedal, or an item from the browser, between two pedals to put it there; onto a pedal to replace it. Scroll or middle-drag to move along a long board.";
		}

		// The rack: In, the pedals joined by cables, Add, Out, with the meters at its two ends. Scrolls sideways when
		// the board is longer than the panel.
		void Board(float width, float height, bool isLate)
		{
			const Snapshot& s = GetSnapshot();
			const VstHostLink::Snapshot vst = VstHostLink::GetSnapshot();
			// A copy: a pedal's menu or a drop sets the chain at once (StompboxLink), which would pull the pedals
			// out from under this loop.
			const std::vector<Pedal> chain = s.chain;
			const int count = static_cast<int>(chain.size());

			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 14));
			ImGui::BeginChild("##stompboxBoard", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);
			const float boardW = ImGui::GetContentRegionAvail().x;
			// Under the rack: a notice for a few seconds, else the load's warning, else what to do.
			const PageState& state = State();
			const bool isNoticeShown = !state.notice.empty() && GetTickCount64() - state.noticeAt < NOTICE_MILLISECONDS;
			const char* warning = LoadWarning(s, isLate);
			const char* boardHint = isNoticeShown ? state.notice.c_str() : warning ? warning : BoardHint(s);
			const ImU32 hintColor = !isNoticeShown && warning ? Ink::Amber : Ink::Dim;
			ImFont* caption = CaptionFont();
			const bool showHint = height >= 230.0f;
			const float hintH = showHint ? caption->CalcTextSizeA(caption->FontSize, FLT_MAX, boardW, boardHint).y + ImGui::GetStyle().ItemSpacing.y + 4.0f : 0.0f;
			const float rackH = std::max(1.0f, ImGui::GetContentRegionAvail().y - hintH);
			const float pedalH = ToneDesignerUi::PEDAL_HEIGHT;
			const ImVec2 board = ImGui::GetCursorScreenPos();
			ImDrawList* outer = ImGui::GetWindowDrawList();
			ToneDesignerUi::DrawRack(outer, board, boardW, rackH);

			const float meterMidY = board.y + std::max(0.0f, (rackH - 6.0f - pedalH) * 0.5f) + pedalH * 0.5f;
			MeterColumn(outer, ImVec2(board.x, meterMidY), METER_COLUMN_WIDTH, "IN", vst.inputPeak);
			MeterColumn(outer, ImVec2(board.x + boardW - METER_COLUMN_WIDTH, meterMidY), METER_COLUMN_WIDTH, "OUT", vst.outputPeak);
			const float rackW = boardW - 2.0f * METER_COLUMN_WIDTH;
			ImGui::SetCursorScreenPos(ImVec2(board.x + METER_COLUMN_WIDTH, board.y));

			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 0));
			ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
			ImGui::BeginChild("##stompboxRack", ImVec2(rackW, rackH), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysUseWindowPadding);
			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 rackStart = ImGui::GetCursorScreenPos();
			const float top = rackStart.y + std::max(0.0f, (rackH - 6.0f - pedalH) * 0.5f);
			const float midY = top + pedalH * 0.5f;
			float x = rackStart.x;
			const auto cable = [&]
			{
				list->AddLine(ImVec2(x, midY), ImVec2(x + 14.0f, midY), Ink::Cable, 3.0f);
				x += 22.0f;
			};

			Jack(list, ImVec2(x + 21.0f, midY - 6.0f), "In");
			x += ToneDesignerUi::JACK_WIDTH + 8.0f;
			cable();
			std::vector<ImRect> gaps;
			bool isOverTile = false;
			for (int i = 0; i < count; ++i)
			{
				gaps.emplace_back(ImVec2(x - 26.0f, top), ImVec2(x + 6.0f, top + pedalH));
				isOverTile |= PedalTileAt(chain[i], i, ImVec2(x, top), list);
				x += LookFor(chain[i].kind).width + 8.0f;
				cable();
			}
			gaps.emplace_back(ImVec2(x - 26.0f, top), ImVec2(x + 6.0f, top + pedalH));

			// Add: a dashed tile, opening the list of pedals.
			const ImVec2 addAt(x, top);
			ImGui::SetCursorScreenPos(addAt);
			if (ImGui::InvisibleButton("##addPedal", ImVec2(ToneDesignerUi::ADD_TILE_WIDTH, pedalH))) ImGui::OpenPopup("##addMenu");
			ImGui::SetItemUsingMouseWheel();
			const bool isAddHovered = ImGui::IsItemHovered();
			Hint("Add a pedal at its usual place. The browser below has search; drag from it onto the board to put a pedal anywhere.");
			ToneDesignerUi::DrawAddTile(list, addAt, midY, isAddHovered);
			x += ToneDesignerUi::ADD_TILE_WIDTH + 8.0f;
			cable();
			GapDropTargets(gaps, list);
			Jack(list, ImVec2(x + 21.0f, midY - 6.0f), "Out");
			x += ToneDesignerUi::JACK_WIDTH;

			// The empty part of the board: one item behind the pedals (submitted last, so the pedals keep the hover),
			// for its right-click menu and the mouse wheel. It also sets the rack's scroll width.
			ImGui::SetCursorScreenPos(rackStart);
			ImGui::InvisibleButton("##boardSpace", ImVec2(x + 10.0f - rackStart.x, std::max(rackH - 6.0f, 1.0f)));
			ImGui::SetItemUsingMouseWheel();
			// Not over a pedal: ImGui 1.85's IsItemHovered is true under the pedals too, and this menu would replace theirs.
			if (!isOverTile && !isAddHovered && ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) ImGui::OpenPopup("##boardMenu");

			// A long board: the wheel scrolls it sideways, and a middle-button drag pans it.
			static bool isPanning = false;
			const ImGuiIO& io = ImGui::GetIO();
			if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
			{
				if (io.MouseWheel != 0.0f) ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseWheel * 90.0f);
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) isPanning = true;
			}
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) isPanning = false;
			if (isPanning && io.MouseDelta.x != 0.0f) ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);

			PushPopupPadding();
			if (ImGui::BeginPopup("##addMenu") || ImGui::BeginPopup("##boardMenu"))
			{
				ImGui::TextDisabled("Add pedal");
				ImGui::Separator();
				AddMenuItems();
				ImGui::Separator();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
				if (ImGui::MenuItem("Clear board", nullptr, false, count > 0)) SetChain({});
				ImGui::PopStyleColor();
				ImGui::EndPopup();
			}
			ImGui::PopStyleVar();
			ImGui::EndChild();
			ImGui::PopStyleVar(2);

			if (showHint)
			{
				ImGui::PushFont(caption);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(hintColor));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + boardW);
				ImGui::TextUnformatted(boardHint);
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
				ImGui::PopFont();
			}
			ImGui::EndChild();
			ImGui::PopStyleVar();
		}

		// ---- Browser ----------------------------------------------------------------------------------------

		// The browser's rows, styled as the VST rig's plugin list: name, then what it is.
		void BrowserList(const std::vector<BrowserItem>& items, float width, float height)
		{
			PageState& state = State();
			const float rowH = ImGui::GetFontSize() + 10.0f;
			ImFont* caption = CaptionFont();
			if (items.empty())
			{
				ImGui::BeginChild("##browserList", ImVec2(width, height), false);
				MutedCaption(state.tab == BrowserTab::Effects ? "Nothing matches."
					: "None yet. Put files in the folder (Folder, below); the engine lists them when the host starts.", width);
				ImGui::EndChild();
				return;
			}
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
			ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
			ImGui::BeginChild("##browserList", ImVec2(width, height), false);
			ImDrawList* list = ImGui::GetWindowDrawList();
			const float rowW = ImGui::GetContentRegionAvail().x;
			const float detailX = rowW * 0.42f;
			const float statusX = std::max(detailX + 60.0f, rowW - 72.0f);
			for (int i = 0; i < static_cast<int>(items.size()); ++i)
			{
				const BrowserItem& item = items[i];
				const PedalDefinition* definition = FindDefinition(item.kind);
				const std::string name = item.file.empty() ? item.kind : ShortFileName(item.file);
				const std::string detail = !item.file.empty() ? item.file : definition ? definition->description : std::string();
				ImGui::PushID(i);
				const ImVec2 at = ImGui::GetCursorScreenPos();
				if (ImGui::InvisibleButton("##item", ImVec2(rowW, rowH))) state.selectedItem = i;
				const bool isHovered = ImGui::IsItemHovered();
				if (isHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) AddPedal(item, -1);
				Hint("Double-click to add it at its usual place, or drag it onto the board: between two pedals to put it there, onto a pedal to replace it.");
				if (ImGui::BeginDragDropSource())
				{
					NewPedalPayload payload{};
					strncpy_s(payload.kind, item.kind.c_str(), _TRUNCATE);
					strncpy_s(payload.file, item.file.c_str(), _TRUNCATE);
					ImGui::SetDragDropPayload(NEW_PAYLOAD, &payload, sizeof(payload));
					ImGui::TextUnformatted(name.c_str());
					ImGui::EndDragDropSource();
				}
				PushPopupPadding();
				if (ImGui::BeginPopupContextItem("##itemMenu"))
				{
					state.selectedItem = i;
					ImGui::TextDisabled("%s", name.c_str());
					ImGui::Separator();
					if (ImGui::MenuItem("Add to the board", "or double-click it")) AddPedal(item, -1);
					if (ImGui::MenuItem("Replace selected pedal", nullptr, false, IndexOfPedal(state.selectedPedal) >= 0)) ReplacePedal(state.selectedPedal, item);
					ImGui::EndPopup();
				}
				ImGui::PopStyleVar();
				const bool isSelected = state.selectedItem == i;
				const ImVec2 end(at.x + rowW, at.y + rowH);
				ToneDesignerUi::RowBackground(list, at, end, isSelected, isHovered);
				// The look's colour as a swatch, so the list reads like the pedals it makes.
				const PedalLook& look = LookFor(item.kind);
				list->AddRectFilled(ImVec2(at.x + 8.0f, at.y + rowH * 0.5f - 5.0f), ImVec2(at.x + 18.0f, at.y + rowH * 0.5f + 5.0f), look.edge, 3.0f);
				list->PushClipRect(at, ImVec2(at.x + detailX - 8.0f, end.y), true);
				list->AddText(ImVec2(at.x + 28.0f, at.y + 5.0f), isSelected ? IM_COL32_WHITE : Ink::Text, name.c_str());
				list->PopClipRect();
				list->PushClipRect(ImVec2(at.x + detailX, at.y), ImVec2(at.x + statusX - 6.0f, end.y), true);
				Text(list, caption, ImVec2(at.x + detailX, at.y + (rowH - caption->FontSize) * 0.5f), Ink::Muted, detail.c_str());
				list->PopClipRect();
				if (IsItemInChain(item))
					Text(list, caption, ImVec2(at.x + statusX, at.y + (rowH - caption->FontSize) * 0.5f), Ink::Good, "In chain");
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::PopStyleVar(2);
		}

		// Effects, Amps, Cabinets: chips and search on the pane's title row, the list, and the foot's buttons.
		void Browser(float width)
		{
			PageState& state = State();
			ImGui::SameLine(0.0f, 12.0f);
			static const char* const chips[3] = { "Effects", "Amps", "Cabinets" };
			for (int c = 0; c < 3; ++c)
			{
				if (c > 0) ImGui::SameLine(0.0f, 6.0f);
				if (Chip(chips[c], static_cast<int>(state.tab) == c) && static_cast<int>(state.tab) != c)
				{
					state.tab = static_cast<BrowserTab>(c);
					state.selectedItem = -1;
				}
			}
			const float left = ImGui::GetCursorStartPos().x;
			const float searchW = std::min(220.0f, width * 0.4f);
			ImGui::SameLine(left + width - searchW);
			ImGui::SetNextItemWidth(searchW);
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
			const bool searchChanged = ImGui::InputTextWithHint("##browserSearch", state.tab == BrowserTab::Effects ? "Search effects" : state.tab == BrowserTab::Amps ? "Search amps" : "Search cabinets",
				state.search, sizeof(state.search));
			if (searchChanged) state.selectedItem = -1;
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();

			const std::vector<BrowserItem> items = BrowserItems(state.tab, state.search);
			if (state.selectedItem < 0 || state.selectedItem >= static_cast<int>(items.size())) state.selectedItem = -1;
			const float footH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 4.0f;
			ImGui::SetCursorPosX(left);
			ImGui::Dummy(ImVec2(0, 2));
			BrowserList(items, width, std::max(1.0f, ImGui::GetContentRegionAvail().y - footH - 12.0f));

			ImGui::Dummy(ImVec2(0, 2));
			const bool hasItem = state.selectedItem >= 0 && state.selectedItem < static_cast<int>(items.size());
			ImGui::BeginDisabled(!hasItem);
			if (Button("Add to the board", true) && hasItem) AddPedal(items[state.selectedItem], -1);
			ImGui::SameLine();
			if (Button("Replace selected pedal") && hasItem) ReplacePedal(state.selectedPedal, items[state.selectedItem]);
			ImGui::EndDisabled();
			if (state.tab != BrowserTab::Effects)
			{
				ImGui::SameLine();
				if (Button("Folder", false, state.tab == BrowserTab::Amps ? "Opens the folder for NAM models (.nam)." : "Opens the folder for cabinet IRs (.wav)."))
					ShellOpenAsync::Open(EngineFolder(state.tab == BrowserTab::Amps ? "NAM" : "Cabinets"));
			}
		}

		// ---- Pedal editor -----------------------------------------------------------------------------------

		float CurrentValue(const Pedal& pedal, const ParameterDefinition& parameter)
		{
			const auto value = pedal.values.find(parameter.name);
			return value == pedal.values.end() ? parameter.defaultValue : static_cast<float>(std::atof(value->second.c_str()));
		}

		std::string NumberText(float value)
		{
			char text[32];
			sprintf_s(text, "%g", value);
			return text;
		}

		// A knob as a horizontal slider over Stompbox's taper: position = ((value - min) / (max - min)) ^ (1 / power).
		void KnobControl(const Pedal& pedal, const ParameterDefinition& parameter, float width)
		{
			const float span = parameter.maximum - parameter.minimum;
			const float power = parameter.rangePower > 0.0f ? parameter.rangePower : 1.0f;
			const float value = CurrentValue(pedal, parameter);
			float position = span > 0.0f ? std::pow(std::clamp((value - parameter.minimum) / span, 0.0f, 1.0f), 1.0f / power) : 0.0f;
			const std::string text = FormatValue(parameter, value);
			ImGui::SetNextItemWidth(width);
			if (ImGui::SliderFloat("##value", &position, 0.0f, 1.0f, text.c_str()))
				SetParameter(pedal.id, parameter.name, NumberText(parameter.minimum + span * std::pow(position, power)));
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				SetParameter(pedal.id, parameter.name, NumberText(parameter.defaultValue));
		}

		void ParameterControl(const Pedal& pedal, const ParameterDefinition& parameter, float labelWidth, float controlWidth)
		{
			ImGui::PushID(parameter.name.c_str());
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Soft));
			ImGui::TextUnformatted(parameter.name.c_str());
			ImGui::PopStyleColor();
			if (!parameter.description.empty()) Hint(parameter.description.c_str());
			ImGui::SameLine(labelWidth);
			const auto stored = pedal.values.find(parameter.name);
			const std::string current = stored == pedal.values.end() ? std::string() : stored->second;
			switch (parameter.type)
			{
			case ParameterType::Bool:
			{
				bool isOn = CurrentValue(pedal, parameter) >= 0.5f;
				if (ImGui::Checkbox("##value", &isOn)) SetParameter(pedal.id, parameter.name, isOn ? "1" : "0");
				break;
			}
			case ParameterType::Int:
			{
				int value = static_cast<int>(std::lround(CurrentValue(pedal, parameter)));
				ImGui::SetNextItemWidth(controlWidth);
				if (ImGui::SliderInt("##value", &value, static_cast<int>(parameter.minimum), static_cast<int>(parameter.maximum)))
					SetParameter(pedal.id, parameter.name, std::to_string(value));
				break;
			}
			case ParameterType::Enum:
			case ParameterType::File:
			{
				const bool isFile = parameter.type == ParameterType::File;
				const std::string preview = current.empty() ? std::string("None") : (isFile ? ShortFileName(current) : current);
				ImGui::SetNextItemWidth(controlWidth);
				PushPopupPadding();
				if (ImGui::BeginCombo("##value", preview.c_str()))
				{
					for (const std::string& choice : parameter.choices)
					{
						const std::string label = isFile ? ShortFileName(choice) : choice;
						if (ImGui::Selectable(label.c_str(), choice == current) && choice != current)
						{
							SetParameter(pedal.id, parameter.name, choice);
							if (isFile) State().loadingPedals.push_back(pedal.id);
						}
					}
					ImGui::EndCombo();
				}
				ImGui::PopStyleVar();
				break;
			}
			default:
				KnobControl(pedal, parameter, controlWidth);
				break;
			}
			ImGui::PopID();
		}

		// Stompbox's sliders (EQ-7's bands): side by side, as on the pedal itself.
		void SliderBank(const Pedal& pedal, const std::vector<const ParameterDefinition*>& sliders)
		{
			const float height = 110.0f;
			ImFont* caption = CaptionFont();
			for (size_t i = 0; i < sliders.size(); ++i)
			{
				const ParameterDefinition& parameter = *sliders[i];
				if (i > 0) ImGui::SameLine(0.0f, 8.0f);
				ImGui::BeginGroup();
				ImGui::PushID(parameter.name.c_str());
				float value = CurrentValue(pedal, parameter);
				if (ImGui::VSliderFloat("##band", ImVec2(26.0f, height), &value, parameter.minimum, parameter.maximum, ""))
					SetParameter(pedal.id, parameter.name, NumberText(value));
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", parameter.name.c_str(), FormatValue(parameter, value).c_str());
				ImGui::PushFont(caption);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Dim));
				ImGui::TextUnformatted(parameter.name.c_str());
				ImGui::PopStyleColor();
				ImGui::PopFont();
				ImGui::PopID();
				ImGui::EndGroup();
			}
		}

		// Controls inside the floating editor, using the engine's parameter definitions and the panel's colours.
		void EditorContents(const Pedal& pedal, float width, float height)
		{
			PageState& state = State();
			const PedalDefinition* definition = FindDefinition(pedal.kind);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(Ink::Library));
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::ColorConvertU32ToFloat4(Ink::RowHover));
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImGui::ColorConvertU32ToFloat4(Ink::RowSelected));
			ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::ColorConvertU32ToFloat4(Ink::Blue));
			ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImGui::ColorConvertU32ToFloat4(Ink::AddText));
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
			ImGui::BeginChild("##stompboxEditor", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
			const float inner = ImGui::GetContentRegionAvail().x;
			const float closeSize = ImGui::GetFrameHeight();

			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(pedal.id.c_str());
			const std::string file = PedalFileLabel(pedal, definition);
			if (!file.empty())
			{
				ImGui::SameLine();
				ImGui::PushFont(CaptionFont());
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Sub));
				ImGui::TextUnformatted(file.c_str());
				ImGui::PopStyleColor();
				ImGui::PopFont();
			}
			ImGui::SameLine(inner - 2.0f * closeSize - 6.0f + ImGui::GetCursorStartPos().x);
			if (IconSquare("##editorPower", Glyph::Power, pedal.isEnabled, pedal.isEnabled ? "On: click to bypass it." : "Bypassed: click to turn it on.", closeSize))
				SetEnabled(pedal.id, !pedal.isEnabled);
			ImGui::SameLine(0.0f, 6.0f);
			if (IconSquare("##closeEditor", Glyph::Close, false, "Close the controls.", closeSize)) state.isEditorOpen = false;
			if (!definition)
			{
				MutedCaption("The engine does not describe this pedal.", inner);
				ImGui::EndChild();
				ImGui::PopStyleVar(2);
				ImGui::PopStyleColor(6);
				return;
			}
			if (!definition->description.empty()) MutedCaption(definition->description.c_str(), inner);
			ImGui::Dummy(ImVec2(0, 4));

			const float labelWidth = std::min(120.0f, inner * 0.38f) + ImGui::GetCursorStartPos().x;
			const float controlWidth = inner - labelWidth + ImGui::GetCursorStartPos().x;
			std::vector<const ParameterDefinition*> sliders, advanced;
			for (const ParameterDefinition& parameter : definition->parameters)
			{
				if (parameter.isOutput) continue;
				if (parameter.type == ParameterType::VerticalSlider) sliders.push_back(&parameter);
				else if (parameter.isAdvanced) advanced.push_back(&parameter);
				else ParameterControl(pedal, parameter, labelWidth, controlWidth);
			}
			if (!sliders.empty()) SliderBank(pedal, sliders);
			if (!advanced.empty() && ImGui::CollapsingHeader("Advanced"))
			{
				for (const ParameterDefinition* parameter : advanced) ParameterControl(pedal, *parameter, labelWidth, controlWidth);
			}
			ImGui::EndChild();
			ImGui::PopStyleVar(2);
			ImGui::PopStyleColor(6);
		}

		// ---- Song tones -------------------------------------------------------------------------------------

		// The song's state on the pane's title row: loading, ready (with what plays) or failed.
		void SongStatePill()
		{
			const Snapshot& s = GetSnapshot();
			const char* text = nullptr;
			ImU32 hue = Ink::Muted;
			std::string playing;
			switch (s.songState)
			{
			case SongState::Preparing:
				text = "Loading the song's tones";
				hue = Ink::Blue;
				break;
			case SongState::Ready:
				playing = "Playing " + (s.playingTone.empty() ? std::string("the board's tone") : s.playingTone);
				text = playing.c_str();
				hue = Ink::Good;
				break;
			case SongState::Failed:
				text = "The song's tones did not load";
				hue = Ink::Bad;
				break;
			default:
				return;
			}
			ImGui::SameLine(0.0f, 12.0f);
			ImFont* caption = CaptionFont();
			const float height = ImGui::GetFrameHeight();
			const ImVec2 at = ImGui::GetCursorScreenPos();
			const PillLook look = TintPill(text, hue);
			ImGui::Dummy(ImVec2(ToneDesignerUi::PillWidth(caption, text, true), height));
			DrawPill(ImGui::GetWindowDrawList(), caption, at, height, look, hue);
		}

		// One song tone and the saved tone it plays; the one the game plays now is marked.
		void SongToneRow(const std::string& songKey, const std::string& slot, const std::string& name, const std::string& toneKey, float width)
		{
			const Snapshot& s = GetSnapshot();
			const bool isPlayingNow = toneKey == s.gameToneKey;
			const auto assigned = s.songTones.find(toneKey);
			const std::string current = assigned == s.songTones.end() ? std::string() : assigned->second;
			ImGui::PushID(toneKey.c_str());
			const float rowH = ImGui::GetFrameHeight() + 6.0f;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			ImDrawList* list = ImGui::GetWindowDrawList();
			const bool isHovered = ImGui::IsMouseHoveringRect(at, ImVec2(at.x + width, at.y + rowH));
			ToneDesignerUi::RowBackground(list, at, ImVec2(at.x + width, at.y + rowH), false, isHovered);
			ImFont* caption = CaptionFont();
			const float textY = at.y + (rowH - ImGui::GetFontSize()) * 0.5f;
			Text(list, caption, ImVec2(at.x + 10.0f, at.y + (rowH - caption->FontSize) * 0.5f), Ink::Dim, slot.c_str());
			list->PushClipRect(at, ImVec2(at.x + width * 0.5f - 8.0f, at.y + rowH), true);
			list->AddText(ImVec2(at.x + 58.0f, textY), isPlayingNow ? IM_COL32_WHITE : Ink::Text, name.c_str());
			list->PopClipRect();
			if (isPlayingNow)
			{
				const float nameW = std::min(ImGui::CalcTextSize(name.c_str()).x, width * 0.5f - 70.0f);
				DrawPill(list, caption, ImVec2(at.x + 66.0f + nameW, at.y + (rowH - caption->FontSize - 6.0f) * 0.5f), caption->FontSize + 6.0f, TintPill("now", Ink::Good));
			}
			const float comboW = width * 0.5f - 10.0f;
			ImGui::SetCursorScreenPos(ImVec2(at.x + width - comboW - 6.0f, at.y + 3.0f));
			ImGui::SetNextItemWidth(comboW);
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
			PushPopupPadding();
			if (ImGui::BeginCombo("##savedTone", current.empty() ? "Keep the tone before" : current.c_str()))
			{
				if (ImGui::Selectable("Keep the tone before", current.empty()) && !current.empty()) AssignSongTone(songKey, toneKey, {});
				for (const std::string& preset : s.presets)
				{
					if (ImGui::Selectable(preset.c_str(), preset == current) && preset != current) AssignSongTone(songKey, toneKey, preset);
				}
				ImGui::EndCombo();
			}
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
			Hint(toneKey.c_str());
			ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + rowH + 2.0f));
			ImGui::Dummy(ImVec2(0, 0));
			ImGui::PopID();
		}

		// Every tone of every arrangement of the chosen song, read from its own manifest (the song list), each with
		// the saved tone it plays. Nothing has to be reached in the game first. The host loads the assigned tones
		// before the song plays and follows the game's tone changes.
		void SongTones(float width)
		{
			const Snapshot& s = GetSnapshot();
			SongStatePill();
			const float left = ImGui::GetCursorStartPos().x;
			ImGui::SetCursorPosX(left);
			ImGui::Dummy(ImVec2(0, 2));
			ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
			ImGui::BeginChild("##songTones", ImVec2(width, ImGui::GetContentRegionAvail().y), false);
			const float rowW = ImGui::GetContentRegionAvail().x;
			if (s.songKey.empty())
			{
				MutedCaption("Choose a song in the game. Its tones show here, each ready to play a saved tone of yours.", rowW);
				ImGui::EndChild();
				ImGui::PopStyleVar();
				return;
			}
			const auto library = SongLibrary::Get();
			const SongLibrary::Song* song = library->Find(s.songKey);
			FoldLabel(song ? (song->artist + " - " + song->title).c_str() : s.songKey.c_str());
			if (!song || song->tones.empty())
			{
				MutedCaption(SongLibrary::GetState() == SongLibrary::State::Reading
					? "Reading the song list for this song's tones..."
					: "This song's tones are not in the song list. They appear once the song list has been read again.", rowW);
				if (!s.gameToneKey.empty()) SongToneRow(s.songKey, "Now", "The tone playing now", s.gameToneKey, rowW);
			}
			else
			{
				std::string arrangement;
				for (const SongLibrary::SongTone& tone : song->tones)
				{
					if (tone.arrangement != arrangement)
					{
						arrangement = tone.arrangement;
						FoldLabel(arrangement.c_str());
					}
					SongToneRow(s.songKey, tone.slot, tone.name, tone.key, rowW);
				}
			}
			ImGui::EndChild();
			ImGui::PopStyleVar();
		}

		// ---- Pane under the board ---------------------------------------------------------------------------

		// Pedals | Song tones: what the pane shows. It is the pane's title, at the start of its first row.
		void PaneToggle()
		{
			static const char* const modes[2] = { "Pedals", "Song tones" };
			static const char* const hints[2] = { "Effects, amps (NAM models) and cabinets (IRs): drag one onto the board to use it.",
				"The chosen song's tones: give each one a saved tone, and the song switches between them as it plays." };
			PageState& state = State();
			const int clicked = ToneDesignerUi::PaneToggle(modes, 2, state.isSongsMode ? 1 : 0, hints);
			if (clicked >= 0) state.isSongsMode = clicked == 1;
		}

		void BottomPane(float width, float height)
		{
			const Snapshot& s = GetSnapshot();
			PageState& state = State();
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 14));
			ImGui::BeginChild("##stompboxPane", ImVec2(width, height), false,
				ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::PopStyleVar();
			const float inner = ImGui::GetContentRegionAvail().x;
			PaneToggle();
			if (state.isSongsMode) SongTones(inner);
			else Browser(inner);
			ImGui::EndChild();
		}

		void DrawEditorWindow()
		{
			PageState& state = State();
			const Snapshot& snapshot = GetSnapshot();
			const int editing = state.isEditorOpen ? IndexOfPedal(state.selectedPedal) : -1;
			if (editing < 0)
			{
				state.isEditorOpen = false;
				return;
			}

			const Pedal& pedal = snapshot.chain[editing];
			constexpr float INITIAL_EDITOR_HEIGHT = 360.0f;
			ImGui::SetNextWindowSize(ImVec2(EDITOR_WIDTH, INITIAL_EDITOR_HEIGHT), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 220.0f), ImVec2(FLT_MAX, FLT_MAX));
			ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - EDITOR_WIDTH - 28.0f,
				std::max(28.0f, ImGui::GetIO().DisplaySize.y - INITIAL_EDITOR_HEIGHT - 28.0f)), ImGuiCond_FirstUseEver);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(Ink::Library));
			bool isOpen = state.isEditorOpen;
			const std::string title = pedal.kind + "###stompboxEditor_" + pedal.id;
			if (ImGui::Begin(title.c_str(), &isOpen, ImGuiWindowFlags_NoCollapse))
			{
				EditorContents(pedal, ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
			}
			ImGui::End();
			ImGui::PopStyleColor();
			ImGui::PopStyleVar();
			state.isEditorOpen = isOpen && state.isEditorOpen;
		}

		// ---- Status -----------------------------------------------------------------------------------------

		// False when the board cannot be used yet (the host or the engine is not there); says why.
		bool DrawStatus()
		{
			const Snapshot& s = GetSnapshot();
			if (!VstHostLink::HostProcessRunning())
			{
				BeginCard();
				Caption("The Tone designer runs in the Rocksmith Audio Bridge host, which is not running.");
				if (Button("Start", true)) VstHostLink::StartHost();
				EndCard();
				return false;
			}
			if (!s.isConnected || s.engineState == EngineState::Loading || s.engineState == EngineState::Off)
			{
				BeginCard();
				Caption("Starting the Tone designer engine...");
				EndCard();
				return false;
			}
			if (s.engineState == EngineState::Failed)
			{
				BeginCard();
				PillRow("Engine", s.error.empty() ? "Did not start" : s.error.c_str(), Color::Bad);
				EndCard();
				return false;
			}
			return true;
		}

		// The framed panel, laid out as the VST rig's: bar, library and board, a divider, then the pane. Backgrounds
		// go on the page's draw list under the parts, which are child windows. Returns the page's natural height.
		float Panel()
		{
			PageState& state = State();
			const bool isLate = HasRecentLateBlocks();
			const float width = ImGui::GetContentRegionAvail().x;
			const float barH = ImGui::GetFontSize() + 20.0f;
			const float available = ImGui::GetContentRegionAvail().y;
			const float bodyHeight = std::max(1.0f, available - barH - DIVIDER_HEIGHT - 12.0f);
			const float boardMinimum = std::min(MIN_BOARD_HEIGHT, bodyHeight * 0.55f);
			const float paneMinimum = std::min(MIN_PANE_HEIGHT, bodyHeight - boardMinimum);
			const float maxPaneH = bodyHeight - boardMinimum;
			const float paneH = std::clamp(std::floor(BASE_PANE_HEIGHT + state.paneExtra), paneMinimum, maxPaneH);
			const float naturalHeight = ImGui::GetCursorPosY() + barH + NATURAL_MAIN_HEIGHT + BASE_PANE_HEIGHT + DIVIDER_HEIGHT + 12.0f + 20.0f;
			const float mainH = bodyHeight - paneH;
			const float libraryW = std::min(240.0f, width * 0.32f);
			const ImVec2 at = ImGui::GetCursorScreenPos();
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->ChannelsSplit(2);
			list->ChannelsSetCurrent(1);

			Bar(width, barH, isLate);
			ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + barH));
			LibraryColumn(libraryW, mainH);
			ImGui::SameLine(0.0f, 0.0f);
			Board(width - libraryW, mainH, isLate);
			const float dividerTop = at.y + barH + mainH;
			const float paneTop = dividerTop + DIVIDER_HEIGHT;
			ImGui::SetCursorScreenPos(ImVec2(at.x, paneTop));
			BottomPane(width, paneH);
			const float bottom = paneTop + paneH;

			// The divider: drag the line between the board and the pane; double-click puts it back.
			ImGui::SetCursorScreenPos(ImVec2(at.x + 12.0f, dividerTop));
			ImGui::InvisibleButton("##stompboxDivider", ImVec2(width - 24.0f, DIVIDER_HEIGHT));
			const bool isDividerHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
			if (isDividerHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
			if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0.0f)
				state.paneExtra = std::clamp(paneH - ImGui::GetIO().MouseDelta.y, paneMinimum, maxPaneH) - BASE_PANE_HEIGHT;
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) state.paneExtra = 0.0f;

			list->ChannelsSetCurrent(0);
			ToneDesignerUi::PanelFrame frame;
			frame.at = at;
			frame.width = width;
			frame.barHeight = barH;
			frame.mainHeight = mainH;
			frame.libraryWidth = libraryW;
			frame.bottom = bottom;
			frame.hasPane = true;
			frame.dividerTop = dividerTop;
			frame.dividerHeight = DIVIDER_HEIGHT;
			frame.isDividerHot = isDividerHot;
			ToneDesignerUi::DrawPanelFrame(list, frame);
			list->ChannelsMerge();
			ImGui::SetCursorScreenPos(ImVec2(at.x, bottom + 6.0f));
			ImGui::Dummy(ImVec2(0, 0));
			return naturalHeight;
		}
	}

	float DrawStompboxPage()
	{
		PageState& state = State();
		const Snapshot& s = GetSnapshot();
		if (!s.isWanted) SetWanted(true);   // this tab plays the Tone designer engine
		SongLibrary::Refresh();             // once per session: the song list, and the tones in it
		if (!DrawStatus()) return 0.0f;
		if (!s.isBusy && !s.isPresetLoading) state.loadingPedals.clear();
		if (!s.isPlaying) StatusPill("Switching to the Tone designer...", Color::Warn);
		if (!s.error.empty()) StatusPill(s.error.c_str(), Color::Bad);
		const float naturalHeight = Panel();
		if (state.isEditorOpen) DrawEditorWindow();
		return naturalHeight;
	}
}
