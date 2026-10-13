using System;
using System.IO;
using Microsoft.Win32;
using NAudio.CoreAudioApi;
using RSMods.Audio;

namespace RSMods
{
	// The audit's checks, per guitar input mode, because each mode fails in its own ways (decided 2026-10-13).
	// Both modes: Rocksmith's own audio settings. Real Tone Cable: the cable seen by Windows (or bound to WinUSB for
	// the direct USB read) and Windows letting desktop apps use the microphone. ASIO interface: RS_ASIO installed,
	// RS_ASIO.ini set up through the bridge, the bridge has an interface to open, and the last launch opened it.
	// Deliberately basic (decided 2026-10-12): the bridge replaces the drivers and players can change settings live in
	// game, so only the things that stop a launch from having sound are worth checking. An earlier version audited
	// every RS_ASIO.ini section, 64-bit-only drivers, NoCableLauncher and more, and showed a 25-row Rocksmith.ini
	// table; that was more than players need.
	internal enum AuditState { Ok, Warning, Problem, NotUsed }

	internal sealed class AuditCheck
	{
		public string Name;
		public AuditState State;
		public string Detail;      // what is wrong, or why it is fine
		public string FixLabel;    // null: nothing to click
		public Action Fix;
	}

	internal static class AuditChecks
	{
		public static string RocksmithIniPath(string gameDirectory) => Path.Combine(gameDirectory, "Rocksmith.ini");

		/// <summary>True when RS_ASIO is the guitar input (its files are enabled, not .disabled).</summary>
		public static bool AsioMode(string gameDirectory) => Mode(gameDirectory) == AudioInputMode.Asio;

		/// <summary>Asio (RS_ASIO on), Cable (RS_ASIO's files renamed .disabled: installed but off) or Unavailable (no
		/// RS_ASIO files). Incomplete files read as Asio here; the RS_ASIO installed check reports them.</summary>
		public static AudioInputMode Mode(string gameDirectory)
		{
			try { return new AudioInputModeConfiguration(gameDirectory).ReadMode(); }
			catch { return AudioInputMode.Asio; }
		}

		/// <summary>The recommended Rocksmith.ini [Audio] values for both modes, current one first, for the tooltip.</summary>
		public static string RecommendedText(bool asio)
		{
			string Line(bool forAsio) => (forAsio ? "ASIO interface: " : "Real Tone Cable: ") + string.Join(", ", Array.ConvertAll(RocksmithWanted(forAsio), w => w.Key + "=" + w.Value))
				+ (forAsio ? "" : " (ExclusiveMode either way)");
			return "Recommended in Rocksmith.ini [Audio]:\n" + Line(asio) + "   (this mode)\n" + Line(!asio);
		}

		// ---- 1. Rocksmith settings ---------------------------------------------------------------------------------
		// The values each mode needs. ASIO: RS_ASIO's README asks for ExclusiveMode=1 and Win32UltraLowLatencyMode=1, and
		// ExclusiveMode=0 makes the game ask RS_ASIO for shared mode, which it refuses (no guitar, RØDECaster report).
		// Cable: ExclusiveMode is not checked. The bridge's cable input drops the exclusive flag itself
		// (DLL/AudioBridge/Cable/CableInput.cpp), and the cable's -9996 error was proven not to be exclusive mode
		// (2026-09-04), so the stock ExclusiveMode=1 is fine. Both: a MaxOutputBufferSize smaller than the interface's
		// buffer leaves the game silent; 0 lets Rocksmith pick.
		public static (string Key, string Value)[] RocksmithWanted(bool asio) => asio
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
		// Checked in cable mode too (RS_ASIO installed but off), so a player sees it is ready before switching; the card
		// shows it as off rather than as a fault.
		public static AuditCheck CheckRsAsioInstalled(string gameDirectory)
		{
			var check = new AuditCheck { Name = "RS_ASIO installed" };
			AudioInputMode mode;
			try { mode = new AudioInputModeConfiguration(gameDirectory).ReadMode(); }
			catch (Exception error)
			{
				check.State = AuditState.Problem;
				check.Detail = error.Message + " Reinstall RS_ASIO (RS_ASIO.dll and avrt.dll).";
				return check;
			}
			if (mode == AudioInputMode.Unavailable)
			{
				check.Name = "RS_ASIO not installed";
				check.State = AuditState.NotUsed;
				check.Detail = "RS_ASIO is only needed for an audio interface. The guitar uses the Real Tone Cable.";
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
		public static AuditCheck CheckRsAsioSetUp(string gameDirectory)
		{
			var check = new AuditCheck { Name = "RS_ASIO set up" };
			string path = Path.Combine(gameDirectory, "RS_ASIO.ini");
			if (!File.Exists(path)) { check.State = AuditState.Problem; check.Detail = "RS_ASIO.ini is missing, so RS_ASIO offers the game no device. Set up for me writes it."; return check; }
			if (IniText.Load(path).Get("Config", "EnableAsio") != "1") { check.State = AuditState.Problem; check.Detail = "RS_ASIO.ini does not turn ASIO on (EnableAsio=1). Set up for me fixes it."; return check; }
			if (!AsioProxySetup.IsLinked(gameDirectory)) { check.State = AuditState.Warning; check.Detail = "RS_ASIO.ini does not go through Rocksmith Audio Bridge ASIO, so the bridge hears nothing. Set up for me fixes it (your file is backed up)."; return check; }
			check.State = AuditState.Ok;
			check.Detail = "RS_ASIO.ini goes through Rocksmith Audio Bridge ASIO.";
			return check;
		}

		// ---- Real Tone Cable --------------------------------------------------------------------------------------
		// The cable as Windows sees it: an active capture endpoint named "Rocksmith USB Guitar Adapter", or, once bound
		// to WinUSB for the direct USB read (DLL/AudioBridge/Cable/UsbCableCapture.cpp, VID_12BA&PID_00FF&MI_00), no
		// endpoint at all, which is then expected.
		public static AuditCheck CheckCable()
		{
			var check = new AuditCheck { Name = "Real Tone Cable" };
			if (CableOnWinUsb())
			{
				check.State = AuditState.Ok;
				check.Detail = "Set up for the direct USB read (WinUSB), so Windows does not list it as a sound device. That is expected.";
				return check;
			}
			DeviceState found = 0;
			try
			{
				using (var enumerator = new MMDeviceEnumerator())
					foreach (MMDevice device in enumerator.EnumerateAudioEndPoints(DataFlow.Capture, DeviceState.Active | DeviceState.Disabled | DeviceState.Unplugged))
					{
						string name;
						try { name = device.FriendlyName; } catch { continue; }
						if (name.IndexOf("Rocksmith", StringComparison.OrdinalIgnoreCase) < 0) continue;
						if (device.State == DeviceState.Active) { found = DeviceState.Active; break; }
						found = device.State;
					}
			}
			catch (Exception error) { check.State = AuditState.Warning; check.Detail = "Could not list sound devices: " + error.Message; return check; }
			switch (found)
			{
				case DeviceState.Active:
					check.State = AuditState.Ok;
					check.Detail = "Windows sees the cable as a recording device.";
					break;
				case DeviceState.Disabled:
					check.State = AuditState.Problem;
					check.Detail = "The cable is disabled in Windows' Sound settings, so Rocksmith cannot hear it. Enable it under Recording.";
					check.FixLabel = "Open Sound settings";
					check.Fix = () => System.Diagnostics.Process.Start("control.exe", "mmsys.cpl,,1");
					break;
				default:
					check.State = AuditState.Problem;
					check.Detail = "Windows does not see the cable. Plug it in (straight into the PC, not a hub if you can) and check again.";
					break;
			}
			return check;
		}

		private static bool CableOnWinUsb()
		{
			try
			{
				using (var usb = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\USB\VID_12BA&PID_00FF&MI_00"))
				{
					if (usb == null) return false;
					foreach (string instance in usb.GetSubKeyNames())
						using (var key = usb.OpenSubKey(instance))
							if (string.Equals(key?.GetValue("Service") as string, "WinUSB", StringComparison.OrdinalIgnoreCase)) return true;
				}
			}
			catch { }
			return false;
		}

		// Windows' microphone privacy switch also blocks the cable: with "Let desktop apps access your microphone" off,
		// Rocksmith gets silence from it. Read from the per-user and machine consent stores.
		public static AuditCheck CheckMicrophoneAccess()
		{
			var check = new AuditCheck { Name = "Windows microphone access" };
			const string store = @"Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\microphone";
			bool denied = false;
			foreach (RegistryKey root in new[] { Registry.CurrentUser, Registry.LocalMachine })
			{
				try
				{
					using (var key = root.OpenSubKey(store))
						if (string.Equals(key?.GetValue("Value") as string, "Deny", StringComparison.OrdinalIgnoreCase)) denied = true;
					using (var key = root.OpenSubKey(store + @"\NonPackaged"))
						if (string.Equals(key?.GetValue("Value") as string, "Deny", StringComparison.OrdinalIgnoreCase)) denied = true;
				}
				catch { }
			}
			check.State = denied ? AuditState.Problem : AuditState.Ok;
			check.Detail = denied
				? "Windows blocks desktop apps from the microphone, which includes the cable. Turn on \"Let desktop apps access your microphone\"."
				: "Windows lets desktop apps use recording devices.";
			if (denied) { check.FixLabel = "Open privacy settings"; check.Fix = () => System.Diagnostics.Process.Start("ms-settings:privacy-microphone"); }
			return check;
		}

		// ---- ASIO interface: the bridge's side ----------------------------------------------------------------------
		// Which real interface the bridge opens (HKCU AsioProxy Target), and whether it opened on the last launch
		// (LastBindError, written by DLL/AsioProxy/AsioProxyDriver.cpp).
		public static AuditCheck CheckBridgeInterface()
		{
			var check = new AuditCheck { Name = "Audio interface" };
			string target = AsioProxySetup.ReadTarget();
			if (target.Length == 0)
			{
				check.State = AuditState.Problem;
				check.Detail = "The bridge has no interface to open. Pick yours under ASIO interface on the Rocksmith Audio Bridge tab.";
				return check;
			}
			if (AsioProxySetup.ReadLastBindError(out string failed, out string reason))
			{
				check.State = AuditState.Problem;
				check.Detail = "Last launch: " + (failed.Length > 0 ? failed : target) + " would not open (" + reason.TrimEnd('.') + "), so the guitar was silent. Close other apps using it (its control app, a DAW, OBS) and start Rocksmith again.";
				return check;
			}
			check.State = AuditState.Ok;
			check.Detail = "The bridge opens " + target + ".";
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
