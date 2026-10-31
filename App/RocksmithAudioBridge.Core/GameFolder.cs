#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace RocksmithAudioBridge.Core
{
	/// <summary>
	/// Finds the Rocksmith 2014 folder. Same search as the 4.x GUI (GUI/Util/GenUtil.GetRSDirectory) without its
	/// message boxes: the saved choice, the folder above the running exe (the installed layout is &lt;game&gt;\RSMods),
	/// Steam's own library, the install registry keys, then every Steam library in libraryfolders.vdf. A folder counts
	/// when it holds cache.psarc. The app asks the player only when this returns null.
	/// </summary>
	public static class GameFolder
	{
		private static readonly (string Key, string Value)[] InstallRegKeys =
		{
			(@"HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\Ubisoft\Rocksmith2014", "installdir"),
			(@"HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 221680", "InstallLocation"),
			(@"HKEY_LOCAL_MACHINE\SOFTWARE\Ubisoft\Rocksmith2014", "InstallLocation"),
			(@"HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 221680", "InstallLocation"),
		};

		public static bool IsRocksmithFolder(string? folder) =>
			!string.IsNullOrWhiteSpace(folder) && Directory.Exists(folder) && File.Exists(Path.Combine(folder, "cache.psarc"));

		/// <summary>The first Rocksmith folder found, or null.</summary>
		public static string? Detect(string? saved = null)
		{
			foreach (string candidate in Candidates(saved))
				if (IsRocksmithFolder(candidate)) return Path.GetFullPath(candidate);
			return null;
		}

		private static IEnumerable<string> Candidates(string? saved)
		{
			if (!string.IsNullOrWhiteSpace(saved)) yield return saved;
			string? parent = Directory.GetParent(AppContext.BaseDirectory.TrimEnd('\\'))?.FullName;
			if (parent != null) yield return parent;
			string steam = SteamDirectory();
			if (steam.Length > 0) yield return Path.Combine(steam, "steamapps", "common", "Rocksmith2014");
			foreach (var (key, value) in InstallRegKeys)
			{
				string path = ReadRegistryString(key, value);
				if (path.Length > 0) yield return path;
			}
			if (steam.Length > 0)
				foreach (string library in SteamLibraries(steam))
					yield return Path.Combine(library, "steamapps", "common", "Rocksmith2014");
		}

		public static string SteamDirectory() => ReadRegistryString(@"HKEY_CURRENT_USER\SOFTWARE\Valve\Steam", "SteamPath").Replace('/', '\\');

		// libraryfolders.vdf: the old format lists libraries as "1" "D:\\Steam", the current one as "path" "D:\\Steam".
		private static readonly Regex LibraryLine = new("^\\s*\"(?:[1-9][0-9]*|path)\"\\s+\"(.+)\"\\s*$", RegexOptions.Compiled);

		internal static List<string> SteamLibraries(string steamDirectory)
		{
			var libraries = new List<string>();
			string vdf = Path.Combine(steamDirectory, "steamapps", "libraryfolders.vdf");
			if (!File.Exists(vdf)) return libraries;
			foreach (string line in File.ReadAllLines(vdf))
			{
				var match = LibraryLine.Match(line);
				if (match.Success) libraries.Add(match.Groups[1].Value.Replace("\\\\", "\\"));
			}
			return libraries;
		}

		private static string ReadRegistryString(string key, string value)
		{
			try { return Registry.GetValue(key, value, null) as string ?? ""; }
			catch { return ""; }
		}
	}
}
