using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
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
	/// It NEVER edits RS_ASIO.ini. Pointing RS_ASIO at the bridge (Output and Input Driver=Rocksmith Audio
	/// Bridge ASIO) is left entirely to the user, so the GUI can never silently revert an ini the user configured.
	/// The old Link/Unlink machinery did exactly that on the bridge power toggle and stranded users on the raw
	/// driver, which then failed to boot when the interface was absent. IsLinked only READS the ini to report
	/// whether the user has pointed it at the bridge; nothing here writes it.
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
			// When the 64-bit guest ships beside it (external amp link), it must be registered too, so
			// 64-bit amp sims such as AmpliTube 5 can list the driver. A missing one shows Repair, which registers both.
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
		/// (DLL/Audio/AsioProxyRegistration.cpp). After a fresh machine registration it is redundant, and after an
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
				UseShellExecute = true,      // required for the "runas" (elevation) verb
				Verb = "runas",
				WindowStyle = ProcessWindowStyle.Hidden,
			};
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

		/// <summary>Records which real ASIO driver the proxy forwards to. This is the one piece of setup the GUI
		/// still owns; the proxy reads it from HKCU at boot. RS_ASIO.ini stays the user's to edit.</summary>
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

		/// <summary>Read-only: RS_ASIO.ini's [Asio.Output] Driver, or "" when the file or line is missing.</summary>
		public static string ReadOutputDriver(string gameDirectory)
		{
			if (string.IsNullOrWhiteSpace(gameDirectory)) return "";
			string ini = Path.Combine(Path.GetFullPath(gameDirectory), "RS_ASIO.ini");
			return File.Exists(ini) ? ReadDriver(ini) : "";
		}

		// RS_ASIO reads its .ini itself; we only ever READ it, to report link state. We never write it.
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
