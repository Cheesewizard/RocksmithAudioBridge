#include "PedalEditor.hpp"

#include "HostLog.hpp"
#include "PedalUi/PedalSkin.hpp"

#include <Windows.h>

#include <algorithm>

namespace VstHost
{
	namespace
	{
		constexpr wchar_t kSettingsKey[] = L"Software\\RSMods\\VstHost";

		bool ReadInt(const wchar_t* name, int& value)
		{
			DWORD data = 0, size = sizeof(data);
			if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS) return false;
			value = static_cast<int>(data);
			return true;
		}

		void WriteInt(const wchar_t* name, int value)
		{
			const DWORD data = static_cast<DWORD>(value);
			RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &data, sizeof(data));
		}

		// "Delay_2" as the window's title shows it: "Delay 2".
		juce::String Title(const juce::String& id) { return id.replaceCharacter('_', ' '); }

		// A number as a Stompbox command takes it.
		std::string Number(double value)
		{
			auto text = juce::String(value, 6);
			if (text.containsChar('.')) text = text.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
			return text.toStdString();
		}

		// The kinds the picker treats apart, as the page does (StompboxPage's AMP_KIND, CABINET_KIND).
		constexpr char kNamKind[] = "NAM";
		constexpr char kCabinetKind[] = "Cabinet";
		bool IsNamKind(const juce::String& kind) { return kind == kNamKind || kind == "NAMMulti"; }

		// The capture or IR picker of a pedal: its File parameter on that folder of the data path.
		const PedalUi::ParamDesc* FileParam(const PedalUi::PluginDesc* plugin, const char* tree)
		{
			if (!plugin) return nullptr;
			for (auto& param : plugin->params)
				if (param.type == PedalUi::ParamType::File && param.fileTree == tree) return &param;
			return nullptr;
		}

		const char* TreeOf(const juce::String& kind) { return kind == kCabinetKind ? "Cabinets" : "NAM"; }

		// The browser's order: clean amps first, then crunch and high gain; files the manifest does not know last.
		int SoundOrder(const PedalUi::CaptureInfo& info)
		{
			if (info.kind.isEmpty()) return 9;
			if (info.character == "clean") return 0;
			if (info.character == "crunch") return 1;
			if (info.character == "high-gain") return 2;
			return 3;
		}
	}

	PedalEditor::PedalEditor(StompboxService& service) : service(service) {}

	PedalEditor::~PedalEditor()
	{
		if (stage && isShown) Remember();
		stage.reset();
	}

	void PedalEditor::EnsureWindow()
	{
		if (stage) return;
		// The art (PedalSkin\ next to the host, deployed with it); without it the editors draw their vector look.
		if (!PedalUi::Skin::IsLoaded())
		{
			const auto folder = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile("PedalSkin");
			PedalUi::Skin::Load(folder);
			Log("pedal window: %s", PedalUi::Skin::IsLoaded() ? "artwork loaded" : "no artwork (PedalSkin folder missing), vector look");
		}
		stage = std::make_unique<PedalUi::PedalStage>();
		stage->SetWindowControls(true);
		stage->onClose = [this] { Close(); };
		stage->onMovedOrResized = [this] { Remember(); };
		stage->onSelect = [this](int index) { SwapTo(index); };

		// The window's size follows the pedal (PedalStage::SetFitWindow), so what is remembered is its top centre and
		// the pedal's scale; the first time, the top of a 600-high window centred on the main display.
		int centreX = 0, top = 0, scalePercent = 100;
		const auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userBounds.toNearestInt();
		const bool remembered = ReadInt(L"PedalWindowCentreX", centreX) && ReadInt(L"PedalWindowTop", top)
			&& juce::Desktop::getInstance().getDisplays().getDisplayForPoint(juce::Point<float>(static_cast<float>(centreX), static_cast<float>(top + 10))) != nullptr;
		if (!remembered) { centreX = area.getCentreX(); top = area.getCentreY() - 300; }
		ReadInt(L"PedalWindowScale", scalePercent);
		stage->setBounds(centreX - 450, top, 900, 600);
		stage->SetFitWindow(true, juce::jlimit(40, 300, scalePercent) / 100.0f);

		// No taskbar button (a tool window), a drop shadow, topmost. Software drawing, as for the plugin windows
		// (PluginHost::OpenEditor): JUCE's GPU threads stay out of the host.
		stage->addToDesktop(juce::ComponentPeer::windowHasDropShadow);
		stage->setAlwaysOnTop(true);
		if (auto* peer = stage->getPeer()) peer->setCurrentRenderingEngine(0);
	}

	void PedalEditor::Open(const std::string& pedalId)
	{
		if (pedalId.empty()) { Close(); return; }
		EnsureWindow();
		ReadCatalog();
		ReadProgram();
		isOpen = true;
		ShowPedal(juce::String::fromUTF8(pedalId.c_str()));
		if (isShown) stage->toFront(false);
		Log("pedal window: %s", pedalId.c_str());
	}

	void PedalEditor::Close()
	{
		if (!isOpen) return;
		Flush();
		isOpen = false;
		if (stage && isShown) { Remember(); stage->setVisible(false); }
		isShown = false;
		if (stage) stage->SetPedal(nullptr);
		shownId.clear();
		Log("pedal window: closed");
	}

	void PedalEditor::Poll(bool overlayShown)
	{
		Flush();
		if (!isOpen || !stage) return;

		if (service.CatalogVersion() != catalogSeen)
		{
			ReadCatalog();
			ShowPedal(shownId);   // the controls may have changed (a Rescan found new captures)
		}
		if (service.ProgramVersion() != programSeen || (refreshWaiting && !IsHeld()))
		{
			ReadProgram();
			// The pedal left the board, or another kind took its id (a new tone): build the editor again.
			bool same = false;
			for (auto& pedal : board)
				same |= pedal.id == shownId && stage->Pedal() && stage->Pedal()->Plugin().name == pedal.kind;
			if (same) ApplyValues();
			else ShowPedal(shownId);
		}
		if (titlesStale)
		{
			titlesStale = false;
			swapSelected = -1;   // shown again even when the list itself is the same
			UpdateTitles();
		}

		const bool show = overlayShown;
		if (show != isShown)
		{
			isShown = show;
			if (!show) Remember();
			stage->setVisible(show);
			if (show) stage->toFront(false);
		}
	}

	void PedalEditor::ReadCatalog()
	{
		catalogSeen = service.CatalogVersion();
		// What each starter capture is (an amp or a pedal) and its description, from the installed tones manifest.
		captures = PedalUi::ReadCaptureManifest(juce::File(juce::String(service.DataPath().c_str()) + "\\StarterTones\\manifest.json"));
		catalog.Parse(juce::String::fromUTF8(service.Catalog().c_str()));
	}

	void PedalEditor::ReadProgram()
	{
		programSeen = service.ProgramVersion();
		board = PedalUi::ParseProgram(juce::String::fromUTF8(service.Program().c_str()), StompboxControl::CHAIN_NAME);
		UpdateTitles();
	}

	// The top bar: a picker of what can take this pedal's place, between a previous and a next button, showing what
	// the pedal is now. The general rule is same type only:
	//   amp      a NAM pedal playing an amp capture (or a file the manifest does not know, taken for an amp, as the
	//            page does): every amp capture
	//   cabinet  every IR in the Cabinets folder
	//   pedal    an effect, or a NAM pedal playing a pedal capture: every effect the browser offers, then every pedal
	//            capture
	// An amp, pedal capture or cab is listed by its capture or IR ("JCM2000 Crunch"), an effect by its kind ("Delay").
	void PedalEditor::UpdateTitles()
	{
		if (!stage) return;
		const PedalUi::ProgramPedal* pedal = nullptr;
		for (auto& candidate : board) if (candidate.id == shownId) pedal = &candidate;
		if (!pedal)
		{
			swaps.clear();
			swapTitles.clear();
			swapSelected = -1;
			stage->SetTitle(Title(shownId));
			return;
		}

		const auto lookup = [this](const char* tree, const juce::String& file) { return captures ? captures(tree, file) : PedalUi::CaptureInfo{}; };
		juce::String file;
		if (pedal->kind == kCabinetKind || IsNamKind(pedal->kind))
			if (const auto* param = FileParam(catalog.Find(pedal->kind), TreeOf(pedal->kind)))
				if (const auto value = pedal->values.find(param->name); value != pedal->values.end()) file = value->second;
		const bool isCabinet = pedal->kind == kCabinetKind;
		const bool isAmp = IsNamKind(pedal->kind) && lookup("NAM", file).kind != "pedal";

		std::vector<Swap> list;
		const auto addFiles = [&](const char* kind, const auto& keep)
		{
			const auto* param = FileParam(catalog.Find(kind), TreeOf(kind));
			if (!param) return;
			std::vector<std::pair<int, Swap>> found;
			for (auto& choice : param->choices)
			{
				const auto info = lookup(TreeOf(kind), choice);
				if (keep(info)) found.push_back({ SoundOrder(info), { kind, choice } });
			}
			std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			for (auto& item : found) list.push_back(item.second);
		};
		if (isCabinet) addFiles(kCabinetKind, [](const PedalUi::CaptureInfo&) { return true; });
		else if (isAmp) addFiles(kNamKind, [](const PedalUi::CaptureInfo& info) { return info.kind != "pedal"; });
		else
		{
			for (auto& plugin : catalog.Plugins())
				if (plugin.userSelectable && !IsNamKind(plugin.name) && plugin.name != kCabinetKind) list.push_back({ plugin.name, {} });
			addFiles(kNamKind, [](const PedalUi::CaptureInfo& info) { return info.kind == "pedal"; });
		}

		int selected = -1;
		for (int i = 0; i < static_cast<int>(list.size()) && selected < 0; ++i)
			if (list[static_cast<size_t>(i)].kind == pedal->kind && (list[static_cast<size_t>(i)].file.isEmpty() || list[static_cast<size_t>(i)].file == file)) selected = i;
		if (selected < 0)
		{
			// What it plays is not on offer (a kind the browser hides, a file gone since): it stays first, as it is.
			list.insert(list.begin(), { pedal->kind, file });
			selected = 0;
		}

		juce::StringArray titles;
		for (auto& item : list) titles.add(item.file.isNotEmpty() ? item.file.fromLastOccurrenceOf("/", false, false) : item.kind);
		swaps = std::move(list);
		if (titles == swapTitles && selected == swapSelected) return;
		swapTitles = titles;
		swapSelected = selected;
		stage->SetTitles(titles, selected);
	}

	// The picker's choice takes the pedal's place. Another capture or IR for the same kind is a file change on the
	// same pedal (its knobs stay as they are); another kind is a new pedal in the same place, at the page's starting
	// settings (StompboxControl::STARTING_SETTINGS), like a browser item dropped on the pedal.
	void PedalEditor::SwapTo(int index)
	{
		if (index < 0 || index >= static_cast<int>(swaps.size()) || index == swapSelected) return;
		const Swap target = swaps[static_cast<size_t>(index)];
		const PedalUi::ProgramPedal* pedal = nullptr;
		for (auto& candidate : board) if (candidate.id == shownId) pedal = &candidate;
		if (!pedal) return;
		Flush();   // edits for the pedal being swapped go first
		const std::string id = shownId.toStdString();

		if (target.kind == pedal->kind && target.file.isNotEmpty())
		{
			const auto* param = FileParam(catalog.Find(target.kind), TreeOf(target.kind));
			const bool ran = param && service.RunLocalCommand("SetParam " + id + " " + param->name.toStdString() + " "
				+ StompboxControl::Quote(target.file.toStdString()));
			if (!ran) { titlesStale = true; Log("pedal window: could not swap %s now", id.c_str()); return; }
			Log("pedal window: %s now plays %s", id.c_str(), target.file.toRawUTF8());
			return;
		}

		// A new id for the kind, as the page makes them (StompboxLink's NewPedalId): the kind, else kind_2, kind_3...
		const auto taken = [this](const juce::String& candidate)
		{
			return std::any_of(board.begin(), board.end(), [&](const PedalUi::ProgramPedal& p) { return p.id == candidate; });
		};
		juce::String newId = target.kind;
		for (int n = 2; taken(newId); ++n) newId = target.kind + "_" + juce::String(n);
		std::string chain = std::string("SetChain ") + StompboxControl::CHAIN_NAME;
		for (auto& item : board) chain += " " + (item.id == shownId ? newId : item.id).toStdString();
		if (!service.RunLocalCommand(chain)) { titlesStale = true; Log("pedal window: could not swap %s now", id.c_str()); return; }
		const std::string added = newId.toStdString();
		for (const auto& setting : StompboxControl::STARTING_SETTINGS)
			if (target.kind == setting.kind)
				service.RunLocalCommand("SetParam " + added + " " + setting.parameter + " " + StompboxControl::Quote(setting.value));
		if (target.file.isNotEmpty())
			if (const auto* param = FileParam(catalog.Find(target.kind), TreeOf(target.kind)))
				service.RunLocalCommand("SetParam " + added + " " + param->name.toStdString() + " " + StompboxControl::Quote(target.file.toStdString()));
		Log("pedal window: %s replaced by %s %s", id.c_str(), added.c_str(), target.file.toRawUTF8());
		shownId = newId;   // the next program has it in the old one's place: the window builds its editor for it
	}

	// Builds the editor for one of the board's pedals. A pedal that left the board gives way to the first one left;
	// an empty board closes the window.
	void PedalEditor::ShowPedal(const juce::String& id)
	{
		const PedalUi::ProgramPedal* pedal = nullptr;
		for (auto& candidate : board) if (candidate.id == id) pedal = &candidate;
		if (!pedal && !board.empty()) pedal = &board.front();
		const PedalUi::PluginDesc* desc = pedal ? catalog.Find(pedal->kind) : nullptr;
		if (!desc)
		{
			if (pedal) Log("pedal window: %s is not in Stompbox's catalogue", pedal->kind.toRawUTF8());
			Close();
			return;
		}

		Flush();   // edits for the pedal being left go first
		shownId = pedal->id;
		const std::string target = shownId.toStdString();
		auto view = std::make_unique<PedalUi::PedalView>(*desc, nullptr, captures);
		view->onNameChange = [this] { UpdateTitles(); };
		view->onParamChange = [this, target](const juce::String& param, double value)
		{
			const std::string key = "SetParam " + target + " " + param.toStdString();
			Queue(key, key + " " + Number(value));
		};
		view->onChoiceChange = [this, target](const juce::String& param, const juce::String& choice)
		{
			const std::string key = "SetParam " + target + " " + param.toStdString();
			Queue(key, key + " " + StompboxControl::Quote(choice.toStdString()));
		};
		view->onEnabledChange = [this, target](bool enabled)
		{
			const std::string key = "SetParam " + target + " Enabled";
			Queue(key, key + (enabled ? " 1" : " 0"));
		};
		stage->SetPedal(std::move(view));
		UpdateTitles();
		ApplyValues();
	}

	// The program's values into the window, except while the player holds a control (a value still on its way would
	// pull the knob back): those wait for the release.
	void PedalEditor::ApplyValues()
	{
		auto* view = stage ? stage->Pedal() : nullptr;
		if (!view) return;
		if (IsHeld()) { refreshWaiting = true; return; }
		refreshWaiting = false;
		for (auto& pedal : board)
		{
			if (pedal.id != shownId) continue;
			view->SetEnabled(pedal.enabled);
			for (auto& [param, value] : pedal.values) view->SetValueText(param, value);
		}
	}

	bool PedalEditor::IsHeld() const
	{
		return stage && stage->isMouseButtonDownAnywhere() && stage->isMouseOverOrDragging(true);
	}

	void PedalEditor::Queue(const std::string& key, const std::string& line)
	{
		for (auto& item : pending)
			if (item.first == key) { item.second = line; return; }
		pending.emplace_back(key, line);
	}

	// Runs the waiting edits; one the service cannot take now (a Rescan) stays for the next tick. The programs they
	// publish come back through Poll like the page's: a change the service refused (StompboxChecks) puts the control
	// back to what the rig plays.
	void PedalEditor::Flush()
	{
		while (!pending.empty())
		{
			if (!service.RunLocalCommand(pending.front().second)) return;
			pending.erase(pending.begin());
		}
	}

	void PedalEditor::Remember() const
	{
		if (!stage) return;
		const auto bounds = stage->getBounds();
		WriteInt(L"PedalWindowCentreX", bounds.getCentreX());
		WriteInt(L"PedalWindowTop", bounds.getY());
		WriteInt(L"PedalWindowScale", juce::roundToInt(stage->FitScale() * 100.0f));
	}
}
