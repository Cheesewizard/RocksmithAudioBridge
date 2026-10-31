// Pedal gallery: a prototype window for the generated pedal editors (Source/PedalUi), without the game, the
// bridge or the Stompbox engine. It reads a saved Stompbox config (the tones repo's tools/stompbox-catalog.txt)
// and shows every pedal a player can pick, one at a time on the popout's stage. Turning a control logs the
// Stompbox command the host will send for it.
//
// Usage: PedalGallery.exe --catalog <stompbox-catalog.txt> [--data <folder with NAM\ and Cabinets\>] [--skin <art folder>]
//                         [--snapshot <folder>]   (renders every pedal to a PNG there and exits)
// --data defaults to the catalog's folder's parent, which is the tones repo's root for its tools\ copy.

#include "../Source/PedalUi/PedalCatalog.hpp"
#include "../Source/PedalUi/PedalSkin.hpp"
#include "../Source/PedalUi/PedalStage.hpp"
#include "../Source/PedalUi/PedalView.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

using namespace juce;
using namespace PedalUi;

namespace
{
	CaptureLookup g_captures;   // the tones manifest next to --data: amp or pedal captures, their descriptions

	String ArgAfter(const StringArray& args, const String& flag)
	{
		const int index = args.indexOf(flag);
		return index >= 0 && index + 1 < args.size() ? args[index + 1].unquoted() : String();
	}

	// What Stompbox lists for a File parameter: the files under its folder, relative, without the extension.
	FileLister MakeFileLister(const File& data)
	{
		return [data](const String& tree)
		{
			StringArray names;
			const auto folder = data.getChildFile(tree);
			for (auto& entry : RangedDirectoryIterator(folder, true, "*.nam;*.wav", File::findFiles))
				names.add(entry.getFile().getRelativePathFrom(folder).upToLastOccurrenceOf(".", false, false).replaceCharacter('\\', '/'));
			names.sortNatural();
			return names;
		};
	}

	// The host's pedal window as it is in the game (PedalEditor): the stage is the window, fitted to the pedal, with
	// the pedal's name in its bar. The left and right arrow keys step through the pedals (the game's window has none).
	class GalleryWindow final
	{
	public:
		GalleryWindow(const std::vector<const PluginDesc*>& pedals, FileLister files) : pedals(pedals), files(std::move(files))
		{
			const auto area = Desktop::getInstance().getDisplays().getPrimaryDisplay()->userBounds.toNearestInt();
			stage.setBounds(area.getCentreX() - 450, area.getY() + area.getHeight() / 6, 900, 600);
			stage.SetWindowControls(true);
			stage.SetFitWindow(true, 1.0f);
			stage.onClose = [] { JUCEApplication::getInstance()->systemRequestedQuit(); };
			stage.onStep = [this](int delta)
			{
				const int count = static_cast<int>(this->pedals.size());
				Show((shown + delta + count) % count);
			};
			Show(0);
			stage.addToDesktop(ComponentPeer::windowAppearsOnTaskbar | ComponentPeer::windowHasDropShadow);
			stage.setVisible(true);
			stage.grabKeyboardFocus();
		}

	private:
		PedalStage stage;
		std::vector<const PluginDesc*> pedals;
		FileLister files;
		int shown = 0;

		void Show(int index)
		{
			if (!isPositiveAndBelow(index, static_cast<int>(pedals.size()))) return;
			shown = index;
			auto view = std::make_unique<PedalView>(*pedals[index], files, g_captures);
			const auto name = pedals[index]->name;
			view->onParamChange = [name](const String& param, double value)
			{
				Logger::writeToLog("SetParam " + name + " " + param + " " + String(value, 4));
			};
			view->onChoiceChange = [name](const String& param, const String& choice)
			{
				Logger::writeToLog("SetParam " + name + " " + param + " \"" + choice + "\"");
			};
			view->onEnabledChange = [name](bool enabled)
			{
				Logger::writeToLog("SetParam " + name + " Enabled " + String(enabled ? 1 : 0));
			};
			stage.SetPedal(std::move(view));
			const auto* pedal = stage.Pedal();
			stage.SetTitle(pedal->DisplayName() + "   (" + String(index + 1) + "/" + String(static_cast<int>(pedals.size())) + ", arrow keys)");
		}
	};

	class GalleryApp final : public JUCEApplication
	{
	public:
		const String getApplicationName() override { return "Pedal gallery"; }
		const String getApplicationVersion() override { return "0.1"; }
		bool moreThanOneInstanceAllowed() override { return true; }

		void initialise(const String& commandLine) override
		{
			const auto args = StringArray::fromTokens(commandLine, true);
			const File catalogFile(ArgAfter(args, "--catalog"));
			if (!catalog.LoadFile(catalogFile))
			{
				NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Pedal gallery",
					"Start it with --catalog <stompbox-catalog.txt> (the tones repo's tools folder has one).");
				quit();
				return;
			}
			const auto dataArg = ArgAfter(args, "--data");
			const File data = dataArg.isNotEmpty() ? File(dataArg) : catalogFile.getParentDirectory().getParentDirectory();
			g_captures = ReadCaptureManifest(data.getChildFile("manifest.json"));
			// The art: --skin <folder>, else the repo's VstHost\PedalSkin seen from build\PedalGallery\<config>\.
			const auto exe = File::getSpecialLocation(File::currentExecutableFile).getParentDirectory();
			const auto skinArg = ArgAfter(args, "--skin");
			Skin::Load(skinArg.isNotEmpty() ? File(skinArg) : exe.getChildFile("../../../VstHost/PedalSkin"));
			if (!Skin::IsLoaded()) Skin::Load(exe.getChildFile("PedalSkin"));

			std::vector<const PluginDesc*> pedals;
			for (auto& plugin : catalog.Plugins())
				if (plugin.userSelectable) pedals.push_back(&plugin);

			const auto snapshotArg = ArgAfter(args, "--snapshot");
			const auto programArg = ArgAfter(args, "--program");
			if (snapshotArg.isNotEmpty() && programArg.isNotEmpty())
			{
				// A saved tone's board, each pedal with the tone's values: what the host's pedal window shows for it.
				SnapshotProgram(File(programArg).loadFileAsString(), MakeFileLister(data), File(snapshotArg));
				quit();
				return;
			}
			if (snapshotArg.isNotEmpty())
			{
				Snapshot(pedals, MakeFileLister(data), File(snapshotArg));
				quit();
				return;
			}
			window = std::make_unique<GalleryWindow>(pedals, MakeFileLister(data));
		}

		void shutdown() override { window.reset(); }

	private:
		Catalog catalog;
		std::unique_ptr<GalleryWindow> window;

		void SnapshotProgram(const String& program, FileLister files, const File& folder) const
		{
			folder.createDirectory();
			PedalStage stage;
			stage.setSize(1100, 700);
			stage.SetFitWindow(true, 1.0f);   // the host window's size for each pedal
			const auto board = ParseProgram(program, "Main");
			StringArray titles;
			for (auto& pedal : board) titles.add(pedal.id);
			for (size_t i = 0; i < board.size(); ++i)
			{
				const auto* desc = catalog.Find(board[i].kind);
				if (!desc) { Logger::writeToLog("not in the catalogue: " + board[i].kind); continue; }
				auto view = std::make_unique<PedalView>(*desc, files, g_captures);
				view->SetEnabled(board[i].enabled);
				for (auto& [param, value] : board[i].values) view->SetValueText(param, value);
				stage.SetTitles(titles, static_cast<int>(i));
				stage.SetPedal(std::move(view));
				stage.RefreshReflection();
				const auto image = stage.createComponentSnapshot(stage.getLocalBounds(), true, 1.0f);
				FileOutputStream out(folder.getChildFile(String(static_cast<int>(i) + 1) + " " + File::createLegalFileName(board[i].id) + ".png"));
				if (out.openedOk()) { out.setPosition(0); out.truncate(); PNGImageFormat().writeImageToStream(image, out); }
			}
		}

		static void Snapshot(const std::vector<const PluginDesc*>& pedals, FileLister files, const File& folder)
		{
			folder.createDirectory();
			PedalStage stage;
			stage.setSize(1100, 700);
			StringArray titles;
			for (auto* pedal : pedals) titles.add(pedal->name);

			auto save = [&](const String& name)
			{
				stage.RefreshReflection();
				const auto image = stage.createComponentSnapshot(stage.getLocalBounds(), true, 1.0f);
				FileOutputStream out(folder.getChildFile(File::createLegalFileName(name) + ".png"));
				if (out.openedOk()) { out.setPosition(0); out.truncate(); PNGImageFormat().writeImageToStream(image, out); }
			};

			for (int i = 0; i < static_cast<int>(pedals.size()); ++i)
			{
				stage.SetTitles(titles, i);
				stage.SetPedal(std::make_unique<PedalView>(*pedals[i], files, g_captures));
				save(pedals[i]->name);
			}
		}
	};
}

START_JUCE_APPLICATION(GalleryApp)
