using System;
using System.IO;
using RSMods.Audio;

namespace RSMods
{
	// The audit's three basic checks: Rocksmith's own audio settings, RS_ASIO installed correctly, and RS_ASIO.ini set
	// up to go through the bridge. Deliberately basic (decided 2026-10-12): the bridge replaces the drivers and players
	// can change settings live in game, so only the things that stop a launch from having sound are worth checking.
	// The earlier version also audited every RS_ASIO.ini section, 64-bit-only drivers, NoCableLauncher and more, and
	// showed a 25-row Rocksmith.ini table; that was more than players need.
	internal enum AuditState { Ok, Warning, Problem, NotUsed }

	internal sealed class AuditCheck
	{
		public string Name;
		public AuditState State;
		public string Detail;    // the tooltip: what is wrong, or why it is fine
	}

	internal static class AuditChecks
	{
		public static string RocksmithIniPath(string gameDirectory) => Path.Combine(gameDirectory, "Rocksmith.ini");

		/// <summary>True when RS_ASIO is the guitar input (its files are enabled, not .disabled).</summary>
		public static bool AsioMode(string gameDirectory)
		{
			try { return new AudioInputModeConfiguration(gameDirectory).ReadMode() == AudioInputMode.Asio; }
			catch { return false; }
		}

		// ---- 1. Rocksmith settings ---------------------------------------------------------------------------------
		// The values each mode needs. ASIO: RS_ASIO's README asks for ExclusiveMode=1 and Win32UltraLowLatencyMode=1, and
		// ExclusiveMode=0 makes the game ask RS_ASIO for shared mode, which it refuses (no guitar, RØDECaster report).
		// Cable: ExclusiveMode is not checked. The bridge's cable input drops the exclusive flag itself
		// (DLL/AudioBridge/Cable/CableInput.cpp), and the cable's -9996 error was proven not to be exclusive mode
		// (2026-09-04), so the stock ExclusiveMode=1 is fine. Both: a MaxOutputBufferSize smaller than the interface's
		// buffer leaves the game silent; 0 lets Rocksmith pick.
		private static (string Key, string Value)[] RocksmithWanted(bool asio) => asio
			? new[] { ("ExclusiveMode", "1"), ("Win32UltraLowLatencyMode", "1"), ("MaxOutputBufferSize", "0") }
			: new[] { ("MaxOutputBufferSize", "0") };

		public static AuditCheck CheckRocksmithSettings(string gameDirectory, bool asio)
		{
			var check = new AuditCheck { Name = "Rocksmith settings" };
			string path = RocksmithIniPath(gameDirectory);
			if (!File.Exists(path))
			{
				check.State = AuditState.Ok;
				check.Detail = "Rocksmith.ini is written the first time Rocksmith starts.";
				return check;
			}
			var ini = IniText.Load(path);
			string wrong = "";
			foreach (var (key, value) in RocksmithWanted(asio))
			{
				string current = ini.Get("Audio", key);
				if (!string.IsNullOrEmpty(current) && current != value) wrong += (wrong.Length > 0 ? ", " : "") + key + "=" + current + " (needs " + value + ")";
			}
			check.State = wrong.Length == 0 ? AuditState.Ok : AuditState.Problem;
			check.Detail = wrong.Length == 0
				? "Rocksmith.ini's audio settings suit " + (asio ? "an ASIO interface." : "the Real Tone Cable.")
				: "Rocksmith.ini: " + wrong + ". Fix sets them (a backup is kept).";
			return check;
		}

		/// <summary>Sets Rocksmith.ini's audio values for the current mode, after a dated backup.</summary>
		public static void FixRocksmithSettings(string gameDirectory, bool asio)
		{
			string path = RocksmithIniPath(gameDirectory);
			if (!File.Exists(path)) return;
			RsAsioIniSetup.BackUpGameFile(path);
			var ini = IniText.Load(path);
			foreach (var (key, value) in RocksmithWanted(asio)) ini.Set("Audio", key, value);
			ini.Save(path);
		}

		// ---- 2. RS_ASIO installed ----------------------------------------------------------------------------------
		public static AuditCheck CheckRsAsioInstalled(string gameDirectory, bool asio)
		{
			var check = new AuditCheck { Name = "RS_ASIO installed" };
			try { new AudioInputModeConfiguration(gameDirectory).ReadMode(); }
			catch (Exception error)
			{
				check.State = AuditState.Problem;
				check.Detail = error.Message + " Reinstall RS_ASIO (RS_ASIO.dll and avrt.dll).";
				return check;
			}
			if (!asio)
			{
				check.State = AuditState.NotUsed;
				check.Detail = "Not used: the guitar uses the Real Tone Cable.";
				return check;
			}
			if (!AsioProxySetup.IsProxyRegistrationCurrent(Path.Combine(gameDirectory, "RocksmithAudioBridgeAsio.dll")))
			{
				check.State = AuditState.Problem;
				check.Detail = "The Rocksmith Audio Bridge ASIO driver is not registered, so RS_ASIO cannot find it. Run the installer's Reinstall / Repair.";
				return check;
			}
			// < 0.6.1 cannot patch this Rocksmith or crashes with RSMods; < 0.7.2 crashed Audio Bridge 4.0 at profile select.
			string version = RsAsioVersion(gameDirectory);
			check.State = AuditState.Ok;
			check.Detail = "RS_ASIO " + (version ?? "(version shows after Rocksmith has started once)") + " and the bridge driver are installed.";
			if (version != null && Version.TryParse(version.TrimStart('v'), out Version parsed))
			{
				if (parsed < new Version(0, 6, 1)) { check.State = AuditState.Problem; check.Detail = "RS_ASIO " + version + " is too old for this Rocksmith. Install RS_ASIO 0.7.2 or newer."; }
				else if (parsed < new Version(0, 7, 2)) { check.State = AuditState.Warning; check.Detail = "RS_ASIO " + version + " is older than recommended. Install 0.7.2 or newer."; }
			}
			return check;
		}

		// ---- 3. RS_ASIO set up -------------------------------------------------------------------------------------
		public static AuditCheck CheckRsAsioSetUp(string gameDirectory, bool asio)
		{
			var check = new AuditCheck { Name = "RS_ASIO set up" };
			if (!asio)
			{
				check.State = AuditState.NotUsed;
				check.Detail = "Not used: the guitar uses the Real Tone Cable.";
				return check;
			}
			string path = Path.Combine(gameDirectory, "RS_ASIO.ini");
			if (!File.Exists(path)) { check.State = AuditState.Problem; check.Detail = "RS_ASIO.ini is missing, so RS_ASIO offers the game no device. Set up for me writes it."; return check; }
			if (IniText.Load(path).Get("Config", "EnableAsio") != "1") { check.State = AuditState.Problem; check.Detail = "RS_ASIO.ini does not turn ASIO on (EnableAsio=1). Set up for me fixes it."; return check; }
			if (!AsioProxySetup.IsLinked(gameDirectory)) { check.State = AuditState.Warning; check.Detail = "RS_ASIO.ini does not go through Rocksmith Audio Bridge ASIO, so the bridge hears nothing. Set up for me fixes it (your file is backed up)."; return check; }
			check.State = AuditState.Ok;
			check.Detail = "RS_ASIO.ini goes through Rocksmith Audio Bridge ASIO.";
			return check;
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
