#include "StompboxPage.hpp"
#include "../../Shell/OverlayUi.hpp"
#include "ToneDesignerUi.hpp"
#include "Lib/ImGUI/imgui_internal.h"   // ImRect, drop targets on the gaps, SetItemUsingMouseWheel
#include "../StompboxLink.hpp"
#include "../VstHostLink.hpp"
#include "../SongLibrary.hpp"
#include "../SongToneSets.hpp"
#include "../ToneCatalog.hpp"
#include "../ToneBackup.hpp"
#include "../../Shared/StompboxControl.hpp"
#include "../../Core/ShellOpenAsync.hpp"
#include "../../ExternalAmp/ExternalAmp.hpp"

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

		// The panel's proportions, as on the VST rig page (ToneDesignerPage.cpp, Panel): the board a strip, the
		// Inspector the rest (ToneDesignerUi.hpp, the panel's layout).
		constexpr float NATURAL_INSPECTOR_HEIGHT = 320.0f;
		constexpr float MIN_INSPECTOR_HEIGHT = 170.0f;
		constexpr float NATURAL_MAIN_HEIGHT = 420.0f;
		constexpr float METER_COLUMN_WIDTH = 44.0f;
		// The load meter's lines. The typical block's share of real time is the tone's own cost, judged against the
		// base game (480-frame blocks): kept under half, a tone has room on slower PCs than the one it was made on.
		// The slowest block is what this PC's block size risks: ASIO's 128 or 64 frames are a bonus, not the target.
		constexpr float SHARE_WARN_LOAD = 0.5f;
		constexpr float SHARE_HEAVY_LOAD = 0.7f;
		constexpr float PEAK_WARN_LOAD = 0.85f;
		constexpr ULONGLONG NOTICE_MILLISECONDS = 6000;
		// Input calibration: the player's hardest playing is trimmed to peak here, the level the starter tones are
		// set for (RocksmithAudioBridge-Tones checks every tone with a DI test signal peaking at -12 dBFS). Captures
		// distort as much as they are driven, so a quiet cable makes every amp sound weaker than it should.
		constexpr float INPUT_TARGET_PEAK_DB = -12.0f;
		constexpr ULONGLONG INPUT_MEASURE_MILLISECONDS = 5000;
		constexpr float INPUT_SILENCE_DB = -55.0f;   // a measurement under this heard no guitar
		constexpr ULONGLONG LATE_MILLISECONDS = 10000;
		constexpr char AMP_KIND[] = "NAM";
		constexpr char CABINET_KIND[] = "Cabinet";
		constexpr char PEDAL_PAYLOAD[] = "STOMPBOX_PEDAL";
		constexpr char NEW_PAYLOAD[] = "STOMPBOX_NEW";

		enum class BrowserTab { Effects, Amps, Cabinets };
		// The browser column's tabs. With Songs shown, the area under the board shows the chosen song's tones in
		// place of the Inspector: that table needs the width the column does not have.
		enum ColumnTab { TonesColumn, PedalsColumn, SongsColumn };

		// What the Amps and Cabinets lists can be narrowed to, from the starter tones' manifest (ToneCatalog). Index 0
		// is everything, including files the player added.
		constexpr const char* AMP_FILTERS[] = { "All", "Clean", "Crunch", "High gain", "Pedals", "Bass" };
		constexpr const char* CABINET_FILTERS[] = { "All", "Guitar", "Bass" };

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
			int column = TonesColumn;           // ColumnTab
			bool isBrowserShown = true;         // View: the browser column and the Inspector, for the session
			bool isInspectorShown = true;
			int gapMenu = -1;                   // the gap whose right-click menu is open
			BrowserTab tab = BrowserTab::Effects;
			int soundFilter = 0;                // AMP_FILTERS or CABINET_FILTERS, for the tab shown
			int selectedItem = -1;              // row in the browser's current list
			char search[64] = {};
			char toneSearch[64] = {};
			char toneName[64] = {};
			std::string selectedTone;           // saved tone row in the library
			bool isSaveAsWanted = false;        // opened from a menu: the bar opens the popup in its own ID stack
			std::string notice;                 // a refusal or hint under the board, for a few seconds
			ULONGLONG noticeAt = 0;
			std::vector<std::string> loadingPedals;   // a file was just chosen: spinner until the host confirms
			uint32_t lastLateBlocks = 0;
			ULONGLONG lateAt = 0;
			bool isInputWanted = false;         // the bar's Input button was pressed: open its popup in the bar's ID stack
			ULONGLONG measureStartedAt = 0;     // 0: not measuring the input
			float measuredPeak = 0.0f;          // linear, before the trim
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

		// The file an amp or cabinet plays, as its tile and editor show it; empty for an effect. Stompbox names files
		// without their extension ("JCM800 v1.2" for "JCM800 v1.2.nam"), so the name is shown as it is.
		std::string PedalFileLabel(const Pedal& pedal, const PedalDefinition* definition)
		{
			const ParameterDefinition* file = FileParameter(definition);
			if (!file) return {};
			const auto value = pedal.values.find(file->name);
			return value == pedal.values.end() || value->second.empty() ? std::string("No file") : value->second;
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

		const ToneCatalog::FileInfo* CatalogInfo(const BrowserItem& item)
		{
			if (item.file.empty()) return nullptr;
			return ToneCatalog::Find(item.kind == CABINET_KIND ? "Cabinets" : "NAM", item.file);
		}

		// A NAM pedal playing a pedal capture (drive, boost): it goes in front of the amp and needs no cabinet. A file
		// the starter tones do not describe is taken for an amp, as most captures are.
		bool IsPedalCapture(const BrowserItem& item)
		{
			const ToneCatalog::FileInfo* info = CatalogInfo(item);
			return IsAmpKind(item.kind) && info && info->isPedal;
		}

		BrowserItem ItemOf(const Pedal& pedal)
		{
			const ParameterDefinition* file = FileParameter(FindDefinition(pedal.kind));
			const auto value = file ? pedal.values.find(file->name) : pedal.values.end();
			return { pedal.kind, value == pedal.values.end() ? std::string() : value->second };
		}

		bool IsAmpPedal(const Pedal& pedal) { return IsAmpKind(pedal.kind) && !IsPedalCapture(ItemOf(pedal)); }

		// Where a pedal added without a drop position goes: a gate near the input, a pedal capture in front of the
		// first amp, an amp before the first cabinet, a cabinet after the last amp, anything else at the end. Only a
		// default: the player can drag it anywhere.
		int DefaultInsertIndex(const BrowserItem& item)
		{
			const auto& chain = GetSnapshot().chain;
			const int count = static_cast<int>(chain.size());
			const auto lastOf = [&](auto predicate) { for (int i = count - 1; i >= 0; --i) if (predicate(chain[i])) return i; return -1; };
			const auto firstOf = [&](auto predicate) { for (int i = 0; i < count; ++i) if (predicate(chain[i])) return i; return -1; };
			const auto isGate = [](const Pedal& pedal) { return IsGateKind(pedal.kind); };
			const auto isCabinet = [](const Pedal& pedal) { return pedal.kind == CABINET_KIND; };
			if (IsGateKind(item.kind)) return lastOf(isGate) + 1;
			if (IsPedalCapture(item))
			{
				const int amp = firstOf(IsAmpPedal);
				return amp >= 0 ? amp : std::max(lastOf(isGate) + 1, firstOf(isCabinet) >= 0 ? firstOf(isCabinet) : count);
			}
			if (IsAmpKind(item.kind))
			{
				const int cabinet = firstOf(isCabinet);
				return cabinet >= 0 ? cabinet : count;
			}
			if (item.kind == CABINET_KIND)
			{
				const int amp = lastOf(IsAmpPedal);
				return amp >= 0 ? amp + 1 : count;
			}
			return count;
		}

		// The cabinet put after an amp added to a board that has none: an amp on its own is its line out, which
		// sounds thin and fizzy (it is the same with a real amp's direct out).
		constexpr char GUITAR_CABINET[] = "4x12 V30 SM57";
		constexpr char BASS_CABINET[] = "Synthetic 8x10 Bass";

		void ApplyStartingSettings(const std::string& id, const std::string& kind)
		{
			for (const StompboxControl::StartingSetting& setting : StompboxControl::STARTING_SETTINGS)
			{
				if (kind == setting.kind) SetParameter(id, setting.parameter, setting.value);
			}
		}

		void SetPedalFile(const std::string& id, const PedalDefinition* definition, const std::string& file)
		{
			if (file.empty()) return;
			if (const ParameterDefinition* parameter = FileParameter(definition))
			{
				SetParameter(id, parameter->name, file);
				State().loadingPedals.push_back(id);
			}
		}

		bool HasCabinetChoice(const std::string& name)
		{
			const ParameterDefinition* file = FileParameter(FindDefinition(CABINET_KIND));
			return file && std::find(file->choices.begin(), file->choices.end(), name) != file->choices.end();
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
			const auto& chain = GetSnapshot().chain;
			const bool needsCabinet = IsAmpKind(item.kind) && !IsPedalCapture(item)
				&& std::none_of(chain.begin(), chain.end(), [](const Pedal& pedal) { return pedal.kind == CABINET_KIND; });
			std::vector<std::string> ids = ChainIds();
			const int at = index < 0 ? DefaultInsertIndex(item) : std::clamp(index, 0, static_cast<int>(ids.size()));
			const std::string id = NewPedalId(item.kind);
			ids.insert(ids.begin() + at, id);
			std::string cabinetId, cabinetFile;
			if (needsCabinet)
			{
				const ToneCatalog::FileInfo* info = CatalogInfo(item);
				cabinetFile = info && info->isBass ? BASS_CABINET : GUITAR_CABINET;
				if (HasCabinetChoice(cabinetFile))
				{
					cabinetId = NewPedalId(CABINET_KIND);
					ids.insert(ids.begin() + at + 1, cabinetId);
					Notice("Added a " + cabinetFile + " cabinet after the amp: an amp on its own sounds thin and fizzy. Swap it in Cabinets.");
				}
				else Notice("Add a cabinet after the amp (Cabinets): an amp on its own sounds thin and fizzy.");
			}
			SetChain(ids);
			ApplyStartingSettings(id, item.kind);
			SetPedalFile(id, definition, item.file);
			if (!cabinetId.empty()) SetPedalFile(cabinetId, FindDefinition(CABINET_KIND), cabinetFile);
			State().selectedPedal = id;
		}

		void RemovePedal(const std::string& id)
		{
			std::vector<std::string> ids = ChainIds();
			ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
			SetChain(ids);
			if (State().selectedPedal == id) State().selectedPedal.clear();
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

		// The pedal's own window (the host's generated pedal editor, over the game like a plugin window).
		void OpenEditor(const std::string& id)
		{
			State().selectedPedal = id;
			StompboxLink::OpenPedalWindow(id);
		}

		// The controls inside the overlay (the Inspector): for exclusive fullscreen, where no window can show over
		// the game. Selecting the pedal shows them; this makes sure the Inspector is what is under the board.
		void OpenEditorInOverlay(const std::string& id)
		{
			State().selectedPedal = id;
			State().isInspectorShown = true;
			if (State().column == SongsColumn) State().column = TonesColumn;
		}

		bool PassesFilter(BrowserTab tab, int filter, const ToneCatalog::FileInfo* info)
		{
			using ToneCatalog::Character;
			if (filter == 0) return true;
			if (!info) return false;
			if (tab == BrowserTab::Cabinets) return filter == 1 ? !info->isBass : info->isBass;
			switch (filter)
			{
			case 1: return !info->isPedal && !info->isBass && info->character == Character::Clean;
			case 2: return !info->isPedal && !info->isBass && info->character == Character::Crunch;
			case 3: return !info->isPedal && !info->isBass && info->character == Character::HighGain;
			case 4: return info->isPedal;
			default: return info->isBass;
			}
		}

		// Clean amps first, then crunch, high gain, pedals and bass; the player's own files last.
		int SoundOrder(const ToneCatalog::FileInfo* info)
		{
			if (!info) return 9;
			if (info->isBass) return 5;
			if (info->isPedal) return 4;
			return static_cast<int>(info->character);
		}

		std::vector<BrowserItem> BrowserItems(BrowserTab tab, const char* search, int filter)
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
					const BrowserItem item{ kind, choice };
					const ToneCatalog::FileInfo* info = CatalogInfo(item);
					const bool isMatch = ContainsText(choice, search) || (info && ContainsText(info->description, search));
					if (isMatch && PassesFilter(tab, filter, info)) items.push_back(item);
				}
			}
			std::stable_sort(items.begin(), items.end(), [](const BrowserItem& a, const BrowserItem& b) { return SoundOrder(CatalogInfo(a)) < SoundOrder(CatalogInfo(b)); });
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

		// ---- Input level ------------------------------------------------------------------------------------

		float DecibelsOf(float linear) { return 20.0f * std::log10(std::max(linear, 1e-6f)); }

		// The guitar as it arrives, before the trim: the board's IN meter is after it.
		float RawInputPeak()
		{
			return VstHostLink::GetSnapshot().inputPeak / std::pow(10.0f, GetSnapshot().inputTrimDb / 20.0f);
		}

		std::string InputText(float trimDb, bool automatic = false)
		{
			char text[48];
			sprintf_s(text, automatic ? "Input %+.1f dB auto" : "Input %+.1f dB", trimDb);
			return text;
		}

		// A level bar from -60 to 0 dBFS with the target marked, `width` wide at the cursor.
		void InputLevelBar(float peak, float width)
		{
			ImDrawList* list = ImGui::GetWindowDrawList();
			const float height = 10.0f;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			const auto toX = [&](float decibels) { return at.x + width * std::clamp((decibels + 60.0f) / 60.0f, 0.0f, 1.0f); };
			const float level = DecibelsOf(peak);
			list->AddRectFilled(at, ImVec2(at.x + width, at.y + height), Ink::Field, 3.0f);
			const ImU32 color = level > -3.0f ? Ink::Bad : level > INPUT_TARGET_PEAK_DB + 4.0f ? Ink::Amber : Ink::Meter;
			list->AddRectFilled(at, ImVec2(toX(level), at.y + height), color, 3.0f);
			const float target = toX(INPUT_TARGET_PEAK_DB);
			list->AddLine(ImVec2(target, at.y - 3.0f), ImVec2(target, at.y + height + 3.0f), IM_COL32_WHITE, 2.0f);
			ImGui::Dummy(ImVec2(width, height));
		}

		// The Input popup: what the guitar sends, a measurement that sets the trim, and the trim by hand.
		void InputPopup()
		{
			PageState& state = State();
			const Snapshot& s = GetSnapshot();
			if (state.isInputWanted)
			{
				state.isInputWanted = false;
				ImGui::OpenPopup("##inputLevel");
			}
			PushPopupPadding();
			if (ImGui::BeginPopup("##inputLevel"))
			{
				const float width = 320.0f;
				ImGui::TextUnformatted("Input level");
				MutedCaption("Amp captures distort as much as your guitar drives them. The trim brings your hardest playing to the white line, the level the starter tones are made for.", width);
				ImGui::Dummy(ImVec2(0, 4));

				// Automatic (the default): the host listens to your playing and sets the trim itself.
				bool automatic = s.isInputAutomatic;
				if (ImGui::Checkbox("Automatic", &automatic)) SetInputAutomatic(automatic);
				Hint("Follows your playing: the trim is set from how hard you play, and keeps up if you change guitar or pickups. Measure or the slider turns it off.");
				if (automatic)
				{
					char text[96];
					if (s.hasAutoTrim) sprintf_s(text, "Set from %.0f s of your playing.", s.autoPlayingSeconds);
					else sprintf_s(text, "Listening: %.0f of 15 s of playing heard.", std::min(s.autoPlayingSeconds, 15.0f));
					MutedCaption(text, width);
				}
				ImGui::Dummy(ImVec2(0, 4));

				const float raw = RawInputPeak();
				ImGui::TextDisabled("Your guitar");
				InputLevelBar(raw, width);
				ImGui::TextDisabled("Into the amps");
				InputLevelBar(raw * std::pow(10.0f, s.inputTrimDb / 20.0f), width);
				ImGui::Dummy(ImVec2(0, 4));

				const ULONGLONG now = GetTickCount64();
				if (state.measureStartedAt != 0)
				{
					state.measuredPeak = std::max(state.measuredPeak, raw);
					const ULONGLONG elapsed = now - state.measureStartedAt;
					if (elapsed >= INPUT_MEASURE_MILLISECONDS)
					{
						state.measureStartedAt = 0;
						const float measured = DecibelsOf(state.measuredPeak);
						if (measured < INPUT_SILENCE_DB) Notice("No guitar was heard while measuring. Check the cable and play again.");
						else
						{
							SetInputTrim(INPUT_TARGET_PEAK_DB - measured);
							Notice("Input set to " + InputText(GetSnapshot().inputTrimDb).substr(6) + " from your playing.");
						}
					}
					else
					{
						char text[64];
						sprintf_s(text, "Play your hardest chords... %d", static_cast<int>((INPUT_MEASURE_MILLISECONDS - elapsed + 999) / 1000));
						ImGui::TextUnformatted(text);
					}
				}
				else if (Button("Measure", true, "Then play your hardest, loudest chords for 5 seconds."))
				{
					state.measureStartedAt = now;
					state.measuredPeak = 0.0f;
				}

				float trim = s.inputTrimDb;
				ImGui::SetNextItemWidth(width);
				if (ImGui::SliderFloat("##inputTrim", &trim, MIN_INPUT_TRIM_DB, MAX_INPUT_TRIM_DB, "Trim %+.1f dB")) SetInputTrim(trim);
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) SetInputTrim(0.0f);
				Hint("Double-click to set it back to 0 dB.");
				ImGui::EndPopup();
			}
			else state.measureStartedAt = 0;   // closed: a measurement in progress is dropped
			ImGui::PopStyleVar();
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

		// After Export my setup or Import a setup: what happened, and for an import the plugins this PC lacks. A modal of
		// its own (opened from the bar, like Save as), shown once; it waits until the page is drawn again after an import.
		void SetupResultPopup()
		{
			static ToneBackup::Result result;
			if (ToneBackup::TakeResult(result)) ImGui::OpenPopup("##setupResult");
			ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 18));
			if (ImGui::BeginPopupModal("##setupResult", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
			{
				ImGui::PushFont(GetFonts().title ? GetFonts().title : ImGui::GetFont());
				ImGui::TextUnformatted(result.title.c_str());
				ImGui::PopFont();
				Caption(result.text.c_str());
				if (!result.missing.empty())
				{
					ImGui::Spacing();
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Warn));
					ImGui::TextWrapped("Not on this PC. Install these plugins and the tones that use them play again:");
					ImGui::PopStyleColor();
					for (const std::string& line : result.missing) ImGui::BulletText("%s", line.c_str());
				}
				if (!result.moved.empty())
				{
					ImGui::Spacing();
					Caption("In another folder on this PC; your tones now use them from there:");
					for (const std::string& line : result.moved) ImGui::BulletText("%s", line.c_str());
				}
				ImGui::Spacing();
				if (Button("OK", true)) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			ImGui::PopStyleVar();
		}

		void FileMenu()
		{
			const Snapshot& s = GetSnapshot();
			SetupResultPopup();
			if (!ImGui::BeginMenu("File")) return;
			if (ImGui::MenuItem("New tone", nullptr, false, !s.chain.empty())) NewTone();
			if (ImGui::MenuItem("Save", nullptr, false, !s.currentPreset.empty())) SavePreset(s.currentPreset);
			if (ImGui::MenuItem("Save as...")) RequestSaveAs();
			ImGui::Separator();
			if (ImGui::MenuItem("Open amps folder")) ShellOpenAsync::Open(EngineFolder("NAM"));
			if (ImGui::MenuItem("Open cabinets folder")) ShellOpenAsync::Open(EngineFolder("Cabinets"));
			ImGui::Separator();
			// The whole setup in one file, to move to another PC (ToneBackup.hpp).
			if (ImGui::MenuItem("Export my setup...", nullptr, false, !ToneBackup::Busy())) ToneBackup::ExportSetup();
			Hint("Your tones, captures, cabinets and song tones here and on the VST rig, with their settings, in one file. Save it anywhere, a USB stick or a cloud folder.");
			if (ImGui::MenuItem("Import a setup...", nullptr, false, !ToneBackup::Busy())) ToneBackup::ImportSetup();
			Hint("Adds a setup you exported, on this PC or another. Files of the same name are replaced; nothing else is deleted. What you have now is backed up first. Tone designer closes for a moment.");
			ImGui::EndMenu();
		}

		// Edit: Undo and Redo (the host's history, juce::UndoManager, which names each step), the selected pedal, the board.
		void EditMenu()
		{
			const Snapshot& s = GetSnapshot();
			if (!ImGui::BeginMenu("Edit")) return;
			const std::string undo = s.undoLabel.empty() ? std::string("Undo") : "Undo " + s.undoLabel;
			const std::string redo = s.redoLabel.empty() ? std::string("Redo") : "Redo " + s.redoLabel;
			if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, !s.undoLabel.empty())) StompboxLink::Undo();
			if (ImGui::MenuItem(redo.c_str(), "Ctrl+Y", false, !s.redoLabel.empty())) StompboxLink::Redo();
			ImGui::Separator();
			const int index = IndexOfPedal(State().selectedPedal);
			ImGui::TextDisabled("%s", index >= 0 ? s.chain[index].kind.c_str() : "No pedal selected");
			if (index >= 0)
			{
				const Pedal pedal = s.chain[index];
				if (ImGui::MenuItem("Edit in its window")) OpenEditor(pedal.id);
				if (ImGui::MenuItem(pedal.isEnabled ? "Bypass" : "Turn on", "or its footswitch")) SetEnabled(pedal.id, !pedal.isEnabled);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
				if (ImGui::MenuItem("Remove")) RemovePedal(pedal.id);
				ImGui::PopStyleColor();
			}
			ImGui::Separator();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
			if (ImGui::MenuItem("Clear board", nullptr, false, !s.chain.empty())) SetChain({});
			ImGui::PopStyleColor();
			ImGui::EndMenu();
		}

		void ViewMenu()
		{
			if (!ImGui::BeginMenu("View")) return;
			PageState& state = State();
			if (ImGui::MenuItem("Browser", nullptr, state.isBrowserShown)) state.isBrowserShown = !state.isBrowserShown;
			if (ImGui::MenuItem("Inspector", nullptr, state.isInspectorShown)) state.isInspectorShown = !state.isInspectorShown;
			ToneDesignerUi::BoardSizeMenu();
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
				EditMenu();
				ViewMenu();
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

				// Left of it: the input trim, which opens the input calibration.
				const std::string inputText = InputText(s.inputTrimDb, s.isInputAutomatic);
				const float inputW = TextWidth(caption, inputText.c_str()) + 22.0f;
				const ImVec2 inputAt(origin.x + pillX - 32.0f - inputW, origin.y + y);
				ImGui::SetCursorScreenPos(inputAt);
				if (ImGui::InvisibleButton("##input", ImVec2(inputW, button))) State().isInputWanted = true;
				const bool isInputHovered = ImGui::IsItemHovered();
				Hint(s.isInputAutomatic ? "How hard your guitar drives the amps, set automatically from your playing. Click for details."
					: "How hard your guitar drives the amps. Click to measure it from your playing.");
				list->AddRectFilled(inputAt, ImVec2(inputAt.x + inputW, inputAt.y + button), isInputHovered ? Ink::Edge : Ink::Field, 6.0f);
				list->AddRect(inputAt, ImVec2(inputAt.x + inputW, inputAt.y + button), Ink::FieldEdge, 6.0f);
				Text(list, caption, ImVec2(inputAt.x + 11.0f, inputAt.y + (button - caption->FontSize) * 0.5f), Ink::Soft, inputText.c_str());

				// Middle: < tone name > Save, centred between the menus and the input button.
				const float saveW = TextWidth(ImGui::GetFont(), "Save") + 22.0f;
				const float left = menusEnd + 12.0f, right = inputAt.x - origin.x - 12.0f;
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
				InputPopup();
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

		const std::string& BrowsedSong();   // the Song tones pane's song (below)

		// The song the Song tones pane shows, and how many of its tones play a saved tone, for the library's Song tones line.
		std::string SongToneSummary(const std::string& songKey, const SongLibrary::Song* song)
		{
			if (songKey.empty()) return "Choose a song";
			const size_t assigned = SongToneSets::Assignments(songKey).size();
			const std::string title = song ? song->title + ": " : std::string();
			if (!song || song->tones.empty()) return title + std::to_string(assigned) + " assigned";
			return title + std::to_string(assigned) + " of " + std::to_string(song->tones.size()) + " assigned";
		}

		// The browser's Tones tab: search, the saved tones, and the chosen song's tones (a click opens the Songs tab).
		void ToneList(float width, float height)
		{
			const Snapshot& s = GetSnapshot();
			PageState& state = State();
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
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
			const std::string browsed = BrowsedSong();
			const SongLibrary::Song* song = browsed.empty() ? nullptr : library->Find(browsed);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
			ImGui::TextUnformatted(SongToneSummary(browsed, song).c_str());
			if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			if (ImGui::IsItemClicked()) State().column = SongsColumn;
			Hint(browsed.empty() ? "Click to choose a song (the Songs tab) and give each of its tones a saved tone."
				: "Click to see the song's tones (the Songs tab) and pick a saved tone for each.");
			ImGui::PopStyleColor();
			PushPopupPadding();
			if (ImGui::BeginPopupContextWindow("##libraryMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
			{
				ImGui::TextDisabled("Tones");
				ImGui::Separator();
				if (ImGui::MenuItem("New tone", nullptr, false, !s.chain.empty())) NewTone();
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

		// The Add menu, a gap's "Add here" and the board's right-click menu: every effect, then the amps and cabinets
		// found. `index` is where it goes: -1 its usual place, n before the pedal at n.
		void AddMenuItems(int index = -1)
		{
			for (const BrowserTab tab : { BrowserTab::Effects, BrowserTab::Amps, BrowserTab::Cabinets })
			{
				const std::vector<BrowserItem> items = BrowserItems(tab, "", 0);
				const char* label = tab == BrowserTab::Effects ? "Effects" : tab == BrowserTab::Amps ? "Amps" : "Cabinets";
				if (!ImGui::BeginMenu(label, !items.empty())) continue;
				for (const BrowserItem& item : items)
				{
					const std::string name = item.file.empty() ? item.kind : item.file;
					if (ImGui::MenuItem(name.c_str())) AddPedal(item, index);
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Browse pedals...")) State().column = PedalsColumn;
		}

		void PedalMenu(const Pedal& pedal)
		{
			PushPopupPadding();
			if (ImGui::BeginPopupContextItem("##pedalMenu"))
			{
				State().selectedPedal = pedal.id;
				ImGui::TextDisabled("%s", pedal.id.c_str());
				ImGui::Separator();
				if (ImGui::MenuItem("Edit in its window", "or double-click it")) OpenEditor(pedal.id);
				if (ImGui::MenuItem("Edit here", "the Inspector, under the board")) OpenEditorInOverlay(pedal.id);
				if (ImGui::MenuItem(pedal.isEnabled ? "Bypass" : "Turn on", "or its footswitch")) SetEnabled(pedal.id, !pedal.isEnabled);
				ImGui::Separator();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
				if (ImGui::MenuItem("Remove")) RemovePedal(pedal.id);
				ImGui::PopStyleColor();
				ImGui::EndPopup();
			}
			ImGui::PopStyleVar();
		}

		// The symbol in a pedal's middle (ToneDesignerUi::DrawSymbol): an amp's grille, a cabinet's speaker, an EQ's
		// sliders, a gate's or compressor's meter, else a waveform.
		ToneDesignerUi::Symbol SymbolFor(const std::string& kind)
		{
			if (IsAmpKind(kind)) return ToneDesignerUi::Symbol::Amp;
			if (kind == CABINET_KIND) return ToneDesignerUi::Symbol::Cabinet;
			if (kind == "EQ-7" || kind == "BEQ-7" || kind == "Tonestack") return ToneDesignerUi::Symbol::Eq;
			if (IsDynamicsKind(kind)) return ToneDesignerUi::Symbol::Dynamics;
			return ToneDesignerUi::Symbol::Wave;
		}

		// True the first time a drop is delivered, false if ImGui delivers the same drop again. It can: the
		// payload keeps one Delivery flag that every AcceptDragDropPayload call rewrites, and the board-space
		// target (submitted last) clears the gap's or Add tile's delivery, so the payload lives one more frame
		// and the same target delivers again, adding the pedal twice. A drop is known by the frame its source
		// last set it (DataFrameCount), which does not change once the mouse is released.
		bool IsFirstDelivery(const ImGuiPayload* payload)
		{
			static int deliveredFrame = -1;
			if (!payload->IsDelivery() || payload->DataFrameCount == deliveredFrame) return false;
			deliveredFrame = payload->DataFrameCount;
			return true;
		}

		// One pedal on the rack at `at`: click selects, the footswitch bypasses, double-click edits, drag moves it,
		// and a browser item dropped on it replaces it. Returns whether the mouse is on it.
		bool PedalTileAt(const Pedal& pedal, int index, ImVec2 at, ImDrawList* list, float sc)
		{
			PageState& state = State();
			const PedalDefinition* definition = FindDefinition(pedal.kind);
			const PedalLook& look = LookFor(pedal.kind);
			ImGui::PushID(pedal.id.c_str());
			ImGui::SetCursorScreenPos(at);
			const bool isPressed = ImGui::InvisibleButton("##pedal", ImVec2(ToneDesignerUi::PedalWidth(look, sc), ToneDesignerUi::PedalHeight(sc)));
			const bool isHovered = ImGui::IsItemHovered();
			const bool isOnSwitch = ToneDesignerUi::IsOnFootswitch(at, look, ImGui::GetIO().MousePos, sc);
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
				const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(NEW_PAYLOAD);
				if (payload && IsFirstDelivery(payload))
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
			tile.symbol = SymbolFor(pedal.kind);
			tile.isPlaying = pedal.isEnabled;
			tile.isSelected = state.selectedPedal == pedal.id;
			tile.isHovered = isHovered;
			tile.isOnSwitch = isOnSwitch;
			ToneDesignerUi::DrawPedalTile(list, at, tile, sc);
			if (IsPedalLoading(pedal, definition)) ToneDesignerUi::DrawTileBusy(list, at, ToneDesignerUi::PedalWidth(look, sc), "Loading", sc);
			ImGui::PopID();
			return isHovered;
		}

		// The last item takes a browser item dropped on it, adding it at `index` (-1: its usual place).
		void AcceptNewPedal(int index)
		{
			if (!ImGui::BeginDragDropTarget()) return;
			const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(NEW_PAYLOAD);
			if (payload && IsFirstDelivery(payload))
			{
				const auto& data = *static_cast<const NewPedalPayload*>(payload->Data);
				AddPedal({ data.kind, data.file }, index);
			}
			ImGui::EndDragDropTarget();
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
					if (IsFirstDelivery(payload)) MovePedal(from, g > from ? g - 1 : g);
				}
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(NEW_PAYLOAD, peek))
				{
					list->AddLine(ImVec2(gx, gap.Min.y), ImVec2(gx, gap.Max.y), Ink::Outline, 4.0f);
					if (IsFirstDelivery(payload))
					{
						const auto& data = *static_cast<const NewPedalPayload*>(payload->Data);
						AddPedal({ data.kind, data.file }, g);
					}
				}
				ImGui::EndDragDropTarget();
			}
		}

		// The one line under the rack: a notice for a few seconds, else the load's warning, else why an empty board is
		// quiet; nullptr when there is nothing to say (the how-to lives in the tooltips). `color` gets its ink.
		const char* BoardLine(const Snapshot& s, bool isLate, ImU32* color = nullptr)
		{
			const PageState& state = State();
			if (!state.notice.empty() && GetTickCount64() - state.noticeAt < NOTICE_MILLISECONDS)
			{
				if (color) *color = Ink::Dim;
				return state.notice.c_str();
			}
			if (const char* warning = LoadWarning(s, isLate))
			{
				if (color) *color = Ink::Amber;
				return warning;
			}
			if (color) *color = Ink::Dim;
			if (s.chain.empty()) return "The board is empty, so you hear your clean guitar. Drag an amp from the Pedals tab, then a cabinet after it.";
			return nullptr;
		}

		// The rack's items in order, each one's width at scale `sc`: In, the pedals, Add, Out.
		std::vector<float> RackWidths(const std::vector<Pedal>& chain, float sc)
		{
			std::vector<float> widths{ std::floor(ToneDesignerUi::JACK_WIDTH * sc) };
			for (const Pedal& pedal : chain) widths.push_back(ToneDesignerUi::PedalWidth(LookFor(pedal.kind), sc));
			widths.push_back(std::floor(ToneDesignerUi::ADD_TILE_WIDTH * sc));
			widths.push_back(std::floor(ToneDesignerUi::JACK_WIDTH * sc));
			return widths;
		}

		// The rack's height on a board `boardWidth` wide: as many rows as the board wraps into (Panel sizes the strip by it).
		float RackHeight(float boardWidth)
		{
			const float sc = ToneDesignerUi::BoardScale();
			return ToneDesignerUi::RackLayout(RackWidths(GetSnapshot().chain, sc), ToneDesignerUi::RackInnerWidth(boardWidth, true), sc).height;
		}

		// The rack: In, the pedals joined by cables, Add, Out, with the meters at its two ends. Wraps into rows when the
		// board is longer than the panel is wide (RackLayout); the pedals' size is View, Board size (Ctrl+wheel).
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
			PageState& state = State();
			ImU32 hintColor = Ink::Dim;
			const char* boardHint = BoardLine(s, isLate, &hintColor);
			ImFont* caption = CaptionFont();
			const bool shouldShowHint = boardHint != nullptr;
			const float hintH = shouldShowHint ? caption->FontSize + ImGui::GetStyle().ItemSpacing.y + 4.0f : 0.0f;
			const float rackH = std::max(1.0f, ImGui::GetContentRegionAvail().y - hintH);
			const float sc = ToneDesignerUi::BoardScale();
			const float pedalH = ToneDesignerUi::PedalHeight(sc);
			const ImVec2 board = ImGui::GetCursorScreenPos();
			ImDrawList* outer = ImGui::GetWindowDrawList();
			ToneDesignerUi::DrawRack(outer, board, boardW, rackH);

			const float meterMidY = board.y + 12.0f + pedalH * 0.5f;
			MeterColumn(outer, ImVec2(board.x, meterMidY), METER_COLUMN_WIDTH, "IN", vst.inputPeak);
			MeterColumn(outer, ImVec2(board.x + boardW - METER_COLUMN_WIDTH, meterMidY), METER_COLUMN_WIDTH, "OUT", vst.outputPeak);
			const float rackW = boardW - 2.0f * METER_COLUMN_WIDTH;
			ImGui::SetCursorScreenPos(ImVec2(board.x + METER_COLUMN_WIDTH, board.y));

			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ToneDesignerUi::RACK_INSET, 12));
			ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
			ImGui::BeginChild("##stompboxRack", ImVec2(rackW, rackH), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 rackStart = ImGui::GetCursorScreenPos();
			const std::vector<float> widths = RackWidths(chain, sc);
			const ToneDesignerUi::RackPlacement placement = ToneDesignerUi::RackLayout(widths, ToneDesignerUi::RackInnerWidth(width, true), sc);
			const auto itemAt = [&](size_t item) { return ImVec2(rackStart.x + placement.at[item].x, rackStart.y + placement.at[item].y); };

			ToneDesignerUi::DrawJack(list, itemAt(0), "In", sc);
			// The gaps before each pedal and before Add: drop targets that insert there (GapDropTargets).
			std::vector<ImRect> gaps;
			const auto gapBefore = [&](ImVec2 at) { gaps.emplace_back(ImVec2(at.x - 26.0f * sc, at.y), ImVec2(at.x + 6.0f * sc, at.y + pedalH)); };
			bool isOverTile = false;
			for (int i = 0; i < count; ++i)
			{
				const size_t item = static_cast<size_t>(i) + 1;
				ToneDesignerUi::DrawCable(list, rackStart, placement, widths, item, sc);
				gapBefore(itemAt(item));
				isOverTile |= PedalTileAt(chain[i], i, itemAt(item), list, sc);
			}

			// Add: a dashed tile, opening the list of pedals.
			const size_t addItem = static_cast<size_t>(count) + 1;
			ToneDesignerUi::DrawCable(list, rackStart, placement, widths, addItem, sc);
			const ImVec2 addAt = itemAt(addItem);
			gapBefore(addAt);
			ImGui::SetCursorScreenPos(addAt);
			if (ImGui::InvisibleButton("##addPedal", ImVec2(widths[addItem], pedalH))) ImGui::OpenPopup("##addMenu");
			const bool isAddHovered = ImGui::IsItemHovered();
			Hint("Add a pedal at its usual place. The Pedals tab has search; drag from it onto the board to put a pedal anywhere.");
			AcceptNewPedal(count);
			ToneDesignerUi::DrawAddTile(list, addAt, isAddHovered, sc);
			GapDropTargets(gaps, list);
			ToneDesignerUi::DrawCable(list, rackStart, placement, widths, addItem + 1, sc);
			ToneDesignerUi::DrawJack(list, itemAt(addItem + 1), "Out", sc);

			// The empty part of the board: one item behind the pedals (submitted last, so the pedals keep the hover),
			// for its right-click menu.
			ImGui::SetCursorScreenPos(rackStart);
			ImGui::InvisibleButton("##boardSpace", ImVec2(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(placement.height, 1.0f)));
			// A browser item dropped anywhere else on the board goes in at its usual place. ImGui delivers a drop to
			// the smallest target under the mouse, so the pedals, the gaps and the Add tile still take theirs.
			AcceptNewPedal(-1);
			// Not over a pedal: ImGui 1.85's IsItemHovered is true under the pedals too, and this menu would replace theirs.
			// On the cable between two pedals (a gap) it is that gap's menu: Add here puts a pedal in at that place.
			if (!isOverTile && !isAddHovered && ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
			{
				state.gapMenu = -1;
				const ImVec2 mouse = ImGui::GetIO().MousePos;
				for (int g = 0; g < static_cast<int>(gaps.size()) && g < count; ++g)
				{
					if (gaps[g].Contains(mouse)) state.gapMenu = g;
				}
				ImGui::OpenPopup(state.gapMenu >= 0 ? "##gapMenu" : "##boardMenu");
			}
			if (state.gapMenu >= 0 && state.gapMenu < static_cast<int>(gaps.size()) && ImGui::IsPopupOpen("##gapMenu"))
			{
				const float gx = gaps[state.gapMenu].Min.x + 14.0f;
				list->AddLine(ImVec2(gx, gaps[state.gapMenu].Min.y), ImVec2(gx, gaps[state.gapMenu].Max.y), Ink::Outline, 4.0f);
			}

			// The board wraps instead of scrolling, so the wheel is free: Ctrl+wheel changes the pedals' size.
			ToneDesignerUi::BoardSizeWheel();

			PushPopupPadding();
			if (ImGui::BeginPopup("##gapMenu"))
			{
				if (ImGui::BeginMenu("Add here"))
				{
					AddMenuItems(state.gapMenu);
					ImGui::EndMenu();
				}
				ImGui::Separator();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
				if (ImGui::MenuItem("Clear board", nullptr, false, count > 0)) SetChain({});
				ImGui::PopStyleColor();
				ImGui::EndPopup();
			}
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

			if (shouldShowHint)
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

		// What a capture or cabinet sounds like, as a pill before its description; null for effects and the player's
		// own files.
		const PillLook* SoundPill(const BrowserItem& item, const ToneCatalog::FileInfo* info)
		{
			using ToneCatalog::Character;
			static const PillLook clean = TintPill("Clean", Ink::Good);
			static const PillLook crunch = TintPill("Crunch", Ink::Amber);
			static const PillLook highGain = TintPill("High gain", Ink::Bad);
			static const PillLook drive = TintPill("Drive", Ink::TagText);
			static const PillLook boost = TintPill("Boost", Ink::TagText);
			static const PillLook bass = TintPill("Bass", Ink::Outline);
			static const PillLook guitar = TintPill("Guitar", Ink::Soft);
			if (!info) return nullptr;
			if (info->isBass) return &bass;
			if (item.kind == CABINET_KIND) return &guitar;
			switch (info->character)
			{
			case Character::Clean: return &clean;
			case Character::Crunch: return &crunch;
			case Character::HighGain: return &highGain;
			case Character::Drive: return &drive;
			case Character::Boost: return &boost;
			default: return nullptr;
			}
		}

		// The browser's rows, styled as the VST rig's plugin list: the name over what it sounds like, a green dot when
		// it is on the board.
		void BrowserList(const std::vector<BrowserItem>& items, float width, float height)
		{
			PageState& state = State();
			ImFont* caption = CaptionFont();
			const float rowH = ImGui::GetFontSize() + caption->FontSize + 14.0f;
			if (items.empty())
			{
				ImGui::BeginChild("##browserList", ImVec2(width, height), false);
				MutedCaption(state.tab == BrowserTab::Effects ? "Nothing matches."
					: "None yet. Put files in the folder (the gear button, Open folder), then Rescan.", width);
				ImGui::EndChild();
				return;
			}
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
			ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
			ImGui::BeginChild("##browserList", ImVec2(width, height), false);
			ImDrawList* list = ImGui::GetWindowDrawList();
			const float rowW = ImGui::GetContentRegionAvail().x;
			for (int i = 0; i < static_cast<int>(items.size()); ++i)
			{
				const BrowserItem& item = items[i];
				const PedalDefinition* definition = FindDefinition(item.kind);
				const std::string name = item.file.empty() ? item.kind : item.file;
				const ToneCatalog::FileInfo* info = CatalogInfo(item);
				const std::string detail = info ? info->description : !item.file.empty() ? std::string("Your file") : definition ? definition->description : std::string();
				ImGui::PushID(i);
				const ImVec2 at = ImGui::GetCursorScreenPos();
				if (ImGui::InvisibleButton("##item", ImVec2(rowW, rowH))) state.selectedItem = i;
				const bool isHovered = ImGui::IsItemHovered();
				// No double-click to add: a few double-clicks while browsing would stack amps at the end of the board.
				if (isHovered) Hint(((detail.empty() ? std::string() : detail + "\n\n") + "Drag it onto the board: between two pedals to put it there, onto a pedal to replace it. Or right-click it to add it at its usual place.").c_str());
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
					if (ImGui::MenuItem("Add to the board")) AddPedal(item, -1);
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
				const bool isInChain = IsItemInChain(item);
				const float right = end.x - (isInChain ? 16.0f : 4.0f);
				if (isInChain) list->AddCircleFilled(ImVec2(end.x - 9.0f, at.y + rowH * 0.5f), 3.5f, Ink::Good, 12);
				list->PushClipRect(at, ImVec2(right, end.y), true);
				list->AddText(ImVec2(at.x + 28.0f, at.y + 5.0f), isSelected ? IM_COL32_WHITE : Ink::Text, name.c_str());
				float detailStart = at.x + 28.0f;
				const float lineY = at.y + 7.0f + ImGui::GetFontSize();
				if (const PillLook* sound = SoundPill(item, info))
				{
					const float pillH = caption->FontSize + 4.0f;
					detailStart += DrawPill(list, caption, ImVec2(detailStart, lineY - 2.0f), pillH, *sound) + 6.0f;
				}
				Text(list, caption, ImVec2(detailStart, lineY), Ink::Muted, detail.c_str());
				list->PopClipRect();
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::PopStyleVar(2);
		}

		// The browser's Pedals tab: Effects, Amps, Cabinets as chips, search, what they sound like, the list, and the
		// selected item's buttons. The gear button holds the folders and Rescan.
		void PedalList(float width, float height)
		{
			PageState& state = State();
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
			ImGui::BeginChild("##pedalColumn", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::PopStyleVar();
			const float inner = ImGui::GetContentRegionAvail().x;
			const float left = ImGui::GetCursorPosX();
			const float gear = ImGui::GetFrameHeight();
			ImGui::SetNextItemWidth(inner - gear - 6.0f);
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
			const bool isSearchChanged = ImGui::InputTextWithHint("##browserSearch", state.tab == BrowserTab::Effects ? "Search effects" : state.tab == BrowserTab::Amps ? "Search amps" : "Search cabinets",
				state.search, sizeof(state.search));
			if (isSearchChanged) state.selectedItem = -1;
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
			ImGui::SameLine(0.0f, 6.0f);
			if (IconSquare("##pedalFolders", Glyph::Settings, false, "The amps and cabinets folders, and Rescan.", gear)) ImGui::OpenPopup("##pedalFoldersMenu");
			PushPopupPadding();
			if (ImGui::BeginPopup("##pedalFoldersMenu"))
			{
				if (ImGui::MenuItem("Open amps folder")) ShellOpenAsync::Open(EngineFolder("NAM"));
				Hint("NAM models (.nam) put here show under Amps.");
				if (ImGui::MenuItem("Open cabinets folder")) ShellOpenAsync::Open(EngineFolder("Cabinets"));
				Hint("Cabinet IRs (.wav) put here show under Cabinets.");
				ImGui::Separator();
				if (ImGui::MenuItem("Rescan")) Rescan();
				Hint("Lists files added to the folders, without restarting the host.");
				ImGui::EndPopup();
			}
			ImGui::PopStyleVar();

			static const char* const chips[3] = { "Effects", "Amps", "Cabinets" };
			for (int c = 0; c < 3; ++c)
			{
				if (c > 0) ImGui::SameLine(0.0f, 6.0f);
				if (Chip(chips[c], static_cast<int>(state.tab) == c) && static_cast<int>(state.tab) != c)
				{
					state.tab = static_cast<BrowserTab>(c);
					state.soundFilter = 0;
					state.selectedItem = -1;
				}
			}

			// Amps and Cabinets: narrow the list by what it sounds like (wrapped to the column).
			if (state.tab != BrowserTab::Effects)
			{
				const bool isAmps = state.tab == BrowserTab::Amps;
				const char* const* filters = isAmps ? AMP_FILTERS : CABINET_FILTERS;
				const int count = isAmps ? static_cast<int>(std::size(AMP_FILTERS)) : static_cast<int>(std::size(CABINET_FILTERS));
				for (int f = 0; f < count; ++f)
				{
					if (f > 0)
					{
						ImGui::SameLine(0.0f, 6.0f);
						if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(filters[f]).x + 20.0f > left + inner) ImGui::NewLine();
					}
					if (Chip(filters[f], state.soundFilter == f) && state.soundFilter != f)
					{
						state.soundFilter = f;
						state.selectedItem = -1;
					}
				}
			}

			const std::vector<BrowserItem> items = BrowserItems(state.tab, state.search, state.soundFilter);
			if (state.selectedItem < 0 || state.selectedItem >= static_cast<int>(items.size())) state.selectedItem = -1;
			const float footH = ImGui::GetFontSize() + 14.0f + 2.0f * ImGui::GetStyle().ItemSpacing.y + 8.0f;
			ImGui::Dummy(ImVec2(0, 2));
			BrowserList(items, inner, std::max(40.0f, ImGui::GetContentRegionAvail().y - footH));

			ImGui::Dummy(ImVec2(0, 2));
			const bool hasItem = state.selectedItem >= 0 && state.selectedItem < static_cast<int>(items.size());
			ImGui::BeginDisabled(!hasItem);
			if (Button("Add", true, "Adds it at its usual place on the board.") && hasItem) AddPedal(items[state.selectedItem], -1);
			ImGui::SameLine();
			if (Button("Replace selected", false, "Puts it in place of the pedal selected on the board.") && hasItem) ReplacePedal(state.selectedPedal, items[state.selectedItem]);
			ImGui::EndDisabled();
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
				const std::string preview = current.empty() ? std::string("None") : current;
				ImGui::SetNextItemWidth(controlWidth);
				PushPopupPadding();
				if (ImGui::BeginCombo("##value", preview.c_str()))
				{
					for (const std::string& choice : parameter.choices)
					{
						if (ImGui::Selectable(choice.c_str(), choice == current) && choice != current)
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

		// The Inspector under the board: the selected pedal's controls inside the overlay (Edit in its window is the
		// host's own pedal window, the other way). Knobs for its continuous parameters, as many to a row as fit; then
		// its switches, choices and files as rows; an EQ's sliders side by side; the advanced ones folded away. The
		// < > beside its name steps through the files of its kind (captures for an amp, IRs for a cabinet). Each change
		// is a Stompbox command the host keeps in its history (Edit, Undo), a knob drag as one step.
		void StompInspector(float width, float height)
		{
			const Snapshot& s = GetSnapshot();
			PageState& state = State();
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::ColorConvertU32ToFloat4(Ink::RowHover));
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImGui::ColorConvertU32ToFloat4(Ink::RowSelected));
			ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::ColorConvertU32ToFloat4(Ink::Blue));
			ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImGui::ColorConvertU32ToFloat4(Ink::AddText));
			ImGui::BeginChild("##stompboxInspector", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::PopStyleVar();
			const float inner = ImGui::GetContentRegionAvail().x;
			const int index = IndexOfPedal(state.selectedPedal);
			if (index < 0)
			{
				ToneDesignerUi::InspectorEmpty(s.chain.empty() ? "Add a pedal, then click it on the board to edit it here." : "Click a pedal on the board to edit it here.",
					inner, ImGui::GetContentRegionAvail().y);
				ImGui::EndChild();
				ImGui::PopStyleColor(5);
				return;
			}
			const Pedal pedal = s.chain[index];   // a copy: an edit can set the chain at once (StompboxLink)
			const PedalDefinition* definition = FindDefinition(pedal.kind);
			ImFont* caption = CaptionFont();

			// Header, left: the pedal over its file (or what it does). Right: < > through its files, Bypass, its window.
			const ImVec2 head = ImGui::GetCursorScreenPos();
			const float button = ImGui::GetFrameHeight();
			ImGui::PushFont(GetFonts().title ? GetFonts().title : ImGui::GetFont());
			ImGui::TextUnformatted(pedal.kind.c_str());
			ImGui::PopFont();
			const std::string file = PedalFileLabel(pedal, definition);
			const std::string sub = !file.empty() ? file : definition ? definition->description : std::string();
			ImGui::PushFont(caption);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(120.0f, inner * 0.45f));
			ImGui::TextUnformatted(sub.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::PopFont();
			const float headBottom = ImGui::GetCursorScreenPos().y;

			const char* openLabel = "Edit in its window";
			float x = head.x + inner - ToneDesignerUi::BarButtonWidth(openLabel);
			ImGui::SetCursorScreenPos(ImVec2(x, head.y));
			if (ToneDesignerUi::BarButton("##openPedalWindow", openLabel, button, true, "The pedal's own window, over the game while this overlay is open (or double-click the pedal)."))
				OpenEditor(pedal.id);
			x -= ToneDesignerUi::BarToggleWidth("Bypass") + 16.0f;
			ImGui::SetCursorScreenPos(ImVec2(x, head.y));
			if (ToneDesignerUi::BarToggle("##stompBypass", "Bypass", !pedal.isEnabled, button, true, pedal.isEnabled ? "Skip this pedal (or click its footswitch)." : "Bypassed: click to turn it on."))
				SetEnabled(pedal.id, !pedal.isEnabled);
			const ParameterDefinition* fileParameter = FileParameter(definition);
			if (fileParameter && fileParameter->choices.size() > 1)
			{
				const auto stored = pedal.values.find(fileParameter->name);
				const std::string current = stored == pedal.values.end() ? std::string() : stored->second;
				const std::vector<std::string>& choices = fileParameter->choices;
				const int count = static_cast<int>(choices.size());
				int at = -1;
				for (int i = 0; i < count; ++i) if (choices[i] == current) at = i;
				auto step = [&](int delta)
				{
					const int next = at < 0 ? (delta > 0 ? 0 : count - 1) : ((at + delta) % count + count) % count;
					SetPedalFile(pedal.id, definition, choices[next]);
					state.loadingPedals.push_back(pedal.id);
				};
				x -= 2.0f * button + 6.0f + 16.0f;
				ImGui::SetCursorScreenPos(ImVec2(x, head.y));
				if (IconSquare("##filePrev", Glyph::ChevronLeft, false, IsAmpKind(pedal.kind) ? "The previous capture." : "The previous file of this kind.", button)) step(-1);
				ImGui::SameLine(0.0f, 6.0f);
				if (IconSquare("##fileNext", Glyph::ChevronRight, false, IsAmpKind(pedal.kind) ? "The next capture." : "The next file of this kind.", button)) step(+1);
			}
			ImGui::SetCursorScreenPos(ImVec2(head.x, std::max(headBottom, head.y + button) + 8.0f));
			ImGui::GetWindowDrawList()->AddLine(ImGui::GetCursorScreenPos(), ImVec2(head.x + inner, ImGui::GetCursorScreenPos().y), Ink::Edge);
			ImGui::Dummy(ImVec2(0, 6));

			ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
			ImGui::BeginChild("##stompboxControls", ImVec2(inner, std::max(40.0f, ImGui::GetContentRegionAvail().y)), false);
			if (!definition) MutedCaption("The engine does not describe this pedal.", inner);
			else
			{
				std::vector<const ParameterDefinition*> knobs, rows, sliders, advanced;
				for (const ParameterDefinition& parameter : definition->parameters)
				{
					if (parameter.isOutput || parameter.type == ParameterType::Power) continue;
					if (parameter.type == ParameterType::VerticalSlider) sliders.push_back(&parameter);
					else if (parameter.isAdvanced) advanced.push_back(&parameter);
					else if (parameter.type == ParameterType::Knob) knobs.push_back(&parameter);
					else rows.push_back(&parameter);
				}
				// While a knob is dragged its own position is shown, not the host's last program (a few frames behind).
				static std::string dragKey;
				static float dragPosition = 0.0f;
				const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / ToneDesignerUi::KNOB_CELL_WIDTH));
				for (size_t k = 0; k < knobs.size(); ++k)
				{
					const ParameterDefinition& parameter = *knobs[k];
					if (k % columns != 0) ImGui::SameLine(0.0f, 0.0f);
					ImGui::PushID(parameter.name.c_str());
					const float span = parameter.maximum - parameter.minimum;
					const float power = parameter.rangePower > 0.0f ? parameter.rangePower : 1.0f;
					const std::string key = pedal.id + "/" + parameter.name;
					float position = span > 0.0f ? std::pow(std::clamp((CurrentValue(pedal, parameter) - parameter.minimum) / span, 0.0f, 1.0f), 1.0f / power) : 0.0f;
					if (dragKey == key) position = dragPosition;
					const float value = parameter.minimum + span * std::pow(position, power);
					const std::string text = FormatValue(parameter, value);
					const std::string hint = (parameter.description.empty() ? parameter.name : parameter.description) + ". Drag up or down (Shift for fine steps); double-click for its default.";
					const ToneDesignerUi::KnobResult knob = ToneDesignerUi::Knob("##knob", parameter.name.c_str(), text.c_str(), &position, hint.c_str());
					if (knob.began) { dragKey = key; dragPosition = position; }
					if (knob.changed)
					{
						dragPosition = position;
						SetParameter(pedal.id, parameter.name, NumberText(parameter.minimum + span * std::pow(position, power)));
					}
					if (knob.ended) dragKey.clear();
					if (knob.reset) SetParameter(pedal.id, parameter.name, NumberText(parameter.defaultValue));
					ImGui::PopID();
				}
				if (!knobs.empty()) ImGui::Dummy(ImVec2(0, 4));
				const float labelWidth = std::min(140.0f, inner * 0.32f);
				const float controlWidth = std::min(320.0f, inner - labelWidth - 8.0f);
				for (const ParameterDefinition* parameter : rows) ParameterControl(pedal, *parameter, labelWidth, controlWidth);
				if (!sliders.empty()) SliderBank(pedal, sliders);
				if (!advanced.empty() && ImGui::CollapsingHeader("Advanced"))
				{
					for (const ParameterDefinition* parameter : advanced) ParameterControl(pedal, *parameter, labelWidth, controlWidth);
				}
			}
			ImGui::EndChild();
			ImGui::PopStyleVar();
			ImGui::EndChild();
			ImGui::PopStyleColor(5);
		}

		// ---- Song tones -------------------------------------------------------------------------------------

		// The Song tones pane's own state: the song it shows (browsed: the game's song by default, any other from the
		// song list), the list's search and its matches.
		struct SongPaneState
		{
			std::string browsed;                // song key shown; empty: none chosen
			std::string lastGameSong;           // the game's song when last drawn: a new one is shown at once
			char search[64] = {};
			std::string matchedSearch;          // what `matches` was built for
			unsigned parts = 0;                 // arrangement chips: Lead 1, Rhythm 2, Bass 4 (any lit one); 0 = every song
			unsigned matchedParts = 0;
			// Held, not just its address: a new library could be allocated where a freed one was, and the old
			// indexes would then be kept against a shorter list.
			std::shared_ptr<const SongLibrary::Library> matchedLibrary;
			std::vector<size_t> matches;        // indexes into the library's songs
			bool isExportConfirmWanted = false;
		};

		SongPaneState& SongPane()
		{
			static SongPaneState state;
			return state;
		}

		// The song the pane shows. The game choosing a song shows it; the player can browse away from it.
		const std::string& BrowsedSong()
		{
			SongPaneState& pane = SongPane();
			const std::string& game = GetSnapshot().songKey;
			if (game != pane.lastGameSong)
			{
				pane.lastGameSong = game;
				if (!game.empty()) pane.browsed = game;
			}
			return pane.browsed;
		}

		bool SameSong(const std::string& a, const std::string& b) { return !a.empty() && _stricmp(a.c_str(), b.c_str()) == 0; }

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

		// Lead, Rhythm and Bass as three small chips, lit when the song has that arrangement.
		void ArrangementChips(ImDrawList* list, ImVec2 rightTop, float rowH, const std::string& arrangements)
		{
			static const char* const letters[3] = { "L", "R", "B" };
			static const char* const names[3] = { "Lead", "Rhythm", "Bass" };
			ImFont* caption = CaptionFont();
			const float size = caption->FontSize + 6.0f;
			float x = rightTop.x - 3.0f * (size + 3.0f);
			const float y = rightTop.y + (rowH - size) * 0.5f;
			for (int i = 0; i < 3; ++i)
			{
				const bool has = ContainsText(arrangements, names[i]);
				const ImVec2 a(x, y), b(x + size, y + size);
				list->AddRectFilled(a, b, has ? Ink::Edge : Ink::Field, 4.0f);
				list->AddRect(a, b, Ink::FieldEdge, 4.0f);
				Text(list, caption, ImVec2(x + (size - TextWidth(caption, letters[i])) * 0.5f, y + 3.0f), has ? Ink::Text : Ink::Edge, letters[i]);
				x += size + 3.0f;
			}
		}

		// Left third of the pane: search, the way back to the game's song, and every song in the song list.
		void SongBrowser(float width, float height)
		{
			const Snapshot& s = GetSnapshot();
			SongPaneState& pane = SongPane();
			const auto library = SongLibrary::Get();
			ImGui::BeginChild("##songBrowser", ImVec2(width, height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			const float rowW = ImGui::GetContentRegionAvail().x;
			ImGui::SetNextItemWidth(rowW);
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
			ImGui::InputTextWithHint("##songSearch", "Search songs, artists or albums", pane.search, sizeof(pane.search));
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
			static const char* const parts[3] = { "Lead", "Rhythm", "Bass" };
			for (int part = 0; part < 3; ++part)
			{
				if (part > 0) ImGui::SameLine(0.0f, 6.0f);
				if (Chip(parts[part], (pane.parts & (1u << part)) != 0)) pane.parts ^= 1u << part;
			}
			Hint("Only songs with that arrangement. Light several for any of them; none lit shows every song.");
			MutedCaption("Assign tones without starting the song", rowW);

			ImFont* caption = CaptionFont();
			if (!s.songKey.empty())
			{
				const SongLibrary::Song* game = library->Find(s.songKey);
				const std::string text = "Now playing: " + (game ? game->title : s.songKey);
				const float pillH = caption->FontSize + 8.0f;
				const ImVec2 at = ImGui::GetCursorScreenPos();
				const float pillW = std::min(ToneDesignerUi::PillWidth(caption, text.c_str(), true), rowW);
				if (ImGui::InvisibleButton("##nowPlaying", ImVec2(pillW, pillH))) pane.browsed = s.songKey;
				if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				Hint("Show the song chosen in the game.");
				ImDrawList* list = ImGui::GetWindowDrawList();
				list->PushClipRect(at, ImVec2(at.x + rowW, at.y + pillH), true);
				DrawPill(list, caption, at, pillH, TintPill(text.c_str(), Ink::Good), Ink::Good);
				list->PopClipRect();
			}

			if (pane.matchedLibrary != library || pane.matchedSearch != pane.search || pane.matchedParts != pane.parts)
			{
				pane.matchedLibrary = library;
				pane.matchedSearch = pane.search;
				pane.matchedParts = pane.parts;
				pane.matches.clear();
				for (size_t i = 0; i < library->songs.size(); ++i)
				{
					const SongLibrary::Song& song = library->songs[i];
					bool hasPart = pane.parts == 0;
					for (int part = 0; part < 3 && !hasPart; ++part) hasPart = (pane.parts & (1u << part)) && ContainsText(song.arrangements, parts[part]);
					if (hasPart && (ContainsText(song.title, pane.search) || ContainsText(song.artist, pane.search) || ContainsText(song.album, pane.search))) pane.matches.push_back(i);
				}
			}
			ImGui::Dummy(ImVec2(0, 2));
			const float listH = std::max(ImGui::GetContentRegionAvail().y, 40.0f);
			if (pane.matches.empty())
			{
				MutedCaption(SongLibrary::GetState() == SongLibrary::State::Reading && library->songs.empty() ? "Reading your songs..."
					: library->songs.empty() ? "Your songs could not be read: RSMods.exe needs to be in the game's RSMods folder."
					: "Nothing matches.", rowW);
				ImGui::EndChild();
				return;
			}
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
			ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
			ImGui::BeginChild("##songList", ImVec2(rowW, listH), false);
			ImDrawList* list = ImGui::GetWindowDrawList();
			const float innerW = ImGui::GetContentRegionAvail().x;
			const float rowH = ImGui::GetFontSize() + caption->FontSize + 10.0f;
			const float chipsW = 3.0f * (caption->FontSize + 9.0f) + 8.0f;
			ImGuiListClipper clipper;   // thousands of songs: only the visible rows are drawn
			clipper.Begin(static_cast<int>(pane.matches.size()), rowH);
			while (clipper.Step())
			{
				for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
				{
					const SongLibrary::Song& song = library->songs[pane.matches[static_cast<size_t>(r)]];
					ImGui::PushID(song.key.c_str());
					const ImVec2 at = ImGui::GetCursorScreenPos();
					if (ImGui::InvisibleButton("##song", ImVec2(innerW, rowH))) pane.browsed = song.key;
					const bool isHovered = ImGui::IsItemHovered();
					if (isHovered) Hint((song.album + (song.year > 0 ? " (" + std::to_string(song.year) + ")" : std::string()) + "\n" + song.arrangements + ", in " + song.file).c_str());
					const ImVec2 end(at.x + innerW, at.y + rowH);
					ToneDesignerUi::RowBackground(list, at, end, SameSong(song.key, pane.browsed), isHovered);
					float x = at.x + 8.0f;
					if (SameSong(song.key, s.songKey))
					{
						list->AddCircleFilled(ImVec2(x + 3.0f, at.y + rowH * 0.5f), 3.5f, Ink::Good, 12);
						x += 12.0f;
					}
					list->PushClipRect(at, ImVec2(end.x - chipsW, end.y), true);
					list->AddText(ImVec2(x, at.y + 4.0f), IM_COL32_WHITE, song.title.c_str());
					Text(list, caption, ImVec2(x, at.y + 5.0f + ImGui::GetFontSize()), Ink::Muted, song.artist.c_str());
					list->PopClipRect();
					ArrangementChips(list, ImVec2(end.x - 6.0f, at.y), rowH, song.arrangements);
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
			ImGui::PopStyleVar(2);
			ImGui::EndChild();
		}

		// A small dark button for the banner (Use mine instead, Use the song's tones).
		bool BannerButton(const char* label)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(Ink::Tag));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(Ink::Blue));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 3));
			const bool pressed = ImGui::Button(label);
			ImGui::PopStyleVar();
			ImGui::PopStyleColor(2);
			return pressed;
		}

		float BannerButtonWidth(const char* label) { return ImGui::CalcTextSize(label).x + 20.0f; }

		// Whether the song carries its own tones, and what the player can do about it.
		void SongSetBanner(const std::string& songKey, const SongToneSets::View& view, float width)
		{
			if (view.isImporting)
			{
				MutedCaption("Reading this song's tones...", width);
				return;
			}
			const std::string by = view.author.empty() ? std::string() : ", by " + view.author;
			std::string text;
			ImU32 hue = Ink::Good;
			const char* action = nullptr;   // Use mine instead / Use the song's tones
			switch (view.state)
			{
			case SongToneSets::SetState::Ready:
				if (view.isExportedHere) text = "This song carries your tones (" + view.fileName + "), exported from here.";
				else if (view.useMine)
				{
					text = "This song carries its own tones (" + view.fileName + by + "). You play yours instead.";
					hue = Ink::Muted;
					action = "Use the song's tones";
				}
				else
				{
					text = "This song carries its own tones (" + view.fileName + by + "). Playing them.";
					action = "Use mine instead";
				}
				break;
			case SongToneSets::SetState::Refused:
				text = "This song's tones (" + view.fileName + ") were not used: " + view.reason;
				hue = Ink::Amber;
				break;
			case SongToneSets::SetState::Failed:
				text = "This song's tones (" + view.fileName + ") could not be read. " + view.reason;
				hue = Ink::Amber;
				break;
			default:
				return;
			}
			const float buttonsW = (action ? BannerButtonWidth(action) + 6.0f : 0.0f);
			const float textX = 30.0f;
			const float wrapW = std::max(80.0f, width - textX - buttonsW - 10.0f);
			const float textH = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapW).y;
			const float bannerH = std::max(textH, ImGui::GetFrameHeight()) + 12.0f;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			const ImVec2 end(at.x + width, at.y + bannerH);
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->AddRectFilled(at, end, Mix(Ink::App, hue, 0.12f), 6.0f);
			list->AddRect(at, end, Mix(Ink::App, hue, 0.45f), 6.0f);
			list->AddCircleFilled(ImVec2(at.x + 15.0f, at.y + bannerH * 0.5f), 4.5f, hue, 12);
			list->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + textX, at.y + (bannerH - textH) * 0.5f), hue == Ink::Amber ? Ink::Amber : Ink::Text, text.c_str(), nullptr, wrapW);
			const float x = end.x - buttonsW;
			const float buttonY = at.y + (bannerH - ImGui::GetFrameHeight()) * 0.5f;
			if (action)
			{
				ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
				if (BannerButton(action))
				{
					if (view.useMine) SongToneSets::UseSongTones(songKey);
					else SongToneSets::UseMine(songKey);
				}
				Hint(view.useMine ? "Play the tones the song carries again, where you have not chosen your own."
					: "Stop playing the song's tones: the tones it set go back to none. Tones you chose yourself stay.");
			}
			ImGui::SetCursorScreenPos(ImVec2(at.x, end.y + 6.0f));
			ImGui::Dummy(ImVec2(0, 0));
		}

		// The table's columns, as fractions of its width: Arrangement, Game tone, Tone designer tone, Source.
		constexpr float GAME_TONE_COLUMN = 0.16f;
		constexpr float PICKER_COLUMN = 0.46f;
		constexpr float SOURCE_COLUMN = 0.82f;

		void SongToneHeader(float width)
		{
			ImFont* caption = CaptionFont();
			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 at = ImGui::GetCursorScreenPos();
			Text(list, caption, ImVec2(at.x + 10.0f, at.y), Ink::Dim, "Arrangement");
			Text(list, caption, ImVec2(at.x + width * GAME_TONE_COLUMN, at.y), Ink::Dim, "Game tone");
			Text(list, caption, ImVec2(at.x + width * PICKER_COLUMN, at.y), Ink::Dim, "Tone designer tone");
			Text(list, caption, ImVec2(at.x + width * SOURCE_COLUMN, at.y), Ink::Dim, "Source");
			ImGui::Dummy(ImVec2(width, caption->FontSize + 2.0f));
			list->AddLine(ImVec2(at.x, at.y + caption->FontSize + 4.0f), ImVec2(at.x + width, at.y + caption->FontSize + 4.0f), Ink::Edge);
			ImGui::Dummy(ImVec2(0, 2));
		}

		// One of the song's tones: its arrangement, the game's tone, the saved tone it plays and where that came from.
		void SongToneRow(const std::string& songKey, const SongLibrary::SongTone& tone, const SongToneSets::View& view, float width)
		{
			const Snapshot& s = GetSnapshot();
			const std::map<std::string, std::string>& assignments = SongToneSets::Assignments(songKey);
			const bool isPlayingNow = SameSong(songKey, s.songKey) && tone.key == s.gameToneKey;
			const auto assigned = assignments.find(tone.key);
			const std::string current = assigned == assignments.end() ? std::string() : assigned->second;
			ImGui::PushID(tone.key.c_str());
			const float rowH = ImGui::GetFrameHeight() + 6.0f;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			ImDrawList* list = ImGui::GetWindowDrawList();
			const bool isHovered = ImGui::IsMouseHoveringRect(at, ImVec2(at.x + width, at.y + rowH));
			ToneDesignerUi::RowBackground(list, at, ImVec2(at.x + width, at.y + rowH), false, isHovered);
			ImFont* caption = CaptionFont();
			const float textY = at.y + (rowH - ImGui::GetFontSize()) * 0.5f;
			list->PushClipRect(at, ImVec2(at.x + width * GAME_TONE_COLUMN - 6.0f, at.y + rowH), true);
			list->AddText(ImVec2(at.x + 10.0f, textY), Ink::Soft, tone.arrangement.c_str());
			list->PopClipRect();
			// Tone_Base "BHS Clean": the manifest's slot, then the name players see when it has one.
			const std::string gameTone = (tone.slot == "Now" ? std::string("Now") : "Tone_" + tone.slot) + (tone.name.empty() || tone.name == tone.key ? std::string() : " \"" + tone.name + "\"");
			const float gameX = at.x + width * GAME_TONE_COLUMN;
			const float gameEnd = at.x + width * PICKER_COLUMN - 8.0f;
			const float nowW = isPlayingNow ? TextWidth(caption, "now") + 30.0f : 0.0f;
			list->PushClipRect(ImVec2(gameX, at.y), ImVec2(gameEnd - nowW, at.y + rowH), true);
			list->AddText(ImVec2(gameX, textY), isPlayingNow ? IM_COL32_WHITE : Ink::Text, gameTone.c_str());
			list->PopClipRect();
			if (isPlayingNow)
			{
				const float nameW = std::min(ImGui::CalcTextSize(gameTone.c_str()).x, gameEnd - nowW - gameX);
				DrawPill(list, caption, ImVec2(gameX + nameW + 8.0f, at.y + (rowH - caption->FontSize - 6.0f) * 0.5f), caption->FontSize + 6.0f, TintPill("now", Ink::Good));
			}
			ImGui::SetCursorScreenPos(ImVec2(gameX, at.y));
			ImGui::Dummy(ImVec2(gameEnd - gameX, rowH));
			Hint(tone.key.c_str());

			const float comboX = at.x + width * PICKER_COLUMN;
			ImGui::SetCursorScreenPos(ImVec2(comboX, at.y + 3.0f));
			ImGui::SetNextItemWidth(at.x + width * SOURCE_COLUMN - comboX - 10.0f);
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
			PushPopupPadding();
			if (ImGui::BeginCombo("##savedTone", current.empty() ? "(none)" : current.c_str()))
			{
				// The presets are copied: a choice can change the snapshot while the list is drawn.
				const std::vector<std::string> presets = s.presets;
				if (ImGui::Selectable("(none)", current.empty()) && !current.empty()) SongToneSets::Assign(songKey, tone.key, {});
				for (const std::string& preset : presets)
				{
					if (ImGui::Selectable(preset.c_str(), preset == current) && preset != current) SongToneSets::Assign(songKey, tone.key, preset);
				}
				ImGui::EndCombo();
			}
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();

			// Source: the song's own tone, one the player chose, or none (the board's tone keeps playing).
			const bool isFromSong = SongToneSets::IsFromSong(view, tone.key, current);
			const char* source = isFromSong ? "From song" : current.empty() ? "Board" : "Yours";
			const ImU32 hue = isFromSong ? Ink::Good : current.empty() ? Ink::Muted : Ink::Outline;
			const float pillH = caption->FontSize + 6.0f;
			const ImVec2 pillAt(at.x + width * SOURCE_COLUMN, at.y + (rowH - pillH) * 0.5f);
			DrawPill(list, caption, pillAt, pillH, TintPill(source, hue));
			ImGui::SetCursorScreenPos(pillAt);
			ImGui::Dummy(ImVec2(ToneDesignerUi::PillWidth(caption, source), pillH));
			Hint(isFromSong ? "The song carries this tone. Choose another to make it yours."
				: current.empty() ? "No saved tone: the board's own tone plays for it." : "A saved tone you chose.");
			ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + rowH + 2.0f));
			ImGui::Dummy(ImVec2(0, 0));
			ImGui::PopID();
		}

		std::string Count(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

		// "3 tones, 2 captures and 1 cabinet; 1.4 MB. Starter files are named, not copied." from --tones-plan.
		std::string ExportSizeLine(const SongToneSets::View& view)
		{
			if (!view.hasPlan) return {};
			if (view.tones == 0) return "No tones assigned yet: give the song's tones saved tones to export them.";
			std::vector<std::string> parts{ Count(view.tones, "tone", "tones") };
			if (view.captures > 0) parts.push_back(Count(view.captures, "capture", "captures"));
			if (view.cabinets > 0) parts.push_back(Count(view.cabinets, "cabinet", "cabinets"));
			std::string line = parts[0];
			for (size_t i = 1; i < parts.size(); ++i) line += (i + 1 == parts.size() ? " and " : ", ") + parts[i];
			if (view.embeddedFiles == 0) return line + "; uses only built-in amps and cabinets.";
			char size[32]{};
			if (view.bytes >= 1024 * 1024) sprintf_s(size, "%.1f MB", view.bytes / (1024.0 * 1024.0));
			else sprintf_s(size, "%lld KB", std::max(1LL, (view.bytes + 512) / 1024));
			return line + "; " + size + ". Starter files are named, not copied.";
		}

		// Export song tones... (asks first when it would replace a set the song came with), and what it would carry.
		void SongSetButtons(const std::string& songKey, const SongToneSets::View& view, float width)
		{
			SongPaneState& pane = SongPane();
			ImGui::Dummy(ImVec2(0, 4));
			const bool canExport = !view.isBusy && !view.isDisc && !(view.hasPlan && view.tones == 0);
			ImGui::BeginDisabled(!canExport);
			if (Button("Export song tones...", true, view.isDisc ? "Rocksmith's own songs cannot carry tones: only a CDLC has a file of its own to put them beside."
				: "Writes the song's tones beside its .psarc (<song>_p.rabsong), with any captures and cabinets of your own, so the CDLC can carry them."))
			{
				const bool replacesSongs = view.state != SongToneSets::SetState::None && !view.isExportedHere;
				if (replacesSongs) pane.isExportConfirmWanted = true;
				else SongToneSets::Export(songKey);
			}
			ImGui::EndDisabled();
			if (view.isBusy)
			{
				ImGui::SameLine(0.0f, 10.0f);
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("Working...");
			}
			const std::string line = ExportSizeLine(view);
			if (!line.empty()) MutedCaption(line.c_str(), width);
			for (const std::string& problem : view.problems) MutedCaption(problem.c_str(), width);

			if (pane.isExportConfirmWanted)
			{
				ImGui::OpenPopup("##replaceSongTones");
				pane.isExportConfirmWanted = false;
			}
			PushPopupPadding();
			if (ImGui::BeginPopup("##replaceSongTones"))
			{
				ImGui::Text("Replace %s?", view.fileName.c_str());
				ImGui::TextDisabled("It holds the tones the song came with. Your tones for the song replace them.");
				if (Button("Replace", true))
				{
					SongToneSets::Export(songKey);
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (Button("Cancel")) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			ImGui::PopStyleVar();
		}

		// Right of the song list: the browsed song, whether it carries its own tones, every tone of every arrangement
		// (from its own manifest, the song list) with the saved tone it plays, then export and import. Nothing has to
		// be reached in the game first. The host loads the assigned tones before the song plays and follows the
		// game's tone changes.
		void SongToneTable(const std::string& songKey, float width, float height)
		{
			const Snapshot& s = GetSnapshot();
			ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
			ImGui::BeginChild("##songTones", ImVec2(width, height), false);
			const float rowW = ImGui::GetContentRegionAvail().x;
			if (songKey.empty())
			{
				MutedCaption("Choose a song on the left, or in the game. Its tones show here, each ready to play a saved tone of yours.", rowW);
				ImGui::EndChild();
				ImGui::PopStyleVar();
				return;
			}
			SongToneSets::Watch(songKey);
			const SongToneSets::View& view = SongToneSets::Get(songKey);
			const auto library = SongLibrary::Get();
			const SongLibrary::Song* song = library->Find(songKey);
			ImGui::TextUnformatted(song ? (song->title + " - " + song->artist).c_str() : songKey.c_str());
			if (!view.author.empty() && view.state == SongToneSets::SetState::Ready) MutedCaption(("Charted by " + view.author).c_str(), rowW);
			ImGui::Dummy(ImVec2(0, 2));
			SongSetBanner(songKey, view, rowW);
			SongToneHeader(rowW);
			if (!song || song->tones.empty())
			{
				MutedCaption(SongLibrary::GetState() == SongLibrary::State::Reading
					? "Reading the song list for this song's tones..."
					: "This song's tones are not in the song list. They appear once the song list has been read again.", rowW);
				if (SameSong(songKey, s.songKey) && !s.gameToneKey.empty())
					SongToneRow(songKey, SongLibrary::SongTone{ "Now", "Now", s.gameToneKey, "The tone playing now" }, view, rowW);
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
					SongToneRow(songKey, tone, view, rowW);
				}
			}
			SongSetButtons(songKey, view, rowW);
			ImGui::EndChild();
			ImGui::PopStyleVar();
		}

		// The Songs tab: search, the way back to the game's song, every song. Choosing one shows its tones under the
		// board (SongDetail).
		void SongColumn(float width, float height)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
			ImGui::BeginChild("##songColumn", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::PopStyleVar();
			SongBrowser(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
			ImGui::EndChild();
		}

		// Under the board while the Songs tab is shown: the chosen song's tones, each with the saved tone it plays.
		void SongDetail(float width, float height)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
			ImGui::BeginChild("##songDetail", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::PopStyleVar();
			SongStatePill();
			const std::string notice = SongToneSets::TakeNotice();
			if (!notice.empty()) Notice(notice);
			ImGui::Dummy(ImVec2(0, 2));
			const std::string songKey = BrowsedSong();   // a copy: a click in the list changes it
			SongToneTable(songKey, ImGui::GetContentRegionAvail().x, std::max(1.0f, ImGui::GetContentRegionAvail().y));
			ImGui::EndChild();
		}

		// ---- Browser column ---------------------------------------------------------------------------------

		// Tones | Pedals | Songs, then that tab's list under it, the full height of the panel.
		void BrowserColumn(float width, float height)
		{
			PageState& state = State();
			ImGui::BeginChild("##stompboxBrowser", ImVec2(width, height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			static const char* const tabs[3] = { "Tones", "Pedals", "Songs" };
			static const char* const hints[3] = { "Your saved tones: double-click one to load it.",
				"Effects, amps (NAM captures) and cabinets (IRs) that ship with the mod or that you added: drag one onto the board to use it.",
				"Choose a song: its tones show under the board, each ready to play one of your saved tones." };
			ImGui::SetCursorPos(ImVec2(8.0f, 10.0f));
			const int clicked = ToneDesignerUi::PaneToggle(tabs, 3, state.column, hints, width - 16.0f);
			if (clicked >= 0) state.column = clicked;
			ImGui::SetCursorPos(ImVec2(0.0f, ImGui::GetCursorPosY() + 4.0f));
			const float listH = std::max(40.0f, height - ImGui::GetCursorPosY());
			if (state.column == PedalsColumn) PedalList(width, listH);
			else if (state.column == SongsColumn) SongColumn(width, listH);
			else ToneList(width, listH);
			ImGui::EndChild();
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
				// Not while a setup import or restore runs: it stops the host on purpose and starts it again itself.
				ImGui::BeginDisabled(ToneBackup::Busy());
				if (Button("Start", true)) VstHostLink::StartHost();
				ImGui::EndDisabled();
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

		// The framed panel, laid out as the VST rig's: the bar; the browser column on the left; on the right the board
		// strip and, under it, the Inspector (or the chosen song's tones while the Songs tab is shown). Backgrounds go
		// on the page's draw list under the parts, which are child windows. Returns the page's natural height.
		float Panel()
		{
			PageState& state = State();
			const Snapshot& s = GetSnapshot();
			const bool isLate = HasRecentLateBlocks();
			const float width = ImGui::GetContentRegionAvail().x;
			const float barH = ImGui::GetFontSize() + 20.0f;
			const bool isSongs = state.isBrowserShown && state.column == SongsColumn;
			const bool hasLower = state.isInspectorShown || isSongs;
			const float libraryW = state.isBrowserShown ? ToneDesignerUi::BrowserWidth(width) : 0.0f;
			const float boardH = ToneDesignerUi::BoardStripHeight(RackHeight(width - libraryW), BoardLine(s, isLate) != nullptr);
			const float naturalHeight = ImGui::GetCursorPosY() + barH + std::max(NATURAL_MAIN_HEIGHT, boardH + (hasLower ? NATURAL_INSPECTOR_HEIGHT : 0.0f)) + 12.0f + 20.0f;
			const float mainH = std::max(boardH + (hasLower ? MIN_INSPECTOR_HEIGHT : 0.0f), std::floor(ImGui::GetContentRegionAvail().y - barH - 12.0f));
			const ImVec2 at = ImGui::GetCursorScreenPos();
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->ChannelsSplit(2);
			list->ChannelsSetCurrent(1);

			Bar(width, barH, isLate);
			const float top = at.y + barH;
			if (libraryW > 0.0f)
			{
				ImGui::SetCursorScreenPos(ImVec2(at.x, top));
				BrowserColumn(libraryW, mainH);
			}
			ImGui::SetCursorScreenPos(ImVec2(at.x + libraryW, top));
			Board(width - libraryW, hasLower ? boardH : mainH, isLate);
			if (hasLower)
			{
				ImGui::SetCursorScreenPos(ImVec2(at.x + libraryW, top + boardH));
				if (isSongs) SongDetail(width - libraryW, mainH - boardH);
				else StompInspector(width - libraryW, mainH - boardH);
			}
			const float bottom = top + mainH;

			// Ctrl+Z and Ctrl+Y (Ctrl+Shift+Z) on the page, as Edit's Undo and Redo, unless a text box has the keys.
			const ImGuiIO& io = ImGui::GetIO();
			if (io.KeyCtrl && !io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
			{
				if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Z), false))
				{
					if (io.KeyShift) { if (!s.redoLabel.empty()) StompboxLink::Redo(); }
					else if (!s.undoLabel.empty()) StompboxLink::Undo();
				}
				if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Y), false) && !s.redoLabel.empty()) StompboxLink::Redo();
			}

			list->ChannelsSetCurrent(0);
			ToneDesignerUi::PanelFrame frame;
			frame.at = at;
			frame.width = width;
			frame.barHeight = barH;
			frame.mainHeight = mainH;
			frame.libraryWidth = libraryW;
			frame.bottom = bottom;
			frame.inspectorTop = hasLower ? top + boardH : 0.0f;
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
		return Panel();
	}
}
