namespace RocksmithAudioBridge.Core.Tests;

public class SettingsAndManifestTests
{
	private const string SampleManifest = """
	[
	  { "key": "OverlayKey", "ini": { "section": "Keybinds", "name": "OverlayKey" }, "type": "Key", "default": "VK_INSERT",
	    "label": "Overlay", "category": "Keybinds", "choices": [], "mod": "AudioBridgeShellMod" },
	  { "key": "AudioBridgeDropPedalEnabled", "ini": { "section": "Drop Pedal", "name": "EnableDropPedal" }, "type": "Bool", "default": "on",
	    "label": "Drop Pedal", "category": "Drop Pedal", "choices": [], "mod": "DropPedalMod" },
	  { "key": "AudioBridgeInputGain", "ini": { "section": "Mod Settings", "name": "AsioInputGain" }, "type": "Int", "default": "0",
	    "label": "Gain", "category": "Mod Settings", "min": -240, "max": 240, "scale": 0.1, "choices": [], "mod": "InputConditionerMod" },
	  { "key": "RemoveLyrics", "ini": { "section": "Toggle Switches", "name": "Lyrics" }, "type": "Bool", "default": "off",
	    "label": "Remove lyrics", "category": "Toggle Switches", "choices": [], "mod": "RemoveLyricsMod" },
	  { "key": "GuitarSpeak", "ini": { "section": "Toggle Switches", "name": "GuitarSpeak" }, "type": "Bool", "default": "off",
	    "label": "Guitar Speak", "category": "Toggle Switches", "choices": [], "mod": "GuitarSpeakMod" }
	]
	""";

	[Fact]
	public void Manifest_keeps_only_our_mods()
	{
		var manifest = OurModsManifest.Parse(SampleManifest);
		Assert.Equal(new[] { "OverlayKey", "AudioBridgeDropPedalEnabled", "AudioBridgeInputGain" }, manifest.Settings.Select(s => s.Key));
		Assert.Single(manifest.ForMod("DropPedal"));
		Assert.Equal(0.1, manifest.ForMod("InputConditioner").Single().Scale);
	}

	[Fact]
	public void The_build_manifest_has_our_settings_and_none_of_upstreams()
	{
		var manifest = OurModsManifest.Load();
		if (manifest.SourcePath == null) return;   // no mod build in this checkout yet
		Assert.Null(manifest.LoadError);
		Assert.NotEmpty(manifest.Settings);
		Assert.All(manifest.Settings, s => Assert.True(OurModsManifest.IsOurs(s), s.Mod));
		Assert.Contains(manifest.Settings, s => s.Mod == "DropPedalMod");
		Assert.Contains(manifest.Settings, s => s.Mod == "NoteByNoteMod");
		Assert.DoesNotContain(manifest.Settings, s => s.Mod == "GuitarSpeakMod");
	}

	[Fact]
	public void RSMods_ini_write_changes_one_value_and_keeps_the_rest()
	{
		using var folder = new TempFolder();
		const string original = "[Toggle Switches]\r\nLyrics=on\r\n; a comment the game wrote\r\n[Audio Bridge]\r\nMasterEnabled=1\r\nRecordingDirectory=D:\\Takes\r\n";
		File.WriteAllText(folder.File("RSMods.ini"), original);
		var ini = new RsModsIni(folder.Path, notifyGame: false);

		Assert.True(BridgeSettings.ReadPower(ini));
		BridgeSettings.WritePower(ini, false);
		string text = File.ReadAllText(folder.File("RSMods.ini"));
		Assert.Contains("MasterEnabled=0", text);
		Assert.Contains("Enabled=0", text);
		Assert.Contains("Lyrics=on", text);
		Assert.Contains("; a comment the game wrote", text);
		Assert.Contains("RecordingDirectory=D:\\Takes", text);
		Assert.False(BridgeSettings.ReadPower(ini));
	}

	[Fact]
	public void RSMods_ini_reads_defaults_without_writing()
	{
		using var folder = new TempFolder();
		var ini = new RsModsIni(folder.Path, notifyGame: false);
		Assert.True(BridgeSettings.ReadPower(ini));
		Assert.True(BridgeSettings.ReadPreferVideo(ini));
		Assert.Equal("High", BridgeSettings.ReadVideoQuality(ini));
		Assert.Equal(Path.Combine(folder.Path, "Recordings"), BridgeSettings.ReadRecordingFolder(ini, folder.Path));
		Assert.False(File.Exists(folder.File("RSMods.ini")));
	}

	[Fact]
	public void RSMods_ini_write_creates_the_section_when_missing()
	{
		using var folder = new TempFolder();
		File.WriteAllText(folder.File("RSMods.ini"), "[Drop Pedal]\r\nEnableDropPedal=on\r\n");
		var ini = new RsModsIni(folder.Path, notifyGame: false);
		BridgeSettings.WriteVideoQuality(ini, "Small");
		Assert.Equal("Small", BridgeSettings.ReadVideoQuality(ini));
		Assert.Equal("on", ini.Get("Drop Pedal", "EnableDropPedal"));
	}

	[Fact]
	public void Cable_mode_needs_low_latency_mode_but_not_exclusive_mode()
	{
		using var folder = new TempFolder();
		File.WriteAllText(folder.File("Rocksmith.ini"), "[Audio]\r\nExclusiveMode=0\r\nWin32UltraLowLatencyMode=0\r\nMaxOutputBufferSize=0\r\n");
		var cable = AuditChecks.CheckRocksmithSettings(folder.Path, asio: false);
		Assert.Equal(AuditState.Problem, cable.State);
		Assert.Contains("Win32UltraLowLatencyMode=0 (needs 1)", cable.Detail);
		Assert.DoesNotContain("ExclusiveMode", cable.Detail);

		var asio = AuditChecks.CheckRocksmithSettings(folder.Path, asio: true);
		Assert.Contains("ExclusiveMode=0 (needs 1)", asio.Detail);

		File.WriteAllText(folder.File("Rocksmith.ini"), "[Audio]\r\nExclusiveMode=0\r\nWin32UltraLowLatencyMode=1\r\nMaxOutputBufferSize=0\r\n");
		Assert.Equal(AuditState.Ok, AuditChecks.CheckRocksmithSettings(folder.Path, asio: false).State);
	}

	[Fact]
	public void Bridge_interface_is_never_guessed_among_several_drivers()
	{
		var drivers = new List<string> { "Focusrite USB ASIO", "Realtek ASIO" };
		Assert.Equal("Realtek ASIO", AsioProxySetup.ResolveBridgeInterface(drivers, "Realtek ASIO", "Focusrite USB ASIO"));
		Assert.Equal("Focusrite USB ASIO", AsioProxySetup.ResolveBridgeInterface(drivers, "Gone ASIO", "Focusrite USB ASIO"));
		Assert.Null(AsioProxySetup.ResolveBridgeInterface(drivers, "", ""));
		Assert.Equal("Only ASIO", AsioProxySetup.ResolveBridgeInterface(new List<string> { "Only ASIO" }, "", ""));
	}

	[Fact]
	public void Unregister_falls_back_to_deleting_the_keys_when_the_dll_is_gone()
	{
		bool registered = true, keysRemoved = false, cleared = false, ranRegsvr = false;
		AsioProxySetup.Unregister("C:\\missing.dll", _ => false, _ => ranRegsvr = true,
			() => { keysRemoved = true; registered = false; }, () => registered, () => cleared = true);
		Assert.False(ranRegsvr);
		Assert.True(keysRemoved);
		Assert.True(cleared);
	}

	[Fact]
	public void Steam_library_list_reads_old_and_current_vdf()
	{
		using var steam = new TempFolder();
		Directory.CreateDirectory(Path.Combine(steam.Path, "steamapps"));
		File.WriteAllText(Path.Combine(steam.Path, "steamapps", "libraryfolders.vdf"),
			"\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t}\n\t\"1\"\t\t\"D:\\\\SteamLibrary\"\n}\n");
		Assert.Equal(new[] { "C:\\Program Files (x86)\\Steam", "D:\\SteamLibrary" }, GameFolder.SteamLibraries(steam.Path));
	}

	[Fact]
	public void A_Rocksmith_folder_is_one_with_cache_psarc()
	{
		using var folder = new TempFolder();
		Assert.False(GameFolder.IsRocksmithFolder(folder.Path));
		folder.Touch("cache.psarc");
		Assert.True(GameFolder.IsRocksmithFolder(folder.Path));
		Assert.Equal(Path.GetFullPath(folder.Path), GameFolder.Detect(folder.Path));
	}
}
