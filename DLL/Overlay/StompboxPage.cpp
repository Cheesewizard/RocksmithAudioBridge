#include "stdafx.h"
#include "StompboxPage.hpp"
#include "OverlayUi.hpp"
#include "ToneDesigner/StompboxLink.hpp"
#include "ToneDesigner/VstHostLink.hpp"
#include "Shared/ShellOpenAsync.hpp"

#include <ShlObj.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace Overlay
{
	namespace
	{
		using namespace StompboxLink;

		constexpr float PEDAL_WIDTH = 112.0f;
		constexpr float PEDAL_HEIGHT = 128.0f;
		constexpr float PEDAL_GAP = 22.0f;
		constexpr float FOOTSWITCH_HEIGHT = 30.0f;
		constexpr float LIBRARY_WIDTH = 190.0f;
		constexpr float BOARD_HEIGHT = PEDAL_HEIGHT + 44.0f;
		constexpr float EDITOR_WIDTH = 360.0f;
		constexpr int MAX_NAM_PEDALS = 2;      // each NAM amp costs a large share of a 64-frame block
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

		BrowserItem FromPayload(const ImGuiPayload& payload)
		{
			const auto& data = *static_cast<const NewPedalPayload*>(payload.Data);
			return { data.kind, data.file };
		}

		struct PageState
		{
			std::string selectedPedal;          // pedal id on the board
			bool isEditorOpen = false;
			BrowserTab tab = BrowserTab::Effects;
			int selectedItem = -1;              // row in the browser's current list
			char search[64] = {};
			char toneName[64] = {};
			bool isSaveOpen = false;
			std::string notice;                 // a refusal or hint under the board, for a few seconds
			ULONGLONG noticeAt = 0;
			std::vector<std::string> loadingPedals;   // a file was just chosen: spinner until the host confirms
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

		ImU32 ParseColor(const std::string& hex, ImU32 fallback)
		{
			unsigned r = 0, g = 0, b = 0;
			if (hex.size() == 7 && hex[0] == '#' && sscanf_s(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) return IM_COL32(r, g, b, 255);
			return fallback;
		}

		bool IsAmpKind(const std::string& kind) { return kind == AMP_KIND || kind == "NAMMulti"; }

		const ParameterDefinition* FileParameter(const PedalDefinition* definition)
		{
			if (!definition) return nullptr;
			for (const ParameterDefinition& parameter : definition->parameters)
				if (parameter.type == ParameterType::File) return &parameter;
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

		std::vector<std::string> ChainIds()
		{
			std::vector<std::string> ids;
			for (const Pedal& pedal : GetSnapshot().chain) ids.push_back(pedal.id);
			return ids;
		}

		int CountAmps()
		{
			const auto& chain = GetSnapshot().chain;
			return static_cast<int>(std::count_if(chain.begin(), chain.end(), [](const Pedal& p) { return IsAmpKind(p.kind); }));
		}

		// Where a pedal added without a drop position goes: a gate near the input, an amp before the first cabinet,
		// a cabinet after the last amp, anything else at the end. Only a default: the player can drag it anywhere.
		int DefaultInsertIndex(const std::string& kind)
		{
			const auto& chain = GetSnapshot().chain;
			const int count = static_cast<int>(chain.size());
			const auto lastOf = [&](auto predicate) { for (int i = count - 1; i >= 0; --i) if (predicate(chain[i].kind)) return i; return -1; };
			const auto firstOf = [&](auto predicate) { for (int i = 0; i < count; ++i) if (predicate(chain[i].kind)) return i; return -1; };
			if (kind == "NoiseGate" || kind == "SimpleGate")
			{
				const int lastGate = lastOf([](const std::string& k) { return k == "NoiseGate" || k == "SimpleGate"; });
				return lastGate + 1;
			}
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
			if (IsAmpKind(item.kind) && CountAmps() >= MAX_NAM_PEDALS)
			{
				Notice("A tone can have at most two NAM amps: each one takes a large part of the audio time.");
				return;
			}
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

		void MovePedal(int from, int to)
		{
			std::vector<std::string> ids = ChainIds();
			if (from < 0 || from >= static_cast<int>(ids.size())) return;
			const std::string id = ids[from];
			ids.erase(ids.begin() + from);
			if (to > from) --to;
			ids.insert(ids.begin() + std::clamp(to, 0, static_cast<int>(ids.size())), id);
			SetChain(ids);
		}

		std::vector<BrowserItem> BrowserItems(BrowserTab tab, const std::string& search)
		{
			std::vector<BrowserItem> items;
			const auto matches = [&](const std::string& text)
			{
				if (search.empty()) return true;
				std::string lowerText = text, lowerSearch = search;
				std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), ::tolower);
				std::transform(lowerSearch.begin(), lowerSearch.end(), lowerSearch.begin(), ::tolower);
				return lowerText.find(lowerSearch) != std::string::npos;
			};
			if (tab == BrowserTab::Effects)
			{
				for (const PedalDefinition& definition : GetSnapshot().definitions)
					if (definition.isUserSelectable && !IsAmpKind(definition.name) && definition.name != CABINET_KIND && matches(definition.name))
						items.push_back({ definition.name, {} });
				return items;
			}
			const char* kind = tab == BrowserTab::Amps ? AMP_KIND : CABINET_KIND;
			if (const ParameterDefinition* file = FileParameter(FindDefinition(kind)))
				for (const std::string& choice : file->choices)
					if (matches(choice)) items.push_back({ kind, choice });
			return items;
		}

		std::wstring EngineFolder(const char* subfolder)
		{
			PWSTR appData = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData))) return {};
			std::wstring folder = std::wstring(appData) + L"\\RocksmithAudioBridge\\Stompbox\\" + std::wstring(subfolder, subfolder + strlen(subfolder));
			CoTaskMemFree(appData);
			SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
			return folder;
		}

		void Spinner(ImDrawList* list, ImVec2 center, float radius, ImU32 color)
		{
			const float start = static_cast<float>(ImGui::GetTime()) * 6.0f;
			list->PathArcTo(center, radius, start, start + 4.2f, 20);
			list->PathStroke(color, false, 2.5f);
		}

		// ---- Saved tones ------------------------------------------------------------------------------------

		void DrawLibrary(float height)
		{
			const Snapshot& s = GetSnapshot();
			ImGui::BeginChild("##stompboxTones", ImVec2(LIBRARY_WIDTH, height), true);
			SectionLabel("Tones");
			if (s.presets.empty()) Caption("None yet. Save keeps the board with every pedal's settings.");
			for (const std::string& preset : s.presets)
			{
				if (ImGui::Selectable(preset.c_str(), preset == s.currentPreset)) LoadPreset(preset);
				if (ImGui::BeginPopupContextItem())
				{
					if (ImGui::MenuItem("Delete")) DeletePreset(preset);
					ImGui::EndPopup();
				}
			}
			ImGui::EndChild();
		}

		void DrawToolbar()
		{
			const Snapshot& s = GetSnapshot();
			PageState& state = State();
			ImGui::TextUnformatted(s.currentPreset.empty() ? "Unsaved tone" : s.currentPreset.c_str());
			ImGui::SameLine();
			if (!s.currentPreset.empty() && Button("Save", true, "Saves the board over this tone")) SavePreset(s.currentPreset);
			ImGui::SameLine();
			if (Button("Save as...", s.currentPreset.empty()))
			{
				state.isSaveOpen = true;
				strncpy_s(state.toneName, s.currentPreset.c_str(), _TRUNCATE);
			}
			if (state.isSaveOpen)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(200.0f);
				const bool entered = ImGui::InputText("##toneName", state.toneName, sizeof(state.toneName), ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::SameLine();
				if ((Button("Save tone", true) || entered) && state.toneName[0])
				{
					SavePreset(state.toneName);
					state.isSaveOpen = false;
				}
				ImGui::SameLine();
				if (Button("Cancel")) state.isSaveOpen = false;
			}
			if (s.isPresetLoading || s.isBusy)
			{
				ImGui::SameLine();
				ImDrawList* list = ImGui::GetWindowDrawList();
				const ImVec2 at = ImGui::GetCursorScreenPos();
				Spinner(list, ImVec2(at.x + 9.0f, at.y + ImGui::GetFrameHeight() * 0.5f), 6.0f, Color::AccentHi);
				ImGui::Dummy(ImVec2(18.0f, ImGui::GetFrameHeight()));
			}
		}

		// ---- Pedalboard -------------------------------------------------------------------------------------

		void DrawPedal(const Pedal& pedal, int index, ImVec2 at)
		{
			const Snapshot& s = GetSnapshot();
			PageState& state = State();
			const PedalDefinition* definition = FindDefinition(pedal.kind);
			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 end(at.x + PEDAL_WIDTH, at.y + PEDAL_HEIGHT);
			const bool selected = state.selectedPedal == pedal.id;
			const ImU32 body = ParseColor(definition ? definition->backgroundColor : std::string(), Color::Card);
			const ImU32 ink = ParseColor(definition ? definition->foregroundColor : std::string(), Color::Text);

			ImGui::SetCursorScreenPos(at);
			ImGui::PushID(pedal.id.c_str());
			ImGui::InvisibleButton("##pedal", ImVec2(PEDAL_WIDTH, PEDAL_HEIGHT));
			const bool hovered = ImGui::IsItemHovered();
			const bool onFootswitch = hovered && ImGui::GetIO().MousePos.y > end.y - FOOTSWITCH_HEIGHT;
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			{
				if (onFootswitch) SetEnabled(pedal.id, !pedal.isEnabled);
				else state.selectedPedal = pedal.id;
			}
			if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !onFootswitch)
			{
				state.selectedPedal = pedal.id;
				state.isEditorOpen = true;
			}
			if (ImGui::BeginDragDropSource())
			{
				ImGui::SetDragDropPayload(PEDAL_PAYLOAD, &index, sizeof(index));
				ImGui::TextUnformatted(pedal.kind.c_str());
				ImGui::EndDragDropSource();
			}
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(PEDAL_PAYLOAD)) MovePedal(*static_cast<const int*>(payload->Data), index);
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(NEW_PAYLOAD)) AddPedal(FromPayload(*payload), index);
				ImGui::EndDragDropTarget();
			}
			if (ImGui::BeginPopupContextItem("##pedalMenu"))
			{
				if (ImGui::MenuItem("Edit")) { state.selectedPedal = pedal.id; state.isEditorOpen = true; }
				if (ImGui::MenuItem(pedal.isEnabled ? "Bypass" : "Turn on")) SetEnabled(pedal.id, !pedal.isEnabled);
				if (ImGui::MenuItem("Remove")) RemovePedal(pedal.id);
				ImGui::EndPopup();
			}
			ImGui::PopID();

			list->AddRectFilled(at, end, pedal.isEnabled ? body : Mix(body, Color::Window, 0.55f), 8.0f);
			list->AddRect(at, end, selected ? Color::AccentHi : (hovered ? Color::Muted : Color::CardEdge), 8.0f, 0, selected ? 2.5f : 1.0f);
			const ImVec2 nameSize = ImGui::CalcTextSize(pedal.kind.c_str());
			list->AddText(ImVec2(at.x + (PEDAL_WIDTH - nameSize.x) * 0.5f, at.y + 10.0f), ink, pedal.kind.c_str());
			// The file it plays ("G5 Crunch UK" on an amp, "British 4x12" on a cabinet).
			if (const ParameterDefinition* file = FileParameter(definition))
			{
				const auto value = pedal.values.find(file->name);
				const std::string label = value == pedal.values.end() || value->second.empty() ? std::string("No file") : ShortFileName(value->second);
				ImGui::PushClipRect(ImVec2(at.x + 6.0f, at.y), ImVec2(end.x - 6.0f, end.y), true);
				const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
				list->AddText(ImVec2(at.x + std::max(6.0f, (PEDAL_WIDTH - labelSize.x) * 0.5f), at.y + 44.0f), Mix(ink, body, 0.3f), label.c_str());
				ImGui::PopClipRect();
			}
			const bool loading = std::find(state.loadingPedals.begin(), state.loadingPedals.end(), pedal.id) != state.loadingPedals.end();
			if (loading || (s.isPresetLoading && FileParameter(definition))) Spinner(list, ImVec2(at.x + PEDAL_WIDTH * 0.5f, at.y + 76.0f), 9.0f, ink);
			const ImVec2 led(at.x + PEDAL_WIDTH * 0.5f, end.y - FOOTSWITCH_HEIGHT * 0.5f);
			list->AddCircleFilled(led, 7.0f, pedal.isEnabled ? Color::Good : IM_COL32(40, 48, 60, 255));
			list->AddCircle(led, 8.0f, onFootswitch ? Color::Text : Color::CardEdge);
		}

		void DrawBoard(float width)
		{
			const Snapshot& s = GetSnapshot();
			ImGui::BeginChild("##stompboxBoard", ImVec2(width, BOARD_HEIGHT), true, ImGuiWindowFlags_HorizontalScrollbar);
			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const float y = origin.y + 8.0f;
			float x = origin.x + 34.0f;
			list->AddText(ImVec2(origin.x + 2.0f, y + PEDAL_HEIGHT * 0.5f - 7.0f), Color::Muted, "IN");
			for (int i = 0; i < static_cast<int>(s.chain.size()); ++i)
			{
				if (i > 0) list->AddLine(ImVec2(x - PEDAL_GAP, y + PEDAL_HEIGHT * 0.5f), ImVec2(x, y + PEDAL_HEIGHT * 0.5f), Color::Faint, 2.0f);
				DrawPedal(s.chain[i], i, ImVec2(x, y));
				x += PEDAL_WIDTH + PEDAL_GAP;
			}
			// The end slot: drop here to append.
			ImGui::SetCursorScreenPos(ImVec2(x, y));
			ImGui::InvisibleButton("##boardEnd", ImVec2(PEDAL_WIDTH * 0.6f, PEDAL_HEIGHT));
			if (ImGui::BeginDragDropTarget())
			{
				const int count = static_cast<int>(s.chain.size());
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(PEDAL_PAYLOAD)) MovePedal(*static_cast<const int*>(payload->Data), count);
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(NEW_PAYLOAD)) AddPedal(FromPayload(*payload), count);
				ImGui::EndDragDropTarget();
			}
			list->AddRect(ImVec2(x, y), ImVec2(x + PEDAL_WIDTH * 0.6f, y + PEDAL_HEIGHT), Color::CardEdge, 8.0f);
			const ImVec2 plusSize = ImGui::CalcTextSize("+");
			list->AddText(ImVec2(x + (PEDAL_WIDTH * 0.6f - plusSize.x) * 0.5f, y + PEDAL_HEIGHT * 0.5f - plusSize.y * 0.5f), Color::Muted, "+");
			x += PEDAL_WIDTH * 0.6f + 14.0f;
			list->AddText(ImVec2(x, y + PEDAL_HEIGHT * 0.5f - 7.0f), Color::Muted, "OUT");
			ImGui::SetCursorScreenPos(ImVec2(x + 30.0f, y + PEDAL_HEIGHT + 6.0f));
			ImGui::Dummy(ImVec2(1.0f, 1.0f));
			ImGui::EndChild();
		}

		// ---- Browser ----------------------------------------------------------------------------------------

		void DrawBrowser(float width, float height)
		{
			PageState& state = State();
			ImGui::BeginChild("##stompboxBrowser", ImVec2(width, height), true);
			static const char* const tabs[] = { "Effects", "Amps", "Cabinets" };
			int tab = static_cast<int>(state.tab);
			if (Segmented("##browserTab", &tab, tabs, 3, 300.0f))
			{
				state.tab = static_cast<BrowserTab>(tab);
				state.selectedItem = -1;
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 120.0f));
			ImGui::InputTextWithHint("##browserSearch", "Search...", state.search, sizeof(state.search));
			if (state.tab != BrowserTab::Effects)
			{
				ImGui::SameLine();
				if (Button("Folder", false, state.tab == BrowserTab::Amps ? "Opens the folder for NAM models (.nam)" : "Opens the folder for cabinet IRs (.wav)"))
					ShellOpenAsync::Open(EngineFolder(state.tab == BrowserTab::Amps ? "NAM" : "Cabinets"));
			}

			const std::vector<BrowserItem> items = BrowserItems(state.tab, state.search);
			const float listHeight = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - 4.0f;
			ImGui::BeginChild("##browserList", ImVec2(0, listHeight), false);
			if (items.empty())
				Caption(state.tab == BrowserTab::Effects ? "No effects match." : "None yet. Put files in the folder (Folder button); the engine lists them when the host starts.");
			for (int i = 0; i < static_cast<int>(items.size()); ++i)
			{
				const BrowserItem& item = items[i];
				const PedalDefinition* definition = FindDefinition(item.kind);
				const std::string label = item.file.empty() ? item.kind : ShortFileName(item.file);
				ImGui::PushID(i);
				if (ImGui::Selectable(label.c_str(), state.selectedItem == i, ImGuiSelectableFlags_AllowDoubleClick))
				{
					state.selectedItem = i;
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) AddPedal(item, -1);
				}
				if (item.file.empty() && definition && !definition->description.empty()) Hint(definition->description.c_str());
				if (ImGui::BeginDragDropSource())
				{
					NewPedalPayload payload{};
					strncpy_s(payload.kind, item.kind.c_str(), _TRUNCATE);
					strncpy_s(payload.file, item.file.c_str(), _TRUNCATE);
					ImGui::SetDragDropPayload(NEW_PAYLOAD, &payload, sizeof(payload));
					ImGui::TextUnformatted(label.c_str());
					ImGui::EndDragDropSource();
				}
				ImGui::PopID();
			}
			ImGui::EndChild();
			const bool hasItem = state.selectedItem >= 0 && state.selectedItem < static_cast<int>(items.size());
			if (Button("Add to chain", hasItem) && hasItem) AddPedal(items[state.selectedItem], -1);
			ImGui::SameLine();
			if (Button("Replace selected") && hasItem && !state.selectedPedal.empty())
			{
				const auto ids = ChainIds();
				const auto at = std::find(ids.begin(), ids.end(), state.selectedPedal);
				if (at != ids.end())
				{
					const int index = static_cast<int>(at - ids.begin());
					RemovePedal(*at);
					AddPedal(items[state.selectedItem], index);
				}
			}
			ImGui::EndChild();
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
		void KnobControl(const Pedal& pedal, const ParameterDefinition& parameter)
		{
			const float span = parameter.maximum - parameter.minimum;
			const float power = parameter.rangePower > 0.0f ? parameter.rangePower : 1.0f;
			const float value = CurrentValue(pedal, parameter);
			float position = span > 0.0f ? std::pow(std::clamp((value - parameter.minimum) / span, 0.0f, 1.0f), 1.0f / power) : 0.0f;
			const std::string text = FormatValue(parameter, value);
			ImGui::SetNextItemWidth(EDITOR_WIDTH - 150.0f);
			if (ImGui::SliderFloat(("##" + parameter.name).c_str(), &position, 0.0f, 1.0f, text.c_str()))
				SetParameter(pedal.id, parameter.name, NumberText(parameter.minimum + span * std::pow(position, power)));
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				SetParameter(pedal.id, parameter.name, NumberText(parameter.defaultValue));
		}

		void ParameterControl(const Pedal& pedal, const ParameterDefinition& parameter)
		{
			ImGui::PushID(parameter.name.c_str());
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(parameter.name.c_str());
			if (!parameter.description.empty()) Hint(parameter.description.c_str());
			ImGui::SameLine(120.0f);
			const auto stored = pedal.values.find(parameter.name);
			const std::string current = stored == pedal.values.end() ? std::string() : stored->second;
			switch (parameter.type)
			{
			case ParameterType::Bool:
			{
				bool on = CurrentValue(pedal, parameter) >= 0.5f;
				if (ImGui::Checkbox("##value", &on)) SetParameter(pedal.id, parameter.name, on ? "1" : "0");
				break;
			}
			case ParameterType::Int:
			{
				int value = static_cast<int>(std::lround(CurrentValue(pedal, parameter)));
				ImGui::SetNextItemWidth(EDITOR_WIDTH - 150.0f);
				if (ImGui::SliderInt("##value", &value, static_cast<int>(parameter.minimum), static_cast<int>(parameter.maximum)))
					SetParameter(pedal.id, parameter.name, std::to_string(value));
				break;
			}
			case ParameterType::Enum:
			case ParameterType::File:
			{
				const std::string preview = current.empty() ? std::string("None") : (parameter.type == ParameterType::File ? ShortFileName(current) : current);
				ImGui::SetNextItemWidth(EDITOR_WIDTH - 150.0f);
				if (ImGui::BeginCombo("##value", preview.c_str()))
				{
					for (const std::string& choice : parameter.choices)
					{
						const std::string label = parameter.type == ParameterType::File ? ShortFileName(choice) : choice;
						if (ImGui::Selectable(label.c_str(), choice == current))
						{
							SetParameter(pedal.id, parameter.name, choice);
							if (parameter.type == ParameterType::File) State().loadingPedals.push_back(pedal.id);
						}
					}
					ImGui::EndCombo();
				}
				break;
			}
			default:
				KnobControl(pedal, parameter);
				break;
			}
			ImGui::PopID();
		}

		// Stompbox's sliders (EQ-7's bands): side by side, as on the pedal itself.
		void SliderBank(const Pedal& pedal, const std::vector<const ParameterDefinition*>& sliders)
		{
			const float height = 120.0f;
			for (size_t i = 0; i < sliders.size(); ++i)
			{
				const ParameterDefinition& parameter = *sliders[i];
				if (i > 0) ImGui::SameLine();
				ImGui::BeginGroup();
				ImGui::PushID(parameter.name.c_str());
				float value = CurrentValue(pedal, parameter);
				if (ImGui::VSliderFloat("##band", ImVec2(28.0f, height), &value, parameter.minimum, parameter.maximum, ""))
					SetParameter(pedal.id, parameter.name, NumberText(value));
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", parameter.name.c_str(), FormatValue(parameter, value).c_str());
				ImGui::TextUnformatted(parameter.name.c_str());
				ImGui::PopID();
				ImGui::EndGroup();
			}
		}

		void DrawEditor(float height)
		{
			PageState& state = State();
			const Snapshot& s = GetSnapshot();
			const auto pedal = std::find_if(s.chain.begin(), s.chain.end(), [&](const Pedal& p) { return p.id == state.selectedPedal; });
			if (pedal == s.chain.end())
			{
				state.isEditorOpen = false;
				return;
			}
			const PedalDefinition* definition = FindDefinition(pedal->kind);
			ImGui::BeginChild("##stompboxEditor", ImVec2(EDITOR_WIDTH, height), true);
			ImGui::PushFont(GetFonts().title ? GetFonts().title : ImGui::GetFont());
			ImGui::TextUnformatted(pedal->id.c_str());
			ImGui::PopFont();
			ImGui::SameLine(EDITOR_WIDTH - 40.0f);
			if (IconButton("##closeEditor", Glyph::Close, 22.0f)) state.isEditorOpen = false;
			bool enabled = pedal->isEnabled;
			if (ImGui::Checkbox("On", &enabled)) SetEnabled(pedal->id, enabled);
			ImGui::SameLine();
			if (Button("Remove")) RemovePedal(pedal->id);
			if (!definition)
			{
				Caption("The engine does not describe this pedal.");
				ImGui::EndChild();
				return;
			}
			if (!definition->description.empty()) Caption(definition->description.c_str());
			ImGui::Separator();
			std::vector<const ParameterDefinition*> sliders, advanced;
			for (const ParameterDefinition& parameter : definition->parameters)
			{
				if (parameter.isOutput) continue;   // meters the effect writes
				if (parameter.type == ParameterType::VerticalSlider) sliders.push_back(&parameter);
				else if (parameter.isAdvanced) advanced.push_back(&parameter);
				else ParameterControl(*pedal, parameter);
			}
			if (!sliders.empty()) SliderBank(*pedal, sliders);
			if (!advanced.empty() && ImGui::CollapsingHeader("Advanced"))
				for (const ParameterDefinition* parameter : advanced) ParameterControl(*pedal, *parameter);
			ImGui::EndChild();
		}

		// ---- Status -----------------------------------------------------------------------------------------

		// False when the board cannot be used yet (the host or the engine is not there); says why.
		bool DrawStatus()
		{
			const Snapshot& s = GetSnapshot();
			if (!VstHostLink::HostProcessRunning())
			{
				Caption("The Tone designer runs in the Rocksmith Audio Bridge host, which is not running.");
				if (Button("Start", true)) VstHostLink::StartHost();
				return false;
			}
			if (!s.isConnected || s.engineState == EngineState::Loading || s.engineState == EngineState::Off)
			{
				Caption("Starting the Tone designer engine...");
				return false;
			}
			if (s.engineState == EngineState::Failed)
			{
				PillRow("Engine", s.error.empty() ? "Did not start" : s.error.c_str(), Color::Bad);
				return false;
			}
			if (!s.isPlaying) PillRow("Playing", "Switching to the Tone designer...", Color::Warn);
			if (!s.error.empty()) PillRow("Problem", s.error.c_str(), Color::Bad);
			return true;
		}
	}

	void DrawStompboxPage()
	{
		PageState& state = State();
		const Snapshot& s = GetSnapshot();
		if (!s.isWanted) SetWanted(true);   // this tab plays the Tone designer engine
		if (!DrawStatus()) return;
		if (!s.isBusy && !s.isPresetLoading) state.loadingPedals.clear();

		DrawToolbar();
		const float width = ImGui::GetContentRegionAvail().x;
		const ImVec2 top = ImGui::GetCursorPos();
		DrawLibrary(BOARD_HEIGHT);
		ImGui::SetCursorPos(ImVec2(top.x + LIBRARY_WIDTH + 8.0f, top.y));
		DrawBoard(width - LIBRARY_WIDTH - 8.0f);
		if (!state.notice.empty() && GetTickCount64() - state.noticeAt < 6000) Caption(state.notice.c_str());
		else Caption("Drag to reorder. Double-click a pedal to edit it. Click its footswitch to bypass it.");

		const float bottom = std::max(200.0f, ImGui::GetContentRegionAvail().y);
		const bool editor = state.isEditorOpen && !state.selectedPedal.empty();
		DrawBrowser(editor ? width - EDITOR_WIDTH - 8.0f : width, bottom);
		if (editor)
		{
			ImGui::SameLine();
			DrawEditor(bottom);
		}
	}
}
