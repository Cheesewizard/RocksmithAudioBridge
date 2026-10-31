#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using RS2014_Mod_Installer.Core;
using RS2014_Mod_Installer.Payload;
using RocksmithAudioBridge.Core;

namespace RocksmithAudioBridge.Setup
{
	/// <summary>
	/// Builds the install and uninstall plans. Planning only reads; the steps act through an IInstallHost when run.
	///
	/// Install (each step names the 4.x Installer/ step it replaces):
	///   checks       game folder, Rocksmith2014.exe hash (upstream RocksmithCompatibility; was GUI.IsVoid/ExeUtil),
	///                game closed, our app / the configurator / the helper closed (was IsGameRunning/IsOurToolRunning),
	///                payload complete, what xinput1_3.dll is now (upstream ExistingNativeModInspector)
	///    1 rsmods-framework  upstream RsModsInstallation.Install: our DLL + upstream configurator (was InjectDLL/InjectGUI)
	///    2 loose-manifest    RSMods\mods.manifest.json, after (1) since (1) sets a custom build's manifest aside
	///    3-6 copy groups     game\, helper\, app\, notices\ (was InjectDLL's driver/host files and InjectGUI's licences)
	///    7 starter-tones     (was StarterTones.Install)
	///    8 legacy-4x-cleanup 4.x-only files (RSMods\RocksmithAudioBridge.dll, licences in RSMods\, ...) and its runtime cache
	///    9 install-record    always runs (was DLLStuff.SaveInstallManifest)
	///   10 driver            app --install-audio-bridge-driver: UsbDk, regsvr32 x86+x64 (one UAC), RS_ASIO.ini set-up
	///                        (was Uninstaller.RunDriverInstall through RSMods.exe); failure rolls the install back as 4.x did
	///   11 menu-row          app --install-note-by-note-menu (was RunMenuTool add)
	///   12 shortcuts         Start menu (+ optional desktop) .lnk to the app (was CreateDesktopShortcut's RSMods.url)
	///   13 launch            optional (was Process.Start(RSMods.exe))
	///
	/// Uninstall (the 4.x order: Installer/Uninstaller.cs header):
	///    1 driver-unregister, 2 menu-row-remove, 3 rs-asio-restore, 4 remove-files, 5 shortcuts-remove,
	///    6 starter-tones-remove, 7 usbdk-remove.
	/// </summary>
	public sealed class InstallPlanner
	{
		private static readonly TimeSpan VerbTimeout = TimeSpan.FromMinutes(3);
		private readonly ISystemProbe _probe;

		public InstallPlanner(ISystemProbe probe) => _probe = probe;

		// =============================================================================================================
		// Install
		// =============================================================================================================

		public InstallPlan PlanInstall(InstallOptions options)
		{
			string game = Path.GetFullPath(options.GameFolder);
			var plan = new InstallPlan(PlanOperation.Install, game);
			var payload = options.Payload;

			// ---- checks ---------------------------------------------------------------------------------------------
			if (!_probe.IsRocksmithFolder(game))
				plan.Blockers.Add("Not a Rocksmith 2014 folder (needs cache.psarc and a non-empty dlc folder): " + game);
			else plan.Checks.Add("Rocksmith 2014 folder: ok");

			if (!options.SkipCompatibilityCheck && File.Exists(Path.Combine(game, "Rocksmith2014.exe")))
			{
				CompatibilityResult compatibility = _probe.CheckCompatibility(game);
				if (compatibility.IsCompatible) plan.Checks.Add("Rocksmith2014.exe is a supported Steam build: ok");
				else plan.Blockers.Add(compatibility.ExecutableUnreadable
					? "Rocksmith2014.exe is in use. Close Rocksmith, then try again."
					: "Unsupported Rocksmith version: " + string.Join(" ", compatibility.Reasons));
			}

			AddRunningChecks(plan, game);

			var missing = payload.MissingRequired();
			if (missing.Count == 0) plan.Checks.Add("Payload " + payload.Root + ": complete");
			else plan.Blockers.Add("The payload at " + payload.Root + " is missing " + missing.Count + " required file(s): " + string.Join(", ", missing));

			ExistingNativeMod existing = _probe.InspectNativeMod(game);
			bool ours = IsOurNativeMod(existing, game);
			bool replaceNativeMod = true;
			string existingText = existing.Kind + (existing.Description.Length > 0 ? " (" + existing.Description + ")" : "");
			if (existing.NeedsConfirmation && !ours)
			{
				if (options.ReplaceForeignNativeMod == null)
					plan.Blockers.Add("xinput1_3.dll is " + existingText + ". Replacing it needs the player's say (--replace-native-mod or --keep-native-mod).");
				else replaceNativeMod = options.ReplaceForeignNativeMod.Value;
			}
			plan.Checks.Add("Current xinput1_3.dll: " + existingText + (ours ? ", ours: replaced without asking" : "")
				+ (replaceNativeMod ? "" : ", KEPT (our mods will not run in game)"));
			if (Legacy4xPresent(game)) plan.Checks.Add("Rocksmith Audio Bridge 4.x found: it is migrated (4.x-only files removed after the new ones are in)");

			// ---- 1. upstream framework + configurator ---------------------------------------------------------------
			string dll = payload.PathOf(Path.Combine(PayloadLayout.NativeFolder, ProductLayout.NativeModFile));
			string pdb = payload.PathOf(Path.Combine(PayloadLayout.NativeFolder, ProductLayout.NativeModPdb));
			string zip = payload.PathOf(Path.Combine(PayloadLayout.ConfiguratorFolder, PayloadLayout.ConfiguratorZip));
			string zipManifest = payload.PathOf(Path.Combine(PayloadLayout.ConfiguratorFolder, PayloadLayout.ConfiguratorManifest));
			var framework = new PlannedStep("rsmods-framework",
				"Upstream RSMods: our xinput1_3.dll and upstream's configurator (RsModsInstallation.Install, all or nothing)",
				"DLLStuff.InjectDLL (xinput1_3.dll, D3DX9_42.dll removal) + DLLStuff.InjectGUI (RSMods.exe, now upstream's Avalonia configurator; the WinForms GUI is no longer installed)",
				context =>
				{
					context.Host.InstallRsMods(game, existing, replaceNativeMod, dll, pdb, zip, zipManifest);
				});
			framework.Details.Add((replaceNativeMod ? "replace " : "keep ") + Path.Combine(game, ProductLayout.NativeModFile) + " (+ .pdb)");
			framework.Details.Add("configurator payload -> " + ConfiguratorPaths.RsModsFolder(game) + " (stale files of the previous payload and the old WinForms GUI's leftovers removed by upstream)");
			plan.Steps.Add(framework);

			// ---- 2. loose manifest ----------------------------------------------------------------------------------
			string looseSource = payload.PathOf(Path.Combine(PayloadLayout.RsModsFolder, "mods.manifest.json"));
			var loose = new PlannedStep("loose-manifest",
				"Our DLL's mods.manifest.json beside the configurator (kept out of the configurator payload)",
				"none (new: the 4.x settings app had our settings built in)",
				context =>
				{
					Copy(context, looseSource, ProductLayout.LooseManifest, game);
					// RsModsInstallation set our own previous manifest aside as .bak (it treats any custom build so).
					if (existing.Kind == ExistingNativeModKind.CustomRsMods && ours)
						context.Host.DeleteFile(ProductLayout.InGame(game, ProductLayout.LooseManifestSetAside));
				});
			loose.Details.Add(looseSource + " -> " + ProductLayout.InGame(game, ProductLayout.LooseManifest));
			plan.Steps.Add(loose);

			// ---- 3-6. copy groups -----------------------------------------------------------------------------------
			foreach (var group in PayloadLayout.CopyGroups)
			{
				IReadOnlyList<string> files = payload.FilesUnder(group.PayloadFolder);
				if (files.Count == 0) files = PayloadLayout.ExpectedFilesUnder(group.PayloadFolder).ToList();
				if (files.Count == 0) continue;   // an optional group this payload does not carry
				var copies = files.Select(f => (Source: payload.PathOf(Path.Combine(group.PayloadFolder, f)),
					Relative: group.GameRelativeDestination.Length == 0 ? f : Path.Combine(group.GameRelativeDestination, f))).ToList();
				var step = new PlannedStep("copy-" + group.PayloadFolder, group.Title, group.Replaces4x, context =>
				{
					foreach (var copy in copies) Copy(context, copy.Source, copy.Relative, game);
				});
				step.Details.Add(copies.Count + " file(s) -> " + ProductLayout.InGame(game, group.GameRelativeDestination.Length == 0 ? "." : group.GameRelativeDestination));
				foreach (var copy in copies.Take(12)) step.Details.Add(copy.Relative + (File.Exists(copy.Source) ? "" : "   (not in payload)"));
				if (copies.Count > 12) step.Details.Add("... and " + (copies.Count - 12) + " more");
				plan.Steps.Add(step);
			}

			// ---- 7. starter tones -----------------------------------------------------------------------------------
			string tones = payload.PathOf(Path.Combine(PayloadLayout.TonesFolder, PayloadLayout.StarterTonesZip));
			string engineFolder = Path.Combine(_probe.AppData, "RocksmithAudioBridge", "Stompbox");
			var tonesStep = new PlannedStep("starter-tones", "Tone designer starter tones (the player's own files are never overwritten)",
				"StarterTones.Install", context =>
				{
					if (!File.Exists(tones)) { context.Host.Log("   no starter tones in this payload; skipped"); return; }
					StarterTones.Install(tones, engineFolder, context.Host);
				})
			{ OnFailure = StepFailure.Warn };
			tonesStep.Details.Add(File.Exists(tones) ? tones + " -> " + engineFolder : "skipped: the payload has no " + PayloadLayout.TonesFolder + "\\" + PayloadLayout.StarterTonesZip);
			plan.Steps.Add(tonesStep);

			// ---- 8. 4.x leftovers -----------------------------------------------------------------------------------
			var legacy = Legacy4xLeftovers(game);
			string legacyRuntime = ProductLayout.Legacy4xRuntimeFolder(_probe.LocalAppData);
			var legacyStep = new PlannedStep("legacy-4x-cleanup",
				"Remove what only the 4.x install used (the WinForms settings app's library, its licence copies, its runtime cache)",
				"none (migration from 4.x)", context =>
				{
					foreach (string relative in legacy) context.Host.DeleteFile(ProductLayout.InGame(game, relative));
					context.Host.DeleteDirectory(legacyRuntime, onlyIfEmpty: false);
				})
			{ OnFailure = StepFailure.Warn };
			foreach (string relative in legacy) legacyStep.Details.Add("delete " + relative);
			if (Directory.Exists(legacyRuntime)) legacyStep.Details.Add("delete folder " + legacyRuntime);
			if (legacyStep.Details.Count == 0) legacyStep.Details.Add("nothing from 4.x found");
			plan.Steps.Add(legacyStep);

			// ---- 9. install record ----------------------------------------------------------------------------------
			var record = new PlannedStep("install-record", "Record every file written, so Uninstall removes exactly those",
				"DLLStuff.SaveInstallManifest (RSMods\\RSModsPlus-installed-files.txt)", context =>
				{
					var lines = new SortedSet<string>(InstallRecord.Read(game), StringComparer.OrdinalIgnoreCase);
					lines.UnionWith(context.State.Written);
					lines.Add(ProductLayout.InstallRecord);
					context.Host.WriteAllText(ProductLayout.InGame(game, ProductLayout.InstallRecord), string.Join(Environment.NewLine, lines) + Environment.NewLine);
				})
			{ AlwaysRun = true, OnFailure = StepFailure.Warn };
			record.Details.Add(ProductLayout.InGame(game, ProductLayout.InstallRecord));
			plan.Steps.Add(record);

			// ---- 10. driver, UsbDk, RS_ASIO ------------------------------------------------------------------------
			string app = ProductLayout.InGame(game, ProductLayout.AppExe);
			if (options.RegisterDriver)
			{
				var driver = new PlannedStep("driver",
					"Register the ASIO bridge driver x86 + x64 guest (one UAC prompt), install UsbDk, record the wrapped interface, set RS_ASIO.ini up on a fresh install",
					"Uninstaller.RunDriverInstall (RSMods.exe --install-audio-bridge-driver; same verb and exit codes, now the desktop app)",
					context => RunDriverInstall(context, app, game))
				{ OnFailure = StepFailure.AbortAndRollBack };
				driver.Details.Add("\"" + app + "\" " + AsioProxySetup.InstallArgument + " \"" + game + "\"");
				driver.Details.Add("exit 0 ok; 2 admin prompt declined (rolled back); 3 UsbDk failed (warning); 4 UsbDk needs a restart; other: see "
					+ ProductLayout.InGame(game, ProductLayout.AppVerbLogs[0]));
				plan.Steps.Add(driver);
			}

			// ---- 11. Note by Note menu row --------------------------------------------------------------------------
			if (options.InstallMenuRow)
			{
				var menu = new PlannedStep("menu-row", "Add the Note by Note row to Riff Repeater in cache.psarc (only our entry is touched)",
					"Uninstaller.RunMenuTool(add: true)", context =>
					{
						int exit = context.Host.RunProcess(app, "--install-note-by-note-menu " + Quote(game), Path.GetDirectoryName(app)!, VerbTimeout, elevated: false);
						if (exit != 0) throw new StepFailedException("the Note by Note entry could not be added to Riff Repeater (exit " + exit + "; see "
							+ ProductLayout.InGame(game, ProductLayout.AppVerbLogs[1]) + "). Run the installer again to retry.");
					})
				{ OnFailure = StepFailure.Warn };
				menu.Details.Add("\"" + app + "\" --install-note-by-note-menu \"" + game + "\" (backs cache.psarc up as " + ProductLayout.MenuBackup + " first)");
				plan.Steps.Add(menu);
			}

			// ---- 12. shortcuts --------------------------------------------------------------------------------------
			var shortcuts = new List<string>();
			if (options.CreateStartMenuShortcut) shortcuts.Add(Path.Combine(_probe.StartMenuPrograms, ProductLayout.StartMenuShortcutName));
			if (options.CreateDesktopShortcut) shortcuts.Add(Path.Combine(_probe.Desktop, ProductLayout.StartMenuShortcutName));
			if (shortcuts.Count > 0)
			{
				var step = new PlannedStep("shortcuts", "Shortcuts to the desktop app", "GUI.CreateDesktopShortcut (RSMods.url on the desktop)", context =>
				{
					foreach (string shortcut in shortcuts)
						context.Host.CreateShortcut(shortcut, app, Path.GetDirectoryName(app)!, ProductLayout.ProductName);
				})
				{ OnFailure = StepFailure.Warn };
				step.Details.AddRange(shortcuts.Select(s => s + " -> " + app));
				plan.Steps.Add(step);
			}

			// ---- 13. launch -----------------------------------------------------------------------------------------
			if (options.LaunchApp)
				plan.Steps.Add(new PlannedStep("launch", "Open the desktop app", "Process.Start(RSMods.exe)", context => context.Host.StartProgram(app))
				{ OnFailure = StepFailure.Warn });

			plan.RollbackFactory = keepLogs => RemovalSteps(game, removeSettings: false, keepLogs: keepLogs, keepConfigurator: false, includeStarterTones: true);
			return plan;
		}

		private static void Copy(StepContext context, string source, string relative, string game)
		{
			string destination = ProductLayout.InGame(game, relative);
			context.Host.CopyFile(source, destination);
			context.State.Written.Add(relative);
		}

		private void RunDriverInstall(StepContext context, string app, string game)
		{
			int exit = context.Host.RunProcess(app, AsioProxySetup.InstallArgument + " " + Quote(game), Path.GetDirectoryName(app)!, VerbTimeout, elevated: false);
			switch (exit)
			{
				case 0: return;
				case 2: throw new OperationCanceledException("Rocksmith Audio Bridge was not installed: its audio driver is required and the Windows admin prompt was declined. Run the installer again and approve it.");
				case AsioProxySetup.EXIT_USBDK_FAILED:
					context.State.Warnings.Add("Direct USB capture for the Real Tone Cable is not available yet: UsbDk could not be installed. The cable still works the usual way. Details: "
						+ ProductLayout.InGame(game, ProductLayout.UsbDkFiles[2]));
					return;
				case AsioProxySetup.EXIT_USBDK_RESTART:
					// A fresh UsbDk install (it always needs a restart): remembered, so Uninstall offers to remove it.
					context.State.RestartNeeded = true;
					context.Host.SetUserRegistryDword(ProductLayout.InstallerStateKey, ProductLayout.UsbDkInstalledByUsValue, 1);
					return;
				case -1: throw new StepFailedException("the driver set-up did not finish within 3 minutes");
				default: throw new StepFailedException("the audio driver could not be set up (exit " + exit + "); see " + ProductLayout.InGame(game, ProductLayout.AppVerbLogs[0]));
			}
		}

		private void AddRunningChecks(InstallPlan plan, string game)
		{
			if (_probe.IsGameRunning()) plan.Blockers.Add("Rocksmith is open. Close it first.");
			else plan.Checks.Add("Rocksmith is closed: ok");
			foreach (string relative in new[] { ProductLayout.AppExe, ProductLayout.HelperExe, @"RSMods\RSMods.exe", "RSMods.exe" })
			{
				string exe = ProductLayout.InGame(game, relative);
				if (File.Exists(exe) && _probe.IsRunningFrom(exe)) plan.Blockers.Add(relative + " is open. Close it first.");
			}
		}

		/// <summary>Our build: SpecialBuild "External mods: RocksmithAudioBridge", or the 4.x fork's DLL (no version info)
		/// in a folder the 4.x installer set up.</summary>
		public static bool IsOurNativeMod(ExistingNativeMod existing, string game) =>
			(existing.Kind == ExistingNativeModKind.CustomRsMods
				&& existing.Description.Split(',').Select(s => s.Trim()).All(s => s.Equals(ProductLayout.ExternalModName, StringComparison.OrdinalIgnoreCase)))
			|| (existing.Kind == ExistingNativeModKind.RsMods && existing.Description.Length == 0 && Legacy4xPresent(game));

		public static bool Legacy4xPresent(string game) =>
			File.Exists(ProductLayout.InGame(game, ProductLayout.Legacy4xInstallRecord))
			|| File.Exists(ProductLayout.InGame(game, @"RSMods\RocksmithAudioBridge.dll"))
			|| File.Exists(ProductLayout.InGame(game, "RocksmithAudioBridge.dll"));

		/// <summary>Paths the new layout owns: never removed as 4.x leftovers, since this install writes them again.</summary>
		internal static bool OwnedByNewLayout(string relative)
		{
			string[] owned = { ProductLayout.NativeModFile, ProductLayout.NativeModPdb, ProductLayout.AsioDriver, ProductLayout.AsioDriver64,
				ProductLayout.DriverHost, ProductLayout.VstHost, ProductLayout.StompboxEngine, @"RSMods\RSMods.exe", ProductLayout.LooseManifest };
			if (owned.Contains(relative, StringComparer.OrdinalIgnoreCase)) return true;
			foreach (string prefix in new[] { @"PedalSkin\", ProductLayout.HelperFolder + @"\", ProductLayout.AppFolder + @"\", @"RSMods\UsbDk\" })
				if (relative.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) return true;
			return false;
		}

		/// <summary>4.x files present in the game folder that the new layout does not write again.</summary>
		internal static List<string> Legacy4xLeftovers(string game)
		{
			var candidates = new SortedSet<string>(ProductLayout.Legacy4xFiles, StringComparer.OrdinalIgnoreCase);
			candidates.UnionWith(ReadLines(ProductLayout.InGame(game, ProductLayout.Legacy4xInstallRecord)));
			candidates.UnionWith(ProductLayout.Legacy4xLogs);
			var configurator = ConfiguratorFiles(game);
			return candidates
				.Where(relative => !OwnedByNewLayout(relative) && !configurator.Contains(relative))
				.Where(relative => SafeExists(game, relative))
				.ToList();
		}

		// =============================================================================================================
		// Uninstall
		// =============================================================================================================

		public InstallPlan PlanUninstall(UninstallOptions options)
		{
			string game = Path.GetFullPath(options.GameFolder);
			var plan = new InstallPlan(PlanOperation.Uninstall, game);
			bool installed = File.Exists(ProductLayout.InGame(game, ProductLayout.InstallRecord))
				|| File.Exists(ProductLayout.InGame(game, ProductLayout.AppExe)) || Legacy4xPresent(game);
			if (!installed) plan.Blockers.Add("Rocksmith Audio Bridge was not found in " + game);
			else plan.Checks.Add("Rocksmith Audio Bridge found: ok");
			AddRunningChecks(plan, game);

			string? tool = VerbTool(game);
			plan.Checks.Add(tool == null ? "No installed app to run the menu and RS_ASIO verbs; those steps are skipped" : "Verbs run through " + tool);

			// ---- 1. driver ------------------------------------------------------------------------------------------
			string proxy = ProductLayout.InGame(game, ProductLayout.AsioDriver);
			bool registered = _probe.IsDriverRegistered();
			if (registered)
			{
				var step = new PlannedStep("driver-unregister",
					"Remove the ASIO bridge driver registration while its file is still there (one UAC prompt; declining stops here, nothing removed)",
					"Uninstaller.RemoveDriverRegistration", context => context.Host.UnregisterDriver(proxy));
				step.Details.Add("HKLM\\SOFTWARE\\ASIO\\" + AsioProxySetup.ProxyName + " and its CLSID, 32- and 64-bit views; HKCU CLSID override");
				plan.Steps.Add(step);
			}
			else plan.Checks.Add("ASIO bridge driver not registered: nothing to unregister");

			// ---- 2. menu row ----------------------------------------------------------------------------------------
			var menu = new PlannedStep("menu-row-remove", "Remove our Riff Repeater row from the current cache.psarc (surgically)",
				"Uninstaller.RunMenuTool(add: false)", context =>
				{
					if (tool == null) throw new StepFailedException("no installed app to remove the Note by Note row with; it stays as an inactive row");
					int exit = context.Host.RunProcess(tool, "--uninstall-note-by-note-menu " + Quote(game), Path.GetDirectoryName(tool)!, VerbTimeout, elevated: false);
					if (exit != 0) throw new StepFailedException("the Note by Note entry could not be removed from Riff Repeater (exit " + exit
						+ "). Without the mod it only shows an inactive row; " + ProductLayout.MenuBackup + " is kept.");
					context.State.MenuEntryRemoved = true;
				})
			{ OnFailure = StepFailure.Warn };
			if (tool != null) menu.Details.Add("\"" + tool + "\" --uninstall-note-by-note-menu \"" + game + "\"");
			plan.Steps.Add(menu);

			// ---- 3. RS_ASIO.ini restore -----------------------------------------------------------------------------
			var restore = new PlannedStep("rs-asio-restore",
				"Put the player's own RS_ASIO.ini back if \"Set up for me\" replaced it (before the files: it needs the app and its registry state)",
				"Uninstaller.RunRsAsioRestore", context =>
				{
					if (tool == null) throw new StepFailedException("no installed app to restore RS_ASIO.ini with");
					int exit = context.Host.RunProcess(tool, RsAsioIniSetup.RestoreArgument + " " + Quote(game), Path.GetDirectoryName(tool)!, TimeSpan.FromMinutes(1), elevated: false);
					if (exit != 0) throw new StepFailedException("your original RS_ASIO.ini could not be put back. A copy is in " + RsAsioIniSetup.BackupFolder + ".");
				})
			{ OnFailure = StepFailure.Warn };
			if (tool != null) restore.Details.Add("\"" + tool + "\" " + RsAsioIniSetup.RestoreArgument + " \"" + game + "\"");
			plan.Steps.Add(restore);

			// ---- 4-6. files, shortcuts, starter tones ---------------------------------------------------------------
			plan.Steps.AddRange(RemovalSteps(game, options.RemoveSettings, keepLogs: false, options.KeepRsModsConfigurator, includeStarterTones: true));

			// ---- 7. UsbDk -------------------------------------------------------------------------------------------
			bool usbDkInstalled = _probe.IsUsbDkInstalled();
			bool removeUsbDk = usbDkInstalled && (options.RemoveUsbDk ?? _probe.UsbDkInstalledByUs());
			if (usbDkInstalled)
				plan.Checks.Add("UsbDk is installed" + (_probe.UsbDkInstalledByUs() ? " (by this installer)" : " (not by this installer)") + ": " + (removeUsbDk ? "removed" : "kept"));
			if (removeUsbDk)
			{
				string msiexec = Path.Combine(_probe.SystemFolder, "msiexec.exe");
				string arguments = "/x " + ProductLayout.UsbDkProductCode + " /qn /norestart";
				var step = new PlannedStep("usbdk-remove", "Remove UsbDk through Windows Installer (its own UAC prompt), last so declining only leaves UsbDk",
					"Uninstaller.RemoveUsbDk", context =>
					{
						int exit = context.Host.RunProcess(msiexec, arguments, _probe.SystemFolder, VerbTimeout, elevated: true);
						// 1605: not installed (already removed). 3010 / 1641: removed, finished at the next restart.
						if (exit is 0 or 1605 or 3010 or 1641)
						{
							context.State.RestartNeeded |= exit is 3010 or 1641;
							context.Host.SetUserRegistryDword(ProductLayout.InstallerStateKey, ProductLayout.UsbDkInstalledByUsValue, null);
							return;
						}
						throw new StepFailedException("UsbDk could not be removed (Windows Installer exit code " + exit
							+ "). You can remove \"UsbDk Runtime Libraries\" from Windows Settings > Apps > Installed apps.");
					})
				{ OnFailure = StepFailure.Warn };
				step.Details.Add("\"" + msiexec + "\" " + arguments);
				plan.Steps.Add(step);
			}
			return plan;
		}

		/// <summary>The installed program that answers the menu and RS_ASIO verbs: our app, else the 4.x RSMods.exe fork.</summary>
		private static string? VerbTool(string game)
		{
			string app = ProductLayout.InGame(game, ProductLayout.AppExe);
			if (File.Exists(app)) return app;
			string legacy = ProductLayout.InGame(game, @"RSMods\RSMods.exe");
			return File.Exists(legacy) && File.Exists(ProductLayout.InGame(game, @"RSMods\RocksmithAudioBridge.dll")) ? legacy : null;
		}

		/// <summary>Uninstall steps 4-6, also the install's rollback (keepLogs: a rolled-back install keeps the logs its
		/// error message points at).</summary>
		private IEnumerable<PlannedStep> RemovalSteps(string game, bool removeSettings, bool keepLogs, bool keepConfigurator, bool includeStarterTones)
		{
			string localRoot = Path.Combine(_probe.LocalAppData, ProductLayout.ProductName);
			var removal = new PlannedStep("remove-files",
				"Remove our files (install record, the known file list, 4.x leftovers), then only folders left empty",
				"Uninstaller.RemoveFiles", context =>
				{
					var failed = new List<string>();
					foreach (string relative in FilesToRemove(game, removeSettings, context.State.MenuEntryRemoved, keepLogs, keepConfigurator))
					{
						try { context.Host.DeleteFile(ProductLayout.InGame(game, relative)); }
						catch (Exception error) when (error is IOException or UnauthorizedAccessException) { failed.Add(relative); }
					}
					// Our own folders: every sub-folder left empty, deepest first (the .vst3 bundles, tools\, ThirdParty\ ...).
					foreach (string own in new[] { ProductLayout.AppFolder, ProductLayout.HelperFolder })
					{
						string root = ProductLayout.InGame(game, own);
						if (!Directory.Exists(root)) continue;
						foreach (string folder in Directory.EnumerateDirectories(root, "*", SearchOption.AllDirectories).OrderByDescending(d => d.Length))
						{
							try { context.Host.DeleteDirectory(folder, onlyIfEmpty: true); } catch { }
						}
					}
					foreach (string folder in FoldersToRemoveWhenEmpty(game, keepConfigurator))
					{
						try { context.Host.DeleteDirectory(ProductLayout.InGame(game, folder), onlyIfEmpty: true); } catch { }
					}
					try { context.Host.DeleteDirectory(Path.Combine(localRoot, "Runtime"), onlyIfEmpty: false); } catch { failed.Add(Path.Combine(localRoot, "Runtime")); }
					if (removeSettings)
					{
						try { context.Host.DeleteDirectory(Path.Combine(localRoot, "Logs"), onlyIfEmpty: false); } catch { }
						try { context.Host.DeleteDirectory(localRoot, onlyIfEmpty: true); } catch { }
						try { context.Host.DeleteUserRegistryTree(ProductLayout.UserSettingsKey); } catch { }
					}
					if (failed.Count > 0)
						throw new StepFailedException("these files could not be removed (close any program using them and run Uninstall again): " + string.Join(", ", failed));
				})
			{ OnFailure = StepFailure.Warn };
			foreach (string relative in FilesToRemove(game, removeSettings, menuEntryRemoved: false, keepLogs, keepConfigurator)) removal.Details.Add("delete " + relative);
			removal.Details.Add("+ " + ProductLayout.MenuBackup + " and the menu log, only once our menu row is gone");
			removal.Details.Add("delete folder " + Path.Combine(localRoot, "Runtime") + " (4.x runtime cache)");
			if (removeSettings) removal.Details.Add("settings: RSMods.ini, AudioRouting.ini, " + Path.Combine(localRoot, "Logs") + ", HKCU\\" + ProductLayout.UserSettingsKey);
			yield return removal;

			var shortcutTargets = new[] { ProductLayout.InGame(game, ProductLayout.AppExe) };
			var shortcuts = new[]
			{
				Path.Combine(_probe.StartMenuPrograms, ProductLayout.StartMenuShortcutName),
				Path.Combine(_probe.Desktop, ProductLayout.StartMenuShortcutName),
			};
			string legacyUrl = Path.Combine(_probe.Desktop, ProductLayout.Legacy4xDesktopShortcutName);
			var shortcutStep = new PlannedStep("shortcuts-remove", "Remove our shortcuts, only those that point at this install",
				"Uninstaller.RemoveFiles (desktop RSMods.url pointing at this install)", context =>
				{
					foreach (string shortcut in shortcuts)
					{
						string? target = ShellLink.ReadTarget(shortcut);
						if (target != null && shortcutTargets.Contains(target, StringComparer.OrdinalIgnoreCase)) context.Host.DeleteFile(shortcut);
					}
					// The 4.x .url pointed at this install's RSMods.exe; now that is upstream's configurator, kept only with it.
					if (!keepConfigurator && File.Exists(legacyUrl))
					{
						string text = File.ReadAllText(legacyUrl);
						foreach (string exe in new[] { ProductLayout.InGame(game, @"RSMods\RSMods.exe"), ProductLayout.InGame(game, "RSMods.exe") })
							if (text.Replace("%20", " ").IndexOf(exe.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase) >= 0) { context.Host.DeleteFile(legacyUrl); break; }
					}
				})
			{ OnFailure = StepFailure.Warn };
			shortcutStep.Details.AddRange(shortcuts.Where(File.Exists));
			if (File.Exists(legacyUrl) && !keepConfigurator) shortcutStep.Details.Add(legacyUrl + " (if it points at this install)");
			yield return shortcutStep;

			if (includeStarterTones)
			{
				string engineFolder = Path.Combine(_probe.AppData, "RocksmithAudioBridge", "Stompbox");
				var tones = new PlannedStep("starter-tones-remove", "Remove the starter tones still exactly as installed (changed ones stay)",
					"StarterTones.Remove", context =>
					{
						var failed = StarterTones.Remove(engineFolder, context.Host);
						if (failed.Count > 0) throw new StepFailedException("could not remove: " + string.Join(", ", failed));
					})
				{ OnFailure = StepFailure.Warn };
				tones.Details.Add(File.Exists(Path.Combine(engineFolder, StarterTones.RecordName)) ? "per " + Path.Combine(engineFolder, StarterTones.RecordName) : "none recorded");
				yield return tones;
			}
		}

		/// <summary>Game-relative files Uninstall deletes (only those that exist).</summary>
		internal List<string> FilesToRemove(string game, bool removeSettings, bool menuEntryRemoved, bool keepLogs, bool keepConfigurator)
		{
			var relative = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
			// Our record, minus the two files handled below by what they are now (a later official install may own them).
			relative.UnionWith(InstallRecord.Read(game).Where(r => !r.Equals(ProductLayout.NativeModFile, StringComparison.OrdinalIgnoreCase)
				&& !r.Equals(ProductLayout.NativeModPdb, StringComparison.OrdinalIgnoreCase)));
			relative.UnionWith(new[] { ProductLayout.AsioDriver, ProductLayout.AsioDriver64, ProductLayout.DriverHost, ProductLayout.VstHost,
				ProductLayout.StompboxEngine, ProductLayout.LooseManifest, ProductLayout.LooseManifestSetAside, ProductLayout.AppExe, ProductLayout.HelperExe,
				ProductLayout.InstallRecord });
			// 4.x: its record and core file list (an install made by 4.x and never migrated).
			relative.UnionWith(ReadLines(ProductLayout.InGame(game, ProductLayout.Legacy4xInstallRecord))
				.Where(r => !r.Equals(ProductLayout.NativeModFile, StringComparison.OrdinalIgnoreCase)));
			relative.UnionWith(ProductLayout.Legacy4xFiles.Where(r => !r.Equals(@"RSMods\RSMods.exe", StringComparison.OrdinalIgnoreCase)
				&& !r.Equals(ProductLayout.NativeModPdb, StringComparison.OrdinalIgnoreCase)));

			ExistingNativeMod native = _probe.InspectNativeMod(game);
			if (IsOurNativeMod(native, game)) relative.UnionWith(new[] { ProductLayout.NativeModFile, ProductLayout.NativeModPdb });

			var configurator = ConfiguratorFiles(game);
			if (!keepConfigurator)
			{
				relative.UnionWith(configurator);
				relative.Add(ProductLayout.ConfiguratorInstalledManifest);
				if (Legacy4xPresent(game)) relative.Add(@"RSMods\RSMods.exe");   // the 4.x fork's settings app
			}
			else relative.ExceptWith(configurator);

			var logs = ProductLayout.AppVerbLogs.Concat(new[] { ProductLayout.UsbDkFiles[2], @"RSMods\AudioBridgeDriverInstall.log" }).ToList();
			if (keepLogs) relative.ExceptWith(logs); else relative.UnionWith(logs.Where(l => !l.EndsWith("NoteByNoteMenuInstall.log", StringComparison.OrdinalIgnoreCase)));
			relative.UnionWith(ProductLayout.UsbDkFiles.Take(2));
			// The recovery copy of cache.psarc goes only once our menu entry is gone; otherwise it is kept.
			if (menuEntryRemoved)
			{
				relative.Add(ProductLayout.MenuBackup);
				relative.UnionWith(ProductLayout.Legacy4xLogs.Where(l => l.EndsWith("NoteByNoteMenuInstall.log", StringComparison.OrdinalIgnoreCase)));
				relative.Add(ProductLayout.AppVerbLogs[1]);
			}
			else
			{
				relative.Remove(ProductLayout.MenuBackup);
				relative.Remove(ProductLayout.AppVerbLogs[1]);
			}
			if (removeSettings) relative.UnionWith(ProductLayout.SettingsFiles);
			else relative.ExceptWith(ProductLayout.SettingsFiles);

			return relative.Where(r => SafeExists(game, r)).ToList();
		}

		private static IEnumerable<string> FoldersToRemoveWhenEmpty(string game, bool keepConfigurator)
		{
			var folders = new List<string>();
			if (!keepConfigurator)
			{
				// The configurator payload's own folders, deepest first.
				var manifest = ReadConfiguratorManifest(game);
				if (manifest?.Directories != null)
					folders.AddRange(manifest.Directories.Where(d => d != null).Select(d => Path.Combine(ProductLayout.RsModsFolder, d.Replace('/', '\\')))
						.OrderByDescending(d => d.Count(c => c == '\\')));
			}
			folders.AddRange(ProductLayout.FoldersRemovedWhenEmpty.Where(f => !keepConfigurator || !f.Equals(ProductLayout.RsModsFolder, StringComparison.OrdinalIgnoreCase)));
			return folders;
		}

		/// <summary>Game-relative paths of the installed configurator payload (RSMods\RSModsGUI.manifest.json).</summary>
		internal static HashSet<string> ConfiguratorFiles(string game)
		{
			var files = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
			var manifest = ReadConfiguratorManifest(game);
			if (manifest?.Files == null) return files;
			foreach (var file in manifest.Files)
			{
				if (file?.Path == null || file.Path.Contains("..") || Path.IsPathRooted(file.Path)) continue;
				files.Add(Path.Combine(ProductLayout.RsModsFolder, file.Path.Replace('/', '\\')));
			}
			return files;
		}

		private static PayloadManifest? ReadConfiguratorManifest(string game)
		{
			string path = ProductLayout.InGame(game, ProductLayout.ConfiguratorInstalledManifest);
			if (!File.Exists(path)) return null;
			try { return PayloadManifest.Parse(File.ReadAllText(path)); }
			catch { return null; }
		}

		private static bool SafeExists(string game, string relative)
		{
			try { return File.Exists(ProductLayout.InGame(game, relative)); }
			catch (InvalidOperationException) { return false; }   // a record line that escapes the game folder
			catch (ArgumentException) { return false; }
		}

		private static IEnumerable<string> ReadLines(string path) =>
			File.Exists(path) ? File.ReadAllLines(path).Select(l => l.Trim()).Where(l => l.Length > 0) : Enumerable.Empty<string>();

		/// <summary>Quotes a path for a command line the way the app's verbs expect (4.x trimmed the trailing backslash,
		/// which would otherwise escape the closing quote).</summary>
		internal static string Quote(string path) => "\"" + path.TrimEnd('\\') + "\"";
	}

	/// <summary>The new install record: game-relative paths, one per line.</summary>
	public static class InstallRecord
	{
		public static IEnumerable<string> Read(string game)
		{
			string path = ProductLayout.InGame(game, ProductLayout.InstallRecord);
			return File.Exists(path) ? File.ReadAllLines(path).Select(l => l.Trim()).Where(l => l.Length > 0).ToList() : new List<string>();
		}
	}
}
