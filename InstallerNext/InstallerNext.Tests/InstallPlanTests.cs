using RS2014_Mod_Installer.Core;

namespace RocksmithAudioBridge.Setup.Tests;

public class InstallPlanTests
{
	[Fact]
	public void Fresh_install_puts_every_part_where_it_belongs()
	{
		using var rig = new TestRig();
		var result = rig.Install(rig.BuildPayload());

		Assert.True(result.Succeeded, string.Join("\n", rig.Log));
		Assert.Equal("our dll", File.ReadAllText(rig.G("xinput1_3.dll")));
		Assert.Equal("upstream configurator", File.ReadAllText(rig.G(@"RSMods\RSMods.exe")));
		Assert.True(File.Exists(rig.G(@"RSMods\RSModsGUI.manifest.json")));
		Assert.Equal(TestRig.ValidManifest, File.ReadAllText(rig.G(@"RSMods\mods.manifest.json")));
		foreach (string file in new[] { "RocksmithAudioBridgeAsio.dll", "RocksmithAudioBridgeAsio64.dll", "RocksmithAudioBridgeDriverHost.exe",
			"RocksmithAudioBridgeVstHost.exe", @"PedalSkin\skin.json", @"RSMods\AudioBridge\RocksmithAudioBridge.Helper.exe",
			@"RSMods\AudioBridge\tools\ww2ogg.exe", @"RocksmithAudioBridge\RocksmithAudioBridge.exe", @"RocksmithAudioBridge\THIRD-PARTY-NOTICES.txt" })
			Assert.True(File.Exists(rig.G(file)), file);

		var record = File.ReadAllLines(rig.G(ProductLayout.InstallRecord));
		Assert.Contains(@"RSMods\AudioBridge\tools\ww2ogg.exe", record);
		Assert.Contains(ProductLayout.LooseManifest, record);
		Assert.Contains(ProductLayout.InstallRecord, record);
		// The configurator's own files are tracked by upstream's RSModsGUI.manifest.json, not by our record.
		Assert.DoesNotContain(@"RSMods\RSMods.exe", record);
	}

	[Fact]
	public void Install_runs_the_driver_and_menu_verbs_through_the_installed_app_then_makes_the_shortcut()
	{
		using var rig = new TestRig();
		rig.Install(rig.BuildPayload());

		string game = "\"" + rig.Game + "\"";
		Assert.Equal(new[]
		{
			"RocksmithAudioBridge.exe --install-audio-bridge-driver " + game,
			"RocksmithAudioBridge.exe --install-note-by-note-menu " + game,
		}, rig.Host.Processes);
		Assert.Single(rig.Host.Shortcuts);
		Assert.EndsWith(@"RocksmithAudioBridge\RocksmithAudioBridge.exe", rig.Host.Shortcuts[0]);
		Assert.True(File.Exists(Path.Combine(rig.Probe.StartMenuPrograms, ProductLayout.StartMenuShortcutName)));
	}

	[Fact]
	public void Declined_admin_prompt_rolls_the_install_back_but_keeps_the_logs()
	{
		using var rig = new TestRig();
		File.WriteAllText(rig.G("RSMods.ini"), "player settings");
		rig.Host.ExitCodes["--install-audio-bridge-driver"] = 2;
		// What the rollback finds in xinput1_3.dll: our build (the real one carries the SpecialBuild stamp).
		rig.Probe.NativeModOverride = new ExistingNativeMod(ExistingNativeModKind.CustomRsMods, "RocksmithAudioBridge");
		var payload = rig.BuildPayload();
		Directory.CreateDirectory(rig.G("RocksmithAudioBridge"));
		File.WriteAllText(rig.G(@"RocksmithAudioBridge\AudioBridgeDriverInstall.log"), "declined");

		var result = rig.Install(payload);

		Assert.False(result.Succeeded);
		Assert.True(result.Cancelled);
		Assert.True(result.RolledBack);
		Assert.False(File.Exists(rig.G("RocksmithAudioBridgeAsio.dll")));
		Assert.False(File.Exists(rig.G(@"RocksmithAudioBridge\RocksmithAudioBridge.exe")));
		Assert.False(File.Exists(rig.G("xinput1_3.dll")));
		Assert.True(File.Exists(rig.G(@"RocksmithAudioBridge\AudioBridgeDriverInstall.log")));
		Assert.Equal("player settings", File.ReadAllText(rig.G("RSMods.ini")));
		Assert.DoesNotContain(rig.Host.Processes, p => p.Contains("--install-note-by-note-menu"));
	}

	[Fact]
	public void Fresh_UsbDk_needs_a_restart_and_is_remembered_as_ours()
	{
		using var rig = new TestRig();
		rig.Host.ExitCodes["--install-audio-bridge-driver"] = 4;
		var result = rig.Install(rig.BuildPayload());

		Assert.True(result.Succeeded);
		Assert.True(result.RestartNeeded);
		Assert.Contains(ProductLayout.InstallerStateKey + "\\" + ProductLayout.UsbDkInstalledByUsValue + "=1", rig.Host.Registry);
	}

	[Fact]
	public void UsbDk_failure_is_only_a_warning()
	{
		using var rig = new TestRig();
		rig.Host.ExitCodes["--install-audio-bridge-driver"] = 3;
		var result = rig.Install(rig.BuildPayload());

		Assert.True(result.Succeeded);
		Assert.Single(result.Warnings);
		Assert.Contains("UsbDk", result.Warnings[0]);
	}

	[Fact]
	public void Menu_failure_is_a_warning_and_the_install_stays()
	{
		using var rig = new TestRig();
		rig.Host.ExitCodes["--install-note-by-note-menu"] = 1;
		var result = rig.Install(rig.BuildPayload());

		Assert.True(result.Succeeded);
		Assert.Contains(result.Warnings, w => w.Contains("Riff Repeater"));
		Assert.True(File.Exists(rig.G(@"RocksmithAudioBridge\RocksmithAudioBridge.exe")));
	}

	[Fact]
	public void Dry_run_changes_nothing()
	{
		using var rig = new TestRig();
		var payload = rig.BuildPayload(withTones: true);
		var before = rig.Snapshot();
		var dryHost = new DryRunInstallHost(rig.Log.Add);

		var result = PlanRunner.Run(new InstallPlanner(rig.Probe).PlanInstall(rig.Options(payload)), dryHost, rig.Probe);

		Assert.True(result.Succeeded);
		Assert.Equal(before, rig.Snapshot());
		Assert.Contains(rig.Log, l => l.Contains("[dry run] run") && l.Contains("--install-audio-bridge-driver"));
		Assert.Contains(rig.Log, l => l.Contains("[dry run] write") && l.EndsWith(@"RocksmithAudioBridge\RocksmithAudioBridge.exe"));
		Assert.Contains(rig.Log, l => l.Contains("starter files"));
	}

	[Fact]
	public void Dry_run_of_an_uninstall_changes_nothing()
	{
		using var rig = new TestRig();
		rig.Install(rig.BuildPayload(withTones: true));
		rig.Probe.DriverRegistered = true;
		var before = rig.Snapshot();

		var plan = new InstallPlanner(rig.Probe).PlanUninstall(new UninstallOptions { GameFolder = rig.Game, RemoveSettings = true });
		var result = PlanRunner.Run(plan, new DryRunInstallHost(rig.Log.Add), rig.Probe);

		Assert.True(result.Succeeded);
		Assert.Equal(before, rig.Snapshot());
	}

	[Fact]
	public void Missing_payload_blocks_the_install_and_nothing_runs()
	{
		using var rig = new TestRig();
		var result = rig.Install(new PayloadLayout(Path.Combine(rig.Root, "nowhere")));

		Assert.False(result.Succeeded);
		Assert.Contains("missing", result.Error);
		Assert.Empty(rig.Host.Processes);
		Assert.False(Directory.Exists(rig.G("RocksmithAudioBridge")));
	}

	[Fact]
	public void Open_game_blocks_the_install()
	{
		using var rig = new TestRig();
		rig.Probe.GameRunning = true;
		var plan = new InstallPlanner(rig.Probe).PlanInstall(rig.Options(rig.BuildPayload()));
		Assert.Contains(plan.Blockers, b => b.Contains("Rocksmith is open"));
	}

	[Fact]
	public void Another_programs_xinput_needs_the_players_say()
	{
		using var rig = new TestRig();
		rig.Probe.NativeModOverride = new ExistingNativeMod(ExistingNativeModKind.OtherProgram, "x360ce 4.0");
		var payload = rig.BuildPayload();

		var undecided = new InstallPlanner(rig.Probe).PlanInstall(rig.Options(payload));
		Assert.Contains(undecided.Blockers, b => b.Contains("x360ce"));

		File.WriteAllText(rig.G("xinput1_3.dll"), "x360ce");
		var kept = rig.Install(payload, o => o.ReplaceForeignNativeMod = false);
		Assert.True(kept.Succeeded, string.Join("\n", rig.Log));
		Assert.Equal("x360ce", File.ReadAllText(rig.G("xinput1_3.dll")));
	}

	[Fact]
	public void Our_own_custom_build_is_replaced_without_asking_and_its_set_aside_manifest_is_dropped()
	{
		using var rig = new TestRig();
		rig.Probe.NativeModOverride = new ExistingNativeMod(ExistingNativeModKind.CustomRsMods, "RocksmithAudioBridge");
		Directory.CreateDirectory(rig.G("RSMods"));
		File.WriteAllText(rig.G(@"RSMods\mods.manifest.json"), "[]");

		var result = rig.Install(rig.BuildPayload());

		Assert.True(result.Succeeded, string.Join("\n", rig.Log));
		Assert.False(File.Exists(rig.G(@"RSMods\mods.manifest.json.bak")));
		Assert.Equal(TestRig.ValidManifest, File.ReadAllText(rig.G(@"RSMods\mods.manifest.json")));
	}

	[Fact]
	public void A_4x_install_is_migrated()
	{
		using var rig = new TestRig();
		Directory.CreateDirectory(rig.G("RSMods"));
		foreach (string file in new[] { @"RSMods\RSMods.exe", @"RSMods\RSMods.exe.config", @"RSMods\RocksmithAudioBridge.dll", @"RSMods\LICENSE.txt",
			@"RSMods\VstHost-LICENSE.txt", "RocksmithAudioBridgeAsio.dll", "xinput1_3.dll" })
			File.WriteAllText(rig.G(file), "4.x");
		File.WriteAllText(rig.G(ProductLayout.Legacy4xInstallRecord), "RSMods\\RSMods.exe\nRSMods\\RocksmithAudioBridge.dll\nRSMods\\LICENSE.txt\nRocksmithAudioBridgeAsio.dll\n");
		File.WriteAllText(rig.G(@"RSMods\GUI_Settings.ini"), "player");
		Directory.CreateDirectory(rig.G(@"RSMods\CustomMods"));
		File.WriteAllText(rig.G(@"RSMods\CustomMods\mine.txt"), "player");
		string runtime = ProductLayout.Legacy4xRuntimeFolder(rig.Probe.LocalAppData);
		Directory.CreateDirectory(runtime);
		File.WriteAllText(Path.Combine(runtime, "NLog.dll"), "4.x runtime");

		var result = rig.Install(rig.BuildPayload());

		Assert.True(result.Succeeded, string.Join("\n", rig.Log));
		Assert.Equal("upstream configurator", File.ReadAllText(rig.G(@"RSMods\RSMods.exe")));
		Assert.Equal("asio x86", File.ReadAllText(rig.G("RocksmithAudioBridgeAsio.dll")));
		foreach (string gone in new[] { @"RSMods\RocksmithAudioBridge.dll", @"RSMods\LICENSE.txt", @"RSMods\VstHost-LICENSE.txt",
			@"RSMods\RSMods.exe.config", ProductLayout.Legacy4xInstallRecord })
			Assert.False(File.Exists(rig.G(gone)), gone);
		Assert.False(Directory.Exists(runtime));
		Assert.Equal("player", File.ReadAllText(rig.G(@"RSMods\GUI_Settings.ini")));
		Assert.Equal("player", File.ReadAllText(rig.G(@"RSMods\CustomMods\mine.txt")));
	}

	[Fact]
	public void Starter_tones_never_overwrite_the_players_files()
	{
		using var rig = new TestRig();
		string engine = Path.Combine(rig.Probe.AppData, "RocksmithAudioBridge", "Stompbox");
		Directory.CreateDirectory(Path.Combine(engine, "NAM"));
		File.WriteAllText(Path.Combine(engine, "NAM", "Clean.nam"), "the player's own");

		rig.Install(rig.BuildPayload(withTones: true));

		Assert.Equal("the player's own", File.ReadAllText(Path.Combine(engine, "NAM", "Clean.nam")));
		Assert.Equal("lead", File.ReadAllText(Path.Combine(engine, "NAM", "Lead.nam")));
		Assert.True(File.Exists(Path.Combine(engine, "StarterTones", "README.md")));
	}

	[Fact]
	public void Plan_names_the_4x_step_each_step_replaces()
	{
		using var rig = new TestRig();
		var plan = new InstallPlanner(rig.Probe).PlanInstall(rig.Options(rig.BuildPayload()));
		Assert.All(plan.Steps, s => Assert.False(string.IsNullOrWhiteSpace(s.Replaces4x)));
		Assert.Equal(new[] { "rsmods-framework", "loose-manifest", "copy-game", "copy-helper", "copy-app", "copy-notices", "starter-tones",
			"legacy-4x-cleanup", "install-record", "driver", "menu-row", "shortcuts" }, plan.Steps.Select(s => s.Id));
	}
}
