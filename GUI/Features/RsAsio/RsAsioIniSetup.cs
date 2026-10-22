using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using Microsoft.Win32;
using RSMods.Audio;

namespace RSMods
{
	// "Set up for me": points RS_ASIO.ini at the Rocksmith Audio Bridge, the one tool that writes RS_ASIO.ini.
	//
	// Why it may: players were left to edit RS_ASIO.ini by hand, and the common mistakes (guitar on Input.1, a driver
	// name that is not installed, the interface opened directly beside the bridge) cost them their sound. It writes
	// only when the player clicks it (the Audit tab) or on a fresh install, never on a toggle. That was the old
	// Link/Unlink's flaw (BridgeDriverSetup.cs): it rewrote the file on the bridge power switch. The file it writes
	// names the bridge, which boots on its virtual clock with the interface unplugged, so it cannot strand anyone.
	//
	// Kept reversible: before the first write the player's own file is copied to
	// %APPDATA%\RocksmithAudioBridge\Backups\RS_ASIO.original.ini, and "Restore" (and the uninstaller) puts it back.
	// A hash of what was written is kept, so a file the player edits afterwards (by hand or on the RS_ASIO Settings
	// tab) reads as "edited by hand" and the installer never overwrites it.
	//
	// What it writes: every guitar section and the output on "Rocksmith Audio Bridge ASIO" at fixed channels
	// (output 0, Player 1 input 0, Player 2 input 1); [Config] EnableAsio=1. Everything else in the file stays. The
	// player's own device and channel choices move into the bridge: the device becomes the bridge's interface
	// (Target) and a channel other than the fixed one becomes a channel route (the overlay's Routing card).
	internal static class RsAsioIniSetup
	{
		public enum State { NoRsAsio, NotSetUp, Managed, EditedByHand }

		private const string StateKey = @"Software\RSMods\AsioProxy";
		private const string HashValue = "RsAsioIniHash";
		private const string OriginalMissingValue = "RsAsioIniOriginalMissing";

		// The RS_ASIO.ini sections the bridge takes over, with the channel key and the fixed channel it writes. Route
		// value names match kRouteValueNames in DLL/AsioProxy/AsioProxyDriver.cpp.
		private static readonly (string Section, string ChannelKey, int HostChannel, string Route)[] Roles =
		{
			("Asio.Output", "BaseChannel", 0, "Output"),
			("Asio.Input.0", "Channel", 0, "Player1"),
			("Asio.Input.1", "Channel", 1, "Player2"),
		};

		public static string BackupFolder =>
			Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "RocksmithAudioBridge", "Backups");
		public static string OriginalBackupPath => Path.Combine(BackupFolder, "RS_ASIO.original.ini");
		private static string IniPath(string gameDirectory) => Path.Combine(gameDirectory, "RS_ASIO.ini");
		private static bool RsAsioInstalled(string gameDirectory) =>
			File.Exists(Path.Combine(gameDirectory, "RS_ASIO.dll")) || File.Exists(Path.Combine(gameDirectory, "RS_ASIO.dll.disabled"));

		public static bool CanRestore => File.Exists(OriginalBackupPath) || ReadState(OriginalMissingValue) == "1";

		public static State GetState(string gameDirectory)
		{
			string ini = IniPath(gameDirectory);
			if (!File.Exists(ini)) return RsAsioInstalled(gameDirectory) ? State.NotSetUp : State.NoRsAsio;
			string written = ReadState(HashValue);
			if (written.Length == 0) return State.NotSetUp;
			return string.Equals(written, Hash(ini), StringComparison.OrdinalIgnoreCase) ? State.Managed : State.EditedByHand;
		}

		/// <summary>What Set up would do with the current file, for the confirmation and the installer.</summary>
		internal sealed class Plan
		{
			public string Target;               // the interface the bridge will wrap; null when none can be chosen
			public string SplitDriver = "";     // the first guitar section on a different interface than Target
			public readonly Dictionary<string, string> SplitByRole = new Dictionary<string, string>();   // "Player1"/"Player2" -> its own interface
			public readonly List<string> Notes = new List<string>();
		}

		public static Plan Analyze(string gameDirectory)
		{
			var plan = new Plan();
			var ini = IniText.Load(IniPath(gameDirectory));
			var installed = AsioProxySetup.ListBridgeDrivers();   // the bridge opens 64-bit-only drivers too
			bool Installed(string name) => installed.Exists(d => string.Equals(d, name, StringComparison.OrdinalIgnoreCase));

			// The interface: the one the player's file already names (output first), else the bridge's current one,
			// else the only interface installed.
			foreach (var role in Roles)
			{
				string driver = ini.Get(role.Section, "Driver") ?? "";
				if (!IsBridge(driver) && Installed(driver)) { plan.Target = driver; break; }
			}
			if (plan.Target == null && Installed(AsioProxySetup.ReadTarget())) plan.Target = AsioProxySetup.ReadTarget();
			if (plan.Target == null && installed.Count == 1) plan.Target = installed[0];
			if (plan.Target == null)
				plan.Notes.Add(installed.Count == 0 ? "No ASIO driver is installed, so the bridge has no interface to open yet. Install your interface's ASIO driver."
					: "Several ASIO drivers are installed and RS_ASIO.ini names none of them. Pick your interface under ASIO interface on the Rocksmith Audio Bridge tab, then set up again.");

			foreach (var role in Roles)
			{
				string driver = ini.Get(role.Section, "Driver") ?? "";
				if (driver.Length == 0 || IsBridge(driver) || plan.Target == null || string.Equals(driver, plan.Target, StringComparison.OrdinalIgnoreCase)) continue;
				if (!Installed(driver)) { plan.Notes.Add("[" + role.Section + "] named '" + driver + "', which is not installed; it now goes through the bridge."); continue; }
				// The output is always the first section with an installed driver, so a different one here is an input:
				// it becomes that player's input interface (SplitInput), with its channel kept.
				if (role.Route == "Output") continue;
				if (plan.SplitDriver.Length == 0) plan.SplitDriver = driver;
				plan.SplitByRole[role.Route] = driver;
				plan.Notes.Add((role.Route == "Player1" ? "Player 1" : "Player 2") + " keeps coming from " + driver + " (its input interface) while the sound plays on " + plan.Target + ".");
			}
			return plan;
		}

		/// <summary>Backs up, writes the bridge setup and moves the player's device and channel choices into the
		/// bridge. Returns what it did, one line each.</summary>
		public static List<string> SetUp(string gameDirectory)
		{
			if (!RsAsioInstalled(gameDirectory) && !File.Exists(IniPath(gameDirectory)))
				throw new InvalidOperationException("RS_ASIO is not installed in the Rocksmith folder.");
			var done = new List<string>();
			string path = IniPath(gameDirectory);
			BackUpBeforeWriting(gameDirectory, done);

			Plan plan = Analyze(gameDirectory);
			var ini = IniText.Load(path);
			if (plan.Target != null && !string.Equals(AsioProxySetup.ReadTarget(), plan.Target, StringComparison.OrdinalIgnoreCase))
			{
				AsioProxySetup.SelectTarget(plan.Target);
				AsioProxySetup.SetPreferRealOutput(true);
				done.Add("The bridge now opens " + plan.Target + ".");
			}

			// Each player on its own interface keeps it. With any player split, the others are pinned to the output
			// interface explicitly ("(output)"): an unset Player 2 would otherwise follow Player 1's interface.
			if (plan.SplitByRole.Count > 0)
				foreach (string player in new[] { "Player1", "Player2" })
				{
					plan.SplitByRole.TryGetValue(player, out string own);
					AsioProxySetup.SelectInputTarget(player, own ?? "");
					if (own != null) done.Add((player == "Player1" ? "Player 1" : "Player 2") + " comes from " + own + " (its input interface).");
				}

			foreach (var role in Roles)
			{
				string driver = ini.Get(role.Section, "Driver") ?? "";
				// The channel route is saved under the interface the role ends up on: its input interface when the section
				// named one, else the output interface.
				string routeDevice = plan.SplitByRole.TryGetValue(role.Route, out string split) ? split
					: IsBridge(driver) || (plan.Target != null && string.Equals(driver, plan.Target, StringComparison.OrdinalIgnoreCase)) ? plan.Target : null;
				if (routeDevice != null && int.TryParse(ini.Get(role.Section, role.ChannelKey), out int channel)
					&& channel != role.HostChannel && channel >= 0 && SavedRoute(routeDevice, role.Route) < 0)
				{
					WriteRoute(routeDevice, role.Route, channel);
					done.Add(role.Route + " keeps channel " + (channel + 1) + " of " + routeDevice + " (a channel route; change it on the overlay's Output page).");
				}
				ini.Set(role.Section, "Driver", AsioProxySetup.ProxyName);
				ini.Set(role.Section, role.ChannelKey, role.HostChannel.ToString());
			}
			// The microphone on the bridge's own interface must go through the bridge too: most ASIO drivers open only
			// once per program, so RS_ASIO opening it directly beside the bridge would fail. Its channel stays.
			string mic = ini.Get("Asio.Input.Mic", "Driver") ?? "";
			if (plan.Target != null && string.Equals(mic, plan.Target, StringComparison.OrdinalIgnoreCase))
			{
				ini.Set("Asio.Input.Mic", "Driver", AsioProxySetup.ProxyName);
				done.Add("The microphone on " + plan.Target + " now goes through the bridge too.");
			}
			ini.Set("Config", "EnableAsio", "1");
			ini.Save(path);
			WriteState(HashValue, Hash(path));
			done.Add("RS_ASIO.ini now points at Rocksmith Audio Bridge ASIO. It applies when Rocksmith starts.");
			done.AddRange(plan.Notes);
			return done;
		}

		/// <summary>Puts the player's original RS_ASIO.ini back (or removes ours if there was none) and forgets the
		/// managed state. The bridge's interface and channel routes stay saved for a later setup. The original copy is
		/// renamed RS_ASIO.restored-&lt;time&gt;.ini, so the next Set up for me backs up the file as it is then.</summary>
		public static void Restore(string gameDirectory)
		{
			string path = IniPath(gameDirectory);
			if (File.Exists(OriginalBackupPath))
			{
				File.Copy(OriginalBackupPath, path, overwrite: true);
				File.Move(OriginalBackupPath, UniqueBackupPath("RS_ASIO.restored-" + DateTime.Now.ToString("yyyyMMdd-HHmmss")));
			}
			else if (ReadState(OriginalMissingValue) == "1") { if (File.Exists(path)) File.Delete(path); }
			else throw new InvalidOperationException("There is no saved original RS_ASIO.ini to restore.");
			WriteState(HashValue, null);
			WriteState(OriginalMissingValue, null);
		}

		// The first write saves the player's own file once; a later write over a hand-edited file keeps that version
		// too, dated, so nothing the player typed is ever lost.
		private static void BackUpBeforeWriting(string gameDirectory, List<string> done)
		{
			string path = IniPath(gameDirectory);
			Directory.CreateDirectory(BackupFolder);
			if (!CanRestore)
			{
				if (File.Exists(path)) { File.Copy(path, OriginalBackupPath, overwrite: false); done.Add("Your original RS_ASIO.ini is saved in " + BackupFolder + "."); }
				else { WriteState(OriginalMissingValue, "1"); File.WriteAllText(path, ""); }
			}
			else if (File.Exists(path) && GetState(gameDirectory) == State.EditedByHand)
			{
				string dated = UniqueBackupPath("RS_ASIO." + DateTime.Now.ToString("yyyy-MM-dd HHmmss"));
				File.Copy(path, dated, overwrite: false);
				done.Add("Your edited RS_ASIO.ini is saved as " + Path.GetFileName(dated) + ".");
			}
		}

		/// <summary>Saves a dated copy of another game file (Rocksmith.ini) before the Audit changes it; the first copy
		/// is also kept as &lt;name&gt;.original.ini.</summary>
		public static void BackUpGameFile(string path)
		{
			if (!File.Exists(path)) return;
			Directory.CreateDirectory(BackupFolder);
			string name = Path.GetFileNameWithoutExtension(path);
			string original = Path.Combine(BackupFolder, name + ".original.ini");
			if (!File.Exists(original)) File.Copy(path, original);
			File.Copy(path, UniqueBackupPath(name + "." + DateTime.Now.ToString("yyyy-MM-dd HHmmss")));
		}

		// <stem>.ini in the backup folder, or <stem> (2).ini and up when two backups land in the same second.
		private static string UniqueBackupPath(string stem)
		{
			string path = Path.Combine(BackupFolder, stem + ".ini");
			for (int copy = 2; File.Exists(path); copy++) path = Path.Combine(BackupFolder, stem + " (" + copy + ").ini");
			return path;
		}

		public static bool IsBridge(string driver) =>
			string.Equals(driver, AsioProxySetup.ProxyName, StringComparison.OrdinalIgnoreCase) || string.Equals(driver, "Rocksmith Audio Bridge", StringComparison.OrdinalIgnoreCase);

		// ---- installer / uninstaller verbs ------------------------------------------------------------------------
		public const string RestoreArgument = "--restore-rs-asio-ini";

		/// <summary>On install: set up only a file nobody has set up yet and that names an installed interface. A
		/// file this tool already wrote or one the player edited is left as it
		/// is (the Audit tab offers the setup).</summary>
		public static string SetUpOnInstall(string gameDirectory)
		{
			try
			{
				if (GetState(gameDirectory) != State.NotSetUp) return "RS_ASIO.ini left as it is (" + GetState(gameDirectory) + ").";
				if (!File.Exists(IniPath(gameDirectory))) return "RS_ASIO.ini left as it is (no file).";
				Plan plan = Analyze(gameDirectory);
				if (plan.Target == null) return "RS_ASIO.ini left as it is (no interface chosen).";
				return string.Join(" ", SetUp(gameDirectory));
			}
			catch (Exception error) { return "RS_ASIO.ini setup failed: " + error.Message; }
		}

		/// <summary>RSMods.exe --restore-rs-asio-ini "&lt;game folder&gt;", run by the uninstaller. Exit 0 restored or
		/// nothing to restore, 1 failed.</summary>
		public static bool TryRunRestoreCommand(string[] arguments, out int exitCode)
		{
			exitCode = 0;
			if (arguments.Length == 0 || !string.Equals(arguments[0], RestoreArgument, StringComparison.OrdinalIgnoreCase)) return false;
			try { if (arguments.Length == 2 && CanRestore) Restore(arguments[1]); }
			catch { exitCode = 1; }
			return true;
		}

		// ---- storage ---------------------------------------------------------------------------------------------
		private static string Hash(string path)
		{
			using (var sha = SHA256.Create())
			using (var stream = File.OpenRead(path))
				return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "");
		}

		private static string ReadState(string name)
		{
			try
			{
				using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32).OpenSubKey(StateKey))
					return key?.GetValue(name) as string ?? "";
			}
			catch { return ""; }
		}

		private static void WriteState(string name, string value)
		{
			using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32).CreateSubKey(StateKey))
			{
				if (value == null) key.DeleteValue(name, throwOnMissingValue: false);
				else key.SetValue(name, value, RegistryValueKind.String);
			}
		}

		// Channel routes, the same values the driver reads (ReadSavedRoute in AsioProxyDriver.cpp): one DWORD per role
		// under Routes\<driver>, with '\' in the driver name turned into '_'.
		private static string RoutesKey(string driver) => StateKey + @"\Routes\" + driver.Replace('\\', '_');

		private static int SavedRoute(string driver, string role)
		{
			try
			{
				using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32).OpenSubKey(RoutesKey(driver)))
					return key?.GetValue(role) is int value ? value : -1;
			}
			catch { return -1; }
		}

		private static void WriteRoute(string driver, string role, int channel)
		{
			using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32).CreateSubKey(RoutesKey(driver)))
				key.SetValue(role, channel, RegistryValueKind.DWord);
		}
	}
}
