using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

namespace RSMods
{
	internal static class RuntimeBootstrap
	{
		private static readonly Dictionary<string, string> assemblies = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
		public static string DirectoryPath { get; private set; }

		public static void Initialize()
		{
			var assembly = Assembly.GetExecutingAssembly();
			using (var payload = assembly.GetManifestResourceStream("RSMods.BundledRuntime.zip"))
			{
				if (payload == null) throw new InvalidOperationException("The bundled runtime is missing.");
				var identity = GetHash(payload);
				payload.Position = 0;
				DirectoryPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RSModsPlus", "Runtime", identity);
				Directory.CreateDirectory(DirectoryPath);
				using (var zip = new ZipArchive(payload, ZipArchiveMode.Read))
				{
					foreach (var entry in zip.Entries)
					{
						var path = Path.GetFullPath(Path.Combine(DirectoryPath, entry.FullName));
						if (!path.StartsWith(DirectoryPath + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Invalid bundled runtime path.");
						Directory.CreateDirectory(Path.GetDirectoryName(path));
						using (var source = entry.Open())
						{
							var expected = GetHash(source);
							if (!File.Exists(path))
							{
								var temporary = path + "." + Guid.NewGuid().ToString("N");
								try
								{
									using (var input = entry.Open())
									using (var output = File.Open(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None)) input.CopyTo(output);
									try { File.Move(temporary, path); }
									catch (IOException) when (File.Exists(path)) { }
								}
								finally { if (File.Exists(temporary)) File.Delete(temporary); }
							}
							string actual;
							using (var cached = File.OpenRead(path)) actual = GetHash(cached);
							if (actual != expected)
							{
								// A damaged cached file (partly removed by antivirus, a disk error) would stop RSMods.exe from
								// starting on every launch. Replace it once.
								File.Delete(path);
								using (var input = entry.Open())
								using (var output = File.Open(path, FileMode.CreateNew, FileAccess.Write, FileShare.None)) input.CopyTo(output);
								using (var repaired = File.OpenRead(path))
									if (GetHash(repaired) != expected) throw new InvalidDataException("Bundled runtime integrity check failed: " + entry.FullName);
							}
						}
						if (Path.GetExtension(path).Equals(".dll", StringComparison.OrdinalIgnoreCase)) assemblies[Path.GetFileNameWithoutExtension(path)] = path;
					}
				}
			}
			PruneOtherRuntimes(DirectoryPath);
			AppDomain.CurrentDomain.AssemblyResolve += ResolveAssembly;
			var gameDirectory = AppDomain.CurrentDomain.BaseDirectory;
			var libraryPath = Path.Combine(gameDirectory, "RocksmithAudioBridge.dll");
			if (!File.Exists(libraryPath)) throw new FileNotFoundException("RocksmithAudioBridge.dll must be installed in the main game folder.", libraryPath);
			assemblies["RocksmithAudioBridge"] = libraryPath;
			var library = Assembly.LoadFrom(libraryPath);
			if (!string.Equals(library.Location, libraryPath, StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("RocksmithAudioBridge.dll was loaded from an unexpected location.");
			foreach (var name in new[] { "soxr.dll", "onnxruntime.dll", "rswindowcapture.dll" })
			{
				if (LoadLibraryEx(Path.Combine(DirectoryPath, name), IntPtr.Zero, 0x1100) == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot load bundled " + name);
			}
		}

		// Every build extracts to its own Runtime\<hash> folder (about 34 MB). Remove the others; a folder still in
		// use (an older RSMods.exe running) fails to delete and is left for next time.
		private static void PruneOtherRuntimes(string current)
		{
			try
			{
				var parent = Path.GetDirectoryName(current);
				foreach (var folder in Directory.GetDirectories(parent))
				{
					if (string.Equals(Path.GetFullPath(folder), Path.GetFullPath(current), StringComparison.OrdinalIgnoreCase)) continue;
					try { Directory.Delete(folder, true); } catch { }
				}
			}
			catch { }
		}

		private static Assembly ResolveAssembly(object sender, ResolveEventArgs arguments)
		{
			var name = new AssemblyName(arguments.Name).Name;
			return assemblies.TryGetValue(name, out var path) ? Assembly.LoadFrom(path) : null;
		}

		private static string GetHash(Stream stream)
		{
			using (var algorithm = SHA256.Create()) return BitConverter.ToString(algorithm.ComputeHash(stream)).Replace("-", "");
		}

		[DllImport("kernel32.dll", EntryPoint = "LoadLibraryExW", CharSet = CharSet.Unicode, SetLastError = true)]
		private static extern IntPtr LoadLibraryEx(string file, IntPtr reserved, uint flags);
	}
}
