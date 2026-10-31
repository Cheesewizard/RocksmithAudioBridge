#nullable enable
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using Microsoft.Win32;
using RS2014_Mod_Installer.Core;
using RS2014_Mod_Installer.Payload;

namespace RocksmithAudioBridge.Setup
{
	/// <summary>
	/// Every side effect the installer has goes through here, so one plan runs three ways: for real
	/// (<see cref="SystemInstallHost"/>), as a dry run that only logs (<see cref="DryRunInstallHost"/>), and in tests
	/// (a <see cref="SystemInstallHost"/> subclass that does the file work in a temp folder and records the rest).
	/// Reads (file exists, registry reads) are not here: they are always real and never change anything.
	/// </summary>
	public interface IInstallHost
	{
		bool IsDryRun { get; }
		Action<string> Log { get; }

		void CreateDirectory(string path);
		void CopyFile(string source, string destination);
		void WriteAllText(string path, string text);
		void DeleteFile(string path);
		/// <summary>Deletes a folder; with onlyIfEmpty it is left alone when anything is still in it.</summary>
		void DeleteDirectory(string path, bool onlyIfEmpty);

		/// <summary>Upstream's own install of the native mod and its configurator (all or nothing).</summary>
		void InstallRsMods(string gameFolder, ExistingNativeMod existing, bool replaceNativeMod, string dllPath, string pdbPath,
			string payloadZipPath, string payloadManifestPath);

		/// <summary>Runs a program and returns its exit code. elevated: through the UAC prompt; a declined prompt throws
		/// OperationCanceledException. A timeout kills it and returns -1.</summary>
		int RunProcess(string exe, string arguments, string workingDirectory, TimeSpan timeout, bool elevated);

		void SetUserRegistryDword(string key, string name, int? value);
		void DeleteUserRegistryTree(string key);
		/// <summary>Removes the ASIO proxy's machine registration (both views) and the per-user override. One UAC prompt.</summary>
		void UnregisterDriver(string proxyDllPath);

		void CreateShortcut(string shortcutPath, string target, string workingDirectory, string description);
		void StartProgram(string path);
	}

	/// <summary>The real thing.</summary>
	public class SystemInstallHost : IInstallHost
	{
		public SystemInstallHost(Action<string> log) => Log = log;

		public virtual bool IsDryRun => false;
		public Action<string> Log { get; }

		public virtual void CreateDirectory(string path) => Directory.CreateDirectory(path);

		public virtual void CopyFile(string source, string destination)
		{
			Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
			// Written beside the target and swapped in, so a copy that fails partway never leaves a truncated file.
			string temporary = destination + ".new";
			File.Copy(source, temporary, overwrite: true);
			File.Move(temporary, destination, overwrite: true);
		}

		public virtual void WriteAllText(string path, string text)
		{
			Directory.CreateDirectory(Path.GetDirectoryName(path)!);
			File.WriteAllText(path, text);
		}

		public virtual void DeleteFile(string path)
		{
			if (!File.Exists(path)) return;
			var attributes = File.GetAttributes(path);
			if ((attributes & FileAttributes.ReadOnly) != 0) File.SetAttributes(path, attributes & ~FileAttributes.ReadOnly);
			File.Delete(path);
		}

		public virtual void DeleteDirectory(string path, bool onlyIfEmpty)
		{
			if (!Directory.Exists(path)) return;
			if (onlyIfEmpty && Directory.EnumerateFileSystemEntries(path).Any()) return;
			Directory.Delete(path, recursive: !onlyIfEmpty);
		}

		public virtual void InstallRsMods(string gameFolder, ExistingNativeMod existing, bool replaceNativeMod, string dllPath, string pdbPath,
			string payloadZipPath, string payloadManifestPath)
		{
			PayloadManifest manifest = PayloadManifest.Parse(File.ReadAllText(payloadManifestPath));
			using Stream zip = File.OpenRead(payloadZipPath);
			RsModsInstallation.Install(gameFolder, existing, replaceNativeMod,
				replaceNativeMod ? File.ReadAllBytes(dllPath) : null, replaceNativeMod ? File.ReadAllBytes(pdbPath) : null,
				zip, manifest, message => Log("  " + message));
		}

		public virtual int RunProcess(string exe, string arguments, string workingDirectory, TimeSpan timeout, bool elevated)
		{
			var info = new ProcessStartInfo(exe, arguments)
			{
				WorkingDirectory = workingDirectory,
				UseShellExecute = elevated,
				CreateNoWindow = !elevated,
				WindowStyle = ProcessWindowStyle.Hidden,
			};
			if (elevated) info.Verb = "runas";
			try
			{
				using var process = Process.Start(info) ?? throw new InvalidOperationException("Could not start " + exe);
				if (!process.WaitForExit((int)timeout.TotalMilliseconds))
				{
					try { process.Kill(); } catch { }
					return -1;
				}
				return process.ExitCode;
			}
			catch (Win32Exception error) when (error.NativeErrorCode == 1223) // ERROR_CANCELLED: the UAC prompt was declined
			{
				throw new OperationCanceledException("The Windows admin prompt was declined.");
			}
		}

		public virtual void SetUserRegistryDword(string key, string name, int? value)
		{
			if (value.HasValue)
			{
				using var created = Registry.CurrentUser.CreateSubKey(key);
				created.SetValue(name, value.Value, RegistryValueKind.DWord);
			}
			else
			{
				using var opened = Registry.CurrentUser.OpenSubKey(key, writable: true);
				opened?.DeleteValue(name, throwOnMissingValue: false);
			}
		}

		public virtual void DeleteUserRegistryTree(string key) => Registry.CurrentUser.DeleteSubKeyTree(key, throwOnMissingSubKey: false);

		public virtual void UnregisterDriver(string proxyDllPath) => RocksmithAudioBridge.Core.AsioProxySetup.Unregister(proxyDllPath);

		public virtual void CreateShortcut(string shortcutPath, string target, string workingDirectory, string description)
		{
			Directory.CreateDirectory(Path.GetDirectoryName(shortcutPath)!);
			ShellLink.Create(shortcutPath, target, workingDirectory, description);
		}

		public virtual void StartProgram(string path) =>
			Process.Start(new ProcessStartInfo(path) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(path) });
	}

	/// <summary>Logs what would happen and touches nothing. Processes "succeed" with exit code 0 so the rest of the
	/// plan is still shown.</summary>
	public sealed class DryRunInstallHost : IInstallHost
	{
		public DryRunInstallHost(Action<string> log) => Log = log;

		public bool IsDryRun => true;
		public Action<string> Log { get; }

		private void Would(string what) => Log("    [dry run] " + what);

		public void CreateDirectory(string path) { if (!Directory.Exists(path)) Would("create folder " + path); }
		public void CopyFile(string source, string destination) =>
			Would((File.Exists(destination) ? "replace " : "write ") + destination + (File.Exists(source) ? "" : "   (MISSING FROM PAYLOAD: " + source + ")"));
		public void WriteAllText(string path, string text) => Would((File.Exists(path) ? "rewrite " : "write ") + path + " (" + text.Length + " chars)");
		public void DeleteFile(string path) { if (File.Exists(path)) Would("delete " + path); }
		public void DeleteDirectory(string path, bool onlyIfEmpty) { if (Directory.Exists(path)) Would((onlyIfEmpty ? "remove folder if empty " : "delete folder tree ") + path); }

		public void InstallRsMods(string gameFolder, ExistingNativeMod existing, bool replaceNativeMod, string dllPath, string pdbPath,
			string payloadZipPath, string payloadManifestPath)
		{
			Would("RsModsInstallation.Install(existing: " + existing.Kind + (existing.Description.Length > 0 ? " \"" + existing.Description + "\"" : "")
				+ ", replaceNativeMod: " + replaceNativeMod + ")");
			Would((replaceNativeMod ? "replace " : "keep ") + Path.Combine(gameFolder, ProductLayout.NativeModFile) + " and .pdb"
				+ (File.Exists(dllPath) ? "" : "   (MISSING FROM PAYLOAD: " + dllPath + ")"));
			if (File.Exists(payloadManifestPath))
			{
				try
				{
					var manifest = PayloadManifest.Parse(File.ReadAllText(payloadManifestPath));
					Would("deploy upstream configurator " + manifest.Application + " " + manifest.ApplicationVersion + " (" + manifest.Files.Count
						+ " files) into " + ConfiguratorPaths.RsModsFolder(gameFolder) + ", replacing RSMods.exe");
				}
				catch (Exception error) { Would("configurator payload manifest unreadable: " + error.Message); }
			}
			else Would("deploy upstream configurator into " + ConfiguratorPaths.RsModsFolder(gameFolder) + "   (MISSING FROM PAYLOAD: " + payloadZipPath + ")");
			if (existing.Kind == ExistingNativeModKind.CustomRsMods && replaceNativeMod)
				Would("set aside " + Path.Combine(gameFolder, ProductLayout.LooseManifest) + " as .bak (upstream: a custom build's manifest)");
			if (replaceNativeMod) Would("remove a legacy D3DX9_42.dll injector (>= 300 KB) if present");
		}

		public int RunProcess(string exe, string arguments, string workingDirectory, TimeSpan timeout, bool elevated)
		{
			Would("run " + (elevated ? "(elevated) " : "") + "\"" + exe + "\" " + arguments);
			return 0;
		}

		public void SetUserRegistryDword(string key, string name, int? value) =>
			Would(value.HasValue ? "set HKCU\\" + key + " " + name + " = " + value : "delete HKCU\\" + key + " " + name);
		public void DeleteUserRegistryTree(string key) => Would("delete HKCU\\" + key);
		public void UnregisterDriver(string proxyDllPath) =>
			Would("unregister the ASIO proxy (regsvr32 /u " + proxyDllPath + " + its 64-bit guest, else delete HKLM\\SOFTWARE\\ASIO\\"
				+ RocksmithAudioBridge.Core.AsioProxySetup.ProxyName + " and its CLSID in both views; clear the HKCU CLSID override). One UAC prompt.");
		public void CreateShortcut(string shortcutPath, string target, string workingDirectory, string description) =>
			Would("create shortcut " + shortcutPath + " -> " + target);
		public void StartProgram(string path) => Would("start " + path);
	}
}
