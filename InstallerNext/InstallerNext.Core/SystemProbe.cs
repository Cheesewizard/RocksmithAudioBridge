#nullable enable
using System;
using System.Diagnostics;
using System.IO;
using Microsoft.Win32;
using RS2014_Mod_Installer.Core;

namespace RocksmithAudioBridge.Setup
{
	/// <summary>Read-only questions about the PC the planner asks. Injected so tests control the answers; nothing here
	/// changes anything, which is what lets a dry run against the real game folder stay read-only.</summary>
	public interface ISystemProbe
	{
		bool IsRocksmithFolder(string gameFolder);
		CompatibilityResult CheckCompatibility(string gameFolder);
		ExistingNativeMod InspectNativeMod(string gameFolder);
		bool IsGameRunning();
		/// <summary>A process running from exactly this file (our app, the configurator, the helper).</summary>
		bool IsRunningFrom(string exePath);
		bool IsDriverRegistered();
		bool IsUsbDkInstalled();
		bool UsbDkInstalledByUs();
		string AppData { get; }
		string LocalAppData { get; }
		string StartMenuPrograms { get; }
		string Desktop { get; }
		string SystemFolder { get; }
	}

	public sealed class SystemProbe : ISystemProbe
	{
		public bool IsRocksmithFolder(string gameFolder) => RocksmithLocator.IsRocksmithFolder(gameFolder);
		public CompatibilityResult CheckCompatibility(string gameFolder) => RocksmithCompatibility.Evaluate(gameFolder);
		public ExistingNativeMod InspectNativeMod(string gameFolder) => ExistingNativeModInspector.Inspect(gameFolder);
		public bool IsGameRunning() => Process.GetProcessesByName("Rocksmith2014").Length != 0;

		public bool IsRunningFrom(string exePath)
		{
			string full = Path.GetFullPath(exePath);
			foreach (var process in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(full)))
			{
				try
				{
					if (string.Equals(process.MainModule?.FileName, full, StringComparison.OrdinalIgnoreCase)) return true;
				}
				catch { return true; }   // cannot inspect it: assume it may be ours and ask for it to be closed (as 4.x)
				finally { process.Dispose(); }
			}
			return false;
		}

		public bool IsDriverRegistered() => RocksmithAudioBridge.Core.AsioProxySetup.IsProxyRegistered() || HasRegistration(RegistryView.Registry64);

		private static bool HasRegistration(RegistryView view)
		{
			if (!Environment.Is64BitOperatingSystem) return false;
			using var root = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view);
			using var driver = root.OpenSubKey(@"Software\ASIO\" + RocksmithAudioBridge.Core.AsioProxySetup.ProxyName);
			return driver != null;
		}

		public bool IsUsbDkInstalled()
		{
			foreach (var view in Environment.Is64BitOperatingSystem ? new[] { RegistryView.Registry64, RegistryView.Registry32 } : new[] { RegistryView.Registry32 })
			{
				using var root = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view);
				using var entry = root.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Uninstall\" + ProductLayout.UsbDkProductCode);
				if (entry != null) return true;
			}
			return false;
		}

		public bool UsbDkInstalledByUs()
		{
			using var key = Registry.CurrentUser.OpenSubKey(ProductLayout.InstallerStateKey);
			return key?.GetValue(ProductLayout.UsbDkInstalledByUsValue) is int value && value == 1;
		}

		public string AppData => Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
		public string LocalAppData => Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
		public string StartMenuPrograms => Environment.GetFolderPath(Environment.SpecialFolder.Programs);
		public string Desktop => Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
		public string SystemFolder => Environment.SystemDirectory;
	}
}
