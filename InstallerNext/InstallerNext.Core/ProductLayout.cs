#nullable enable
using System;
using System.Collections.Generic;
using System.IO;

namespace RocksmithAudioBridge.Setup
{
	/// <summary>
	/// Where every part of Rocksmith Audio Bridge lives once installed, and what the 4.x installer (Installer/) left
	/// behind. One place, so the planner, the uninstaller and the tests agree.
	///
	///   &lt;game&gt;\xinput1_3.dll (+ .pdb)            upstream RSMods built with our mods (Mod/Build-ModDll.ps1), put in by
	///                                               upstream's RsModsInstallation together with its configurator
	///   &lt;game&gt;\RSMods\RSMods.exe and its payload  upstream's Avalonia configurator (RSModsGUI.zip), listed in
	///                                               RSMods\RSModsGUI.manifest.json
	///   &lt;game&gt;\RSMods\mods.manifest.json         our DLL's own manifest, loose (never inside the configurator payload:
	///                                               a later official install deletes files the previous payload listed)
	///   &lt;game&gt;\RocksmithAudioBridgeAsio.dll, ...Asio64.dll, ...DriverHost.exe, ...VstHost.exe, stompbox-capi.dll,
	///   PedalSkin\                                  the native pieces, where the 4.x installer put them (the driver is
	///                                               registered from this path; the DLL finds the VST host beside it)
	///   &lt;game&gt;\RSMods\AudioBridge\               the helper exe (Mod/Core/HelperExe.cpp looks for it here)
	///   &lt;game&gt;\RocksmithAudioBridge\             our desktop app (App/), its notices and our install record. A folder
	///                                               directly in the game folder, so the app's own game folder detection
	///                                               (GameFolder: "the folder above the running exe") finds the game.
	/// </summary>
	public static class ProductLayout
	{
		public const string ProductName = "Rocksmith Audio Bridge";

		public const string NativeModFile = "xinput1_3.dll";
		public const string NativeModPdb = "xinput1_3.pdb";
		public const string RsModsFolder = "RSMods";
		public const string LooseManifest = @"RSMods\mods.manifest.json";
		public const string LooseManifestSetAside = @"RSMods\mods.manifest.json.bak";
		public const string ConfiguratorInstalledManifest = @"RSMods\RSModsGUI.manifest.json";

		public const string HelperFolder = @"RSMods\AudioBridge";
		public const string HelperExe = @"RSMods\AudioBridge\RocksmithAudioBridge.Helper.exe";

		public const string AppFolder = "RocksmithAudioBridge";
		public const string AppExeName = "RocksmithAudioBridge.exe";
		public const string AppExe = @"RocksmithAudioBridge\RocksmithAudioBridge.exe";
		/// <summary>Every file this installer wrote, relative to the game folder (the 4.x installer's
		/// RSModsPlus-installed-files.txt, moved beside the app).</summary>
		public const string InstallRecord = @"RocksmithAudioBridge\RocksmithAudioBridge-installed-files.txt";

		public const string AsioDriver = "RocksmithAudioBridgeAsio.dll";
		public const string AsioDriver64 = "RocksmithAudioBridgeAsio64.dll";
		public const string DriverHost = "RocksmithAudioBridgeDriverHost.exe";
		public const string VstHost = "RocksmithAudioBridgeVstHost.exe";
		public const string StompboxEngine = "stompbox-capi.dll";

		/// <summary>The external mod name the DLL build stamps into SpecialBuild ("External mods: RocksmithAudioBridge").</summary>
		public const string ExternalModName = "RocksmithAudioBridge";

		/// <summary>Logs the app's installer verbs write beside the app (AsioProxySetup, NoteByNoteMenuInstaller).</summary>
		public static readonly string[] AppVerbLogs = { @"RocksmithAudioBridge\AudioBridgeDriverInstall.log", @"RocksmithAudioBridge\NoteByNoteMenuInstall.log" };
		/// <summary>UsbDkSetup writes its MSI copy, licence and log here.</summary>
		public static readonly string[] UsbDkFiles = { @"RSMods\UsbDk\UsbDk_1.0.22_x64.msi", @"RSMods\UsbDk\LICENSE.txt", @"RSMods\UsbDk\UsbDkInstall.log" };

		/// <summary>The Note by Note menu installer's recovery copy of cache.psarc (NoteByNoteMenuInstaller.BackupPath).</summary>
		public const string MenuBackup = "cache.pre-note-by-note-menu.psarc";

		/// <summary>Settings removed only when the player asks (as in 4.x).</summary>
		public static readonly string[] SettingsFiles = { "RSMods.ini", "AudioRouting.ini" };

		/// <summary>Folders removed when they end up empty, deepest first. RSMods\ can hold the player's own files.</summary>
		public static readonly string[] FoldersRemovedWhenEmpty =
		{
			@"RocksmithAudioBridge\ThirdParty", @"RocksmithAudioBridge\tools", @"RocksmithAudioBridge\ddc", AppFolder,
			@"RSMods\AudioBridge\tools", @"RSMods\AudioBridge\ThirdParty", HelperFolder,
			@"RSMods\UsbDk", @"RSMods\ddc", @"RSMods\tools", @"RSMods\CustomMods", RsModsFolder,
			"PedalSkin",
		};

		// ---- 4.x (Installer/, RSMods.exe fork) -------------------------------------------------------------------------

		/// <summary>The 4.x installer's record of what it wrote (Installer/Worker.cs DLLStuff.InstallManifestName).</summary>
		public const string Legacy4xInstallRecord = @"RSMods\RSModsPlus-installed-files.txt";

		/// <summary>Files every 4.x version wrote (Installer/Uninstaller.cs CoreFiles plus the licence texts InjectGUI
		/// wrote). Removed when migrating, except the ones this installer writes again.</summary>
		public static readonly string[] Legacy4xFiles =
		{
			"xinput1_3.pdb", ".rsmods-audio-mode.lock",
			@"RSMods\RSMods.exe", @"RSMods\RSMods.exe.config", @"RSMods\RocksmithAudioBridge.dll", @"RSMods\RSMods.pdb",
			@"RSMods\LICENSE.txt", @"RSMods\NOTICE.txt", @"RSMods\VstHost-LICENSE.txt", @"RSMods\VstHost-JUCE-LICENSE.txt",
			@"RSMods\VstHost-ThirdParty-Notices.txt", @"RSMods\Stompbox-LICENSE.txt", @"RSMods\Stompbox-ThirdParty-Notices.txt",
			@"RSMods\StompboxEngine-NOTICE.txt", "VstHost-THIRD-PARTY-NOTICES.txt",
			// Earlier 4.0 release candidates put the settings app in the game folder itself.
			"RSMods.exe", "RSMods.exe.config", "RocksmithAudioBridge.dll", "RSMods.pdb",
			Legacy4xInstallRecord,
		};

		/// <summary>Logs the 4.x RSMods.exe verbs wrote beside it.</summary>
		public static readonly string[] Legacy4xLogs = { @"RSMods\AudioBridgeDriverInstall.log", @"RSMods\NoteByNoteMenuInstall.log", "NoteByNoteMenuInstall.log" };

		/// <summary>The 4.x desktop shortcut (Installer/GUI.cs CreateDesktopShortcut) pointed at RSMods.exe.</summary>
		public const string Legacy4xDesktopShortcutName = "RSMods.url";

		/// <summary>%LOCALAPPDATA%\Rocksmith Audio Bridge\Runtime: the 4.x RSMods.exe unpacked its bundled runtime here.</summary>
		public static string Legacy4xRuntimeFolder(string localAppData) => Path.Combine(localAppData, ProductName, "Runtime");

		// ---- per-user state ---------------------------------------------------------------------------------------------

		/// <summary>HKCU key the bridge driver and the app keep their settings in (removed with the settings).</summary>
		public const string UserSettingsKey = @"Software\RSMods\AsioProxy";
		/// <summary>HKCU key remembering that this installer put UsbDk on the PC (kept apart from the settings key, which
		/// removing the settings deletes before the UsbDk question is answered).</summary>
		public const string InstallerStateKey = @"Software\RSMods\Installer";
		public const string UsbDkInstalledByUsValue = "InstalledUsbDk";
		/// <summary>UsbDk 1.0.22 x64, the exact MSI pinned by hash, so its product code is fixed.</summary>
		public const string UsbDkProductCode = "{6D4A6ED0-CF41-4615-A4B3-BDA018C3C1CD}";

		public const string StartMenuShortcutName = ProductName + ".lnk";

		/// <summary>Combines a game-relative path with the game folder, refusing anything that escapes it.</summary>
		public static string InGame(string gameFolder, string relative)
		{
			string root = Path.GetFullPath(gameFolder).TrimEnd('\\') + "\\";
			string full = Path.GetFullPath(Path.Combine(root, relative));
			if (string.Equals(full.TrimEnd('\\'), root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) return root.TrimEnd('\\');
			if (!full.StartsWith(root, StringComparison.OrdinalIgnoreCase))
				throw new InvalidOperationException("Refusing a path outside the game folder: " + relative);
			return full;
		}

		/// <summary>The game-relative form of a full path, or null when it is outside the game folder.</summary>
		public static string? Relative(string gameFolder, string fullPath)
		{
			string root = Path.GetFullPath(gameFolder).TrimEnd('\\') + "\\";
			string full = Path.GetFullPath(fullPath);
			return full.StartsWith(root, StringComparison.OrdinalIgnoreCase) ? full.Substring(root.Length) : null;
		}

		public static IEqualityComparer<string> PathComparer => StringComparer.OrdinalIgnoreCase;
	}
}
