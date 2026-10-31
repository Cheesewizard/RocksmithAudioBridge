using RS2014_Mod_Installer.Core;

namespace RocksmithAudioBridge.Setup.Tests;

public class UninstallPlanTests
{
	private static TestRig Installed(bool withTones = false)
	{
		var rig = new TestRig();
		var result = rig.Install(rig.BuildPayload(withTones));
		Assert.True(result.Succeeded, string.Join("\n", rig.Log));
		// The DLL the installer put in is ours (the build stamps SpecialBuild; the test files carry no version info).
		rig.Probe.NativeModOverride = new ExistingNativeMod(ExistingNativeModKind.CustomRsMods, "RocksmithAudioBridge");
		rig.Host.Processes.Clear();
		return rig;
	}

	[Fact]
	public void Uninstall_removes_our_files_and_keeps_the_players()
	{
		using var rig = Installed();
		File.WriteAllText(rig.G("RSMods.ini"), "settings");
		File.WriteAllText(rig.G(@"RSMods\CustomMods\mine.txt"), "player");
		File.WriteAllText(rig.G("cache.pre-note-by-note-menu.psarc"), "backup");

		var result = rig.Uninstall();

		Assert.True(result.Succeeded, string.Join("\n", rig.Log));
		foreach (string gone in new[] { "xinput1_3.dll", "xinput1_3.pdb", "RocksmithAudioBridgeAsio.dll", "RocksmithAudioBridgeAsio64.dll",
			"RocksmithAudioBridgeDriverHost.exe", "RocksmithAudioBridgeVstHost.exe", @"PedalSkin\skin.json", @"RSMods\mods.manifest.json",
			@"RSMods\RSMods.exe", @"RSMods\ddc\ddc.exe", @"RSMods\AudioBridge\tools\ww2ogg.exe", @"RocksmithAudioBridge\RocksmithAudioBridge.exe",
			"cache.pre-note-by-note-menu.psarc" })
			Assert.False(File.Exists(rig.G(gone)), gone);
		Assert.False(Directory.Exists(rig.G("RocksmithAudioBridge")));
		Assert.False(Directory.Exists(rig.G("PedalSkin")));
		Assert.Equal("settings", File.ReadAllText(rig.G("RSMods.ini")));
		Assert.Equal("player", File.ReadAllText(rig.G(@"RSMods\CustomMods\mine.txt")));
		Assert.True(File.Exists(rig.G("cache.psarc")));
		Assert.False(File.Exists(Path.Combine(rig.Probe.StartMenuPrograms, ProductLayout.StartMenuShortcutName)));
	}

	[Fact]
	public void Uninstall_runs_the_4x_order_driver_then_menu_then_RS_ASIO_then_files()
	{
		using var rig = Installed();
		rig.Probe.DriverRegistered = true;

		rig.Uninstall();

		Assert.Equal(new[] { rig.G("RocksmithAudioBridgeAsio.dll") }, rig.Host.Unregistered);
		string game = "\"" + rig.Game + "\"";
		Assert.Equal(new[]
		{
			"RocksmithAudioBridge.exe --uninstall-note-by-note-menu " + game,
			"RocksmithAudioBridge.exe --restore-rs-asio-ini " + game,
		}, rig.Host.Processes);
	}

	[Fact]
	public void A_menu_row_that_could_not_be_removed_keeps_the_cache_backup()
	{
		using var rig = Installed();
		rig.Host.ExitCodes["--uninstall-note-by-note-menu"] = 1;
		File.WriteAllText(rig.G("cache.pre-note-by-note-menu.psarc"), "backup");

		var result = rig.Uninstall();

		Assert.True(result.Succeeded);
		Assert.Contains(result.Warnings, w => w.Contains("Riff Repeater"));
		Assert.True(File.Exists(rig.G("cache.pre-note-by-note-menu.psarc")));
	}

	[Fact]
	public void Remove_settings_takes_the_ini_and_the_settings_key()
	{
		using var rig = Installed();
		File.WriteAllText(rig.G("RSMods.ini"), "settings");

		rig.Uninstall(o => o.RemoveSettings = true);

		Assert.False(File.Exists(rig.G("RSMods.ini")));
		Assert.Contains("delete " + ProductLayout.UserSettingsKey, rig.Host.Registry);
	}

	[Fact]
	public void Keep_rsmods_leaves_the_configurator_but_takes_our_dll()
	{
		using var rig = Installed();

		rig.Uninstall(o => o.KeepRsModsConfigurator = true);

		Assert.True(File.Exists(rig.G(@"RSMods\RSMods.exe")));
		Assert.True(File.Exists(rig.G(@"RSMods\ddc\ddc.exe")));
		Assert.False(File.Exists(rig.G("xinput1_3.dll")));
	}

	[Fact]
	public void An_official_dll_installed_later_is_not_removed()
	{
		using var rig = Installed();
		rig.Probe.NativeModOverride = new ExistingNativeMod(ExistingNativeModKind.RsMods, "");
		File.WriteAllText(rig.G("xinput1_3.dll"), "official RSMods");

		rig.Uninstall();

		Assert.Equal("official RSMods", File.ReadAllText(rig.G("xinput1_3.dll")));
	}

	[Fact]
	public void UsbDk_is_removed_by_default_only_when_this_installer_put_it_there()
	{
		using var rig = Installed();
		rig.Probe.UsbDkInstalled = true;

		rig.Probe.UsbDkByUs = false;
		Assert.DoesNotContain(new InstallPlanner(rig.Probe).PlanUninstall(new UninstallOptions { GameFolder = rig.Game }).Steps, s => s.Id == "usbdk-remove");

		rig.Probe.UsbDkByUs = true;
		rig.Uninstall();
		Assert.Contains(rig.Host.Processes, p => p.StartsWith("msiexec.exe /x " + ProductLayout.UsbDkProductCode));
		Assert.Contains(ProductLayout.InstallerStateKey + "\\" + ProductLayout.UsbDkInstalledByUsValue + "=deleted", rig.Host.Registry);
	}

	[Fact]
	public void A_record_line_outside_the_game_folder_is_never_deleted()
	{
		using var rig = Installed();
		string outside = Path.Combine(rig.Root, "outside.txt");
		File.WriteAllText(outside, "not ours");
		File.AppendAllText(rig.G(ProductLayout.InstallRecord), @"..\outside.txt" + Environment.NewLine);

		rig.Uninstall();

		Assert.True(File.Exists(outside));
	}

	[Fact]
	public void Starter_tones_the_player_changed_stay()
	{
		using var rig = Installed(withTones: true);
		string engine = Path.Combine(rig.Probe.AppData, "RocksmithAudioBridge", "Stompbox");
		File.WriteAllText(Path.Combine(engine, "NAM", "Lead.nam"), "edited by the player");

		rig.Uninstall();

		Assert.Equal("edited by the player", File.ReadAllText(Path.Combine(engine, "NAM", "Lead.nam")));
		Assert.False(File.Exists(Path.Combine(engine, "NAM", "Clean.nam")));
		Assert.False(File.Exists(Path.Combine(engine, StarterTones.RecordName)));
	}

	[Fact]
	public void Nothing_installed_blocks_the_uninstall()
	{
		using var rig = new TestRig();
		var plan = new InstallPlanner(rig.Probe).PlanUninstall(new UninstallOptions { GameFolder = rig.Game });
		Assert.Contains(plan.Blockers, b => b.Contains("not found"));
	}
}
