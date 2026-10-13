using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32;

namespace RSMods.Audio
{
	/// <summary>
	/// Installs the "Rocksmith Audio Bridge ASIO" proxy driver and records which real device it forwards to.
	/// It does exactly two things: register the proxy machine-wide (HKLM, one elevation prompt, because ASIO
	/// hosts enumerate drivers only from HKLM) and store the wrapped driver name in
	/// HKCU\Software\RSMods\AsioProxy\Target so the proxy can resolve the real device at boot.
	///
	/// Nothing here writes RS_ASIO.ini. The old Link/Unlink machinery rewrote it on the bridge power toggle and
	/// stranded users on the raw driver, which then failed to boot when the interface was absent. Pointing RS_ASIO at
	/// the bridge is now the job of one explicit tool, RsAsioIniSetup ("Set up for me" on the Audit tab, and a fresh
	/// install), which backs the player's file up first and never acts on a toggle. The RS_ASIO Settings tab (upstream)
	/// lets the player edit the file directly. IsLinked only READS the ini.
	/// </summary>
	internal static class AsioProxySetup
	{
		// Must match kAsioName / getDriverName() in DLL/AsioProxy/AsioProxyDriver.cpp.
		public const string ProxyName = "Rocksmith Audio Bridge ASIO";
		private const string TargetKey = @"Software\RSMods\AsioProxy";
		// Must match CLSID_RocksmithAudioBridge in DLL/AsioProxy/AsioProxyDriver.cpp.
		private const string Clsid = "{7B2E5C10-9F3A-4D6B-A1C8-2E4F6A8B0D31}";

		/// <summary>Every ASIO driver Windows knows about, except our own proxy. This is the list the
		/// user picks from instead of reading an RS_ASIO log for the exact name.</summary>
		public static List<string> ListRealDrivers()
		{
			var names = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
			foreach (var hive in new[] { RegistryHive.LocalMachine, RegistryHive.CurrentUser })
			{
				try
				{
					using (var baseKey = RegistryKey.OpenBaseKey(hive, RegistryView.Registry32))
					using (var asio = baseKey.OpenSubKey(@"Software\ASIO"))
					{
						if (asio == null) continue;
						foreach (string name in asio.GetSubKeyNames())
							if (!string.Equals(name, ProxyName, StringComparison.OrdinalIgnoreCase))
								names.Add(name);
					}
				}
				catch { /* hive unreadable; skip */ }
			}
			return new List<string>(names);
		}

		/// <summary>ASIO drivers registered for 64-bit programs only. Rocksmith and RS_ASIO are 32-bit and cannot see
		/// them, so an interface listed here is invisible to the game (the Audit says so).</summary>
		public static List<string> ListDriversOnlyIn64Bit()
		{
			var only64 = new List<string>();
			if (!Environment.Is64BitOperatingSystem) return only64;
			var x86 = new HashSet<string>(ListRealDrivers(), StringComparer.OrdinalIgnoreCase);
			try
			{
				using (var asio = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64).OpenSubKey(@"Software\ASIO"))
					if (asio != null)
						foreach (string name in asio.GetSubKeyNames())
							if (!x86.Contains(name) && !string.Equals(name, ProxyName, StringComparison.OrdinalIgnoreCase)) only64.Add(name);
			}
			catch { /* unreadable: report nothing */ }
			return only64;
		}

		/// <summary>True once the proxy is discoverable by ASIO hosts, i.e. its name is under HKLM\Software\ASIO.</summary>
		public static bool IsProxyRegistered()
		{
			try
			{
				using (var key = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32)
					.OpenSubKey(@"Software\ASIO\" + ProxyName))
					return key != null;
			}
			catch { return false; }
		}

		/// <summary>The DLL path the proxy's ASIO registration resolves to, the way a 32-bit ASIO host (RS_ASIO)
		/// resolves it: HKLM\Software\ASIO\&lt;name&gt; CLSID, then that CLSID's InprocServer32 in the 32-bit classes
		/// view (a per-user HKCU entry wins over the machine one, as it does for the host). "" when unregistered.</summary>
		public static string RegisteredProxyPath() => RegisteredProxyPath(RegistryView.Registry32);

		/// <summary>The same lookup in one registry view: Registry32 is what Rocksmith (32-bit) resolves,
		/// Registry64 what a 64-bit amp sim resolves.</summary>
		public static string RegisteredProxyPath(RegistryView view)
		{
			try
			{
				string clsid;
				using (var asio = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view).OpenSubKey(@"Software\ASIO\" + ProxyName))
					clsid = asio?.GetValue("CLSID") as string;
				if (string.IsNullOrEmpty(clsid)) return "";
				foreach (var hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
					using (var inproc = RegistryKey.OpenBaseKey(hive, view).OpenSubKey(@"Software\Classes\CLSID\" + clsid + @"\InprocServer32"))
					{
						string path = inproc?.GetValue("") as string;
						if (!string.IsNullOrEmpty(path)) return path;
					}
			}
			catch { }
			return "";
		}

		/// <summary>True only when the registration points at the proxy DLL we ship (expectedDllPath) and that file
		/// exists. Catches a stale registration: older versions registered the proxy as RocksmithAudioBridge.dll,
		/// which is now the main managed library, so an old registration makes RS_ASIO load the wrong DLL, find no
		/// device, and Rocksmith stops at "No audio output device".</summary>
		public static bool IsProxyRegistrationCurrent(string expectedDllPath)
		{
			if (!SamePath(RegisteredProxyPath(RegistryView.Registry32), expectedDllPath)) return false;
			// When the 64-bit guest ships beside it (external amp link), it must be registered too, so 64-bit ASIO
			// hosts (amp sims, DAWs) can list the driver.
			string guest = Guest64Path(expectedDllPath);
			return !File.Exists(guest) || SamePath(RegisteredProxyPath(RegistryView.Registry64), guest);
		}

		private static bool SamePath(string registered, string expected)
		{
			if (string.IsNullOrEmpty(registered) || !File.Exists(registered)) return false;
			try { return string.Equals(Path.GetFullPath(registered), Path.GetFullPath(expected), StringComparison.OrdinalIgnoreCase); }
			catch { return false; }
		}

		/// <summary>The 64-bit build of the driver, shipped next to the 32-bit one. In a 64-bit program it is
		/// always the external amp link's guest (DLL/AsioProxy/AmpLinkGuest.cpp) and never opens the interface.</summary>
		public static string Guest64Path(string proxyDllPath) =>
			Path.Combine(Path.GetDirectoryName(proxyDllPath) ?? "", "RocksmithAudioBridgeAsio64.dll");

		/// <summary>Read-only: reports whether the user has pointed RS_ASIO's output at the bridge. Never writes
		/// the ini; the user owns that file.</summary>
		public static bool IsLinked(string gameDirectory)
		{
			if (string.IsNullOrWhiteSpace(gameDirectory)) return false;
			string ini = Path.Combine(Path.GetFullPath(gameDirectory), "RS_ASIO.ini");
			return File.Exists(ini) && string.Equals(ReadDriver(ini), ProxyName, StringComparison.OrdinalIgnoreCase);
		}

		/// <summary>Registers the proxy machine-wide by calling its own DllRegisterServer through an elevated,
		/// 32-bit regsvr32 (one UAC prompt). It must be HKLM: ASIO hosts, RS_ASIO included, enumerate drivers
		/// only from HKLM\Software\ASIO, so a per-user entry is invisible to them. It must be the 32-bit
		/// regsvr32 (SysWOW64) because the proxy is a 32-bit DLL. Throws OperationCanceledException if the user
		/// declines the elevation prompt.</summary>
		public static void Register(string proxyDllPath)
		{
			if (!File.Exists(proxyDllPath)) throw new FileNotFoundException("The audio bridge driver file is missing from the game folder. Reinstall Rocksmith Audio Bridge to restore it.", proxyDllPath);
			RunRegsvr32(proxyDllPath, unregister: false);
			ClearUserOverride();
		}

		/// <summary>Removes the machine registration. regsvr32 /u has to load the proxy DLL to call its
		/// DllUnregisterServer, so when the file is gone (deleted, quarantined by antivirus, a game reinstall) or
		/// will not load, it fails and the HKLM entry used to stay forever: RS_ASIO kept listing a driver it could
		/// not load and the GUI's Remove button only reported regsvr32's exit code. In that case the two keys
		/// DllRegisterServer writes are deleted directly (still one elevation prompt), and success is judged by
		/// re-reading the registry rather than by an exit code.</summary>
		public static void Unregister(string proxyDllPath)
		{
			Unregister(proxyDllPath, File.Exists, path => RunRegsvr32(path, unregister: true), RemoveRegistrationKeys, IsProxyRegistered, ClearUserOverride);
		}

		/// <summary>The removal decision with its side effects injected (asio_config_tests covers it).</summary>
		internal static void Unregister(string proxyDllPath, Func<string, bool> fileExists, Action<string> runUnregister,
			Action removeKeys, Func<bool> isRegistered, Action clearUserOverride)
		{
			bool removed = false;
			if (fileExists(proxyDllPath))
			{
				try { runUnregister(proxyDllPath); removed = !isRegistered(); }
				catch (InvalidOperationException) { /* present but would not unregister: fall through to the keys */ }
			}
			if (!removed)
			{
				removeKeys();
				if (isRegistered())
					throw new InvalidOperationException("The audio bridge driver registration could not be removed.");
			}
			clearUserOverride();
		}

		/// <summary>Deletes exactly what the proxy's DllRegisterServer writes, in the 32-bit registry view:
		/// HKLM\Software\ASIO\Rocksmith Audio Bridge ASIO and the proxy's own CLSID. One elevated reg.exe run;
		/// a missing key is not an error. Throws OperationCanceledException if the prompt is declined.</summary>
		private static void RemoveRegistrationKeys()
		{
			string reg = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "reg.exe");
			string command = "/c \"\"" + reg + "\" delete \"HKLM\\SOFTWARE\\ASIO\\" + ProxyName + "\" /f /reg:32 >nul 2>&1 & \""
				+ reg + "\" delete \"HKLM\\SOFTWARE\\Classes\\CLSID\\" + Clsid + "\" /f /reg:32 >nul 2>&1 & \""
				// The same two keys in the 64-bit view: the 64-bit guest's registration (external amp link).
				+ reg + "\" delete \"HKLM\\SOFTWARE\\ASIO\\" + ProxyName + "\" /f /reg:64 >nul 2>&1 & \""
				+ reg + "\" delete \"HKLM\\SOFTWARE\\Classes\\CLSID\\" + Clsid + "\" /f /reg:64 >nul 2>&1\"";
			var info = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "cmd.exe"), command)
			{
				UseShellExecute = true,      // required for the "runas" (elevation) verb
				Verb = "runas",
				WindowStyle = ProcessWindowStyle.Hidden,
			};
			try
			{
				using (var process = Process.Start(info)) process.WaitForExit();
			}
			catch (Win32Exception error) when (error.NativeErrorCode == 1223) // ERROR_CANCELLED
			{
				throw new OperationCanceledException("Removing the audio bridge driver needs a one-time Windows admin approval, and the prompt was declined.");
			}
		}

		/// <summary>Removes the per-user InprocServer32 the game DLL writes when it finds a stale machine entry
		/// (DLL/AudioBridge/Driver/BridgeDriverRegistration.cpp). After a fresh machine registration it is redundant, and after an
		/// uninstall it must not linger, since a per-user entry wins over the machine one for 32-bit hosts.</summary>
		private static void ClearUserOverride()
		{
			try
			{
				using (var classes = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32).OpenSubKey(@"Software\Classes\CLSID", writable: true))
					classes?.DeleteSubKeyTree(Clsid, throwOnMissingSubKey: false);
			}
			catch { /* nothing to clear, or not ours to clear */ }
		}

		private static void RunRegsvr32(string proxyDllPath, bool unregister)
		{
			// From this 64-bit process, C:\Windows\SysWOW64\regsvr32.exe is the 32-bit one that can load a
			// 32-bit DLL. /s keeps regsvr32 silent so the result is its exit code, not a message box.
			string regsvr = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "SysWOW64", "regsvr32.exe");
			string args = (unregister ? "/u /s \"" : "/s \"") + proxyDllPath + "\"";
			// The 64-bit guest beside it (external amp link) goes through the 64-bit regsvr32 in the same elevated
			// run, so it is still one prompt. cmd's && stops at, and exits with, the first failure.
			string guest = Guest64Path(proxyDllPath);
			if (File.Exists(guest))
			{
				string regsvr64 = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),
					Environment.Is64BitProcess ? "System32" : "Sysnative", "regsvr32.exe");
				string guestArgs = (unregister ? "/u /s \"" : "/s \"") + guest + "\"";
				args = "/c \"\"" + regsvr + "\" " + args + " && \"" + regsvr64 + "\" " + guestArgs + "\"";
				regsvr = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "cmd.exe");
			}
			var info = new ProcessStartInfo(regsvr, args)
			{
				UseShellExecute = !IsAdministrator(),
				WindowStyle = ProcessWindowStyle.Hidden,
			};
			if (!IsAdministrator()) info.Verb = "runas";
			try
			{
				using (var process = Process.Start(info))
				{
					process.WaitForExit();
					if (process.ExitCode != 0)
						throw new InvalidOperationException("The driver registration step failed (regsvr32 exit code " + process.ExitCode + ").");
				}
			}
			catch (Win32Exception error) when (error.NativeErrorCode == 1223) // ERROR_CANCELLED
			{
				throw new OperationCanceledException("Registering the audio bridge driver needs a one-time Windows admin approval, and the prompt was declined.");
			}
		}

		public static string ReadTarget()
		{
			try
			{
				using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32).OpenSubKey(TargetKey))
					return key?.GetValue("Target") as string ?? "";
			}
			catch { return ""; }
		}

		/// <summary>Why the bridge driver could not open the real interface on the last launch, written by
		/// DLL/AsioProxy/AsioProxyDriver.cpp (LastBindError, LastBindErrorDriver) and removed once it binds. Lets the
		/// settings app explain a silent guitar after the game has closed. False when the last launch bound fine.</summary>
		public static bool ReadLastBindError(out string driver, out string reason)
		{
			driver = reason = "";
			try
			{
				using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32).OpenSubKey(TargetKey))
				{
					reason = key?.GetValue("LastBindError") as string ?? "";
					driver = key?.GetValue("LastBindErrorDriver") as string ?? "";
				}
			}
			catch { reason = ""; }
			return reason.Length > 0;
		}

		/// <summary>Records which real ASIO driver the proxy forwards to. This is the one piece of setup the GUI
		/// still owns; the proxy reads it from HKCU at boot. RS_ASIO.ini stays the user's to edit.</summary>
		/// <summary>The separate input interface for the guitar (DLL/AsioProxy/SplitInput.hpp, HKCU InputTarget), or ""
		/// when the guitar comes from the output interface.</summary>
		public static string ReadInputTarget()
		{
			try
			{
				using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32).OpenSubKey(TargetKey))
					return key?.GetValue("InputTarget") as string ?? "";
			}
			catch { return ""; }
		}

		/// <summary>Saves the input interface ("" = same as the output). The proxy reads it at each bind; the running
		/// game applies it live through engine op 30.</summary>
		public static void SelectInputTarget(string realDriverName)
		{
			using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32).CreateSubKey(TargetKey))
			{
				if (string.IsNullOrEmpty(realDriverName)) key.DeleteValue("InputTarget", throwOnMissingValue: false);
				else key.SetValue("InputTarget", realDriverName, RegistryValueKind.String);
				// One interface for every input: per-player choices (made in the overlay) would override it, so they go.
				foreach (string role in new[] { "Player1", "Player2", "Mic" }) key.DeleteValue("InputTarget." + role, throwOnMissingValue: false);
			}
		}

		/// <summary>One input role's own interface ("Player1", "Player2", "Mic"; HKCU InputTarget.&lt;role&gt;). "" stores
		/// the "(output)" marker, so the role stays on the output interface even when Player 1 has another one (an unset
		/// Player 2 and mic follow Player 1, see ReadInputTargetDriverName in AsioProxyDriver.cpp).</summary>
		public static void SelectInputTarget(string role, string realDriverName)
		{
			using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32).CreateSubKey(TargetKey))
				key.SetValue("InputTarget." + role, string.IsNullOrEmpty(realDriverName) ? "(output)" : realDriverName, RegistryValueKind.String);
		}

		public static void SelectTarget(string realDriverName)
		{
			using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32)
				.CreateSubKey(TargetKey))
				key.SetValue("Target", realDriverName ?? "", RegistryValueKind.String);
		}

		public static void SetPreferRealOutput(bool preferReal)
		{
			using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32)
				.CreateSubKey(TargetKey))
				key.SetValue("PreferReal", preferReal ? 1 : 0, RegistryValueKind.DWord);
		}

		// Which interface the bridge wraps; the user is never asked. The saved HKCU target wins (an earlier install
		// set it), then the interface RS_ASIO.ini names directly, then the only installed driver. Every ASIO driver
		// counts (ASIO4ALL, FlexASIO, Voicemeeter included): the bridge does not judge the user's setup. With several
		// drivers and none named it does not guess, since wrapping the wrong one leaves the player with no sound.
		// null when no interface can be chosen.
		public static string ResolveBridgeInterface(List<string> real, string target, string iniDriver)
		{
			bool Installed(string name) => !string.IsNullOrEmpty(name) && real.Exists(r => string.Equals(r, name, StringComparison.OrdinalIgnoreCase));
			if (Installed(target)) return target;
			if (Installed(iniDriver)) return iniDriver;
			return real.Count == 1 ? real[0] : null;
		}

		/// <summary>The driver is part of the mod: the installer sets it up on every Install / Reinstall / Repair
		/// (through RSMods.exe --install-audio-bridge-driver "&lt;game folder&gt;") and its Uninstall removes it.
		/// Installs the shared UsbDk dependency and registers the driver and its 64-bit guest (one admin prompt,
		/// skipped when each is already current). The caller records the interface it wraps after elevation. Throws OperationCanceledException
		/// when the prompt is declined.</summary>
		public static void Install(string gameDirectory)
		{
			UsbDkSetup.Install(gameDirectory);
			string driverPath = Path.Combine(Path.GetFullPath(gameDirectory), "RocksmithAudioBridgeAsio.dll");
			if (!IsProxyRegistrationCurrent(driverPath)) Register(driverPath);
		}

		/// <summary>Records the interface the bridge wraps when none is saved yet and one can be chosen (see
		/// ResolveBridgeInterface). Needs no elevation; the settings app calls it too, so a target that could not be
		/// chosen at install (several drivers, none named) is filled in once RS_ASIO.ini names one.</summary>
		public static void FillTarget(string gameDirectory)
		{
			var real = ListRealDrivers();
			string target = ReadTarget();
			if (real.Exists(r => string.Equals(r, target, StringComparison.OrdinalIgnoreCase))) return;
			string driver = ResolveBridgeInterface(real, target, ReadOutputDriver(gameDirectory));
			if (driver == null) return;
			SelectTarget(driver);
			SetPreferRealOutput(true);
		}

		public const string InstallArgument = "--install-audio-bridge-driver";
		private const string ELEVATED_MARKER = "--elevated";

		/// <summary>Exit codes for the installer: 0 set up, 2 the admin prompt was declined, 1 any other failure
		/// (details in RSMods\AudioBridgeDriverInstall.log).</summary>
		public static bool TryRunInstallCommand(string[] arguments, out int exitCode)
		{
			exitCode = 0;
			if (arguments.Length == 0 || !string.Equals(arguments[0], InstallArgument, StringComparison.OrdinalIgnoreCase)) return false;
			string log = Path.Combine(System.Windows.Forms.Application.StartupPath, "AudioBridgeDriverInstall.log");
			try
			{
				if (arguments.Length != 2 && !(arguments.Length == 3 && string.Equals(arguments[2], ELEVATED_MARKER, StringComparison.OrdinalIgnoreCase)))
					throw new ArgumentException("Usage: " + InstallArgument + " <Rocksmith directory>");
				bool elevatedChild = arguments.Length == 3;
				if (!elevatedChild && !IsAdministrator())
				{
					exitCode = RunElevated(arguments[1]);
					if (exitCode == 0) CompletePerUserSetup(arguments[1], log);
					return true;
				}
				Install(arguments[1]);
				if (elevatedChild) return true;
				CompletePerUserSetup(arguments[1], log);
			}
			catch (OperationCanceledException error) { exitCode = 2; File.WriteAllText(log, error.Message); }
			catch (Exception error) { exitCode = 1; File.WriteAllText(log, error.ToString()); }
			return true;
		}

		private static void CompletePerUserSetup(string gameDirectory, string log)
		{
			// A fresh install also points RS_ASIO.ini at the bridge, backing the player's file up first. Only a
			// file nobody set up yet that uses one interface throughout (RsAsioIniSetup.SetUpOnInstall).
			FillTarget(gameDirectory);
			string iniSetup = RsAsioIniSetup.SetUpOnInstall(gameDirectory);
			string target = ReadTarget();
			File.WriteAllText(log, "Driver registered; " + (string.IsNullOrEmpty(target) ? "no interface chosen yet." : "wraps " + target + ".") + Environment.NewLine + iniSetup);
		}

		private static int RunElevated(string gameDirectory)
		{
			string executable = System.Reflection.Assembly.GetExecutingAssembly().Location;
			var info = new ProcessStartInfo(executable, QuoteArgument(InstallArgument) + " " + QuoteArgument(gameDirectory) + " " + QuoteArgument(ELEVATED_MARKER))
			{
				UseShellExecute = true,
				Verb = "runas",
				WindowStyle = ProcessWindowStyle.Hidden,
			};
			try
			{
				using (var process = Process.Start(info))
				{
					process.WaitForExit();
					return process.ExitCode;
				}
			}
			catch (Win32Exception error) when (error.NativeErrorCode == 1223)
			{
				throw new OperationCanceledException("Installing Rocksmith Audio Bridge needs one Windows admin approval, and the prompt was declined.");
			}
		}

		private static string QuoteArgument(string argument)
		{
			var builder = new StringBuilder("\"");
			int backslashes = 0;
			foreach (char character in argument)
			{
				if (character == '\\') { backslashes++; continue; }
				if (character == '"') builder.Append('\\', backslashes * 2 + 1).Append('"');
				else builder.Append('\\', backslashes).Append(character);
				backslashes = 0;
			}
			builder.Append('\\', backslashes * 2).Append('"');
			return builder.ToString();
		}

		private static bool IsAdministrator()
		{
			using (var identity = WindowsIdentity.GetCurrent())
				return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
		}

		/// <summary>Read-only: RS_ASIO.ini's [Asio.Output] Driver, or "" when the file or line is missing.</summary>
		public static string ReadOutputDriver(string gameDirectory)
		{
			if (string.IsNullOrWhiteSpace(gameDirectory)) return "";
			string ini = Path.Combine(Path.GetFullPath(gameDirectory), "RS_ASIO.ini");
			return File.Exists(ini) ? ReadDriver(ini) : "";
		}

		// RS_ASIO reads its .ini itself; this class only READS it, to report link state (RsAsioIniSetup writes it).
		private static string ReadDriver(string ini) => ReadSection(ini, "Asio.Output", "Driver");

		private static string ReadSection(string ini, string section, string key)
		{
			var sb = new StringBuilder(512);
			GetPrivateProfileString(section, key, "", sb, sb.Capacity, ini);
			return sb.ToString();
		}

		[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
		private static extern uint GetPrivateProfileString(string section, string key, string def, StringBuilder value, int size, string path);
	}
}
