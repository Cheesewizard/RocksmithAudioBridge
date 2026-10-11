using System;
using System.Collections.Generic;
using System.IO;
using RSMods.Audio;

namespace RSMods
{
	// The Audit tab's checks: the RS_ASIO, Rocksmith.ini and bridge driver settings that most often cost a player
	// their sound, each traced to a real report (see the comment on each). A finding carries a fix only when the fix
	// is safe to apply blind; RS_ASIO.ini problems are all fixed the same way, by "Set up for me" (RsAsioIniSetup).
	internal enum AuditSeverity { Problem, Warning, Info }

	internal sealed class AuditFinding
	{
		public AuditSeverity Severity;
		public string Title;
		public string Detail;
		public string FixLabel;      // null: nothing to click
		public Action Fix;
	}

	// One Rocksmith.ini setting for the table: what the file has, the game's default, and ours when we have one.
	internal sealed class RocksmithIniSetting
	{
		public string Section, Key, Current, Default, Recommended, Why;
		public bool DiffersFromDefault => Current != null && !string.Equals(Current, Default, StringComparison.OrdinalIgnoreCase);
	}

	internal static class AuditChecks
	{
		// Rocksmith.ini's settings and the game's defaults, as upstream RSMods writes them for a missing key
		// (GUI/Rocksmith/WriteSettings.cs CreateDefaultOnOldINI).
		private static readonly (string Section, string Key, string Default)[] RocksmithDefaults =
		{
			("Audio", "EnableMicrophone", "1"), ("Audio", "ExclusiveMode", "1"), ("Audio", "LatencyBuffer", "4"),
			("Audio", "ForceDefaultPlaybackDevice", ""), ("Audio", "ForceWDM", "0"), ("Audio", "ForceDirectXSink", "0"),
			("Audio", "DumpAudioLog", "0"), ("Audio", "MaxOutputBufferSize", "0"), ("Audio", "RealToneCableOnly", "0"),
			("Audio", "Win32UltraLowLatencyMode", "1"),
			("Renderer.Win32", "ShowGamepadUI", "0"), ("Renderer.Win32", "ScreenWidth", "0"), ("Renderer.Win32", "ScreenHeight", "0"),
			("Renderer.Win32", "Fullscreen", "2"), ("Renderer.Win32", "VisualQuality", "1"), ("Renderer.Win32", "RenderingWidth", "0"),
			("Renderer.Win32", "RenderingHeight", "0"), ("Renderer.Win32", "EnablePostEffects", "1"), ("Renderer.Win32", "EnableShadows", "1"),
			("Renderer.Win32", "EnableHighResScope", "1"), ("Renderer.Win32", "EnableDepthOfField", "1"), ("Renderer.Win32", "EnablePerPixelLighting", "1"),
			("Renderer.Win32", "MsaaSamples", "4"), ("Renderer.Win32", "DisableBrowser", "0"), ("Net", "UseProxy", "1"),
		};

		public static string RocksmithIniPath(string gameDirectory) => Path.Combine(gameDirectory, "Rocksmith.ini");

		/// <summary>Sets one Rocksmith.ini value, after a dated backup (RsAsioIniSetup.BackUpGameFile).</summary>
		public static void SetRocksmithValue(string gameDirectory, string section, string key, string value)
		{
			string path = RocksmithIniPath(gameDirectory);
			RsAsioIniSetup.BackUpGameFile(path);
			var ini = IniText.Load(path);
			ini.Set(section, key, value);
			ini.Save(path);
		}

		public static List<RocksmithIniSetting> RocksmithIni(string gameDirectory, bool asioMode)
		{
			var ini = IniText.Load(RocksmithIniPath(gameDirectory));
			var rows = new List<RocksmithIniSetting>();
			foreach (var (section, key, def) in RocksmithDefaults)
			{
				var row = new RocksmithIniSetting { Section = section, Key = key, Current = ini.Get(section, key), Default = def };
				switch (key)
				{
					case "ExclusiveMode":
						row.Recommended = asioMode ? "1" : "0";
						row.Why = asioMode ? "RS_ASIO only opens the interface in exclusive mode." : "The Real Tone Cable cannot open in exclusive mode.";
						break;
					case "Win32UltraLowLatencyMode" when asioMode:
						row.Recommended = "1";
						row.Why = "RS_ASIO asks for it with an audio interface.";
						break;
					case "MaxOutputBufferSize":
						row.Recommended = "0";
						row.Why = "0 lets Rocksmith pick. A value smaller than the interface's buffer leaves the game silent.";
						break;
					case "DumpAudioLog":
						row.Recommended = "1";
						row.Why = "Writes audiodump.txt, which a bug report needs.";
						break;
				}
				rows.Add(row);
			}
			return rows;
		}

		public static List<AuditFinding> Run(string gameDirectory, Action setUp)
		{
			var findings = new List<AuditFinding>();
			void Add(AuditSeverity severity, string title, string detail, string fixLabel = null, Action fix = null) =>
				findings.Add(new AuditFinding { Severity = severity, Title = title, Detail = detail, FixLabel = fixLabel, Fix = fix });

			AudioInputMode mode;
			try { mode = new AudioInputModeConfiguration(gameDirectory).ReadMode(); }
			catch (Exception error)
			{
				Add(AuditSeverity.Problem, "RS_ASIO's files are incomplete", error.Message + " Reinstall RS_ASIO (RS_ASIO.dll and avrt.dll).");
				return findings;
			}
			bool asio = mode == AudioInputMode.Asio;
			if (mode == AudioInputMode.Unavailable)
				Add(AuditSeverity.Info, "RS_ASIO is not installed", "The guitar uses the Real Tone Cable. RS_ASIO is only needed for an audio interface.");
			if (asio) CheckRsAsio(gameDirectory, Add, setUp);
			CheckRocksmithIni(gameDirectory, asio, Add);
			if (new AudioInputModeConfiguration(gameDirectory).IsGameRunning())
				Add(AuditSeverity.Info, "Rocksmith is running", "Fixes are saved now and apply the next time Rocksmith starts.");
			return findings;
		}

		private static void CheckRsAsio(string gameDirectory, Action<AuditSeverity, string, string, string, Action> add, Action setUp)
		{
			// RS_ASIO version: < 0.6.1 crashes with RSMods, < 0.7.2 crashed Audio Bridge 4.0 at profile select.
			string version = RsAsioVersion(gameDirectory);
			if (version == null)
				add(AuditSeverity.Info, "RS_ASIO version unknown", "RS_ASIO-log.txt names it once Rocksmith has started with RS_ASIO.", null, null);
			else if (Version.TryParse(version.TrimStart('v'), out Version parsed))
			{
				if (parsed < new Version(0, 6, 1))
					add(AuditSeverity.Problem, "RS_ASIO " + version + " is too old", "It cannot patch this version of Rocksmith or crashes with RSMods. Install RS_ASIO 0.7.2 or newer.", null, null);
				else if (parsed < new Version(0, 7, 2))
					add(AuditSeverity.Warning, "RS_ASIO " + version + " is older than recommended", "0.7.2 or newer is recommended; older versions crash Audio Bridge 4.0 at profile select.", null, null);
			}

			string path = Path.Combine(gameDirectory, "RS_ASIO.ini");
			if (!File.Exists(path))
			{
				add(AuditSeverity.Problem, "RS_ASIO.ini is missing", "Without it RS_ASIO offers the game no device.", "Set up for me", setUp);
				return;
			}
			var ini = IniText.Load(path);
			string output = ini.Get("Asio.Output", "Driver") ?? "", input0 = ini.Get("Asio.Input.0", "Driver") ?? "", input1 = ini.Get("Asio.Input.1", "Driver") ?? "";
			string mic = ini.Get("Asio.Input.Mic", "Driver") ?? "";
			bool viaBridge = RsAsioIniSetup.IsBridge(output) && RsAsioIniSetup.IsBridge(input0);
			string target = AsioProxySetup.ReadTarget();

			// RS_ASIO.ini's content is only checked while the file is the player's own. Once Set up for me has written
			// it, the bridge decides devices and channels and there is nothing in it to get wrong; before then, the
			// setup carries the player's choices over anyway. So the checks here are only the ones that mean no sound
			// at all, and they are warnings (it is the player's file, set up their way), except ASIO switched off.
			RsAsioIniSetup.State state = RsAsioIniSetup.GetState(gameDirectory);
			if (state != RsAsioIniSetup.State.Managed)
			{
				// A file without a readable [Config] section turns everything off: no device at all (10-08 report).
				if (ini.Get("Config", "EnableAsio") != "1")
					add(AuditSeverity.Problem, "RS_ASIO.ini does not turn ASIO on", "[Config] EnableAsio=1 is missing, so RS_ASIO offers the game no ASIO device.", "Set up for me", setUp);
				if (!viaBridge && state == RsAsioIniSetup.State.NotSetUp)
					add(AuditSeverity.Warning, "RS_ASIO.ini is not set up for the bridge", "The bridge's mixer, recording and amp sim only hear the sound when RS_ASIO goes through Rocksmith Audio Bridge ASIO. Set up for me keeps your device and channels.", "Set up for me", setUp);
				if (state == RsAsioIniSetup.State.EditedByHand)
					add(AuditSeverity.Info, "RS_ASIO.ini was edited after setup", "It is left as you set it. Set up for me again rewrites it (your version is backed up first).", null, null);

				// Guitar on Player 2's section only (the RØDECaster report): a single player hears nothing.
				if (input0.Length == 0 && input1.Length > 0)
					add(AuditSeverity.Warning, "The guitar is set up as Player 2", "[Asio.Input.0] (Player 1) is empty while [Asio.Input.1] (Player 2) names a device, so a single player hears nothing.", "Set up for me", setUp);
				// A Driver= that is not installed: often a copied file, or a section the player does not use. RS_ASIO
				// just skips it, so it only matters for the output and Player 1.
				var installed = AsioProxySetup.ListRealDrivers();
				foreach (var (section, driver) in new[] { ("Asio.Output", output), ("Asio.Input.0", input0) })
					if (driver.Length > 0 && !RsAsioIniSetup.IsBridge(driver) && !installed.Exists(d => string.Equals(d, driver, StringComparison.OrdinalIgnoreCase)))
						add(AuditSeverity.Warning, "[" + section + "] names a driver that is not installed", "'" + driver + "' is not an installed 32-bit ASIO driver, so RS_ASIO cannot open it.", "Set up for me", setUp);
				// The interface opened twice: once through the bridge, once directly for another section.
				if (viaBridge && target.Length > 0)
					foreach (var (section, driver) in new[] { ("Asio.Input.1", input1), ("Asio.Input.Mic", mic) })
						if (string.Equals(driver, target, StringComparison.OrdinalIgnoreCase))
							add(AuditSeverity.Warning, "[" + section + "] opens " + target + " a second time", "The bridge already opens it; most ASIO drivers open only once, so one of the two fails.", "Set up for me", setUp);
			}

			// RS_ASIO lists 32-bit ASIO drivers only (its README): an interface with a 64-bit driver alone is invisible.
			foreach (string onlyX64 in AsioProxySetup.ListDriversOnlyIn64Bit())
				add(AuditSeverity.Warning, onlyX64 + " has no 32-bit driver", "Rocksmith is a 32-bit program, so RS_ASIO cannot open it. Install the interface's full driver package (it usually includes both).", null, null);
			// RS_ASIO's README: launchers that patch the cable check get in its way.
			if (File.Exists(Path.Combine(gameDirectory, "NoCableLauncher.exe")))
				add(AuditSeverity.Warning, "NoCableLauncher is in the Rocksmith folder", "RS_ASIO says not to use it (or similar launchers) alongside RS_ASIO. Start Rocksmith from Steam instead.", null, null);

			// The bridge driver itself.
			if (!AsioProxySetup.IsProxyRegistrationCurrent(Path.Combine(gameDirectory, "RocksmithAudioBridgeAsio.dll")))
				add(AuditSeverity.Problem, "The bridge driver is not registered correctly", "RS_ASIO would find no Rocksmith Audio Bridge ASIO. Run the installer's Reinstall / Repair.", null, null);
			else if (viaBridge && target.Length == 0)
				add(AuditSeverity.Warning, "The bridge has no interface to open", "Several ASIO drivers are installed and none is chosen. Pick yours under ASIO interface on the Rocksmith Audio Bridge tab.", null, null);
			if (AsioProxySetup.ReadLastBindError(out string failed, out string reason))
				add(AuditSeverity.Problem, "Last launch: " + (failed.Length > 0 ? failed : "the interface") + " would not open",
					reason.TrimEnd('.') + ". Plug it in or close other apps using it (its control app, a DAW, OBS), then pick it again under Output interface on the in-game Output page. No restart needed.", null, null);

			// RS_ASIO's buffer above the game's maximum: GetBuffer fails and the game stops its output (10-08 report).
			if (string.Equals(ini.Get("Asio", "BufferSizeMode"), "custom", StringComparison.OrdinalIgnoreCase)
				&& int.TryParse(ini.Get("Asio", "CustomBufferSize"), out int custom)
				&& int.TryParse(IniText.Load(RocksmithIniPath(gameDirectory)).Get("Audio", "MaxOutputBufferSize"), out int max) && max > 0 && custom > max)
				add(AuditSeverity.Problem, "RS_ASIO's buffer is bigger than Rocksmith allows", "CustomBufferSize=" + custom + " is above Rocksmith.ini MaxOutputBufferSize=" + max + ", which leaves the game silent.",
					"Set MaxOutputBufferSize=0", () => SetRocksmithValue(gameDirectory, "Audio", "MaxOutputBufferSize", "0"));
		}

		private static void CheckRocksmithIni(string gameDirectory, bool asio, Action<AuditSeverity, string, string, string, Action> add)
		{
			string path = RocksmithIniPath(gameDirectory);
			if (!File.Exists(path))
			{
				add(AuditSeverity.Info, "Rocksmith.ini is missing", "Rocksmith writes it the first time it starts.", null, null);
				return;
			}
			var ini = IniText.Load(path);
			string exclusive = ini.Get("Audio", "ExclusiveMode");
			// ExclusiveMode=0 makes the game ask RS_ASIO for shared mode, which it refuses: no guitar (RØDECaster report).
			if (asio && exclusive == "0")
				add(AuditSeverity.Problem, "Exclusive mode is off", "With an audio interface, Rocksmith.ini ExclusiveMode must be 1: RS_ASIO refuses shared mode, so the guitar is silent.",
					"Turn it on", () => SetRocksmithValue(gameDirectory, "Audio", "ExclusiveMode", "1"));
			// RS_ASIO's README asks for both ExclusiveMode=1 and Win32UltraLowLatencyMode=1.
			if (asio && ini.Get("Audio", "Win32UltraLowLatencyMode") == "0")
				add(AuditSeverity.Problem, "Ultra low latency mode is off", "RS_ASIO needs Rocksmith.ini Win32UltraLowLatencyMode=1 with an audio interface.",
					"Turn it on", () => SetRocksmithValue(gameDirectory, "Audio", "Win32UltraLowLatencyMode", "1"));
			if (!asio && exclusive == "1")
				add(AuditSeverity.Warning, "Exclusive mode is on with the Real Tone Cable", "The cable cannot open in exclusive mode. If Rocksmith reports no cable (error -9996), turn it off.",
					"Turn it off", () => SetRocksmithValue(gameDirectory, "Audio", "ExclusiveMode", "0"));
			string maxBuffer = ini.Get("Audio", "MaxOutputBufferSize");
			if (!string.IsNullOrEmpty(maxBuffer) && maxBuffer != "0")
				add(AuditSeverity.Warning, "MaxOutputBufferSize is " + maxBuffer, "If it is smaller than the interface's buffer, the game plays no sound. 0 (the default) lets Rocksmith pick.",
					"Set it to 0", () => SetRocksmithValue(gameDirectory, "Audio", "MaxOutputBufferSize", "0"));
			if (ini.Get("Audio", "DumpAudioLog") != "1")
				add(AuditSeverity.Info, "Rocksmith's audio log is off", "DumpAudioLog=1 writes audiodump.txt, which a bug report needs to see which devices the game found.",
					"Turn it on", () => SetRocksmithValue(gameDirectory, "Audio", "DumpAudioLog", "1"));
		}

		// "(v0.7.4)" on RS_ASIO-log.txt's first line, as the mod reads it (DLL/ModManager.cpp LogRsAsioVersion).
		private static string RsAsioVersion(string gameDirectory)
		{
			try
			{
				string log = Path.Combine(gameDirectory, "RS_ASIO-log.txt");
				if (!File.Exists(log)) return null;
				using (var reader = new StreamReader(new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)))
				{
					string first = reader.ReadLine() ?? "";
					int start = first.IndexOf("(v", StringComparison.Ordinal), end = start < 0 ? -1 : first.IndexOf(')', start);
					return end > start ? first.Substring(start + 1, end - start - 1) : null;
				}
			}
			catch { return null; }
		}
	}
}
