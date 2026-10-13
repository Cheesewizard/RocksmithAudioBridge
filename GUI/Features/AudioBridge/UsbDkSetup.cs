using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using Microsoft.Win32;

namespace RSMods.Audio
{
	internal static class UsbDkSetup
	{
		private const string PACKAGE_FILE_NAME = "UsbDk_1.0.22_x64.msi";
		private const string LICENSE_FILE_NAME = "LICENSE.txt";
		private const string PACKAGE_SHA256 = "91F6F695E1E13C656024E6D3B55620BF08D8835EF05EE0496935BA6BB62466A5";
		private const string PACKAGE_RESOURCE_NAME = "RSMods.UsbDk_1.0.22_x64.msi";
		private const int MSI_REBOOT_REQUIRED = 3010;

		public static void Install(string gameDirectory)
		{
			if (string.IsNullOrWhiteSpace(gameDirectory)) throw new ArgumentException("The Rocksmith directory is required.", nameof(gameDirectory));

			string packageDirectory = Path.Combine(Path.GetFullPath(gameDirectory), "RSMods", "UsbDk");
			Directory.CreateDirectory(packageDirectory);
			string packagePath = Path.Combine(packageDirectory, PACKAGE_FILE_NAME);
			string logPath = Path.Combine(packageDirectory, "UsbDkInstall.log");

			byte[] package = ReadVerifiedPackage();
			File.WriteAllBytes(packagePath, package);
			string licensePath = Path.Combine(packageDirectory, LICENSE_FILE_NAME);
			if (!File.Exists(licensePath))
			{
				using (var license = Assembly.GetExecutingAssembly().GetManifestResourceStream("RSMods.UsbDk_LICENSE.txt"))
				{
					if (license == null) throw new FileNotFoundException("The bundled UsbDk licence is missing.");
					using (var output = File.Create(licensePath)) license.CopyTo(output);
				}
			}

			if (IsInstalled())
			{
				File.WriteAllText(logPath, "UsbDk is already installed system-wide; the shared dependency was left unchanged." + Environment.NewLine);
				return;
			}

			var info = new ProcessStartInfo("msiexec.exe", "/i \"" + packagePath + "\" /qn /norestart /l*v \"" + logPath + "\"")
			{
				UseShellExecute = false,
				CreateNoWindow = true,
				WorkingDirectory = packageDirectory,
			};
			using (var process = Process.Start(info))
			{
				process.WaitForExit();
				if (process.ExitCode == MSI_REBOOT_REQUIRED)
					throw new InvalidOperationException("UsbDk installation requires a Windows restart. Restart Windows, then run Install again.");
				if (process.ExitCode != 0)
					throw new InvalidOperationException("UsbDk installation failed with Windows Installer exit code " + process.ExitCode + ". See " + logPath + ".");
			}

			if (!IsInstalled()) throw new InvalidOperationException("UsbDk installation completed without registering its system service. See " + logPath + ".");
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
					using (var sha = SHA256.Create())
					{
						string actual = BitConverter.ToString(sha.ComputeHash(package)).Replace("-", "");
						if (!string.Equals(actual, PACKAGE_SHA256, StringComparison.OrdinalIgnoreCase))
							throw new InvalidDataException("The bundled UsbDk installer hash does not match the pinned release.");
					}
					return package;
				}
			}
		}

		private static bool IsInstalled()
		{
			using (var services = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\UsbDk"))
			return services != null;
		}
	}
}
