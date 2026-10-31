using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using RS2014_Mod_Installer.Core;
using RS2014_Mod_Installer.Payload;

namespace RocksmithAudioBridge.Setup.Tests;

/// <summary>A temp "PC": a game folder, a payload, AppData / Start menu / desktop folders. Deleted on Dispose.</summary>
public sealed class TestRig : IDisposable
{
	public TestRig()
	{
		Root = Path.Combine(Path.GetTempPath(), "RabSetupTests_" + Guid.NewGuid().ToString("N"));
		Game = Path.Combine(Root, "Rocksmith2014");
		PayloadRoot = Path.Combine(Root, "payload");
		Directory.CreateDirectory(Path.Combine(Game, "dlc"));
		File.WriteAllText(Path.Combine(Game, "dlc", "song_p.psarc"), "dlc");
		File.WriteAllText(Path.Combine(Game, "cache.psarc"), "cache");
		File.WriteAllText(Path.Combine(Game, "Rocksmith2014.exe"), "game");
		Probe = new FakeProbe(Root);
		Host = new RecordingHost(Log.Add);
	}

	public string Root { get; }
	public string Game { get; }
	public string PayloadRoot { get; }
	public FakeProbe Probe { get; }
	public RecordingHost Host { get; }
	public List<string> Log { get; } = new();

	public string G(string relative) => Path.Combine(Game, relative);

	public PayloadLayout BuildPayload(bool withTones = false)
	{
		void Put(string relative, string text)
		{
			string path = Path.Combine(PayloadRoot, relative);
			Directory.CreateDirectory(Path.GetDirectoryName(path)!);
			File.WriteAllText(path, text);
		}

		Put(@"native\xinput1_3.dll", "our dll");
		Put(@"native\xinput1_3.pdb", "our pdb");
		Put(@"rsmods\mods.manifest.json", ValidManifest);
		Put(@"game\RocksmithAudioBridgeAsio.dll", "asio x86");
		Put(@"game\RocksmithAudioBridgeAsio64.dll", "asio x64");
		Put(@"game\RocksmithAudioBridgeDriverHost.exe", "driver host");
		Put(@"game\RocksmithAudioBridgeVstHost.exe", "vst host");
		Put(@"game\PedalSkin\skin.json", "{}");
		Put(@"helper\RocksmithAudioBridge.Helper.exe", "helper");
		Put(@"helper\tools\ww2ogg.exe", "ww2ogg");
		Put(@"app\RocksmithAudioBridge.exe", "app");
		Put(@"app\RocksmithAudioBridge.Core.dll", "core");
		Put(@"notices\THIRD-PARTY-NOTICES.txt", "notices");
		Put(@"notices\LICENSE.txt", "licence");

		// The upstream configurator payload, the way Build/New-ConfiguratorPayload.ps1 makes it.
		var configurator = new Dictionary<string, string> { ["RSMods.exe"] = "upstream configurator", ["ddc/ddc.exe"] = "ddc", ["RocksmithToolkitLib.dll"] = "toolkit" };
		Directory.CreateDirectory(Path.Combine(PayloadRoot, "configurator"));
		using (var zip = ZipFile.Open(Path.Combine(PayloadRoot, @"configurator\RSModsGUI.zip"), ZipArchiveMode.Create))
			foreach (var file in configurator)
			{
				using var stream = zip.CreateEntry(file.Key).Open();
				stream.Write(Encoding.UTF8.GetBytes(file.Value));
			}
		var manifest = new PayloadManifest
		{
			PayloadFormatVersion = 1,
			Application = "RSMods",
			ApplicationVersion = "1.2.8.4",
			Executable = "RSMods.exe",
			Directories = new List<string> { "ddc" },
			Files = configurator.Select(f => new PayloadFile
			{
				Path = f.Key,
				Length = Encoding.UTF8.GetByteCount(f.Value),
				Sha256 = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(f.Value))).ToLowerInvariant(),
			}).ToList(),
		};
		File.WriteAllText(Path.Combine(PayloadRoot, @"configurator\RSModsGUI.manifest.json"), manifest.ToJson());

		if (withTones)
		{
			Directory.CreateDirectory(Path.Combine(PayloadRoot, "tones"));
			using var zip = ZipFile.Open(Path.Combine(PayloadRoot, @"tones\StarterTones.zip"), ZipArchiveMode.Create);
			foreach (var (name, text) in new[] { ("NAM/Clean.nam", "clean"), ("NAM/Lead.nam", "lead"), ("README.md", "readme") })
			{
				using var stream = zip.CreateEntry(name).Open();
				stream.Write(Encoding.UTF8.GetBytes(text));
			}
		}
		return new PayloadLayout(PayloadRoot);
	}

	public InstallOptions Options(PayloadLayout payload) => new()
	{
		GameFolder = Game,
		Payload = payload,
		SkipCompatibilityCheck = true,
		CreateStartMenuShortcut = true,
	};

	public RunResult Install(PayloadLayout payload, Action<InstallOptions>? tweak = null)
	{
		var options = Options(payload);
		tweak?.Invoke(options);
		return PlanRunner.Run(new InstallPlanner(Probe).PlanInstall(options), Host, Probe);
	}

	public RunResult Uninstall(Action<UninstallOptions>? tweak = null)
	{
		var options = new UninstallOptions { GameFolder = Game };
		tweak?.Invoke(options);
		return PlanRunner.Run(new InstallPlanner(Probe).PlanUninstall(options), Host, Probe);
	}

	/// <summary>Every file under the rig (game, AppData, shortcuts), relative, with its content: a dry run must leave it identical.</summary>
	public Dictionary<string, string> Snapshot() =>
		Directory.EnumerateFiles(Root, "*", SearchOption.AllDirectories)
			.Where(f => !f.StartsWith(PayloadRoot, StringComparison.OrdinalIgnoreCase))
			.ToDictionary(f => f.Substring(Root.Length), File.ReadAllText);

	public void Dispose()
	{
		try { Directory.Delete(Root, recursive: true); } catch { }
	}

	public const string ValidManifest = """
	[
	  { "key": "OverlayKey", "ini": { "section": "Keybinds", "name": "OverlayKey" }, "type": "Key", "default": "VK_INSERT", "label": "Overlay", "category": "Keybinds", "mod": "AudioBridgeShellMod" },
	  { "key": "Routing", "ini": { "section": "Audio Bridge", "name": "Routing" }, "type": "Bool", "default": "on", "label": "Routing", "category": "Audio", "mod": "AudioBridgeRoutingMod" },
	  { "key": "RecordingHotkey", "ini": { "section": "Keybinds", "name": "RecordingHotkey" }, "type": "Key", "default": "VK_F6", "label": "Record", "category": "Keybinds", "mod": "AudioRecorderMod" },
	  { "key": "DropPedalSteps", "ini": { "section": "Drop Pedal", "name": "Steps" }, "type": "Int", "default": "0", "label": "Steps", "category": "Drop Pedal", "min": -12, "max": 12, "mod": "DropPedalMod" },
	  { "key": "HumFilter", "ini": { "section": "Input", "name": "HumFilter" }, "type": "Enum", "default": "Off", "label": "Hum", "category": "Input", "choices": ["Off", "50 Hz", "60 Hz"], "mod": "InputConditionerMod" },
	  { "key": "NoteByNoteOverlay", "ini": { "section": "Note by Note", "name": "Overlay" }, "type": "Bool", "default": "on", "label": "Readout", "category": "Note by Note", "mod": "NoteByNoteMod" },
	  { "key": "VolumeMeter", "ini": { "section": "Toggle Switches", "name": "VolumeMeter" }, "type": "Bool", "default": "off", "label": "Meter", "category": "General", "mod": "UpstreamMod" }
	]
	""";
}

/// <summary>Answers the planner's read-only questions for the temp PC.</summary>
public sealed class FakeProbe : ISystemProbe
{
	public FakeProbe(string root)
	{
		AppData = Path.Combine(root, "AppData", "Roaming");
		LocalAppData = Path.Combine(root, "AppData", "Local");
		StartMenuPrograms = Path.Combine(root, "StartMenu");
		Desktop = Path.Combine(root, "Desktop");
		SystemFolder = Path.Combine(root, "System32");
	}

	public ExistingNativeMod? NativeModOverride { get; set; }
	public bool GameRunning { get; set; }
	public bool DriverRegistered { get; set; }
	public bool UsbDkInstalled { get; set; }
	public bool UsbDkByUs { get; set; }

	public bool IsRocksmithFolder(string gameFolder) => RocksmithLocator.IsRocksmithFolder(gameFolder);
	public CompatibilityResult CheckCompatibility(string gameFolder) => new(true, false, Array.Empty<string>());
	public ExistingNativeMod InspectNativeMod(string gameFolder) => NativeModOverride ?? ExistingNativeModInspector.Inspect(gameFolder);
	public bool IsGameRunning() => GameRunning;
	public bool IsRunningFrom(string exePath) => false;
	public bool IsDriverRegistered() => DriverRegistered;
	public bool IsUsbDkInstalled() => UsbDkInstalled;
	public bool UsbDkInstalledByUs() => UsbDkByUs;
	public string AppData { get; }
	public string LocalAppData { get; }
	public string StartMenuPrograms { get; }
	public string Desktop { get; }
	public string SystemFolder { get; }
}

/// <summary>Real file work (in the temp rig), recorded processes / registry / shortcuts.</summary>
public sealed class RecordingHost : SystemInstallHost
{
	public RecordingHost(Action<string> log) : base(log) { }

	public List<string> Processes { get; } = new();
	public List<string> Registry { get; } = new();
	public List<string> Shortcuts { get; } = new();
	public List<string> Unregistered { get; } = new();
	/// <summary>Exit code per verb (first argument), default 0.</summary>
	public Dictionary<string, int> ExitCodes { get; } = new(StringComparer.OrdinalIgnoreCase);

	public override int RunProcess(string exe, string arguments, string workingDirectory, TimeSpan timeout, bool elevated)
	{
		Processes.Add(Path.GetFileName(exe) + " " + arguments);
		string verb = arguments.Split(' ')[0];
		return ExitCodes.TryGetValue(verb, out int code) ? code : 0;
	}

	public override void SetUserRegistryDword(string key, string name, int? value) => Registry.Add(key + "\\" + name + "=" + (value?.ToString() ?? "deleted"));
	public override void DeleteUserRegistryTree(string key) => Registry.Add("delete " + key);
	public override void UnregisterDriver(string proxyDllPath) => Unregistered.Add(proxyDllPath);
	public override void CreateShortcut(string shortcutPath, string target, string workingDirectory, string description)
	{
		Shortcuts.Add(shortcutPath + " -> " + target);
		base.CreateShortcut(shortcutPath, target, workingDirectory, description);   // a real .lnk, inside the rig
	}
	public override void StartProgram(string path) => Processes.Add("start " + Path.GetFileName(path));
}
