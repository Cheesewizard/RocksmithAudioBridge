#include "ToneDesignerPage.hpp"

#include "ToneDesignerUi.hpp"
#include "../PluginPictureView.hpp"
#include "../SongLibrary.hpp"
#include "../ToneBackup.hpp"
#include "../VstHostLink.hpp"
#include "../../ExternalAmp/ExternalAmp.hpp"
#include "../../Shared/VstHostControl.hpp"
#include "../../Shell/OverlayUi.hpp"
#include "../../Shell/ShellPages.hpp"
#include "../../Shell/Pages/PageCommon.hpp"

#include "Lib/ImGUI/imgui_internal.h"   // SetItemUsingMouseWheel (the pedalboard scrolls sideways)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// The VST rig tab of the Guitar Tone page, ported from 4.x's OverlayShell.cpp (its Tone designer section) as it was.
namespace Overlay
{
	namespace
	{
		using AudioBridge::Pages::InfoRow;
		using AudioBridge::Pages::Utf8;
		using AudioBridge::Pages::WarnCaption;
		using ToneDesignerUi::MeterFraction;   // linear peak to a -60..0 dBFS meter fraction

		// ---- Tone designer --------------------------------------------------------------------------------
		// The VST host (VstHost/, ToneDesigner/VstHostLink.hpp) plays the guitar through a chain of VST3 plugins and shows the
		// selected pedal's own window over the game. This page is laid out like a chain loader, in one framed panel
		// (the same layout as the Tone designer tab, ToneDesignerUi.hpp):
		//   - a bar: File / Edit / View, the tone's name between previous / next arrows (amber dot = unsaved),
		//     Save, Bypass all and the plugin windows button (Game amp is the Guitar Tone page's first tab);
		//   - one browser column on the left with three tabs: Tones (favourites, folders, song tones), Plugins (every
		//     plugin found; drag one onto the board) and Songs (give songs a tone; the song playing now is first);
		//   - the pedalboard as a strip: In jack, cables, one pedal per plugin coloured by kind (LED, knobs, footswitch),
		//     Add, Out jack. Click selects, double-click opens the plugin's window, the footswitch bypasses, drag
		//     reorders, right-click a pedal, a gap or the board for its actions; IN / OUT meters at its ends;
		//   - under it the Inspector: the selected pedal's knobs (the plugin's own parameters), bypass, a same-type
		//     swap and its window, and the chain's trims.
		// View shows or hides the browser, the Inspector and the meters (remembered). Commands are queued to the host;
		// Edit's Undo and Redo are the host's (juce::UndoManager), so the page only asks and shows their names. The
		// page shows the host's state a few times a second. It shares the amp link with the External app tab, which
		// carries one app at a time: each page says when the other one has it.
		namespace ToneDesigner
		{
			// The bits are remembered (VstHostLink::SectionsOpen); the Inspector took the old plugin browser pane's bit.
			enum Section : unsigned { Library = 2, Inspector = 4, Meters = 8 };
			enum BrowserTab { TonesTab, PluginsTab, SongsTab };
			// The Songs tab: which arrangements a song must have (any of the lit ones), and its order.
			enum SongPart : unsigned { LeadPart = 1, RhythmPart = 2, BassPart = 4 };
			enum SongSort { ByArtist, ByTitle, ByYear, SongSortCount };
			enum class Dialog { None, SaveAs, Rename, Delete, NewTone, Discard, Levels, NewFolder, DeleteTones, Restore };
			enum Filter { All, Favourites, Amps, Effects, Dynamics, Instruments, Recent, FilterCount };

			// The drawing parts both Tone designer tabs share (ToneDesignerUi.hpp).
			namespace Ink = ToneDesignerUi::Ink;
			using ToneDesignerUi::PedalLook;
			using ToneDesignerUi::PushPopupPadding;
			using ToneDesignerUi::CaptionFont;
			using ToneDesignerUi::Text;
			using ToneDesignerUi::TextWidth;
			using ToneDesignerUi::IconSquare;
			using ToneDesignerUi::FoldLabel;
			using ToneDesignerUi::Jack;
			using ToneDesignerUi::PillLook;
			using ToneDesignerUi::TintPill;
			using ToneDesignerUi::PillWidth;
			using ToneDesignerUi::DrawPill;
			using ToneDesignerUi::Chip;
			using ToneDesignerUi::MutedCaption;

			// Pedal body and edge by kind: dynamics teal, amps brown and wider, effects purple, others slate.
			PedalLook LookFor(VstHostLink::Kind kind)
			{
				switch (kind) {
				case VstHostLink::Kind::Amp: return ToneDesignerUi::Looks::Amp;
				case VstHostLink::Kind::Dynamics: return ToneDesignerUi::Looks::Dynamics;
				case VstHostLink::Kind::Effect: return ToneDesignerUi::Looks::Effect;
				default: return ToneDesignerUi::Looks::Other;
				}
			}

			ToneDesignerUi::Symbol SymbolFor(VstHostLink::Kind kind)
			{
				switch (kind) {
				case VstHostLink::Kind::Amp: return ToneDesignerUi::Symbol::Amp;
				case VstHostLink::Kind::Dynamics: return ToneDesignerUi::Symbol::Dynamics;
				default: return ToneDesignerUi::Symbol::Wave;
				}
			}

			struct State
			{
				bool loaded = false;
				unsigned open = 0;              // Section bits, remembered
				int selected = -1;              // the pedal selected on the board (not necessarily in the amp window)
				std::wstring selectedPlugin;    // the browser's selected row
				char search[64] = {};
				char toneSearch[48] = {};
				std::string selectedTone;       // the library's selected row (double-click or Load loads it)
				std::vector<std::string> foldedFolders;   // tone folders folded away in the library (this session)
				int filter = All;
				Dialog dialog = Dialog::None;
				bool openDialog = false;
				std::string dialogTone;         // the tone a dialog is about
				std::vector<std::string> dialogTones;   // a batch dialog's tones (New folder, Delete for a selection)
				std::wstring dialogPath;        // Restore: the backup
				// The library's selection: Ctrl+click adds or takes one away, Shift+click takes the range from the
				// anchor (in the order the rows were drawn last frame). Right-click on a selection gives batch actions.
				std::vector<std::string> selectedTones;
				std::string toneAnchor;
				std::vector<std::string> toneOrder, toneOrderNext;
				// The browser column's tab: the tones, the plugins, or the songs (give songs a tone without playing them).
				int browserTab = TonesTab;
				char songSearch[64] = {};
				int songFilter = 0;
				unsigned songParts = 0;         // the Songs tab's arrangement chips: SongPart bits, 0 = any
				int songSort = 0;               // SongSort
				std::vector<std::string> selectedSongs;   // song keys, chosen the same way as tones
				std::string songAnchor;
				std::vector<ToneBackup::Entry> backups;   // File, Backups: listed when the menu opens
				size_t dialogSongs = 0;         // Delete: songs that use it (read once, when the dialog opens)
				char name[64] = {};
				char pickSearch[48] = {};       // "Give it a tone": filters the tone choices (ToneChoices)
				char addSearch[48] = {};        // the Add popover's search
				int addFilter = 0;              // the Add popover's chip
				int addAt = -1;                 // where the Add popover or "Add here" puts the plugin: -1 the end, n before slot n
				int gapMenu = -1;               // the gap whose right-click menu is open
			};

			State& S()
			{
				static State state;
				if (!state.loaded) {
					state.open = VstHostLink::SectionsOpen(Library | Inspector | Meters);
					state.loaded = true;
				}
				return state;
			}

			bool IsOpen(Section section) { return (S().open & section) != 0; }

			// Tone names are "Name" or "Folder/Name" (VstHostLink.hpp): one folder level in the tones folder.
			std::string FolderOf(const std::string& tone)
			{
				const size_t slash = tone.find('/');
				return slash == std::string::npos ? std::string() : tone.substr(0, slash);
			}

			std::string BaseName(const std::string& tone)
			{
				const size_t slash = tone.find('/');
				return slash == std::string::npos ? tone : tone.substr(slash + 1);
			}

			bool InList(const std::vector<std::string>& list, const std::string& item)
			{
				return std::any_of(list.begin(), list.end(), [&](const std::string& i) { return _stricmp(i.c_str(), item.c_str()) == 0; });
			}

			// A click in a list with a selection: Ctrl toggles the row, Shift takes the range from the anchor (in
			// `order`, the rows as shown), a plain click selects only that row and makes it the anchor.
			void SelectClick(std::vector<std::string>& selection, std::string& anchor, const std::vector<std::string>& order, const std::string& item)
			{
				const ImGuiIO& io = ImGui::GetIO();
				if (io.KeyCtrl) {
					const auto it = std::find_if(selection.begin(), selection.end(), [&](const std::string& i) { return _stricmp(i.c_str(), item.c_str()) == 0; });
					if (it != selection.end()) selection.erase(it);
					else selection.push_back(item);
					anchor = item;
					return;
				}
				auto indexOf = [&](const std::string& value) {
					for (size_t i = 0; i < order.size(); ++i) if (_stricmp(order[i].c_str(), value.c_str()) == 0) return static_cast<int>(i);
					return -1;
				};
				const int from = indexOf(anchor), to = indexOf(item);
				if (io.KeyShift && from >= 0 && to >= 0) {
					selection.clear();
					for (int i = std::min(from, to); i <= std::max(from, to); ++i) selection.push_back(order[static_cast<size_t>(i)]);
					return;
				}
				selection = { item };
				anchor = item;
			}


			void Toggle(Section section)
			{
				State& s = S();
				s.open ^= section;
				VstHostLink::SetSectionsOpen(s.open);
			}

			// The browser column shows the songs (tones dragged onto them, "Give to the selected songs").
			bool SongsShown() { return S().browserTab == SongsTab && IsOpen(Library); }

			// Shows the browser column on that tab (the Song tones line, the Add popover's "Open the Plugins tab").
			void ShowBrowserTab(int tab)
			{
				S().browserTab = tab;
				if (!IsOpen(Library)) Toggle(Library);
			}

			// Dialogs open from inside menus and popups, whose ID stack differs from the page's; the request is kept
			// here and the modal opened at page level (Dialogs).
			void Ask(Dialog dialog, const std::string& tone = {})
			{
				State& s = S();
				s.dialog = dialog;
				s.openDialog = true;
				s.dialogTone = tone;
				s.dialogTones.clear();
				s.dialogSongs = dialog == Dialog::Delete ? VstHostLink::SongsUsingTone(tone).size() : 0;
				strncpy_s(s.name, (dialog == Dialog::Rename ? BaseName(tone) : dialog == Dialog::NewFolder ? std::string() : tone).c_str(), _TRUNCATE);
			}

			const VstHostLink::Plugin* FindPlugin(const std::wstring& path)
			{
				for (const VstHostLink::Plugin& plugin : VstHostLink::Plugins())
					if (_wcsicmp(plugin.path.c_str(), path.c_str()) == 0) return &plugin;
				return nullptr;
			}

			const char* KindName(VstHostLink::Kind kind)
			{
				switch (kind) {
				case VstHostLink::Kind::Amp: return "Amp";
				case VstHostLink::Kind::Effect: return "Effect";
				case VstHostLink::Kind::Dynamics: return "Dynamics";
				case VstHostLink::Kind::Instrument: return "Instrument";
				default: return "Plugin";
				}
			}

			ImU32 KindColor(VstHostLink::Kind kind)
			{
				switch (kind) {
				case VstHostLink::Kind::Amp: return IM_COL32(230, 160, 120, 255);
				case VstHostLink::Kind::Effect: return IM_COL32(170, 160, 240, 255);
				case VstHostLink::Kind::Dynamics: return IM_COL32(120, 200, 210, 255);
				default: return Ink::Muted;
				}
			}

			bool InChain(const VstHostLink::Snapshot& vst, const std::wstring& path)
			{
				return std::any_of(vst.slots.begin(), vst.slots.end(),
					[&](const VstHostLink::SlotView& slot) { return _wcsicmp(slot.path.c_str(), path.c_str()) == 0; });
			}

			bool Listed(const std::vector<std::wstring>& list, const std::wstring& path)
			{
				return std::any_of(list.begin(), list.end(), [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; });
			}

			bool KeptLoaded(const VstHostLink::Snapshot& vst, const std::wstring& path) { return Listed(vst.spares, path); }

			// Loading in the background now: the host names it by its file name without .vst3.
			bool Loading(const VstHostLink::Snapshot& vst, const std::wstring& path)
			{
				if (vst.preloadingName.empty()) return false;
				std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
				if (name.size() > 5 && _wcsicmp(name.c_str() + name.size() - 5, L".vst3") == 0) name.resize(name.size() - 5);
				return _stricmp(Utf8(name).c_str(), vst.preloadingName.c_str()) == 0;
			}

			bool HasUnsavedChanges(const VstHostLink::Snapshot& vst) { return vst.toneModified && !vst.slots.empty(); }

			// Loading a tone or starting a new one replaces the chain: ask first when it has changes that are not saved.
			void RequestLoad(const VstHostLink::Snapshot& vst, const std::string& tone)
			{
				if (HasUnsavedChanges(vst)) Ask(Dialog::Discard, tone);
				else VstHostLink::LoadTone(tone);
			}

			void RequestNew(const VstHostLink::Snapshot& vst)
			{
				if (HasUnsavedChanges(vst)) Ask(Dialog::NewTone);
				else VstHostLink::NewTone();
			}

			// Tone file actions (new, open, save, rename) only wait behind a plugin loading in the background: they
			// queue and run once it has loaded, rather than greying out with no reason given.
			bool FileIdle(const VstHostLink::Snapshot& vst) { return !vst.busy || vst.backgroundOnly; }

			std::string WaitNote(const VstHostLink::Snapshot& vst)
			{
				return vst.backgroundOnly ? vst.preloadingName + " is loading in the background; this happens once it is ready." : std::string();
			}

			void Save(const VstHostLink::Snapshot& vst)
			{
				if (vst.toneName.empty()) Ask(Dialog::SaveAs);
				else VstHostLink::SaveTone(vst.toneName);
			}

			// The previous or next saved tone (the arrows beside the tone's name), wrapping around.
			void StepTone(const VstHostLink::Snapshot& vst, int delta)
			{
				const std::vector<std::string>& tones = VstHostLink::Tones();
				if (tones.empty()) return;
				int index = -1;
				for (int i = 0; i < static_cast<int>(tones.size()); ++i)
					if (_stricmp(tones[i].c_str(), vst.toneName.c_str()) == 0) index = i;
				const int count = static_cast<int>(tones.size());
				const int next = index < 0 ? (delta > 0 ? 0 : count - 1) : ((index + delta) % count + count) % count;
				if (next != index) RequestLoad(vst, tones[next]);
			}

			// The host moves a pedal one place at a time; a drag across several is queued as adjacent swaps.
			void MovePedal(int from, int to)
			{
				for (int i = from; i < to; ++i) VstHostLink::MoveSlot(i, +1);
				for (int i = from; i > to; --i) VstHostLink::MoveSlot(i, -1);
				S().selected = to;
			}

			// Each pedal has its own plugin window (VstHost Main.cpp). Opening one also turns "Show plugin windows" on.
			// A plugin window can take seconds to appear (a big amp sim builds its editor first): the pedal shows a
			// spinner from the request until its window is open, or for 15 s at most.
			int g_openingSlot = -1;
			double g_openingSince = 0.0;

			void OpenWindow(int slot)
			{
				VstHostLink::ShowSlot(slot);
				VstHostLink::SetPopout(true);
				g_openingSlot = slot;
				g_openingSince = ImGui::GetTime();
			}

			// The bar's button: all open windows on or off. Turned on with none open, it opens the selected pedal's.
			void ToggleWindows(const VstHostLink::Snapshot& vst)
			{
				const bool anyOpen = std::any_of(vst.slots.begin(), vst.slots.end(), [](const VstHostLink::SlotView& slot) { return slot.windowOpen; });
				if (!vst.popoutWanted && !anyOpen && !vst.slots.empty())
					VstHostLink::ShowSlot(S().selected >= 0 && S().selected < static_cast<int>(vst.slots.size()) ? S().selected : 0);
				VstHostLink::SetPopout(!vst.popoutWanted);
			}

			// The plugins as menu items, vendor on the right. Instruments are left out (they make no sound from a
			// guitar); the browser still lists them.
			// Dragging: a pedal carries its slot ("RAB_PEDAL", an int); a plugin from the browser carries its path
			// ("RAB_PLUGIN"). Either can be dropped between two pedals to go in there.
			struct PluginDrag { wchar_t path[VstHostControl::kPathChars]; };

			bool MatchesFilter(const VstHostLink::Plugin& plugin, int filter);
			bool MatchesSearch(const VstHostLink::Plugin& plugin, const char* search);
			const VstHostLink::Plugin* FindPlugin(const std::wstring& path);

			// `filter` narrows it to one of the browser's chips (Amps, Effects...); Recent lists the plugins last added.
			template <typename Pick> void PluginMenuItems(Pick pick, int filter = All)
			{
				std::vector<const VstHostLink::Plugin*> plugins;
				if (filter == Recent) {
					for (const std::wstring& path : VstHostLink::RecentPlugins())
						if (const VstHostLink::Plugin* plugin = FindPlugin(path)) plugins.push_back(plugin);
				}
				else for (const VstHostLink::Plugin& plugin : VstHostLink::Plugins()) if (MatchesFilter(plugin, filter)) plugins.push_back(&plugin);
				if (VstHostLink::Plugins().empty()) ImGui::TextDisabled("No 64-bit VST3 plugins found");
				else if (plugins.empty()) ImGui::TextDisabled(filter == Favourites ? "No favourites yet" : filter == Recent ? "None added yet" : "None found");
				// A dot before each loaded plugin, as in the browser: green in the chain, blue kept loaded outside it
				// (adding that one is instant).
				const VstHostLink::Snapshot vst = VstHostLink::GetSnapshot();
				for (const VstHostLink::Plugin* entry : plugins) {
					const VstHostLink::Plugin& plugin = *entry;
					if (plugin.kind == VstHostLink::Kind::Instrument) continue;
					const std::string label = "    " + plugin.name;   // room for the dot
					if (ImGui::MenuItem(label.c_str(), plugin.vendor.c_str())) pick(plugin);
					const bool inChain = InChain(vst, plugin.path), loaded = KeptLoaded(vst, plugin.path);
					if (inChain || loaded) {
						const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
						ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(min.x + 7.0f, (min.y + max.y) * 0.5f), 3.5f, inChain ? Ink::Good : Ink::AddText, 12);
						Hint(inChain ? "In the chain now." : "Kept loaded: adding it is instant.");
					}
				}
			}

			// "Add here" and the board's "Add plugin": the plugins by kind, each a submenu, as the browser's chips sort them.
			// `at` is where it goes: -1 the end of the chain, n before slot n.
			void AddByKindMenu(const VstHostLink::Snapshot& vst, int at)
			{
				if (vst.slots.size() >= static_cast<size_t>(VstHostControl::kMaxSlots)) { ImGui::TextDisabled("The chain is full (8 plugins)"); return; }
				static const char* const kinds[] = { "Amps", "Effects", "Dynamics", "Favourites", "Recent" };
				static const int filters[] = { Amps, Effects, Dynamics, Favourites, Recent };
				for (int k = 0; k < 5; ++k)
					if (ImGui::BeginMenu(kinds[k], !vst.busy)) {
						PluginMenuItems([&](const VstHostLink::Plugin& plugin) {
							if (at < 0) VstHostLink::AddPlugin(plugin.path);
							else { VstHostLink::AddPlugin(plugin.path, at); S().selected = at; }
						}, filters[k]);
						ImGui::EndMenu();
					}
				ImGui::Separator();
				if (ImGui::MenuItem("Browse plugins...")) ShowBrowserTab(PluginsTab);
			}

			// One pedal's actions: the pedal's right-click menu, and Edit for the selected pedal.
			void PedalActions(const VstHostLink::Snapshot& vst, int i)
			{
				const int count = static_cast<int>(vst.slots.size());
				const bool valid = i >= 0 && i < count;
				const bool idle = !vst.busy;
				const bool windowOpen = valid && vst.slots[i].windowOpen;
				if (ImGui::MenuItem(windowOpen ? "Bring window to front" : "Open window", "or double-click it", false, valid)) OpenWindow(i);
				if (windowOpen && ImGui::MenuItem("Close window")) VstHostLink::HideSlot(i);
				if (ImGui::MenuItem(valid && vst.slots[i].bypass ? "Turn on" : "Bypass", "or click its footswitch", false, valid && idle))
					VstHostLink::SetBypass(i, !vst.slots[i].bypass);
				ImGui::Separator();
				if (ImGui::BeginMenu("Replace with", valid && idle)) {
					PluginMenuItems([&](const VstHostLink::Plugin& plugin) { VstHostLink::ReplaceSlot(i, plugin.path); });
					ImGui::EndMenu();
				}
				if (ImGui::MenuItem("Duplicate", nullptr, false, valid && idle && count < VstHostControl::kMaxSlots)) VstHostLink::DuplicateSlot(i);
				if (ImGui::MenuItem("Reload plugin", nullptr, false, valid && idle)) VstHostLink::Reload(vst.slots[i].path);
				Hint("Closes the plugin completely and starts it again, keeping its settings: for when it needs a fresh start to see something new, such as tones you imported into it. Every copy of it reloads; the rest of the chain keeps playing meanwhile.");
				if (ImGui::MenuItem("Move left", nullptr, false, valid && idle && i > 0)) { VstHostLink::MoveSlot(i, -1); S().selected = i - 1; }
				if (ImGui::MenuItem("Move right", nullptr, false, valid && idle && i + 1 < count)) { VstHostLink::MoveSlot(i, +1); S().selected = i + 1; }
				ImGui::Separator();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
				if (ImGui::MenuItem("Remove", nullptr, false, valid && idle)) VstHostLink::RemoveSlot(i);
				ImGui::PopStyleColor();
			}

			void FileMenu(const VstHostLink::Snapshot& vst)
			{
				if (!ImGui::BeginMenu("File")) return;
				const bool idle = FileIdle(vst);
				const std::string wait = WaitNote(vst);
				if (!wait.empty()) { ImGui::TextDisabled("Loading %s in the background", vst.preloadingName.c_str()); ImGui::Separator(); }
				if (ImGui::MenuItem("New tone", nullptr, false, idle)) RequestNew(vst);
				Hint(wait.c_str());
				if (ImGui::BeginMenu("Open tone", idle)) {
					const std::vector<std::string>& tones = VstHostLink::Tones();
					if (tones.empty()) ImGui::TextDisabled("No saved tones yet");
					for (const std::string& tone : tones)
						if (ImGui::MenuItem(tone.c_str(), nullptr, _stricmp(tone.c_str(), vst.toneName.c_str()) == 0)) RequestLoad(vst, tone);
					ImGui::EndMenu();
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Save", nullptr, false, idle && (vst.toneName.empty() || vst.toneModified))) Save(vst);
				Hint(wait.c_str());
				if (ImGui::MenuItem("Save as...", nullptr, false, idle)) Ask(Dialog::SaveAs, vst.toneName);
				Hint(wait.c_str());
				if (ImGui::MenuItem("Revert to saved", nullptr, false, idle && !vst.toneName.empty() && vst.toneModified)) VstHostLink::LoadTone(vst.toneName);
				Hint(wait.c_str());
				if (ImGui::MenuItem("Rename...", nullptr, false, idle && !vst.toneName.empty())) Ask(Dialog::Rename, vst.toneName);
				Hint(wait.c_str());
				ImGui::Separator();
				if (ImGui::MenuItem("Show tones folder")) VstHostLink::OpenTonesFolder();
				// Backups: one zip with tones, song tones, plugin settings and settings (ToneBackup.hpp).
				if (ImGui::BeginMenu("Backups")) {
					State& s = S();
					if (ImGui::IsWindowAppearing()) s.backups = ToneBackup::List();
					if (ImGui::MenuItem("Back up now", nullptr, false, !ToneBackup::Busy())) ToneBackup::BackupNow();
					Hint("Your tones, song tones, plugin settings and Tone designer settings in one zip. One is also made each day on its own; the last 7 of those are kept.");
					ImGui::Separator();
					ImGui::TextDisabled(s.backups.empty() ? "No backups yet" : "Restore");
					for (size_t i = 0; i < s.backups.size() && i < 14; ++i)
						if (ImGui::MenuItem(s.backups[i].name.c_str(), nullptr, false, !ToneBackup::Busy())) {
							Ask(Dialog::Restore, s.backups[i].name);
							s.dialogPath = s.backups[i].path;
						}
					ImGui::Separator();
					if (ImGui::MenuItem("Open backups folder")) ToneBackup::OpenFolder();
					ImGui::EndMenu();
				}
				ImGui::Separator();
				// The Game amp tab only turns it off (standby, plugins kept loaded); this closes the program and frees them.
				if (ImGui::MenuItem("Close Tone designer")) VstHostLink::StopHost();
				ImGui::EndMenu();
			}

			void EditMenu(const VstHostLink::Snapshot& vst)
			{
				if (!ImGui::BeginMenu("Edit")) return;
				// The host's history (juce::UndoManager): it names each step, so the menu says what Undo takes back.
				const std::string undo = vst.undoLabel.empty() ? std::string("Undo") : "Undo " + vst.undoLabel;
				const std::string redo = vst.redoLabel.empty() ? std::string("Redo") : "Redo " + vst.redoLabel;
				if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, !vst.undoLabel.empty() && !vst.busy)) VstHostLink::Undo();
				if (ImGui::MenuItem(redo.c_str(), "Ctrl+Y", false, !vst.redoLabel.empty() && !vst.busy)) VstHostLink::Redo();
				ImGui::Separator();
				const int selected = S().selected;
				const bool valid = selected >= 0 && selected < static_cast<int>(vst.slots.size());
				ImGui::TextDisabled("%s", valid ? vst.slots[selected].name.c_str() : "No pedal selected");
				ImGui::Separator();
				PedalActions(vst, selected);
				ImGui::Separator();
				if (ImGui::MenuItem("Bypass whole chain", nullptr, vst.chainBypassed, !vst.slots.empty()))
					VstHostLink::SetChainBypass(!vst.chainBypassed);
				ImGui::Separator();
				if (ImGui::MenuItem("Levels...")) Ask(Dialog::Levels);
				ImGui::EndMenu();
			}

			void ViewMenu(const VstHostLink::Snapshot& vst)
			{
				if (!ImGui::BeginMenu("View")) return;
				if (ImGui::MenuItem("Browser", nullptr, IsOpen(Library))) Toggle(Library);
				if (ImGui::MenuItem("Inspector", nullptr, IsOpen(Inspector))) Toggle(Inspector);
				if (ImGui::MenuItem("Meters", nullptr, IsOpen(Meters))) Toggle(Meters);
				ToneDesignerUi::BoardSizeMenu();
				if (ImGui::MenuItem("Plugin windows", nullptr, vst.popoutWanted)) ToggleWindows(vst);
				ImGui::Separator();
				if (ImGui::MenuItem("Start with the game", nullptr, vst.autoStart)) VstHostLink::SetAutoStart(!vst.autoStart);
				// No "Fast tone changes" menu: the host keeps recent plugins loaded on its own (RefreshKeepSettings);
				// pinning one is the browser's right-click, and a failed background load shows on its status pill.
				ImGui::Separator();
				if (ImGui::MenuItem("Rescan plugins")) VstHostLink::Rescan();
				if (ImGui::MenuItem("Add plugin folder...")) VstHostLink::AddScanFolder();
				ImGui::EndMenu();
			}

			// A square icon button drawn on the bar; `on` lights it blue.
			// The bar: menus, the tone's name between arrows, the plugin windows button. The A/B switch and the power
			// button that were here are gone: the Guitar Tone page's Game amp tab is the way back to the game's amp, and
			// it puts this host in standby as the power button did (SelectGuitarTone).
			void Bar(const VstHostLink::Snapshot& vst, float width, float height)
			{
				ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0, 0, 0, 0));
				ImGui::PushStyleColor(ImGuiCol_Header, ImGui::ColorConvertU32ToFloat4(Ink::Edge));
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::ColorConvertU32ToFloat4(Ink::Edge));
				// The bar's height and its labels' vertical place both come from FramePadding (Begin, BeginMenuBar);
				// it is popped right after, so the menus' popups keep normal item heights.
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9, (height - ImGui::GetFontSize()) * 0.5f));
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 0));
				ImGui::BeginChild("##toneBar", ImVec2(width, height), false,
					ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
				ImGui::PopStyleVar();
				const bool bar = ImGui::BeginMenuBar();
				ImGui::PopStyleVar();
				PushPopupPadding();
				if (bar) {
					FileMenu(vst);
					EditMenu(vst);
					ViewMenu(vst);
					float menusEnd = ImGui::GetCursorPosX();
					ImDrawList* list = ImGui::GetWindowDrawList();
					const ImVec2 origin = ImGui::GetWindowPos();
					const float button = height - 12.0f;
					const float y = (height - button) * 0.5f;

					// Right: Save, Bypass all, the plugin windows button.
					float x = width - 10.0f - button;
					ImGui::SetCursorScreenPos(ImVec2(origin.x + x, origin.y + y));
					if (IconSquare("##ampWindow", Glyph::NewWindow, vst.popoutWanted,
						vst.popoutWanted ? "Hide the plugin windows." : "Show the plugin windows over the game whenever this overlay is open. Each pedal has its own: double-click a pedal to open it. Drag a window's top bar to move it.", button))
						ToggleWindows(vst);
					x -= ToneDesignerUi::BarToggleWidth("Bypass all") + 10.0f;
					ImGui::SetCursorScreenPos(ImVec2(origin.x + x, origin.y + y));
					if (ToneDesignerUi::BarToggle("##bypassAll", "Bypass all", vst.chainBypassed, button, !vst.slots.empty(),
						vst.chainBypassed ? "Every plugin is skipped: you hear your clean guitar. Click to play through the chain again." : "Skip every plugin to hear your clean guitar; each pedal keeps its own on or off."))
						VstHostLink::SetChainBypass(!vst.chainBypassed);
					const bool canSave = FileIdle(vst) && (vst.toneName.empty() ? !vst.slots.empty() : vst.toneModified);
					x -= ToneDesignerUi::BarButtonWidth("Save") + 8.0f;
					ImGui::SetCursorScreenPos(ImVec2(origin.x + x, origin.y + y));
					if (ToneDesignerUi::BarButton("##save", "Save", button, canSave, vst.toneName.empty() ? "Save the chain as a new tone." : "Save the changes to this tone."))
						Save(vst);

					// Middle: < tone name >, centred in the space between the menus and the buttons on the right.
					const float left = menusEnd + 12.0f, right = x - 12.0f;
					const float nameW = std::clamp(right - left - 2.0f * (button + 6.0f), 80.0f, 240.0f);
					const float groupW = nameW + 2.0f * (button + 6.0f);
					float gx = left + std::max(0.0f, (right - left - groupW) * 0.5f);
					ImGui::SetCursorScreenPos(ImVec2(origin.x + gx, origin.y + y));
					if (IconSquare("##prevTone", Glyph::ChevronLeft, false, "Previous saved tone.", button) && FileIdle(vst)) StepTone(vst, -1);
					gx += button + 6.0f;
					const ImVec2 box(origin.x + gx, origin.y + y);
					ImGui::SetCursorScreenPos(box);
					if (ImGui::InvisibleButton("##toneName", ImVec2(nameW, button))) ImGui::OpenPopup("##toneList");
					Hint("Your tones. Click to pick one; File has Save and Save as.");
					list->AddRectFilled(box, ImVec2(box.x + nameW, box.y + button), Ink::Field, 6.0f);
					list->AddRect(box, ImVec2(box.x + nameW, box.y + button), Ink::FieldEdge, 6.0f);
					const std::string name = vst.loading ? "Loading..." : vst.toneName.empty() ? (vst.slots.empty() ? "New tone" : "Unsaved tone") : BaseName(vst.toneName);
					const bool dot = HasUnsavedChanges(vst);
					const float textW = ImGui::CalcTextSize(name.c_str()).x + (dot ? 13.0f : 0.0f);
					const float tx = box.x + std::max(8.0f, (nameW - textW) * 0.5f);
					const float ty = box.y + (button - ImGui::GetFontSize()) * 0.5f;
					list->PushClipRect(box, ImVec2(box.x + nameW - 6.0f, box.y + button), true);
					list->AddText(ImVec2(tx, ty), IM_COL32_WHITE, name.c_str());
					list->PopClipRect();
					if (dot) list->AddCircleFilled(ImVec2(std::min(tx + textW - 3.5f, box.x + nameW - 8.0f), ty + ImGui::GetFontSize() * 0.55f), 3.5f, Ink::Amber, 12);
					gx += nameW + 6.0f;
					ImGui::SetCursorScreenPos(ImVec2(origin.x + gx, origin.y + y));
					if (IconSquare("##nextTone", Glyph::ChevronRight, false, "Next saved tone.", button) && FileIdle(vst)) StepTone(vst, +1);
					if (ImGui::BeginPopup("##toneList")) {
						const std::vector<std::string>& tones = VstHostLink::Tones();
						if (tones.empty()) ImGui::TextDisabled("No saved tones yet");
						for (const std::string& tone : tones)
							if (ImGui::MenuItem(tone.c_str(), nullptr, _stricmp(tone.c_str(), vst.toneName.c_str()) == 0) && FileIdle(vst)) RequestLoad(vst, tone);
						ImGui::Separator();
						if (ImGui::MenuItem("Save as...", nullptr, false, FileIdle(vst))) Ask(Dialog::SaveAs, vst.toneName);
						ImGui::EndPopup();
					}
					ImGui::EndMenuBar();
				}
				ImGui::PopStyleVar();
				ImGui::EndChild();
				ImGui::PopStyleColor(3);
			}

			std::vector<std::string> ToneFolders(const std::vector<std::string>& tones)
			{
				std::vector<std::string> folders;
				for (const std::string& tone : tones) {
					const std::string folder = FolderOf(tone);
					if (!folder.empty() && std::find(folders.begin(), folders.end(), folder) == folders.end()) folders.push_back(folder);
				}
				std::sort(folders.begin(), folders.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
				return folders;
			}

			void MoveToFolder(const std::string& tone, const std::string& folder)
			{
				const std::string to = folder.empty() ? BaseName(tone) : folder + "/" + BaseName(tone);
				if (to != tone) VstHostLink::RenameTone(tone, to);
				State& s = S();
				if (_stricmp(s.selectedTone.c_str(), tone.c_str()) == 0) s.selectedTone = to;
				for (std::string& selected : s.selectedTones) if (_stricmp(selected.c_str(), tone.c_str()) == 0) selected = to;
				if (_stricmp(s.toneAnchor.c_str(), tone.c_str()) == 0) s.toneAnchor = to;
			}

			// Right-click on a selection of several tones.
			void BatchToneMenu(const VstHostLink::Snapshot& vst)
			{
				State& s = S();
				const std::vector<std::string> batch = s.selectedTones;
				ImGui::TextDisabled("%d tones", static_cast<int>(batch.size()));
				ImGui::Separator();
				if (ImGui::BeginMenu("Move to folder", FileIdle(vst))) {
					if (ImGui::MenuItem("No folder")) for (const std::string& t : batch) MoveToFolder(t, {});
					ImGui::Separator();
					for (const std::string& folder : ToneFolders(VstHostLink::Tones()))
						if (ImGui::MenuItem(folder.c_str())) for (const std::string& t : batch) MoveToFolder(t, folder);
					ImGui::Separator();
					if (ImGui::MenuItem("New folder...")) { Ask(Dialog::NewFolder); s.dialogTones = batch; }
					ImGui::EndMenu();
				}
				bool allFavourite = true;
				for (const std::string& t : batch) allFavourite = allFavourite && VstHostLink::IsFavouriteTone(t);
				if (ImGui::MenuItem("Favourite", nullptr, allFavourite)) for (const std::string& t : batch) VstHostLink::SetFavouriteTone(t, !allFavourite);
				if (ImGui::MenuItem("Select none")) { s.selectedTones.clear(); s.toneAnchor.clear(); }
				ImGui::Separator();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
				if (ImGui::MenuItem(("Delete " + std::to_string(batch.size()) + " tones...").c_str(), nullptr, false, FileIdle(vst))) {
					Ask(Dialog::DeleteTones);
					s.dialogTones = batch;
				}
				ImGui::PopStyleColor();
			}

			// One row of the tone library: click loads, the star marks a favourite, right-click for the rest.
			// `section` keeps rows apart: a favourite is listed twice (Favourites and its own place), and two rows with
			// one ImGui ID share their click and hover, so neither could be selected.
			void ToneRow(const VstHostLink::Snapshot& vst, const std::string& tone, float width, const char* section)
			{
				ImGui::PushID(section);
				ImGui::PushID(tone.c_str());
				const bool current = _stricmp(tone.c_str(), vst.toneName.c_str()) == 0;
				const bool favourite = VstHostLink::IsFavouriteTone(tone);
				const float height = ImGui::GetFontSize() + 10.0f;
				const ImVec2 at = ImGui::GetCursorScreenPos();
				const bool pressed = ImGui::InvisibleButton("##tone", ImVec2(width, height));
				const bool hovered = ImGui::IsItemHovered();
				const bool onStar = ImGui::GetIO().MousePos.x > at.x + width - 24.0f;
				// Click selects, double-click loads (like the plugin browser); the star toggles a favourite.
				State& s = S();
				if (!InList(s.toneOrderNext, tone)) s.toneOrderNext.push_back(tone);
				if (pressed) {
					if (onStar) VstHostLink::SetFavouriteTone(tone, !favourite);
					else { SelectClick(s.selectedTones, s.toneAnchor, s.toneOrder, tone); s.selectedTone = tone; }
				}
				if (hovered && !onStar && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !current && FileIdle(vst)) RequestLoad(vst, tone);
				// Drag onto songs to give them this tone: over the Songs tab, the browser switches to it.
				if (ImGui::BeginDragDropSource()) {
					ImGui::SetDragDropPayload("RAB_TONE", tone.c_str(), tone.size() + 1);
					ImGui::Text("%s  (drop on songs)", BaseName(tone).c_str());
					ImGui::EndDragDropSource();
				}
				const bool selected = InList(s.selectedTones, tone);
				Hint(current ? "The tone playing now. Right-click for more. Ctrl or Shift+click to select several."
					: "Double-click to load it. Right-click for more; Ctrl or Shift+click to select several for a batch. The star keeps it at the top.");
				ImDrawList* list = ImGui::GetWindowDrawList();
				const ImVec2 end(at.x + width, at.y + height);
				if (selected) list->AddRectFilled(at, end, Ink::RowSelected, 5.0f);
				else if (hovered) list->AddRectFilled(at, end, Ink::RowHover, 5.0f);
				// The tone playing now: a green dot before its name.
				if (current) list->AddCircleFilled(ImVec2(at.x + 9.0f, at.y + height * 0.5f), 3.5f, Ink::Good, 12);
				list->PushClipRect(at, ImVec2(end.x - 24.0f, end.y), true);
				const std::string shown = BaseName(tone);
				list->AddText(ImVec2(at.x + 18.0f, at.y + 5.0f), current || selected ? IM_COL32_WHITE : IM_COL32(195, 200, 210, 255), shown.c_str());
				list->PopClipRect();
				if (_stricmp(vst.defaultTone.c_str(), tone.c_str()) == 0 && !hovered && !favourite) {
					ImFont* caption = CaptionFont();
					const float w = TextWidth(caption, "default");
					Text(list, caption, ImVec2(end.x - w - 6.0f, at.y + (height - caption->FontSize) * 0.5f), Ink::Dim, "default");
				}
				if (favourite || hovered)
					DrawIcon(list, ImVec2(end.x - 12.0f, at.y + height * 0.5f), favourite ? Glyph::StarFilled : Glyph::Star, favourite ? Ink::Amber : Ink::Dim);

				PushPopupPadding();
				if (ImGui::BeginPopupContextItem("##toneMenu")) {
					// Right-click outside the selection: that tone alone. On a selection of several: the batch.
					if (!InList(s.selectedTones, tone)) { s.selectedTones = { tone }; s.toneAnchor = tone; }
					if (s.selectedTones.size() > 1) BatchToneMenu(vst);
					else {
					s.selectedTone = tone;
					ImGui::TextDisabled("%s", tone.c_str());
					ImGui::Separator();
					if (ImGui::MenuItem("Load", "or double-click it", false, !current && FileIdle(vst))) RequestLoad(vst, tone);
					if (SongsShown() && !s.selectedSongs.empty()) {
						const std::string label = "Give to the " + (s.selectedSongs.size() == 1 ? std::string("selected song") : std::to_string(s.selectedSongs.size()) + " selected songs");
						if (ImGui::MenuItem(label.c_str())) for (const std::string& key : s.selectedSongs) VstHostLink::SetToneForSong(key, tone);
					}
					if (ImGui::MenuItem("Favourite", nullptr, favourite)) VstHostLink::SetFavouriteTone(tone, !favourite);
					const bool songTone = _stricmp(vst.songTone.c_str(), tone.c_str()) == 0;
					const bool isDefault = _stricmp(vst.defaultTone.c_str(), tone.c_str()) == 0;
					if (ImGui::MenuItem("Use as default tone", nullptr, isDefault))
						VstHostLink::SetDefaultTone(isDefault ? std::string() : tone);
					Hint("Loads whenever a song without a tone of its own starts.");
					if (ImGui::MenuItem("Use for the current song", nullptr, songTone, !vst.songKey.empty() && !songTone && !vst.busy)) {
						VstHostLink::SetSongTone(tone);
						if (!current) RequestLoad(vst, tone);
					}
					ImGui::Separator();
					if (ImGui::MenuItem("Rename...", nullptr, false, FileIdle(vst))) Ask(Dialog::Rename, tone);
					if (ImGui::BeginMenu("Move to folder", FileIdle(vst))) {
						if (ImGui::MenuItem("No folder", nullptr, FolderOf(tone).empty())) MoveToFolder(tone, {});
						ImGui::Separator();
						for (const std::string& folder : ToneFolders(VstHostLink::Tones()))
							if (ImGui::MenuItem(folder.c_str(), nullptr, folder == FolderOf(tone))) MoveToFolder(tone, folder);
						ImGui::Separator();
						if (ImGui::MenuItem("New folder...")) Ask(Dialog::NewFolder, tone);
						ImGui::EndMenu();
					}
					if (ImGui::MenuItem("Show in folder")) VstHostLink::OpenTonesFolder();
					ImGui::Separator();
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
					if (ImGui::MenuItem("Delete...", nullptr, false, FileIdle(vst))) Ask(Dialog::Delete, tone);
					ImGui::PopStyleColor();
					}
					ImGui::EndPopup();
				}
				ImGui::PopStyleVar();
				ImGui::PopID();
				ImGui::PopID();
			}

			// The browser's Tones tab: search, favourites, all tones, song tones.
			void ToneList(const VstHostLink::Snapshot& vst, float width, float height)
			{
				State& s = S();
				s.toneOrder.swap(s.toneOrderNext);
				s.toneOrderNext.clear();
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 2));
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
				ImGui::BeginChild("##toneLibrary", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
				ImGui::PopStyleColor();
				const float rowW = ImGui::GetContentRegionAvail().x;
				ImGui::SetNextItemWidth(rowW);
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
				ImGui::InputTextWithHint("##toneSearch", "Search tones", s.toneSearch, sizeof(s.toneSearch));
				ImGui::PopStyleVar();

				const std::vector<std::string>& tones = VstHostLink::Tones();
				auto matches = [&](const std::string& tone) {
					if (!s.toneSearch[0]) return true;
					const char* needle = s.toneSearch;
					return std::search(tone.begin(), tone.end(), needle, needle + strlen(needle),
						[](char a, char b) { return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b)); }) != tone.end();
				};
				bool anyFavourite = false;
				for (const std::string& tone : tones) if (VstHostLink::IsFavouriteTone(tone) && matches(tone)) { anyFavourite = true; break; }
				if (anyFavourite) {
					FoldLabel("Favourites");
					for (const std::string& tone : tones) if (VstHostLink::IsFavouriteTone(tone) && matches(tone)) ToneRow(vst, tone, rowW, "favourites");
				}
				FoldLabel(ToneFolders(tones).empty() ? "All tones" : "Tones");
				if (tones.empty()) {
					ImGui::PushFont(CaptionFont());
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowW);
					ImGui::TextUnformatted("None yet. File, Save as keeps the chain with every plugin's settings.");
					// Where they live, so a tone saved elsewhere can be copied in (installing never touches this folder).
					ImGui::TextUnformatted(("Saved in " + Utf8(VstHostLink::TonesFolder())).c_str());
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
					if (ImGui::SmallButton("Open tones folder")) VstHostLink::OpenTonesFolder();
					ImGui::PopFont();
				}
				for (const std::string& tone : tones) if (FolderOf(tone).empty() && matches(tone)) ToneRow(vst, tone, rowW, "tones");
				// Folders: a header that folds the folder away (click), its tones under it.
				for (const std::string& folder : ToneFolders(tones)) {
					size_t count = 0;
					for (const std::string& tone : tones) if (FolderOf(tone) == folder && matches(tone)) ++count;
					if (count == 0) continue;
					auto& folded = s.foldedFolders;
					const auto it = std::find(folded.begin(), folded.end(), folder);
					const bool open = it == folded.end();
					ImGui::PushID(folder.c_str());
					const ImVec2 at = ImGui::GetCursorScreenPos();
					const float headerH = ImGui::GetFontSize() + 8.0f;
					if (ImGui::InvisibleButton("##folder", ImVec2(rowW, headerH))) {
						if (open) folded.push_back(folder);
						else folded.erase(it);
					}
					const bool hovered = ImGui::IsItemHovered();
					ImGui::PopID();
					ImDrawList* list = ImGui::GetWindowDrawList();
					if (hovered) list->AddRectFilled(at, ImVec2(at.x + rowW, at.y + headerH), Ink::RowHover, 5.0f);
					const ImU32 color = hovered ? Ink::Text : Ink::Soft;
					const float cx = at.x + 8.0f, cy = at.y + headerH * 0.5f;
					if (open) list->AddTriangleFilled(ImVec2(cx - 4.0f, cy - 2.0f), ImVec2(cx + 4.0f, cy - 2.0f), ImVec2(cx, cy + 3.0f), color);
					else list->AddTriangleFilled(ImVec2(cx - 2.0f, cy - 4.0f), ImVec2(cx + 3.0f, cy), ImVec2(cx - 2.0f, cy + 4.0f), color);
					list->AddText(ImVec2(at.x + 18.0f, at.y + 4.0f), color, folder.c_str());
					ImFont* caption = CaptionFont();
					const std::string number = std::to_string(count);
					Text(list, caption, ImVec2(at.x + rowW - TextWidth(caption, number.c_str()) - 8.0f, at.y + (headerH - caption->FontSize) * 0.5f), Ink::Dim, number.c_str());
					if (open)
						for (const std::string& tone : tones) if (FolderOf(tone) == folder && matches(tone)) {
							ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
							ToneRow(vst, tone, rowW - 10.0f, "folders");
						}
				}
				FoldLabel("Song tones");
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
				const size_t songs = VstHostLink::SongToneCount();
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
				ImGui::TextUnformatted(songs == 0 ? "None yet" : songs == 1 ? "1 song" : (std::to_string(songs) + " songs").c_str());
				if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				if (ImGui::IsItemClicked()) {
					s.songFilter = songs == 0 ? 0 : 1;
					ShowBrowserTab(SongsTab);
				}
				Hint("Songs that load their own tone when they start. Click to see them (the Songs tab), or give more songs a tone there.");
				ImGui::PopStyleColor();

				// Right-click on the empty part of the column.
				PushPopupPadding();
				if (ImGui::BeginPopupContextWindow("##libraryMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
					ImGui::TextDisabled("Tones");
					ImGui::Separator();
					if (ImGui::MenuItem("New tone", nullptr, false, FileIdle(vst))) RequestNew(vst);
					if (ImGui::MenuItem("Save current as...", nullptr, false, FileIdle(vst))) Ask(Dialog::SaveAs, vst.toneName);
					if (ImGui::MenuItem("Open tones folder")) VstHostLink::OpenTonesFolder();
					ImGui::EndPopup();
				}
				ImGui::PopStyleVar();
				ImGui::EndChild();
				ImGui::PopStyleVar(2);
			}

			// A vertical segmented meter in a column `width` wide whose left edge is at `at.x`, centred on `at.y`:
			// green, then amber, then red at the top; all lit with a red top while it clips. Click for the levels.
			void MeterColumn(ImDrawList* list, ImVec2 at, float width, const char* label, float peak, const char* id, const VstHostLink::Snapshot& vst)
			{
				const float height = ToneDesignerUi::MeterHeight();
				const float labelTop = at.y - height * 0.5f;
				ImGui::SetCursorScreenPos(ImVec2(at.x, labelTop));
				if (ImGui::InvisibleButton(id, ImVec2(width, height))) Ask(Dialog::Levels);
				Hint(vst.outputPeak > 1.0f ? "The chain's output is clipping. Click to lower the output trim, or lower the last plugin's volume."
					: "Your guitar into the chain (IN) and the chain's output (OUT). Click for the levels.");
				ToneDesignerUi::MeterSegments(list, at.x, labelTop, width, label, peak);
			}

			// Over the rack while plugins load: which plugin, the step of a tone, the seconds so far, and a bar that keeps
			// moving. The overlay draws it, so it moves even while the host is busy inside a slow plugin's loader; a
			// load that is truly stuck shows as the seconds climbing (the page offers Force close once the host stops
			// answering).
			void LoadingPanel(const VstHostLink::Snapshot& vst)
			{
				ImDrawList* list = ImGui::GetWindowDrawList();
				const ImVec2 min = ImGui::GetWindowPos();
				const ImVec2 size = ImGui::GetWindowSize();
				const ImVec2 max(min.x + size.x, min.y + size.y);
				list->PushClipRect(min, max, false);
				list->AddRectFilled(min, max, IM_COL32(15, 17, 21, 215), 10.0f);
				ImFont* caption = CaptionFont();
				const float centerX = min.x + size.x * 0.5f;
				const float top = min.y + size.y * 0.5f - 34.0f;

				const std::string title = "Loading " + (vst.loadingName.empty() ? std::string("plugins") : vst.loadingName);
				const float titleW = ImGui::CalcTextSize(title.c_str()).x;
				list->AddText(ImVec2(centerX - titleW * 0.5f, top), IM_COL32_WHITE, title.c_str());

				char detail[96];
				const int seconds = static_cast<int>(vst.loadingSeconds);
				if (vst.loadingTotal > 1) sprintf_s(detail, "%d of %d, %d s", std::max(vst.loadingStep, 1), vst.loadingTotal, seconds);
				else sprintf_s(detail, "%d s", seconds);
				std::string line = detail;
				if (seconds >= 8) line += ". Big plugins (AmpliTube, Helix) can take a while";
				const float lineW = TextWidth(caption, line.c_str());
				Text(list, caption, ImVec2(centerX - lineW * 0.5f, top + ImGui::GetFontSize() + 6.0f), Ink::Muted, line.c_str());

				// The bar: the plugins done so far filled, and a block sweeping through the one loading now.
				const float barW = std::min(320.0f, size.x * 0.6f), barH = 6.0f;
				const ImVec2 bar(centerX - barW * 0.5f, top + ImGui::GetFontSize() + caption->FontSize + 20.0f);
				list->AddRectFilled(bar, ImVec2(bar.x + barW, bar.y + barH), Ink::Field, 3.0f);
				const int total = std::max(vst.loadingTotal, 1);
				const int done = std::clamp(vst.loadingStep - 1, 0, total);
				const float segment = barW / total;
				if (done > 0) list->AddRectFilled(bar, ImVec2(bar.x + segment * done, bar.y + barH), Ink::Blue, 3.0f);
				const float sweep = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
				const float blockW = std::max(segment * 0.35f, 12.0f);
				const float blockX = bar.x + segment * done + (segment - blockW) * sweep;
				list->AddRectFilled(ImVec2(blockX, bar.y), ImVec2(blockX + blockW, bar.y + barH), Ink::AddText, 3.0f);
				list->PopClipRect();
			}

			// Why the chain is quiet, for the line under the rack; nullptr while it plays.
			const char* BoardHint(const VstHostLink::Snapshot& vst)
			{
				if (vst.slots.empty()) return "The chain is empty, so you hear your clean guitar. Click Add, or drag a plugin from the browser.";
				if (vst.chainBypassed) return "Bypass all is on: you hear your clean guitar.";
				return nullptr;
			}

			// The Add tile's popover: search, the kinds as chips, the matches, and the way to the full browser.
			void AddPopover(const VstHostLink::Snapshot& vst)
			{
				State& s = S();
				if (ImGui::IsWindowAppearing()) { s.addSearch[0] = 0; ImGui::SetKeyboardFocusHere(); }
				ImGui::SetNextItemWidth(300.0f);
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
				ImGui::InputTextWithHint("##addSearch", "Search plugins or vendors", s.addSearch, sizeof(s.addSearch));
				ImGui::PopStyleVar();
				ImGui::PopStyleColor();
				static const char* const chips[] = { "All", "Favourites", "Amps", "Effects", "Dynamics" };
				for (int c = 0; c < 5; ++c) {
					if (c > 0) ImGui::SameLine(0.0f, 6.0f);
					if (Chip(chips[c], s.addFilter == c)) s.addFilter = c;
				}
				ImGui::Separator();
				int shown = 0;
				for (const VstHostLink::Plugin& plugin : VstHostLink::Plugins()) {
					if (plugin.kind == VstHostLink::Kind::Instrument || !MatchesFilter(plugin, s.addFilter) || !MatchesSearch(plugin, s.addSearch)) continue;
					if (++shown > 10) break;
					if (ImGui::MenuItem(plugin.name.c_str(), plugin.vendor.c_str(), false, !vst.busy)) {
						if (s.addAt < 0) VstHostLink::AddPlugin(plugin.path);
						else { VstHostLink::AddPlugin(plugin.path, s.addAt); s.selected = s.addAt; }
					}
				}
				if (shown == 0) ImGui::TextDisabled(VstHostLink::Plugins().empty() ? "No 64-bit VST3 plugins found" : "Nothing matches");
				if (shown > 10) ImGui::TextDisabled("More in the Plugins tab");
				ImGui::Separator();
				if (ImGui::MenuItem("Open the Plugins tab")) ShowBrowserTab(PluginsTab);
			}

			// The rack's items in order, each one's width at scale `sc`: In, the pedals, Add (while the chain has room), Out.
			std::vector<float> RackWidths(const VstHostLink::Snapshot& vst, float sc)
			{
				std::vector<float> widths{ std::floor(ToneDesignerUi::JACK_WIDTH * sc) };
				for (const VstHostLink::SlotView& slot : vst.slots) {
					const VstHostLink::Plugin* plugin = FindPlugin(slot.path);
					widths.push_back(ToneDesignerUi::PedalWidth(LookFor(plugin ? plugin->kind : VstHostLink::Kind::Unknown), sc));
				}
				if (vst.slots.size() < static_cast<size_t>(VstHostControl::kMaxSlots)) widths.push_back(std::floor(ToneDesignerUi::ADD_TILE_WIDTH * sc));
				widths.push_back(std::floor(ToneDesignerUi::JACK_WIDTH * sc));
				return widths;
			}

			// The rack's height on a board `boardWidth` wide: as many rows as the chain wraps into (Panel sizes the strip by it).
			float RackHeight(const VstHostLink::Snapshot& vst, float boardWidth)
			{
				const float sc = ToneDesignerUi::BoardScale();
				return ToneDesignerUi::RackLayout(RackWidths(vst, sc), ToneDesignerUi::RackInnerWidth(boardWidth, IsOpen(Meters)), sc).height;
			}

			// The rack: In, the pedals joined by cables, Add, Out, wrapping into rows when the chain is longer than the
			// board is wide (RackLayout). The pedals' size is View, Board size (Ctrl+wheel on the board).
			void Board(const VstHostLink::Snapshot& vst, float width, float height)
			{
				State& s = S();
				const int count = static_cast<int>(vst.slots.size());
				if (s.selected >= count) s.selected = count - 1;
				if (s.selected < 0 && vst.shownSlot >= 0 && vst.shownSlot < count) s.selected = vst.shownSlot;

				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 14));
				ImGui::BeginChild("##board", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);
				const float boardW = ImGui::GetContentRegionAvail().x;
				// One line under the rack, only when the chain is quiet and why; the how-to lives in the tooltips now.
				const char* boardHint = BoardHint(vst);
				const float hintH = boardHint ? CaptionFont()->FontSize + ImGui::GetStyle().ItemSpacing.y + 4.0f : 0.0f;
				const float rackH = std::max(1.0f, ImGui::GetContentRegionAvail().y - hintH);
				const ImVec2 board = ImGui::GetCursorScreenPos();
				ImDrawList* outer = ImGui::GetWindowDrawList();
				ToneDesignerUi::DrawRack(outer, board, boardW, rackH);
				const float sc = ToneDesignerUi::BoardScale();
				const float pedalH = ToneDesignerUi::PedalHeight(sc);

				// IN and OUT meters stand at the board's two ends, beside the first row (View > Meters hides them).
				// Click either for the levels.
				const float meterCol = IsOpen(Meters) ? ToneDesignerUi::METER_COLUMN_WIDTH : 0.0f;
				if (meterCol > 0.0f) {
					const float midY = board.y + 12.0f + pedalH * 0.5f;
					MeterColumn(outer, ImVec2(board.x, midY), meterCol, "IN", vst.inputPeak, "##inMeter", vst);
					MeterColumn(outer, ImVec2(board.x + boardW - meterCol, midY), meterCol, "OUT", vst.outputPeak, "##outMeter", vst);
				}
				const float rackW = boardW - 2.0f * meterCol;
				ImGui::SetCursorScreenPos(ImVec2(board.x + meterCol, board.y));

				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ToneDesignerUi::RACK_INSET, 12));
				ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
				ImGui::BeginChild("##rack", ImVec2(rackW, rackH), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
				ImDrawList* list = ImGui::GetWindowDrawList();
				ImFont* caption = CaptionFont();
				const ImVec2 rackStart = ImGui::GetCursorScreenPos();
				const std::vector<float> widths = RackWidths(vst, sc);
				const ToneDesignerUi::RackPlacement placement = ToneDesignerUi::RackLayout(widths, ToneDesignerUi::RackInnerWidth(width, IsOpen(Meters)), sc);
				auto itemAt = [&](size_t item) { return ImVec2(rackStart.x + placement.at[item].x, rackStart.y + placement.at[item].y); };
				bool overTile = false;   // the mouse is on a pedal (its menu, not the board's)

				ToneDesignerUi::DrawJack(list, itemAt(0), "In", sc);
				// The gaps before each pedal and after the last: drop targets that insert there (see the drop
				// handling after the Add tile). Each spans the cable into the pedal, a little into the pedal too.
				std::vector<ImRect> gaps;
				auto gapBefore = [&](ImVec2 at) { gaps.emplace_back(ImVec2(at.x - 26.0f * sc, at.y), ImVec2(at.x + 6.0f * sc, at.y + pedalH)); };
				for (int i = 0; i < count; ++i) {
					const size_t item = static_cast<size_t>(i) + 1;
					ToneDesignerUi::DrawCable(list, rackStart, placement, widths, item, sc);
					const VstHostLink::SlotView& slot = vst.slots[i];
					const VstHostLink::Plugin* plugin = FindPlugin(slot.path);
					const PedalLook look = LookFor(plugin ? plugin->kind : VstHostLink::Kind::Unknown);
					const bool playing = !slot.bypass && !vst.chainBypassed;
					const bool selected = i == s.selected;
					const ImVec2 at = itemAt(item);
					gapBefore(at);

					ImGui::PushID(i);
					ImGui::SetCursorScreenPos(at);
					const bool pressed = ImGui::InvisibleButton("##pedal", ImVec2(widths[item], pedalH));
					const bool hovered = ImGui::IsItemHovered();
					overTile |= hovered;
					const bool onSwitch = ToneDesignerUi::IsOnFootswitch(at, look, ImGui::GetIO().MousePos, sc);
					if (pressed) {
						if (onSwitch) { if (!vst.busy) VstHostLink::SetBypass(i, !slot.bypass); }
						else s.selected = i;
					}
					if (hovered && !onSwitch && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenWindow(i);
					Hint(onSwitch ? (slot.bypass ? "Footswitch: turn it on." : "Footswitch: bypass it.")
						: "Double-click to open its window. Right-click for more. Drag to move it.");
					if (ImGui::BeginDragDropSource()) {
						ImGui::SetDragDropPayload("RAB_PEDAL", &i, sizeof(i));
						ImGui::TextUnformatted(slot.name.c_str());
						ImGui::EndDragDropSource();
					}
					if (ImGui::BeginDragDropTarget()) {
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAB_PEDAL")) {
							const int from = *static_cast<const int*>(payload->Data);
							if (from != i && !vst.busy) MovePedal(from, i);
						}
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAB_PLUGIN"))
							if (!vst.busy) VstHostLink::ReplaceSlot(i, static_cast<const PluginDrag*>(payload->Data)->path);
						ImGui::EndDragDropTarget();
					}
					PushPopupPadding();
					if (ImGui::BeginPopupContextItem("##pedalMenu")) {
						s.selected = i;
						ImGui::TextDisabled("%s", slot.name.c_str());
						ImGui::Separator();
						PedalActions(vst, i);
						ImGui::EndPopup();
					}
					ImGui::PopStyleVar();

					const std::string sub = plugin && !plugin->vendor.empty() ? plugin->vendor : (slot.bypass ? "bypassed" : "");
					ToneDesignerUi::PedalTile tile;
					tile.name = slot.name.c_str();
					tile.sub = sub.c_str();
					tile.look = look;
					tile.symbol = SymbolFor(plugin ? plugin->kind : VstHostLink::Kind::Unknown);
					tile.isPlaying = playing;
					tile.isSelected = selected;
					tile.isHovered = hovered;
					tile.isOnSwitch = onSwitch;
					if (slot.windowOpen) {   // its plugin window is open
						tile.badge = Glyph::NewWindow;
						tile.badgeColor = vst.popoutWanted ? Ink::AddText : Ink::Dim;
					}
					ToneDesignerUi::DrawPedalTile(list, at, tile, sc);
					// Its window was asked for and is not open yet: dim the pedal and turn a spinner over it.
					if (i == g_openingSlot) {
						if (slot.windowOpen || ImGui::GetTime() - g_openingSince > 15.0) g_openingSlot = -1;
						else ToneDesignerUi::DrawTileBusy(list, at, widths[item], "Opening", sc);
					}
					ImGui::PopID();
				}

				// Add: a dashed tile, opening the plugin list. The gap before it takes a drop at the end of the chain.
				size_t item = static_cast<size_t>(count) + 1;
				if (count < VstHostControl::kMaxSlots) {
					ToneDesignerUi::DrawCable(list, rackStart, placement, widths, item, sc);
					const ImVec2 at = itemAt(item);
					gapBefore(at);
					ImGui::SetCursorScreenPos(at);
					ImGui::BeginDisabled(vst.busy);
					if (ImGui::InvisibleButton("##addPedal", ImVec2(widths[item], pedalH))) { s.addAt = -1; ImGui::OpenPopup("##addMenu"); }
					const bool hovered = ImGui::IsItemHovered();
					ImGui::EndDisabled();
					Hint("Add a plugin at the end of the chain, or drag one from the browser's Plugins tab onto the board.");
					if (ImGui::BeginDragDropTarget()) {
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAB_PLUGIN"))
							if (!vst.busy) VstHostLink::AddPlugin(static_cast<const PluginDrag*>(payload->Data)->path);
						ImGui::EndDragDropTarget();
					}
					ToneDesignerUi::DrawAddTile(list, at, hovered, sc);
					++item;
				}
				else gapBefore(itemAt(item));

				// Dropping into a gap inserts there: a dragged pedal moves to that place (the pedals after it shift
				// along, rather than two pedals swapping), a plugin from the browser goes in at that slot. While
				// something is dragged, every gap shows a faint marker and the one under the mouse a bright bar.
				if (const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
					dragging && (dragging->IsDataType("RAB_PEDAL") || dragging->IsDataType("RAB_PLUGIN"))) {
					for (int g = 0; g < static_cast<int>(gaps.size()); ++g) {
						const ImRect& gap = gaps[g];
						const float gx = gap.Min.x + 14.0f;
						// A pedal's own two sides are no move: no marker there.
						const int from = dragging->IsDataType("RAB_PEDAL") ? *static_cast<const int*>(dragging->Data) : -1;
						if (from >= 0 && (g == from || g == from + 1)) continue;
						if (from < 0 && count >= VstHostControl::kMaxSlots) continue;   // a full chain takes no more
						list->AddLine(ImVec2(gx, gap.Min.y + 18.0f), ImVec2(gx, gap.Max.y - 18.0f), Ink::Jack, 2.0f);
						if (!ImGui::BeginDragDropTargetCustom(gap, ImGui::GetID(("##gap" + std::to_string(g)).c_str())))
							continue;
						const ImGuiDragDropFlags peek = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAB_PEDAL", peek)) {
							list->AddLine(ImVec2(gx, gap.Min.y), ImVec2(gx, gap.Max.y), Ink::Outline, 4.0f);
							if (payload->IsDelivery() && !vst.busy) MovePedal(from, g > from ? g - 1 : g);
						}
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAB_PLUGIN", peek)) {
							list->AddLine(ImVec2(gx, gap.Min.y), ImVec2(gx, gap.Max.y), Ink::Outline, 4.0f);
							if (payload->IsDelivery() && !vst.busy && count < VstHostControl::kMaxSlots) {
								VstHostLink::AddPlugin(static_cast<const PluginDrag*>(payload->Data)->path, g);
								S().selected = g;
							}
						}
						ImGui::EndDragDropTarget();
					}
				}
				ToneDesignerUi::DrawCable(list, rackStart, placement, widths, item, sc);
				ToneDesignerUi::DrawJack(list, itemAt(item), "Out", sc);
				// The empty part of the board: one item behind the pedals (submitted last, so the pedals keep the
				// hover), for its right-click menu.
				ImGui::SetCursorScreenPos(rackStart);
				ImGui::InvisibleButton("##board", ImVec2(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(placement.height, 1.0f)));
				// Not over a pedal: IsItemHovered here is true under the pedals too (ImGui 1.85 checks only the
				// rectangle), and opening this menu after a pedal's would replace the pedal's menu. On the cable between
				// two pedals (a gap) it is that gap's menu instead: Add here puts a plugin in at that place.
				if (!overTile && ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
					s.gapMenu = -1;
					const ImVec2 mouse = ImGui::GetIO().MousePos;
					for (int g = 0; g < static_cast<int>(gaps.size()) && g < count; ++g)
						if (gaps[g].Contains(mouse)) s.gapMenu = g;
					ImGui::OpenPopup(s.gapMenu >= 0 ? "##gapMenu" : "##boardMenu");
				}
				// The open gap's marker, a bright bar while its menu is up.
				if (s.gapMenu >= 0 && s.gapMenu < static_cast<int>(gaps.size()) && ImGui::IsPopupOpen("##gapMenu")) {
					const float gx = gaps[s.gapMenu].Min.x + 14.0f;
					list->AddLine(ImVec2(gx, gaps[s.gapMenu].Min.y), ImVec2(gx, gaps[s.gapMenu].Max.y), Ink::Outline, 4.0f);
				}

				// The chain wraps instead of scrolling, so the wheel is free: Ctrl+wheel changes the pedals' size.
				ToneDesignerUi::BoardSizeWheel();
				if (vst.loading) LoadingPanel(vst);

				PushPopupPadding();
				if (ImGui::BeginPopup("##addMenu")) {
					AddPopover(vst);
					ImGui::EndPopup();
				}
				const bool gapOpen = ImGui::BeginPopup("##gapMenu");
				if (gapOpen || ImGui::BeginPopup("##boardMenu")) {
					const int at = gapOpen ? s.gapMenu : -1;
					if (ImGui::BeginMenu(gapOpen ? "Add here" : "Add plugin", count < VstHostControl::kMaxSlots && !vst.busy)) {
						AddByKindMenu(vst, at);
						ImGui::EndMenu();
					}
					ImGui::Separator();
					if (ImGui::MenuItem("Bypass whole chain", nullptr, vst.chainBypassed, count > 0)) VstHostLink::SetChainBypass(!vst.chainBypassed);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Bad));
					if (ImGui::MenuItem("Clear chain", nullptr, false, count > 0 && !vst.busy)) VstHostLink::ClearChain();   // one undo step; New tone (File) is the fresh start
					ImGui::PopStyleColor();
					ImGui::EndPopup();
				}
				ImGui::PopStyleVar();
				ImGui::EndChild();
				ImGui::PopStyleVar(2);

				// Under the rack: why it is quiet.
				if (boardHint) {
					ImGui::PushFont(caption);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Dim));
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + boardW);
					ImGui::TextUnformatted(boardHint);
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
					ImGui::PopFont();
				}
				ImGui::EndChild();
				ImGui::PopStyleVar();
			}

			bool MatchesSearch(const VstHostLink::Plugin& plugin, const char* search)
			{
				if (!search[0]) return true;
				auto contains = [&](const std::string& text) {
					return std::search(text.begin(), text.end(), search, search + strlen(search),
						[](char a, char b) { return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b)); }) != text.end();
				};
				return contains(plugin.name) || contains(plugin.vendor);
			}

			bool MatchesFilter(const VstHostLink::Plugin& plugin, int filter)
			{
				switch (filter) {
				case Favourites: return VstHostLink::IsFavourite(plugin.path);
				case Amps: return plugin.kind == VstHostLink::Kind::Amp;
				case Effects: return plugin.kind == VstHostLink::Kind::Effect || plugin.kind == VstHostLink::Kind::Unknown;
				case Dynamics: return plugin.kind == VstHostLink::Kind::Dynamics;
				case Instruments: return plugin.kind == VstHostLink::Kind::Instrument;
				default: return true;
				}
			}

			// A plugin's status is two separate things, each its own pill: where it stands with the chain (in it,
			// ready to add, or why not), and whether a copy is kept loaded outside it for fast tone changes. One
			// word for both hid "In chain" behind "Always loaded".
			PillLook ChainPill(const VstHostLink::Snapshot& vst, const VstHostLink::Plugin& plugin)
			{
				if (VstHostLink::FailedToLoad(plugin.path)) return TintPill("Failed to load", Ink::Bad);
				if (InChain(vst, plugin.path)) return TintPill("In chain", Ink::Good);
				if (plugin.kind == VstHostLink::Kind::Instrument) return TintPill("Not for guitar", Ink::Amber);
				if (Loading(vst, plugin.path)) return TintPill("Loading...", Ink::Amber);
				return ToneDesignerUi::NeutralPill("Ready");
			}

			bool MemoryPill(const VstHostLink::Snapshot& vst, const VstHostLink::Plugin& plugin, PillLook& look)
			{
				// "Always keep loaded" on a plugin in the chain: the chain's copy is the loaded one (the host keeps no
				// spare copy beside it), so it is loaded now, not waiting. "Loads soon" is only for one that is not
				// loaded yet: the host loads always-loaded plugins in the background outside songs.
				const bool always = Listed(vst.alwaysLoaded, plugin.path);
				if (KeptLoaded(vst, plugin.path) || (always && InChain(vst, plugin.path)))
					look = { always ? "Always loaded" : "Loaded", Ink::Blue, Ink::Blue, IM_COL32_WHITE };
				else if (always) look = TintPill(Listed(vst.preloadFailed, plugin.path) ? "Load failed" : "Loads soon", Ink::Amber);
				else return false;
				return true;
			}

			// Why a starred plugin shows "Load failed". Marks from before the host saved a reason only ever came from
			// the host closing mid-load, so that is the answer for those too.
			std::string PreloadFailedWhy(const VstHostLink::Snapshot& vst, const std::wstring& path)
			{
				for (const auto& [failedPath, why] : vst.preloadFailedWhy)
					if (_wcsicmp(failedPath.c_str(), path.c_str()) == 0 && !why.empty()) return why;
				return "The plugin host closed while this loaded in the background, most likely because the plugin crashed it.";
			}

			// The status column's two pill widths: each as wide as its longest label, so every row's pills line up.
			float ChainPillWidth(ImFont* font)
			{
				float w = 0.0f;
				for (const char* t : { "In chain", "Ready", "Not for guitar", "Failed to load", "Loading..." }) w = std::max(w, PillWidth(font, t));
				return w;
			}
			float MemoryPillWidth(ImFont* font)
			{
				float w = 0.0f;
				for (const char* t : { "Always loaded", "Loaded", "Loads soon", "Load failed" }) w = std::max(w, PillWidth(font, t));
				return w;
			}

			// Tone choices for a menu: a search box, then tones outside folders and a submenu per folder. While the
			// search has text, every matching tone is listed flat, folder included ("Tests/Test 1").
			template <typename Pick> void ToneChoices(Pick pick)
			{
				const std::vector<std::string>& tones = VstHostLink::Tones();
				if (tones.empty()) { ImGui::TextDisabled("No saved tones yet"); return; }
				char* search = S().pickSearch;
				if (ImGui::IsWindowAppearing()) { search[0] = 0; ImGui::SetKeyboardFocusHere(); }
				ImGui::SetNextItemWidth(std::max(180.0f, ImGui::GetContentRegionAvail().x));
				ImGui::InputTextWithHint("##pickSearch", "Search tones", search, sizeof(S().pickSearch));
				if (search[0]) {
					const std::string needle = search;
					bool any = false;
					for (const std::string& tone : tones) {
						const bool hit = std::search(tone.begin(), tone.end(), needle.begin(), needle.end(),
							[](char x, char y) { return tolower(static_cast<unsigned char>(x)) == tolower(static_cast<unsigned char>(y)); }) != tone.end();
						if (!hit) continue;
						any = true;
						if (ImGui::MenuItem(tone.c_str())) pick(tone);
					}
					if (!any) ImGui::TextDisabled("No tone matches");
					return;
				}
				for (const std::string& tone : tones) if (FolderOf(tone).empty() && ImGui::MenuItem(tone.c_str())) pick(tone);
				for (const std::string& folder : ToneFolders(tones))
					if (ImGui::BeginMenu(folder.c_str())) {
						for (const std::string& tone : tones) if (FolderOf(tone) == folder && ImGui::MenuItem(BaseName(tone).c_str())) pick(tone);
						ImGui::EndMenu();
					}
			}

			// The browser's Songs tab: every song in the library (SongLibrary) with its tone and how long its tone took to
			// load when it last started (VstHostLink::SongLoads, the audit). Select songs (Ctrl or Shift for several),
			// then drag a tone from the Tones tab onto them (switch tabs mid-drag), right-click, or use the buttons below.
			// Each row is the title over the artist, with the song's tone as a tag on the right.
			void SongList(const VstHostLink::Snapshot& vst, float width, float height)
			{
				State& s = S();
				SongLibrary::Refresh();
				const std::shared_ptr<const SongLibrary::Library> library = SongLibrary::Get();
				const std::vector<std::pair<std::string, std::string>> songTones = VstHostLink::SongToneList();
				const std::vector<std::pair<std::string, VstHostLink::SongLoad>> loads = VstHostLink::SongLoads();
				auto toneOf = [&](const std::string& key) -> std::string {
					for (const auto& entry : songTones) if (_stricmp(entry.first.c_str(), key.c_str()) == 0) return entry.second;
					return {};
				};
				auto loadOf = [&](const std::string& key) -> const VstHostLink::SongLoad* {
					for (const auto& entry : loads) if (_stricmp(entry.first.c_str(), key.c_str()) == 0) return &entry.second;
					return nullptr;
				};
				auto slow = [](const VstHostLink::SongLoad* load) { return load && load->seconds >= 3.0f && !load->plugins.empty(); };
				auto playing = [&](const std::string& key) { return !vst.songKey.empty() && _stricmp(key.c_str(), vst.songKey.c_str()) == 0; };
				// The song playing now goes through SetSongTone / ClearSongTone as well, so its tone loads at once (or
				// the one playing stays when it is taken away).
				auto assignTo = [&](const std::vector<std::string>& keys, const std::string& tone) {
					for (const std::string& key : keys) {
						if (!playing(key)) VstHostLink::SetToneForSong(key, tone);
						else if (tone.empty()) VstHostLink::ClearSongTone();
						else {
							VstHostLink::SetSongTone(tone);
							if (_stricmp(tone.c_str(), vst.toneName.c_str()) != 0) RequestLoad(vst, tone);
						}
					}
				};

				ImFont* caption = CaptionFont();
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
				ImGui::BeginChild("##songColumn", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
				ImGui::PopStyleVar();
				const float inner = ImGui::GetContentRegionAvail().x;

				ImGui::SetNextItemWidth(inner);
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
				ImGui::InputTextWithHint("##songSearch", "Search songs, artists or albums", s.songSearch, sizeof(s.songSearch));
				ImGui::PopStyleVar();
				ImGui::PopStyleColor();

				// The count, or what the tones' plugins are doing in the background (amber while a change waits for it).
				const SongLibrary::State state = SongLibrary::GetState();
				const std::string count = !vst.preloadingName.empty()
					? (vst.waitingOnBackground ? "Waiting for " + vst.preloadingName + " to load..." : "Getting " + vst.preloadingName + " ready...")
					: state == SongLibrary::State::Reading && library->songs.empty() ? std::string("Reading your songs...")
					: std::to_string(library->songs.size()) + " songs, " + std::to_string(songTones.size()) + " with a tone";
				ImGui::PushFont(caption);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(vst.waitingOnBackground && !vst.preloadingName.empty() ? Ink::Amber : Ink::Dim));
				ImGui::TextUnformatted(count.c_str());
				ImGui::PopStyleColor();
				ImGui::PopFont();

				enum { AllSongs, WithTone, SlowToLoad, SongFilterCount };
				static const char* const chips[SongFilterCount] = { "All", "With a tone", "Slow to load" };
				const float left = ImGui::GetCursorPosX();
				for (int c = 0; c < SongFilterCount; ++c) {
					if (c > 0) {
						ImGui::SameLine(0.0f, 6.0f);
						if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(chips[c]).x + 20.0f > left + inner) ImGui::NewLine();
					}
					if (Chip(chips[c], s.songFilter == c)) s.songFilter = c;
					if (c == SlowToLoad) Hint("Songs whose tone waited 3 s or more for plugins to load when they last started. Always keep loaded (right-click the plugin in the Plugins tab) makes them instant; songs you rarely play can stay as they are.");
				}
				// Second row: the arrangements a song must have (any lit one; none lit is every song), then the order.
				static const char* const parts[3] = { "Lead", "Rhythm", "Bass" };
				for (int part = 0; part < 3; ++part) {
					if (part > 0) ImGui::SameLine(0.0f, 6.0f);
					const unsigned bit = 1u << part;
					if (Chip(parts[part], (s.songParts & bit) != 0)) s.songParts ^= bit;
				}
				Hint("Only songs with that arrangement. Light several for any of them; none lit shows every song.");
				static const char* const sorts[SongSortCount] = { "Artist", "Title", "Year" };
				const std::string sortLabel = std::string("Sort: ") + sorts[s.songSort];
				ImGui::SameLine(0.0f, 6.0f);
				if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(sortLabel.c_str()).x + 20.0f > left + inner) ImGui::NewLine();
				if (Chip(sortLabel.c_str(), false)) ImGui::OpenPopup("##songSort");
				PushPopupPadding();
				if (ImGui::BeginPopup("##songSort")) {
					for (int o = 0; o < SongSortCount; ++o) if (ImGui::MenuItem(sorts[o], nullptr, s.songSort == o)) s.songSort = o;
					ImGui::EndPopup();
				}
				ImGui::PopStyleVar();
				ImGui::Dummy(ImVec2(0, 2));

				// Rows: the library's songs, and song keys with a tone or a load record the library does not have. Sorted
				// by artist, each artist's songs come under a heading row (`isHeading`, no song).
				struct Row { std::string key, title, artist; const SongLibrary::Song* song; bool isHeading = false; int count = 0; };
				std::vector<Row> rows;
				auto has = [&](const std::string& text) {
					const std::string needle = s.songSearch;
					return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
						[](char x, char y) { return tolower(static_cast<unsigned char>(x)) == tolower(static_cast<unsigned char>(y)); }) != text.end();
				};
				auto matches = [&](const std::string& title, const std::string& artist, const std::string& album, const std::string& key) {
					return !s.songSearch[0] || has(title) || has(artist) || has(album) || has(key);
				};
				auto hasPart = [&](const SongLibrary::Song* song) {
					if (s.songParts == 0) return true;
					if (!song) return false;
					for (int part = 0; part < 3; ++part)
						if ((s.songParts & (1u << part)) && std::search(song->arrangements.begin(), song->arrangements.end(), parts[part], parts[part] + strlen(parts[part]),
							[](char x, char y) { return tolower(static_cast<unsigned char>(x)) == tolower(static_cast<unsigned char>(y)); }) != song->arrangements.end())
							return true;
					return false;
				};
				auto wanted = [&](const std::string& key) {
					if (s.songFilter == WithTone) return !toneOf(key).empty();
					if (s.songFilter == SlowToLoad) return slow(loadOf(key));
					return true;
				};
				for (const SongLibrary::Song& song : library->songs)
					if (wanted(song.key) && hasPart(&song) && matches(song.title, song.artist, song.album, song.key)) rows.push_back({ song.key, song.title, song.artist, &song });
				auto addOrphan = [&](const std::string& key) {
					if (library->Find(key) || std::any_of(rows.begin(), rows.end(), [&](const Row& r) { return _stricmp(r.key.c_str(), key.c_str()) == 0; })) return;
					if (wanted(key) && hasPart(nullptr) && matches(key, std::string(), std::string(), key)) rows.push_back({ key, key, std::string(), nullptr });
				};
				for (const auto& entry : songTones) addOrphan(entry.first);
				for (const auto& entry : loads) addOrphan(entry.first);
				auto less = [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; };
				if (s.songFilter == SlowToLoad)
					std::sort(rows.begin(), rows.end(), [&](const Row& x, const Row& y) { return loadOf(x.key)->seconds > loadOf(y.key)->seconds; });
				else if (s.songSort == ByTitle)
					std::stable_sort(rows.begin(), rows.end(), [&](const Row& x, const Row& y) { return less(x.title, y.title); });
				else if (s.songSort == ByYear)
					std::stable_sort(rows.begin(), rows.end(), [&](const Row& x, const Row& y) { return (x.song ? x.song->year : 0) > (y.song ? y.song->year : 0); });
				else
					std::stable_sort(rows.begin(), rows.end(), [&](const Row& x, const Row& y) {
						const int artist = _stricmp(x.artist.c_str(), y.artist.c_str());
						return artist != 0 ? artist < 0 : less(x.title, y.title);
					});
				// The song playing now goes first, with a green dot: its tone is changed here (right-click it).
				std::stable_partition(rows.begin(), rows.end(), [&](const Row& r) { return playing(r.key); });
				std::vector<std::string> order;
				order.reserve(rows.size());
				for (const Row& row : rows) order.push_back(row.key);
				// Artist headings, in the order sorted by artist (not under the song playing now, which leads on its own).
				if (s.songSort == ByArtist && s.songFilter != SlowToLoad) {
					std::vector<Row> grouped;
					grouped.reserve(rows.size() + rows.size() / 4);
					for (size_t r = 0; r < rows.size(); ++r) {
						if (!playing(rows[r].key) && !rows[r].artist.empty() && (r == 0 || _stricmp(rows[r].artist.c_str(), rows[r - 1].artist.c_str()) != 0 || playing(rows[r - 1].key))) {
							Row heading{ std::string(), std::string(), rows[r].artist, nullptr, true, 0 };
							for (size_t n = r; n < rows.size() && _stricmp(rows[n].artist.c_str(), rows[r].artist.c_str()) == 0; ++n) ++heading.count;
							grouped.push_back(heading);
						}
						grouped.push_back(rows[r]);
					}
					rows.swap(grouped);
				}

				// The foot: actions while songs are selected (a line and a row of buttons), a short how-to otherwise.
				const float itemGap = ImGui::GetStyle().ItemSpacing.y;
				const float buttonH = ImGui::GetFontSize() + 14.0f;
				const float footH = s.selectedSongs.empty() ? caption->FontSize * 2.0f + 2.0f * itemGap + 8.0f : caption->FontSize + buttonH + 3.0f * itemGap + 8.0f;
				const float rowH = ImGui::GetFontSize() + caption->FontSize + 12.0f;
				const float listH = std::max(ImGui::GetContentRegionAvail().y - footH, rowH * 2.0f);
				if (rows.empty()) {
					ImGui::BeginChild("##songRows", ImVec2(inner, listH), false);
					MutedCaption(state == SongLibrary::State::Failed && library->songs.empty()
						? "Your songs could not be read: RSMods.exe needs to be in the game's RSMods folder (the installer's Repair puts it back)."
						: state == SongLibrary::State::Reading && library->songs.empty() ? "Reading your songs. The first time takes a few seconds, longer for a big library."
						: s.songFilter == WithTone ? "No song has a tone of its own yet. Select songs, then give them a tone."
						: s.songFilter == SlowToLoad ? "No song has waited for its tone. Songs show here after they start."
						: "Nothing matches.", inner);
					ImGui::EndChild();
				}
				else {
					ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
					ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
					ImGui::BeginChild("##songRows", ImVec2(inner, listH), false);
					ImDrawList* rowsList = ImGui::GetWindowDrawList();
					const float rowW = ImGui::GetContentRegionAvail().x;
					ImGuiListClipper clipper;   // thousands of songs: only the visible rows are drawn
					clipper.Begin(static_cast<int>(rows.size()), rowH);
					while (clipper.Step()) {
						for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
							const Row& row = rows[static_cast<size_t>(r)];
							if (row.isHeading) {
								// An artist's heading: the name in capitals' place, its song count after it.
								const ImVec2 at = ImGui::GetCursorScreenPos();
								ImGui::Dummy(ImVec2(rowW, rowH));
								const float y = at.y + rowH - ImGui::GetFontSize() - 4.0f;
								rowsList->AddText(ImVec2(at.x + 4.0f, y), Ink::AddText, row.artist.c_str());
								const std::string number = std::to_string(row.count);
								Text(rowsList, caption, ImVec2(at.x + rowW - TextWidth(caption, number.c_str()) - 8.0f, y + 2.0f), Ink::Dim, number.c_str());
								rowsList->AddLine(ImVec2(at.x + 4.0f, at.y + rowH - 1.0f), ImVec2(at.x + rowW - 4.0f, at.y + rowH - 1.0f), Ink::Edge);
								continue;
							}
							ImGui::PushID(row.key.c_str());
							const bool selected = InList(s.selectedSongs, row.key);
							const ImVec2 at = ImGui::GetCursorScreenPos();
							if (ImGui::InvisibleButton("##song", ImVec2(rowW, rowH))) SelectClick(s.selectedSongs, s.songAnchor, order, row.key);
							const bool hovered = ImGui::IsItemHovered();
							const std::string tone = toneOf(row.key);
							const VstHostLink::SongLoad* load = loadOf(row.key);
							// A tone dragged from the Tones tab: onto a selected song, the whole selection gets it.
							if (ImGui::BeginDragDropTarget()) {
								if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAB_TONE")) {
									const std::string dropped(static_cast<const char*>(payload->Data));
									if (selected) assignTo(s.selectedSongs, dropped);
									else { s.selectedSongs = { row.key }; s.songAnchor = row.key; assignTo(s.selectedSongs, dropped); }
								}
								ImGui::EndDragDropTarget();
							}
							// Details in the tooltip, not on the page.
							if (hovered) {
								std::string about = row.song ? row.song->album + (row.song->year > 0 ? " (" + std::to_string(row.song->year) + ")" : std::string())
									+ "\n" + row.song->arrangements + ", in " + row.song->file : std::string("Not in your song library (key ") + row.key + ")";
								if (!tone.empty()) about = "Tone: " + tone + "\n" + about;
								if (load) {
									char seconds[16]{};
									sprintf_s(seconds, "%.1f", load->seconds);
									about += std::string("\nLast start: \"") + load->tone + "\" ready in " + seconds + " s"
										+ (load->plugins.empty() ? std::string(", every plugin already loaded.") : ", loading " + load->plugins + " from scratch.");
								}
								if (playing(row.key)) about = "Playing now.\n" + about;
								about += "\n\nRight-click to give it a tone, or drag a tone onto it.";
								Hint(about.c_str());
							}
							PushPopupPadding();
							if (ImGui::BeginPopupContextItem("##songMenu")) {
								if (!selected && ImGui::IsWindowAppearing()) { s.selectedSongs = { row.key }; s.songAnchor = row.key; }
								const std::vector<std::string> keys = s.selectedSongs;
								ImGui::TextDisabled("%s", keys.size() == 1 ? row.title.c_str() : (std::to_string(keys.size()) + " songs").c_str());
								ImGui::Separator();
								if (ImGui::BeginMenu("Give it a tone")) { ToneChoices([&](const std::string& t) { assignTo(keys, t); }); ImGui::EndMenu(); }
								if (ImGui::MenuItem("Give it the current tone", nullptr, false, !vst.toneName.empty())) assignTo(keys, vst.toneName);
								bool anyTone = false;
								for (const std::string& key : keys) anyTone = anyTone || !toneOf(key).empty();
								if (ImGui::MenuItem("Take its tone away", nullptr, false, anyTone)) assignTo(keys, {});
								ImGui::EndPopup();
							}
							ImGui::PopStyleVar();

							const ImVec2 end(at.x + rowW, at.y + rowH);
							if (selected) rowsList->AddRectFilled(at, end, Ink::RowSelected, 5.0f);
							else if (hovered) rowsList->AddRectFilled(at, end, Ink::RowHover, 5.0f);
							// Right: an amber "14 s" when its tone waited for plugins, then the tone's tag (or a dash).
							float right = end.x - 6.0f;
							const float tagY = at.y + (rowH - caption->FontSize) * 0.5f;
							if (!tone.empty()) {
								const std::string tag = BaseName(tone);
								const float tagW = std::min(TextWidth(caption, tag.c_str()) + 16.0f, rowW * 0.42f);
								const ImVec2 tagAt(right - tagW, at.y + (rowH - caption->FontSize) * 0.5f - 4.0f);
								rowsList->AddRectFilled(tagAt, ImVec2(right, tagAt.y + caption->FontSize + 8.0f), Ink::Tag, 5.0f);
								rowsList->PushClipRect(tagAt, ImVec2(right - 4.0f, tagAt.y + caption->FontSize + 8.0f), true);
								Text(rowsList, caption, ImVec2(tagAt.x + 8.0f, tagY), Ink::TagText, tag.c_str());
								rowsList->PopClipRect();
								right -= tagW + 6.0f;
							}
							if (slow(load)) {
								const std::string last = std::to_string(static_cast<int>(load->seconds + 0.5f)) + " s";
								const float lastW = TextWidth(caption, last.c_str());
								Text(rowsList, caption, ImVec2(right - lastW, tagY), Ink::Amber, last.c_str());
								right -= lastW + 6.0f;
							}
							float nameX = at.x + 6.0f;
							if (playing(row.key)) {
								rowsList->AddCircleFilled(ImVec2(nameX + 4.0f, at.y + 5.0f + ImGui::GetFontSize() * 0.5f), 3.5f, Ink::Good, 12);
								nameX += 14.0f;
							}
							rowsList->PushClipRect(at, ImVec2(right - 4.0f, end.y), true);
							rowsList->AddText(ImVec2(nameX, at.y + 5.0f), IM_COL32_WHITE, row.title.c_str());
							// Under its artist's heading the second line is the album (and year); otherwise the artist.
							const bool grouped = s.songSort == ByArtist && s.songFilter != SlowToLoad && !playing(row.key) && row.song;
							const std::string second = grouped ? row.song->album + (row.song->year > 0 ? "  " + std::to_string(row.song->year) : std::string())
								: row.artist + (s.songSort == ByYear && row.song && row.song->year > 0 ? "  " + std::to_string(row.song->year) : std::string());
							Text(rowsList, caption, ImVec2(at.x + 6.0f, at.y + 6.0f + ImGui::GetFontSize()), Ink::Muted, second.c_str());
							rowsList->PopClipRect();
							ImGui::PopID();
						}
					}
					clipper.End();
					ImGui::EndChild();
					ImGui::PopStyleVar(2);
				}

				ImGui::Dummy(ImVec2(0, 4));
				if (!s.selectedSongs.empty()) {
					const std::vector<std::string> keys = s.selectedSongs;
					ImGui::PushFont(caption);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
					ImGui::TextUnformatted(keys.size() == 1 ? "1 song selected" : (std::to_string(keys.size()) + " songs selected").c_str());
					ImGui::PopStyleColor();
					ImGui::PopFont();
					if (Button("Give it a tone...", true)) ImGui::OpenPopup("##songTonePick");
					bool anyTone = false;
					for (const std::string& key : keys) anyTone = anyTone || !toneOf(key).empty();
					if (anyTone) {
						ImGui::SameLine();
						if (Button("Take away")) assignTo(keys, {});
						Hint("Takes their tone away: they play your usual tone again.");
					}
					PushPopupPadding();
					if (ImGui::BeginPopup("##songTonePick")) { ToneChoices([&](const std::string& t) { assignTo(keys, t); }); ImGui::EndPopup(); }
					ImGui::PopStyleVar();
				}
				else MutedCaption("Select songs (Ctrl or Shift+click for several), then right-click to give them a tone.", inner);
				ImGui::EndChild();
			}

			// The browser's Plugins tab: every plugin found, with search and filters, in a list narrow enough for the
			// column. Each row is the plugin's name over its vendor and kind, the star before it and its status as a dot
			// after it (green in the chain, blue kept loaded, amber loading or failed); the tooltip says it in words.
			// Double-click adds it, drag puts it on the board, right-click for the rest; the gear button holds Rescan
			// and Plugin folders.
			void PluginList(const VstHostLink::Snapshot& vst, float width, float height)
			{
				State& s = S();
				const std::vector<VstHostLink::Plugin>& plugins = VstHostLink::Plugins();
				const bool canAdd = !vst.busy && vst.slots.size() < static_cast<size_t>(VstHostControl::kMaxSlots);
				const bool canReplace = !vst.busy && s.selected >= 0 && s.selected < static_cast<int>(vst.slots.size());
				ImFont* caption = CaptionFont();
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
				ImGui::BeginChild("##pluginColumn", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
				ImGui::PopStyleVar();
				const float inner = ImGui::GetContentRegionAvail().x;

				// Search, then the "..." menu beside it.
				const float more = ImGui::GetFrameHeight();
				ImGui::SetNextItemWidth(inner - more - 6.0f);
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Ink::Field));
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
				ImGui::InputTextWithHint("##pluginSearch", "Search plugins or vendors", s.search, sizeof(s.search));
				ImGui::PopStyleVar();
				ImGui::PopStyleColor();
				ImGui::SameLine(0.0f, 6.0f);
				if (IconSquare("##pluginMore", Glyph::Settings, false, "Rescan, plugin folders.", more)) ImGui::OpenPopup("##pluginMoreMenu");
				PushPopupPadding();
				if (ImGui::BeginPopup("##pluginMoreMenu")) {
					if (ImGui::MenuItem("Rescan plugins")) VstHostLink::Rescan();
					Hint("Looks for newly installed plugins, and tries plugins that failed to load again.");
					if (ImGui::MenuItem("Plugin folders...")) VstHostLink::AddScanFolder();
					Hint("Also look for plugins in another folder (opens behind the game, so alt-tab to see it).");
					ImGui::EndPopup();
				}
				ImGui::PopStyleVar();

				// What was found, or the scan's progress or problem, or a plugin loading in the background.
				const size_t folders = VstHostLink::ScanFolders().size() + 1;
				std::string summary = std::to_string(plugins.size()) + (plugins.size() == 1 ? " plugin" : " plugins") + " found, "
					+ std::to_string(vst.slots.size()) + " in chain";
				ImU32 summaryInk = Ink::Dim;
				if (vst.scanningPlugins) {
					static const char* const scanSteps[] = { ".", "..", "..." };
					summary = std::string("Scanning plugins") + scanSteps[static_cast<int>(ImGui::GetTime() * 3.0) % 3];
					summaryInk = Ink::AddText;
				}
				else if (!vst.pluginScanError.empty()) { summary = "Plugin scan failed"; summaryInk = Ink::Bad; }
				else if (!vst.preloadingName.empty()) { summary = "Loading " + vst.preloadingName + " in the background..."; summaryInk = Ink::AddText; }
				ImGui::PushFont(caption);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(summaryInk));
				ImGui::TextUnformatted(summary.c_str());
				ImGui::PopStyleColor();
				ImGui::PopFont();
				Hint(!vst.pluginScanError.empty() ? vst.pluginScanError.c_str()
					: ("Found in " + std::to_string(folders) + (folders == 1 ? " folder" : " folders") + ". The gear button adds a folder; Rescan after installing.").c_str());

				// Filter chips, wrapped to the column.
				static const char* const chips[FilterCount] = { "All", "Favourites", "Amps", "Effects", "Dynamics", "Instruments", "Recent" };
				const float left = ImGui::GetCursorPosX();
				for (int c = 0; c < FilterCount; ++c) {
					if (c > 0) {
						ImGui::SameLine(0.0f, 6.0f);
						if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(chips[c]).x + 20.0f > left + inner) ImGui::NewLine();
					}
					if (Chip(chips[c], s.filter == c)) s.filter = c;
				}
				ImGui::Dummy(ImVec2(0, 2));

				std::vector<const VstHostLink::Plugin*> rows;
				if (s.filter == Recent) {
					for (const std::wstring& path : VstHostLink::RecentPlugins())
						if (const VstHostLink::Plugin* plugin = FindPlugin(path))
							if (MatchesSearch(*plugin, s.search)) rows.push_back(plugin);
				}
				else {
					for (const VstHostLink::Plugin& plugin : plugins)
						if (MatchesFilter(plugin, s.filter) && MatchesSearch(plugin, s.search)) rows.push_back(&plugin);
				}

				const float rowH = ImGui::GetFontSize() + caption->FontSize + 12.0f;
				const float listH = std::max(rowH * 2.0f, ImGui::GetContentRegionAvail().y);
				if (rows.empty()) {
					MutedCaption(plugins.empty() ? "No 64-bit VST3 plugins found. Install some, or add their folder with the gear button."
						: s.filter == Favourites ? "No favourites yet: click the star next to a plugin."
						: s.filter == Recent ? "Plugins you add show here."
						: "Nothing matches.", inner);
					ImGui::EndChild();
					return;
				}
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
				ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
				ImGui::BeginChild("##plugins", ImVec2(inner, listH), false);
				ImDrawList* rowsList = ImGui::GetWindowDrawList();
				const float rowW = ImGui::GetContentRegionAvail().x;
				const float nameX = 26.0f;
				for (size_t r = 0; r < rows.size(); ++r) {
					const VstHostLink::Plugin& plugin = *rows[r];
					ImGui::PushID(static_cast<int>(r));
					const bool selected = _wcsicmp(s.selectedPlugin.c_str(), plugin.path.c_str()) == 0;
					const bool favourite = VstHostLink::IsFavourite(plugin.path);
					const ImVec2 at = ImGui::GetCursorScreenPos();
					const bool pressed = ImGui::InvisibleButton("##plugin", ImVec2(rowW, rowH));
					const bool hovered = ImGui::IsItemHovered();
					if (pressed) {
						if (ImGui::GetIO().MousePos.x < at.x + nameX - 4.0f) VstHostLink::SetFavourite(plugin.path, !favourite);
						else s.selectedPlugin = plugin.path;
					}
					if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && canAdd && plugin.kind != VstHostLink::Kind::Instrument) VstHostLink::AddPlugin(plugin.path);
					// The status in words, then what to do: the row has no room for pills.
					PillLook memory{};
					const bool hasMemory = MemoryPill(vst, plugin, memory);
					if (hovered) {
						std::string about = plugin.name + (plugin.vendor.empty() ? "" : ", " + plugin.vendor) + (plugin.version.empty() ? "" : " " + plugin.version)
							+ "\n" + ChainPill(vst, plugin).text + (hasMemory ? std::string(", ") + memory.text : std::string());
						if (hasMemory && strcmp(memory.text, "Load failed") == 0) about += ": " + PreloadFailedWhy(vst, plugin.path) + " Right-click, Reload to try again.";
						if (plugin.kind == VstHostLink::Kind::Instrument) about += "\nAn instrument: it makes its own sound from MIDI notes and does nothing with a guitar.";
						about += "\n\nDouble-click to add it to the end of the chain, or drag it onto the board: between two pedals to put it there, onto a pedal to replace it. Right-click for more. The star marks a favourite.";
						Hint(about.c_str());
					}
					if (plugin.kind != VstHostLink::Kind::Instrument && ImGui::BeginDragDropSource()) {
						PluginDrag drag{};
						wcsncpy_s(drag.path, plugin.path.c_str(), _TRUNCATE);
						ImGui::SetDragDropPayload("RAB_PLUGIN", &drag, sizeof(drag));
						ImGui::TextUnformatted(plugin.name.c_str());
						ImGui::EndDragDropSource();
					}
					PushPopupPadding();
					if (ImGui::BeginPopupContextItem("##pluginMenu")) {
						s.selectedPlugin = plugin.path;
						ImGui::TextDisabled("%s", plugin.name.c_str());
						ImGui::Separator();
						if (ImGui::MenuItem("Add to the end", "or double-click it", false, canAdd && plugin.kind != VstHostLink::Kind::Instrument)) VstHostLink::AddPlugin(plugin.path);
						if (ImGui::MenuItem("Replace selected pedal", nullptr, false, canReplace && plugin.kind != VstHostLink::Kind::Instrument)) VstHostLink::ReplaceSlot(s.selected, plugin.path);
						ImGui::Separator();
						// Fast tone changes: a plugin kept loaded outside the chain is in a tone in a moment.
						const bool inChain = InChain(vst, plugin.path), loaded = KeptLoaded(vst, plugin.path);
						const bool always = Listed(vst.alwaysLoaded, plugin.path);
						if (ImGui::MenuItem("Favourite", nullptr, favourite)) VstHostLink::SetFavourite(plugin.path, !favourite);
						if (ImGui::MenuItem("Load in the background", nullptr, false, vst.hostRunning && !inChain && !loaded && !Loading(vst, plugin.path)))
							VstHostLink::LoadInBackground(plugin.path);
						Hint("Loads it now without adding it, and keeps it loaded until the game closes: tones that use it switch at once.");
						if (ImGui::MenuItem("Always keep loaded", nullptr, always)) VstHostLink::SetAlwaysLoaded(plugin.path, !always);
						Hint("Loads it in the background every time Tone designer starts (outside songs), so tones that use it always switch at once. Costs memory while loaded.");
						if (ImGui::MenuItem("Unload", nullptr, false, vst.hostRunning && loaded)) VstHostLink::Unload(plugin.path);
						Hint("Frees its memory. In the chain it stays; this only unloads the spare copy.");
						// A plugin whose background load failed has no copy to reload, so Reload is the retry: it loads it
						// again in the background (which clears the failed mark), the same as Load in the background.
						const bool failed = !inChain && !loaded && Listed(vst.preloadFailed, plugin.path);
						if (ImGui::MenuItem("Reload", nullptr, false, vst.hostRunning && !vst.busy && (inChain || loaded || failed) && !Loading(vst, plugin.path))) {
							if (failed) VstHostLink::LoadInBackground(plugin.path);
							else VstHostLink::Reload(plugin.path);
						}
						Hint("Closes the plugin completely and starts it again, keeping each pedal's settings: for when it needs a fresh start to see something new, such as tones you imported into it. The rest of the chain keeps playing while it loads. If it failed to load, this tries again.");
						ImGui::Separator();
						if (ImGui::MenuItem("Show in folder")) VstHostLink::ShowPluginFile(plugin.path);
						ImGui::EndPopup();
					}
					ImGui::PopStyleVar();

					const ImVec2 end(at.x + rowW, at.y + rowH);
					if (selected) rowsList->AddRectFilled(at, end, Ink::RowSelected, 5.0f);
					else if (hovered) rowsList->AddRectFilled(at, end, Ink::RowHover, 5.0f);
					DrawIcon(rowsList, ImVec2(at.x + 12.0f, at.y + rowH * 0.5f), favourite ? Glyph::StarFilled : Glyph::Star, favourite ? Ink::Amber : Ink::Jack);
					// Status dot: green in the chain, blue kept loaded, amber loading or a failed load, red failed to load.
					ImU32 dot = 0;
					if (VstHostLink::FailedToLoad(plugin.path)) dot = Ink::Bad;
					else if (InChain(vst, plugin.path)) dot = Ink::Good;
					else if (Loading(vst, plugin.path) || (hasMemory && memory.fill != Ink::Blue)) dot = Ink::Amber;
					else if (hasMemory) dot = Ink::AddText;
					const float dotRoom = dot ? 16.0f : 4.0f;
					if (dot) rowsList->AddCircleFilled(ImVec2(end.x - 9.0f, at.y + rowH * 0.5f), 3.5f, dot, 12);
					const float top = at.y + 5.0f;
					rowsList->PushClipRect(ImVec2(at.x + nameX, at.y), ImVec2(end.x - dotRoom, end.y), true);
					rowsList->AddText(ImVec2(at.x + nameX, top), plugin.kind == VstHostLink::Kind::Instrument ? Ink::Muted : IM_COL32_WHITE, plugin.name.c_str());
					const std::string sub = (plugin.vendor.empty() ? std::string() : plugin.vendor + "  ") + KindName(plugin.kind);
					Text(rowsList, caption, ImVec2(at.x + nameX, top + ImGui::GetFontSize() + 1.0f), Ink::Muted, sub.c_str());
					rowsList->PopClipRect();
					ImGui::PopID();
				}
				ImGui::EndChild();
				ImGui::PopStyleVar(2);
				ImGui::EndChild();
			}

			void LevelsDialogBody(const VstHostLink::Snapshot& vst)
			{
				// Trims around the chain: the player's own, the same for every tone (a tone's levels are its author's job, set in
				// its plugins; loading one never moves these). Dragging sends the new level as it moves (the overlay coalesces them).
				static float inputDb = 0.0f, outputDb = 0.0f;
				static bool dragging = false;
				if (!dragging) { inputDb = vst.inputTenths / 10.0f; outputDb = vst.outputTenths / 10.0f; }
				bool inputDone = false, outputDone = false;
				const bool inputMoved = SliderRow("Into the chain", &inputDb, -24.0f, 24.0f, "%+.1f dB", &inputDone,
					"Level of your guitar going into the first plugin, the same for every tone. Amp sims expect a real guitar's level; raise it if tones sound thin or quiet.");
				const bool outputMoved = SliderRow("Out of the chain", &outputDb, -24.0f, 24.0f, "%+.1f dB", &outputDone,
					"Level of the whole chain going to the game, the same for every tone.");
				if (inputMoved || outputMoved) {
					dragging = true;
					VstHostLink::SetLevels(static_cast<int>(std::lround(inputDb * 10.0f)), static_cast<int>(std::lround(outputDb * 10.0f)));
				}
				if (inputDone || outputDone) dragging = false;
				Meter("Input", MeterFraction(vst.inputPeak), "After the input trim.");
				Meter("Output", MeterFraction(vst.outputPeak), "After the output trim, before the game's mix.");
				if (vst.outputPeak > 1.0f) WarnCaption("The chain's output is clipping. Lower the output trim or the last plugin's volume.");
			}

			std::string Trimmed(const char* text)
			{
				std::string out = text;
				while (!out.empty() && isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
				size_t first = 0;
				while (first < out.size() && isspace(static_cast<unsigned char>(out[first]))) ++first;
				return out.substr(first);
			}

			bool DangerButton(const char* label)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(IM_COL32(185, 28, 28, 255)));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(Color::Bad));
				const bool pressed = Button(label);
				ImGui::PopStyleColor(2);
				return pressed;
			}

			// Save as, Rename, Delete, Levels and the "changes are not saved" questions, as one modal at page level.
			void Dialogs(const VstHostLink::Snapshot& vst)
			{
				State& s = S();
				const char* id = "##toneDialog";
				if (s.openDialog) { ImGui::OpenPopup(id); s.openDialog = false; }
				ImGui::SetNextWindowSize(ImVec2(s.dialog == Dialog::Levels ? 520.0f : 440.0f, 0.0f), ImGuiCond_Always);
				// Opens over the middle of the overlay window (wherever it has been dragged), not the screen, and can then be dragged
				// by its background like any other window.
				const ImGuiWindow* overlay = ImGui::GetCurrentWindow()->RootWindow;
				ImGui::SetNextWindowPos(ImVec2(overlay->Pos.x + overlay->Size.x * 0.5f, overlay->Pos.y + overlay->Size.y * 0.5f),
					ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 18));
				if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
					auto heading = [](const std::string& text) {
						ImGui::PushFont(GetFonts().title ? GetFonts().title : ImGui::GetFont());
						ImGui::TextUnformatted(text.c_str());
						ImGui::PopFont();
					};
					bool close = false;
					const std::string current = vst.toneName.empty() ? "your chain" : "\"" + vst.toneName + "\"";
					switch (s.dialog) {
					case Dialog::SaveAs:
					case Dialog::Rename: {
						const bool rename = s.dialog == Dialog::Rename;
						heading(rename ? "Rename tone" : "Save tone as");
						Caption(rename ? "Songs that use it keep using it under its new name."
							: "Saves the chain with every plugin's settings. Folder/Name saves it into a folder (Bass tones/Fuzz).");
						if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
						ImGui::SetNextItemWidth(-1.0f);
						const bool enter = ImGui::InputTextWithHint("##name", "Tone name", s.name, sizeof(s.name), ImGuiInputTextFlags_EnterReturnsTrue);
						const std::string typed = Trimmed(s.name);
						// Renaming keeps the tone in its folder (Move to folder moves it).
						const std::string name = rename && !typed.empty() && !FolderOf(s.dialogTone).empty() ? FolderOf(s.dialogTone) + "/" + typed : typed;
						const std::vector<std::string>& tones = VstHostLink::Tones();
						const bool taken = _stricmp(name.c_str(), s.dialogTone.c_str()) != 0
							&& std::any_of(tones.begin(), tones.end(), [&](const std::string& t) { return _stricmp(t.c_str(), name.c_str()) == 0; });
						if (taken) WarnCaption(rename ? "A tone with that name already exists." : "A tone with that name already exists; saving replaces it.");
						const bool ok = !name.empty() && !(rename && taken) && !(rename && name == s.dialogTone);
						ImGui::BeginDisabled(!ok);
						if (Button(rename ? "Rename" : "Save", true) || (enter && ok)) {
							if (rename) VstHostLink::RenameTone(s.dialogTone, name);
							else VstHostLink::SaveTone(name);
							close = true;
						}
						ImGui::EndDisabled();
						ImGui::SameLine();
						if (Button("Cancel")) close = true;
						break;
					}
					case Dialog::Delete: {
						heading("Delete \"" + s.dialogTone + "\"?");
						const size_t songs = s.dialogSongs;
						std::string text = "The tone file is deleted.";
						if (songs > 0) text += " " + std::to_string(songs) + (songs == 1 ? " song uses it; it goes" : " songs use it; they go") + " back to your usual tone.";
						if (_stricmp(s.dialogTone.c_str(), vst.toneName.c_str()) == 0) text += " The chain you hear stays as it is, unsaved.";
						Caption(text.c_str());
						if (DangerButton("Delete")) { VstHostLink::DeleteTone(s.dialogTone); close = true; }
						ImGui::SameLine();
						if (Button("Cancel")) close = true;
						break;
					}
					case Dialog::NewTone:
					case Dialog::Discard: {
						const bool fresh = s.dialog == Dialog::NewTone;
						heading(fresh ? "Start a new tone?" : "Load \"" + s.dialogTone + "\"?");
						Caption(("Changes to " + current + " are not saved.").c_str());
						// The host is busy loading a plugin in the background: say the choice waits for it, rather
						// than the box closing with nothing seeming to happen.
						if (const std::string wait = WaitNote(vst); !wait.empty()) WarnCaption(wait.c_str());
						auto proceed = [&] { if (fresh) VstHostLink::NewTone(); else VstHostLink::LoadTone(s.dialogTone); };
						if (!vst.toneName.empty()) {
							if (Button("Save, then continue", true)) { VstHostLink::SaveTone(vst.toneName); proceed(); close = true; }
							ImGui::SameLine();
						}
						if (Button("Don't save")) { proceed(); close = true; }
						ImGui::SameLine();
						if (Button("Cancel")) close = true;
						break;
					}
					case Dialog::NewFolder: {
						heading("New folder");
						Caption((s.dialogTones.empty() ? "Moves \"" + BaseName(s.dialogTone) + "\" into a new folder, for example Bass tones or Experimental."
							: "Moves the " + std::to_string(s.dialogTones.size()) + " selected tones into a new folder, for example Bass tones or Experimental.").c_str());
						if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
						ImGui::SetNextItemWidth(-1.0f);
						const bool enter = ImGui::InputTextWithHint("##folder", "Folder name", s.name, sizeof(s.name), ImGuiInputTextFlags_EnterReturnsTrue);
						std::string folder = Trimmed(s.name);
						folder.erase(std::remove(folder.begin(), folder.end(), '/'), folder.end());
						ImGui::BeginDisabled(folder.empty());
						if (Button("Move", true) || (enter && !folder.empty())) {
							if (s.dialogTones.empty()) MoveToFolder(s.dialogTone, folder);
							else for (const std::string& t : std::vector<std::string>(s.dialogTones)) MoveToFolder(t, folder);
							close = true;
						}
						ImGui::EndDisabled();
						ImGui::SameLine();
						if (Button("Cancel")) close = true;
						break;
					}
					case Dialog::DeleteTones: {
						heading("Delete " + std::to_string(s.dialogTones.size()) + " tones?");
						std::string names;
						for (size_t i = 0; i < s.dialogTones.size() && i < 6; ++i) names += (i ? ", " : "") + BaseName(s.dialogTones[i]);
						if (s.dialogTones.size() > 6) names += " and " + std::to_string(s.dialogTones.size() - 6) + " more";
						Caption((names + ". The tone files are deleted; songs that use them go back to your usual tone. A backup (File, Backups) can bring them back.").c_str());
						if (DangerButton(("Delete " + std::to_string(s.dialogTones.size()) + " tones").c_str())) {
							for (const std::string& t : s.dialogTones) VstHostLink::DeleteTone(t);
							s.selectedTones.clear();
							close = true;
						}
						ImGui::SameLine();
						if (Button("Cancel")) close = true;
						break;
					}
					case Dialog::Restore: {
						heading("Restore \"" + s.dialogTone + "\"?");
						Caption("Your tones, song tones, plugin settings and Tone designer settings are replaced by the ones in this backup. What you have now is backed up first, so this can be undone. Tone designer closes for a moment and starts again.");
						if (DangerButton("Restore")) { ToneBackup::Restore(s.dialogPath); close = true; }
						ImGui::SameLine();
						if (Button("Cancel")) close = true;
						break;
					}
					case Dialog::Levels:
						heading("Levels");
						LevelsDialogBody(vst);
						if (Button("Done", true)) close = true;
						break;
					default:
						close = true;
						break;
					}
					if (close) { s.dialog = Dialog::None; ImGui::CloseCurrentPopup(); }
					ImGui::EndPopup();
				}
				ImGui::PopStyleVar();
			}

			// ---- Inspector ------------------------------------------------------------------------------------
			// Under the board: the selected pedal's knobs, read from the plugin itself (the host lists its parameters
			// for the inspected slot, VstHostControl v10), bypass, a swap for another plugin of the same kind, its
			// window, and the chain's trims. A knob drag is one gesture: the host makes it one undo step and one
			// automation gesture for the plugin. Edits in the plugin's own window show here too, but are the plugin's to
			// undo (standard host behaviour).

			// The plugin before or after the slot's among the plugins of its kind (amps with amps, effects with effects),
			// wrapping around; it replaces the pedal in place, as the pedal window's < > does.
			void SwapSameKind(const VstHostLink::Snapshot& vst, int slot, int delta)
			{
				const VstHostLink::Plugin* current = FindPlugin(vst.slots[slot].path);
				if (!current) return;
				std::vector<const VstHostLink::Plugin*> same;
				for (const VstHostLink::Plugin& plugin : VstHostLink::Plugins())
					if (plugin.kind == current->kind && plugin.kind != VstHostLink::Kind::Instrument) same.push_back(&plugin);
				int index = -1;
				for (int i = 0; i < static_cast<int>(same.size()); ++i) if (same[i] == current) index = i;
				if (index < 0 || same.size() < 2) return;
				const int count = static_cast<int>(same.size());
				VstHostLink::ReplaceSlot(slot, same[((index + delta) % count + count) % count]->path);
			}

			// Levels: the chain's two trims in one compact row at the foot of the Inspector (the Levels dialog has them
			// too, with meters). Short sliders with the value beside them, not on them. Dragging sends each new level as
			// it moves; the host makes a drag one undo step ("levels"). Double-click puts one back to 0 dB.
			void LevelsRow(const VstHostLink::Snapshot& vst)
			{
				static float inputDb = 0.0f, outputDb = 0.0f;
				static bool dragging = false;
				if (!dragging) { inputDb = vst.inputTenths / 10.0f; outputDb = vst.outputTenths / 10.0f; }
				constexpr float SLIDER_WIDTH = 150.0f;
				bool moved = false, done = false;
				ImFont* caption = CaptionFont();
				auto label = [&](const char* text, ImU32 color) {
					ImGui::PushFont(caption);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(text);
					ImGui::PopStyleColor();
					ImGui::PopFont();
				};
				auto trim = [&](const char* name, const char* id, float* db, const char* hint) {
					label(name, Ink::Soft);
					ImGui::SameLine(0.0f, 8.0f);
					ImGui::SetNextItemWidth(SLIDER_WIDTH);
					moved |= ImGui::SliderFloat(id, db, -24.0f, 24.0f, "");
					done |= ImGui::IsItemDeactivatedAfterEdit();
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { *db = 0.0f; moved = done = true; }
					Hint(hint);
					char value[16];
					sprintf_s(value, "%+.1f dB", *db);
					ImGui::SameLine(0.0f, 8.0f);
					label(value, Ink::Text);
				};
				label("LEVELS", Ink::Dim);
				ImGui::SameLine(0.0f, 16.0f);
				trim("In", "##inputTrim", &inputDb, "Your guitar into the first plugin, the same for every tone. Amp sims expect a real guitar's level; raise it if tones sound thin. Double-click for 0 dB.");
				ImGui::SameLine(0.0f, 24.0f);
				trim("Out", "##outputTrim", &outputDb, "The whole chain into the game, the same for every tone. Double-click for 0 dB.");
				if (moved) {
					dragging = true;
					VstHostLink::SetLevels(static_cast<int>(std::lround(inputDb * 10.0f)), static_cast<int>(std::lround(outputDb * 10.0f)));
				}
				if (done) dragging = false;
			}

			// A name a plugin gives a slot it leaves for the player to map inside the plugin: "Param 1", "Parameter 12",
			// or no name at all. AmpliTube 5 lists only such slots; as knobs they would say nothing.
			bool IsGenericName(const std::string& name)
			{
				size_t at = 0;
				while (at < name.size() && isspace(static_cast<unsigned char>(name[at]))) ++at;
				if (at == name.size()) return true;
				for (const char* prefix : { "parameter", "param" }) {
					const size_t length = strlen(prefix);
					if (name.size() - at >= length && _strnicmp(name.c_str() + at, prefix, length) == 0) { at += length; break; }
				}
				bool digits = false;
				for (; at < name.size(); ++at) {
					const unsigned char c = static_cast<unsigned char>(name[at]);
					if (isdigit(c)) digits = true;
					else if (!isspace(c) && c != '_' && c != '#') return false;
				}
				return digits;
			}

			// The picture of the plugin's own window (PluginPictureView), fitted into `size` at the cursor: click opens the
			// real window. Until the host has captured it, a placeholder says what is happening.
			void PluginPicture(const VstHostLink::Snapshot& vst, int slot, ImVec2 size)
			{
				const VstHostLink::SlotView& view = vst.slots[slot];
				PluginPictureView::Want(slot);
				const PluginPictureView::View picture = PluginPictureView::Get(slot);
				const ImVec2 at = ImGui::GetCursorScreenPos();
				ImDrawList* list = ImGui::GetWindowDrawList();
				ImVec2 fit = size;
				if (picture.texture) {
					const float scale = std::min(size.x / picture.width, size.y / picture.height);
					fit = ImVec2(std::floor(picture.width * scale), std::floor(picture.height * scale));
				}
				const ImVec2 corner(at.x + std::floor((size.x - fit.x) * 0.5f), at.y);
				ImGui::SetCursorScreenPos(corner);
				if (ImGui::InvisibleButton("##pluginPicture", fit)) OpenWindow(slot);
				const bool hovered = ImGui::IsItemHovered();
				const ImVec2 end(corner.x + fit.x, corner.y + fit.y);
				if (picture.texture) {
					list->AddImageRounded(picture.texture, corner, end, ImVec2(0, 0), picture.uv1, IM_COL32_WHITE, 8.0f);
					list->AddRect(corner, end, hovered ? Ink::Outline : Ink::Edge, 8.0f, 0, hovered ? 2.0f : 1.0f);
					if (hovered) {
						const char* open = view.windowOpen ? "Bring its window to the front" : "Open its window";
						ImFont* caption = CaptionFont();
						const ImVec2 textSize(TextWidth(caption, open), caption->FontSize);
						const ImVec2 pill(end.x - textSize.x - 26.0f, end.y - textSize.y - 18.0f);
						list->AddRectFilled(pill, ImVec2(end.x - 8.0f, end.y - 8.0f), IM_COL32(9, 14, 26, 220), 6.0f);
						Text(list, caption, ImVec2(pill.x + 9.0f, pill.y + 5.0f), IM_COL32_WHITE, open);
					}
					Hint(("A picture of " + view.name + "'s own window. Click to open it and play with its controls.").c_str());
				}
				else {
					list->AddRectFilled(corner, end, Ink::Rack, 8.0f);
					list->AddRect(corner, end, Ink::Edge, 8.0f);
					const std::string line = picture.hasNoEditor ? view.name + " has no window of its own."
						: picture.isFailed ? "No picture of " + view.name + (picture.why.empty() ? std::string(".") : ": " + picture.why)
						: "Getting a picture of " + view.name + "...";
					ImFont* caption = CaptionFont();
					const float lineW = std::min(TextWidth(caption, line.c_str()), fit.x - 24.0f);
					list->PushClipRect(corner, end, true);
					Text(list, caption, ImVec2(corner.x + (fit.x - lineW) * 0.5f, corner.y + fit.y * 0.5f - caption->FontSize), Ink::Muted, line.c_str());
					list->PopClipRect();
					Hint("Click to open its window.");
				}
				ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + size.y));
				ImGui::Dummy(ImVec2(size.x, 0.0f));
			}

			void InspectorPanel(const VstHostLink::Snapshot& vst, float width, float height)
			{
				State& s = S();
				const int count = static_cast<int>(vst.slots.size());
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
				ImGui::BeginChild("##inspector", ImVec2(width, height), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
				ImGui::PopStyleVar();
				const float inner = ImGui::GetContentRegionAvail().x;
				const float levelsH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 10.0f;
				if (s.selected < 0 || s.selected >= count) {
					ToneDesignerUi::InspectorEmpty(count == 0 ? "Add a plugin, then click it on the board to edit it here." : "Click a pedal on the board to edit it here.",
						inner, ImGui::GetContentRegionAvail().y - levelsH);
					ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 12.0f - levelsH + 10.0f);
					LevelsRow(vst);
					ImGui::EndChild();
					return;
				}
				const int slot = s.selected;
				const VstHostLink::SlotView& view = vst.slots[slot];
				const VstHostLink::Plugin* plugin = FindPlugin(view.path);
				ImFont* caption = CaptionFont();

				// Header, left: the name over its vendor and kind. Right: < > swap, Bypass, the plugin's window.
				const ImVec2 head = ImGui::GetCursorScreenPos();
				const float button = ImGui::GetFrameHeight();
				ImGui::PushFont(GetFonts().title ? GetFonts().title : ImGui::GetFont());
				ImGui::TextUnformatted(view.name.c_str());
				ImGui::PopFont();
				const std::string sub = plugin ? (plugin->vendor.empty() ? std::string() : plugin->vendor + "  ") + KindName(plugin->kind) : std::string();
				ImGui::PushFont(caption);
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Ink::Muted));
				ImGui::TextUnformatted(sub.c_str());
				ImGui::PopStyleColor();
				ImGui::PopFont();
				const float headBottom = ImGui::GetCursorScreenPos().y;

				const char* openLabel = view.windowOpen ? "Bring window to front" : "Open plugin window";
				float x = head.x + inner - ToneDesignerUi::BarButtonWidth(openLabel);
				ImGui::SetCursorScreenPos(ImVec2(x, head.y));
				if (ToneDesignerUi::BarButton("##openWindow", openLabel, button, true, "Its own window, over the game while this overlay is open (or double-click the pedal, or click its picture)."))
					OpenWindow(slot);
				x -= ToneDesignerUi::BarToggleWidth("Bypass") + 16.0f;
				ImGui::SetCursorScreenPos(ImVec2(x, head.y));
				if (ToneDesignerUi::BarToggle("##inspectorBypass", "Bypass", view.bypass, button, !vst.busy, view.bypass ? "Bypassed: click to turn it on." : "Skip this pedal (or click its footswitch)."))
					VstHostLink::SetBypass(slot, !view.bypass);
				x -= 2.0f * button + 6.0f + 16.0f;
				ImGui::SetCursorScreenPos(ImVec2(x, head.y));
				ImGui::BeginDisabled(vst.busy || !plugin);
				if (IconSquare("##swapPrev", Glyph::ChevronLeft, false, "Swap for the previous plugin of the same kind (amps with amps). Its settings go; Undo brings them back.", button))
					SwapSameKind(vst, slot, -1);
				ImGui::SameLine(0.0f, 6.0f);
				if (IconSquare("##swapNext", Glyph::ChevronRight, false, "Swap for the next plugin of the same kind (amps with amps). Its settings go; Undo brings them back.", button))
					SwapSameKind(vst, slot, +1);
				ImGui::EndDisabled();
				ImGui::SetCursorScreenPos(ImVec2(head.x, std::max(headBottom, head.y + button) + 8.0f));
				ImGui::GetWindowDrawList()->AddLine(ImGui::GetCursorScreenPos(), ImVec2(head.x + inner, ImGui::GetCursorScreenPos().y), Ink::Edge);
				ImGui::Dummy(ImVec2(0, 6));

				// The body: the plugin's named controls as knobs beside a picture of its own window. A plugin that names
				// none (only numbered slots it maps itself) gets the picture alone, the whole body, and a line saying why.
				std::vector<const VstHostLink::ParamView*> named;
				if (vst.paramSlot == slot)
					for (const VstHostLink::ParamView& param : vst.params) if (!IsGenericName(param.name)) named.push_back(&param);
				const bool reading = vst.paramSlot != slot;
				const float bodyH = std::max(60.0f, ImGui::GetContentRegionAvail().y - levelsH);
				const ImVec2 body = ImGui::GetCursorScreenPos();
				const bool knobs = !named.empty();
				const float noteH = knobs ? 0.0f : caption->FontSize + 8.0f;
				const float pictureW = knobs ? std::floor(std::min(inner * 0.42f, (bodyH - 4.0f) * 1.5f)) : inner;
				PluginPicture(vst, slot, ImVec2(pictureW, bodyH - noteH));
				if (!knobs) {
					MutedCaption(reading ? "Reading its controls..."
						: (view.name + (vst.params.empty() ? " lists no controls for the host" : " names no controls for the host, only slots you map inside it")
							+ ". Edit it in its window: click the picture.").c_str(), inner);
				}
				else {
					// While a knob is dragged its own value is shown, not the host's last report (a few frames behind).
					static int dragIndex = -1;
					static float dragValue = 0.0f;
					const float knobsX = body.x + pictureW + 20.0f;
					const float knobsW = inner - pictureW - 20.0f;
					ImGui::SetCursorScreenPos(ImVec2(knobsX, body.y));
					ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
					ImGui::BeginChild("##inspectorKnobs", ImVec2(knobsW, bodyH), false);
					const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / ToneDesignerUi::KNOB_CELL_WIDTH));
					for (size_t p = 0; p < named.size(); ++p) {
						const VstHostLink::ParamView& param = *named[p];
						if (p % columns != 0) ImGui::SameLine(0.0f, 0.0f);
						ImGui::PushID(param.index);
						float position = dragIndex == param.index ? dragValue : param.value;
						const std::string valueText = param.label.empty() ? param.text : param.text + " " + param.label;
						const ToneDesignerUi::KnobResult knob = ToneDesignerUi::Knob("##knob", param.name.c_str(), valueText.c_str(), &position,
							(param.name + (valueText.empty() ? std::string() : ": " + valueText) + ". Drag up or down (Shift for fine steps).").c_str());
						if (knob.began) { dragIndex = param.index; dragValue = position; VstHostLink::SetParam(slot, param.index, position, VstHostControl::ParamPhase::Begin); }
						if (knob.changed) { dragValue = position; VstHostLink::SetParam(slot, param.index, position, VstHostControl::ParamPhase::Change); }
						if (knob.ended) { VstHostLink::SetParam(slot, param.index, position, VstHostControl::ParamPhase::End); dragIndex = -1; }
						ImGui::PopID();
					}
					if (vst.paramTotal > static_cast<int>(vst.params.size())) {
						ImGui::Dummy(ImVec2(0, 4));
						MutedCaption(("The first " + std::to_string(vst.params.size()) + " of its " + std::to_string(vst.paramTotal)
							+ " controls. Its window has the rest.").c_str(), ImGui::GetContentRegionAvail().x);
					}
					ImGui::EndChild();
					ImGui::PopStyleVar();
				}

				ImGui::SetCursorScreenPos(ImVec2(head.x, body.y + bodyH + 8.0f));
				LevelsRow(vst);
				ImGui::EndChild();
			}

			// ---- The panel ------------------------------------------------------------------------------------

			// The browser column: Tones | Plugins | Songs, then that tab's list under it, the full height of the panel.
			void BrowserColumn(const VstHostLink::Snapshot& vst, float width, float height)
			{
				State& s = S();
				ImGui::BeginChild("##browser", ImVec2(width, height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
				static const char* const tabs[] = { "Tones", "Plugins", "Songs" };
				static const char* const hints[] = { "Your saved tones: double-click one to load it.",
					"Every plugin found: drag one onto the board, or double-click to add it.",
					"Your songs: give them tones of their own without playing them. The song playing now is first." };
				ImGui::SetCursorPos(ImVec2(8.0f, 10.0f));
				const ImVec2 tabsAt = ImGui::GetCursorScreenPos();
				const int clicked = ToneDesignerUi::PaneToggle(tabs, 3, s.browserTab, hints, width - 16.0f);
				if (clicked >= 0) s.browserTab = clicked;
				// A tone dragged over the Songs tab opens it, so the tone can be dropped onto songs.
				if (const ImGuiPayload* dragging = ImGui::GetDragDropPayload(); dragging && dragging->IsDataType("RAB_TONE")
					&& ImGui::IsMouseHoveringRect(ImVec2(tabsAt.x + (width - 16.0f) * 2.0f / 3.0f, tabsAt.y), ImVec2(tabsAt.x + width - 16.0f, tabsAt.y + ImGui::GetFrameHeight())))
					s.browserTab = SongsTab;
				ImGui::SetCursorPos(ImVec2(0.0f, ImGui::GetCursorPosY() + 4.0f));
				const float listH = std::max(40.0f, height - ImGui::GetCursorPosY());
				if (s.browserTab == PluginsTab) PluginList(vst, width, listH);
				else if (s.browserTab == SongsTab) SongList(vst, width, listH);
				else ToneList(vst, width, listH);
				ImGui::EndChild();
			}

			// The framed panel: the bar; the browser column on the left; on the right the board strip and the Inspector
			// under it. Backgrounds are drawn on the page's draw list first; the parts are child windows on top.
			void Panel(const VstHostLink::Snapshot& vst)
			{
				State& s = S();
				const float width = ImGui::GetContentRegionAvail().x;
				const float barH = ImGui::GetFontSize() + 20.0f;
				const bool inspector = IsOpen(Inspector);
				// The board is a strip as tall as a pedal (plus a line when the chain is quiet); the Inspector takes the
				// rest. Without the Inspector the board takes the whole height again.
				const float libraryW = IsOpen(Library) ? ToneDesignerUi::BrowserWidth(width) : 0.0f;
				const float boardH = ToneDesignerUi::BoardStripHeight(RackHeight(vst, width - libraryW), BoardHint(vst) != nullptr);
				constexpr float kNaturalInspectorH = 320.0f, kMinInspectorH = 170.0f, kNaturalMainH = 420.0f;
				const float naturalMainH = std::max(kNaturalMainH, boardH + (inspector ? kNaturalInspectorH : 0.0f));
				AudioBridge::Shell::FillWindowHeight(ImGui::GetCursorPosY() + barH + naturalMainH + 12.0f + 20.0f);
				const float mainH = std::max(boardH + (inspector ? kMinInspectorH : 0.0f), std::floor(ImGui::GetContentRegionAvail().y - barH - 12.0f));
				const ImVec2 at = ImGui::GetCursorScreenPos();
				ImDrawList* list = ImGui::GetWindowDrawList();
				// Split the draw list so the panel's background (channel 0) goes under everything drawn after it (channel 1).
				list->ChannelsSplit(2);
				list->ChannelsSetCurrent(1);

				Bar(vst, width, barH);
				const float top = at.y + barH;
				if (libraryW > 0.0f) {
					ImGui::SetCursorScreenPos(ImVec2(at.x, top));
					BrowserColumn(vst, libraryW, mainH);
				}
				ImGui::SetCursorScreenPos(ImVec2(at.x + libraryW, top));
				Board(vst, width - libraryW, inspector ? boardH : mainH);
				if (inspector) {
					ImGui::SetCursorScreenPos(ImVec2(at.x + libraryW, top + boardH));
					InspectorPanel(vst, width - libraryW, mainH - boardH);
				}
				// The host lists the parameters of the pedal the Inspector shows (none while it is hidden).
				const int count = static_cast<int>(vst.slots.size());
				VstHostLink::InspectSlot(inspector && s.selected >= 0 && s.selected < count ? s.selected : -1);
				const float bottom = top + mainH;

				// Ctrl+Z and Ctrl+Y (Ctrl+Shift+Z) on the page, as the Edit menu's Undo and Redo, unless a text box has the keys.
				const ImGuiIO& io = ImGui::GetIO();
				if (io.KeyCtrl && !io.WantTextInput && !vst.busy && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
					if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Z), false)) {
						if (io.KeyShift) { if (!vst.redoLabel.empty()) VstHostLink::Redo(); }
						else if (!vst.undoLabel.empty()) VstHostLink::Undo();
					}
					if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Y), false) && !vst.redoLabel.empty()) VstHostLink::Redo();
				}

				list->ChannelsSetCurrent(0);
				ToneDesignerUi::PanelFrame frame;
				frame.at = at;
				frame.width = width;
				frame.barHeight = barH;
				frame.mainHeight = mainH;
				frame.libraryWidth = libraryW;
				frame.bottom = bottom;
				frame.inspectorTop = inspector ? top + boardH : 0.0f;
				ToneDesignerUi::DrawPanelFrame(list, frame);
				list->ChannelsMerge();
				ImGui::SetCursorScreenPos(ImVec2(at.x, bottom + 6.0f));
				ImGui::Dummy(ImVec2(0, 0));
			}
		}
	}

	void ToneDesignerTab()
	{
		using namespace ToneDesigner;
		// Start reading the song library as soon as the page is shown, so the Songs pane is ready when it is
		// opened. Refresh() runs on its own thread and only starts once a session (later calls return at once).
		SongLibrary::Refresh();
		// A click, scroll or key on the page holds "Always keep loaded" background loads off (VstHostLink), so a
		// New tone or Add plugin is not left waiting behind one. Mouse movement alone does not count.
		const ImGuiIO& io = ImGui::GetIO();
		if (ImGui::IsAnyMouseDown() || io.MouseWheel != 0.0f || io.InputQueueCharacters.Size > 0) VstHostLink::NotePageInUse();
		const ExternalAmp::Snapshot amp = ExternalAmp::GetSnapshot();
		const VstHostLink::Snapshot vst = VstHostLink::GetSnapshot();

		if (!vst.hostInstalled) {
			BeginCard();
			WarnCaption("Tone designer's program (RocksmithAudioBridgeVstHost.exe) is in neither RSMods\\AudioBridge nor the game folder. Run the Rocksmith Audio Bridge installer's Repair.");
			EndCard();
			return;
		}

		// The amp link carries one app at a time; say plainly when another app has it.
		const std::string guest = amp.link.guestName;
		const bool otherApp = amp.link.guestPresent && !guest.empty() && !VstHostLink::IsHostGuest(guest);
		const std::string otherAppText = guest + " (External app tab) is using the amp link, so Tone designer plays nothing. Close "
			+ guest + " to use Tone designer.";

		if (const std::string backup = ToneBackup::Status(); !backup.empty()) {
			BeginCard();
			Caption(backup.c_str());
			EndCard();
		}

		// A plugin crashed the host this session: say which, and that it was restarted (VstHostLink::FollowCrashes).
		if (!vst.crashNote.empty()) {
			BeginCard();
			WarnCaption(vst.crashNote.c_str());
			if (Button("OK")) VstHostLink::DismissCrashNote();
			EndCard();
		}

		if (vst.hostRecovering) {
			BeginCard();
			Caption("Restarting Tone designer with your saved chain...");
			EndCard();
			return;
		}
		if (vst.hostHung) {
			BeginCard();
			WarnCaption("Tone designer has stopped answering.");
			if (Button("Restart Tone designer", true, "Closes the verified stuck host and starts Tone designer again."))
				VstHostLink::RestartHost();
			EndCard();
			return;
		}
		if (vst.hostDisconnected) {
			BeginCard();
			WarnCaption("Tone designer is running but is not connected to the game.");
			if (Button("Restart Tone designer", true, "Closes the verified disconnected host and starts Tone designer again."))
				VstHostLink::RestartHost();
			EndCard();
			return;
		}
		if (vst.hostRunning && (vst.loading || vst.busy)) {
			BeginCard();
			WarnCaption("Tone designer is still loading.");
			if (Button("Restart Tone designer", true, "Stops the current load and starts Tone designer again."))
				VstHostLink::RestartHost();
			EndCard();
		}
		// Off: either closed, or in standby (Game amp or External app chosen, or started for always-loaded plugins), where the host keeps its plugins loaded and goes on
		// loading the always-loaded ones, so turning it on is instant.
		if (!vst.hostRunning || vst.standby) {
			BeginCard();
			if (!vst.standby) Caption("Tone designer is off. Starting it brings back the chain you had last.");
			else if (vst.loading || !vst.preloadingName.empty())
				Caption(("Tone designer is off; the game's amp is playing. Getting " + (vst.loading ? std::string("your chain") : vst.preloadingName) + " ready in the background.").c_str());
			else Caption("Tone designer is off; the game's amp is playing. Your plugins are still loaded, so it turns on at once.");
			if (otherApp) WarnCaption(otherAppText.c_str());
			if (!vst.loadError.empty() && vst.standby) WarnCaption(vst.loadError.c_str());
			if (Button(vst.standby ? "Turn on" : "Start Tone designer", true, vst.standby ? "Your guitar plays through the chain again."
				: "Starts the VST host in its own 64-bit program; your guitar plays through it once it connects."))
				VstHostLink::StartHost();
			if (vst.standby) {
				ImGui::SameLine();
				if (Button("Close completely", false, "Closes the VST host and unloads every plugin, freeing their memory. Always-loaded plugins load again next time the game starts."))
					VstHostLink::StopHost();
			}
			bool autoStart = vst.autoStart;
			if (ToggleRow("Start with the game", "Starts Tone designer with your last chain when Rocksmith starts. Off, but with plugins marked Always keep loaded, it starts switched off and loads those in the background.", &autoStart))
				VstHostLink::SetAutoStart(autoStart);
			EndCard();
			return;
		}

		// What stops it being heard or seen, each said plainly.
		if (otherApp || vst.exclusiveFullscreen || !vst.loadError.empty()) {
			BeginCard();
			if (otherApp) WarnCaption(otherAppText.c_str());
			if (vst.exclusiveFullscreen)
				WarnCaption("Rocksmith runs in exclusive fullscreen, where no window can appear over the game, so plugin windows cannot show. Pick non-exclusive (borderless) fullscreen in RSMods' Rocksmith settings.");
			if (!vst.loadError.empty()) WarnCaption(vst.loadError.c_str());
			EndCard();
		}

		Panel(vst);
		Dialogs(vst);
	}
}
