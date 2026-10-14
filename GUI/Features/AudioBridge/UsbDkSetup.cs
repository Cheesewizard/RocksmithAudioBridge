using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using Microsoft.Win32;

namespace RSMods.Audio
{
	internal static class UsbDkSetup
	{
		public enum Outcome { Ready, RestartNeeded, Failed }

		private const string PACKAGE_FILE_NAME = "UsbDk_1.0.22_x64.msi";
		private const string LICENSE_FILE_NAME = "LICENSE.txt";
		private const string PACKAGE_SHA256 = "91F6F695E1E13C656024E6D3B55620BF08D8835EF05EE0496935BA6BB62466A5";
		private const string PACKAGE_RESOURCE_NAME = "RSMods.UsbDk_1.0.22_x64.msi";
		private const int MSI_REBOOT_REQUIRED = 3010;
		private const int MSI_REBOOT_INITIATED = 1641;

		public static string LogPath(string gameDirectory) => Path.Combine(Path.GetFullPath(gameDirectory), "RSMods", "UsbDk", "UsbDkInstall.log");

		/// <summary>Installs the shared UsbDk runtime the direct USB cable capture uses. Never throws: the bridge driver
		/// does not depend on it, so a failure is reported (note, and UsbDkInstall.log) and the install goes on.</summary>
		public static Outcome Install(string gameDirectory, out string note)
		{
			string logPath = LogPath(gameDirectory);
			try
			{
				string licenseDirectory = Path.GetDirectoryName(logPath);
				Directory.CreateDirectory(licenseDirectory);
				if (File.Exists(logPath)) File.Delete(logPath);
				string licensePath = Path.Combine(licenseDirectory, LICENSE_FILE_NAME);
				if (!File.Exists(licensePath))
				{
					using (var license = Assembly.GetExecutingAssembly().GetManifestResourceStream("RSMods.UsbDk_LICENSE.txt"))
					{
						if (license == null) throw new FileNotFoundException("The bundled UsbDk licence is missing.");
						using (var output = File.Create(licensePath)) license.CopyTo(output);
					}
				}

				if (!Environment.Is64BitOperatingSystem)
				{
					note = "UsbDk was skipped: its installer is for 64-bit Windows only.";
					File.WriteAllText(logPath, note + Environment.NewLine);
					return Outcome.Ready;
				}
				if (IsInstalled())
				{
					note = "UsbDk is already installed system-wide; the shared dependency was left unchanged.";
					File.WriteAllText(logPath, note + Environment.NewLine);
					return Outcome.Ready;
				}

				int exitCode = RunPackage(logPath);
				if (exitCode == MSI_REBOOT_REQUIRED || exitCode == MSI_REBOOT_INITIATED)
				{
					note = "UsbDk was installed and needs a Windows restart before direct USB cable capture works.";
					return Outcome.RestartNeeded;
				}
				if (exitCode != 0) throw new InvalidOperationException("UsbDk installation failed with Windows Installer exit code " + exitCode + ".");
				if (!IsInstalled()) throw new InvalidOperationException("UsbDk installation completed without registering its system service.");
				// A fresh install: its filter only attaches to USB devices Windows starts after it, so the cable plugged in
				// now is only seen after a restart (until then the cable plays through Windows, with its usual delay).
				note = "UsbDk was installed and needs a Windows restart before direct USB cable capture works.";
				return Outcome.RestartNeeded;
			}
			catch (Exception error)
			{
				note = "UsbDk was not installed (" + error.Message + "). Only direct USB cable capture is unavailable until it installs. See " + logPath + ".";
				// When msiexec ran, its own log already holds the details.
				try { if (!File.Exists(logPath)) File.WriteAllText(logPath, error + Environment.NewLine); } catch { }
				return Outcome.Failed;
			}
		}

		// The package runs elevated, so it is written to a fresh folder only administrators and SYSTEM can change, and
		// hashed again on disk right before msiexec opens it. A folder under the player's own %TEMP% (or the game
		// folder) would let any program of theirs swap the file between the check and the install.
		private static int RunPackage(string logPath)
		{
			string packageDirectory = CreatePrivateFolder();
			try
			{
				string packagePath = Path.Combine(packageDirectory, PACKAGE_FILE_NAME);
				File.WriteAllBytes(packagePath, ReadVerifiedPackage());
				if (!HasPinnedHash(File.ReadAllBytes(packagePath)))
					throw new InvalidDataException("The UsbDk installer written to disk does not match the pinned release.");

				string msiexec = Path.Combine(Environment.SystemDirectory, "msiexec.exe");
				var info = new ProcessStartInfo(msiexec, "/i \"" + packagePath + "\" /qn /norestart /l*v \"" + logPath + "\"")
				{
					UseShellExecute = false,
					CreateNoWindow = true,
					WorkingDirectory = packageDirectory,
				};
				using (var process = Process.Start(info))
				{
					process.WaitForExit();
					return process.ExitCode;
				}
			}
			finally
			{
				try { Directory.Delete(packageDirectory, recursive: true); } catch { }
			}
		}

		// %windir%\Temp\RSMods-UsbDk-<guid>, created with its own access list (Administrators and SYSTEM only, nothing
		// inherited). Standard users cannot list %windir%\Temp or rename what others put in it, so the name cannot be
		// claimed or swapped first.
		private static string CreatePrivateFolder()
		{
			string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "Temp", "RSMods-UsbDk-" + Guid.NewGuid().ToString("N"));
			if (Directory.Exists(path)) throw new IOException("The temporary UsbDk folder already exists.");
			var security = new DirectorySecurity();
			// Owned by Administrators rather than the player's account, which could otherwise rewrite the access list.
			security.SetOwner(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
			security.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
			foreach (var sid in new[] { WellKnownSidType.BuiltinAdministratorsSid, WellKnownSidType.LocalSystemSid })
				security.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(sid, null), FileSystemRights.FullControl,
					InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
			Directory.CreateDirectory(path, security);
			return path;
		}

		private static byte[] ReadVerifiedPackage()
		{
			using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(PACKAGE_RESOURCE_NAME))
			{
				if (stream == null) throw new FileNotFoundException("The bundled UsbDk installer is missing from RSMods.exe.");
				using (var buffer = new MemoryStream())
				{
					stream.CopyTo(buffer);
					byte[] package = buffer.ToArray();
					if (!HasPinnedHash(package)) throw new InvalidDataException("The bundled UsbDk installer hash does not match the pinned release.");
					return package;
				}
			}
		}

		private static bool HasPinnedHash(byte[] package)
		{
			using (var sha = SHA256.Create())
				return string.Equals(BitConverter.ToString(sha.ComputeHash(package)).Replace("-", ""), PACKAGE_SHA256, StringComparison.OrdinalIgnoreCase);
		}

		private static bool IsInstalled()
		{
			using (var services = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\UsbDk"))
			return services != null;
		}
	}
}
