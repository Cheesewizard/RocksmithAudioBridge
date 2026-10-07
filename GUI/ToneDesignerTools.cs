using Microsoft.Win32;
using Newtonsoft.Json;
using Rocksmith2014PsarcLib.Psarc;
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace RSMods
{
	// Hidden helpers for the in-game Tone designer page (DLL/Audio/SongLibrary.cpp and ToneBackup.cpp start them; no
	// window, exit code 0 = done):
	//   --song-index <Rocksmith folder> <out.tsv>   every song in songs.psarc and dlc\*_p.psarc, one line each:
	//                                                key, title, artist, album, year, arrangements, file (tab separated,
	//                                                UTF-8). Each .psarc is read once: <out.tsv>.cache.v2.json keeps what
	//                                                was found per file (by size and time), so later runs only read new
	//                                                or changed files.
	//   --tone-backup <out.zip>                     the Tone designer's data (tones, song tones, plugin settings) and its
	//                                                settings (HKCU\Software\RSMods\VstHost) in one zip
	//   --tone-backup-auto <folder>                 one "Auto yyyy-MM-dd.zip" a day in that folder, the newest 7 kept
	//   --tone-restore <in.zip> <safety.zip>        backs the current data up to safety.zip, then puts the zip's back
	// The VST host must not be running during a restore (it autosaves its chain); the page closes it first.
	internal static class ToneDesignerTools
	{
		private const string SettingsKey = @"Software\RSMods\VstHost";
		// Values that describe this run, not the player's setup: never backed up or restored.
		private static readonly string[] TransientValues = { "LastCrash", "LastCrashAt", "LastCrashModule", "LastCrashLoading", "LastCrashGraphics", "LastCrashWindows", "PreloadingNow", "PreloadFailed" };

		public static bool IsCommand(string argument)
		{
			return argument == "--song-index" || argument == "--tone-backup" || argument == "--tone-backup-auto" || argument == "--tone-restore";
		}

		public static bool TryRun(string[] arguments, out int exitCode)
		{
			exitCode = 0;
			if (arguments.Length == 0 || !IsCommand(arguments[0])) return false;
			try
			{
				switch (arguments[0])
				{
					case "--song-index" when arguments.Length == 3: SongIndex(arguments[1], arguments[2]); break;
					case "--tone-backup" when arguments.Length == 2: Backup(arguments[1]); break;
					case "--tone-backup-auto" when arguments.Length == 2: AutoBackup(arguments[1]); break;
					case "--tone-restore" when arguments.Length == 3: Restore(arguments[1], arguments[2]); break;
					default: exitCode = 2; break;
				}
			}
			catch (Exception exception)
			{
				Log(exception.ToString());
				exitCode = 1;
			}
			return true;
		}

		private static void Log(string text)
		{
			try
			{
				var folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Rocksmith Audio Bridge", "Logs");
				Directory.CreateDirectory(folder);
				File.AppendAllText(Path.Combine(folder, "tone-designer-tools.log"), DateTime.Now.ToString("O") + " " + text + Environment.NewLine);
			}
			catch (Exception) { }
		}

		// ---- Song index ------------------------------------------------------------------------------------

		private sealed class Song
		{
			public string Key, Title, Artist, Album, Arrangements, File;
			public int Year;
		}

		private sealed class CachedFile
		{
			public long Size;
			public long Time;
			public List<Song> Songs = new List<Song>();
		}

		private static void SongIndex(string rocksmithFolder, string outPath)
		{
			var files = new List<string>();
			var dlc = Path.Combine(rocksmithFolder, "dlc");
			if (Directory.Exists(dlc)) files.AddRange(Directory.GetFiles(dlc, "*_p.psarc", SearchOption.AllDirectories));
			var disc = Path.Combine(rocksmithFolder, "songs.psarc");
			if (File.Exists(disc)) files.Add(disc);

			var cachePath = outPath + ".cache.v2.json";   // v2: manifests read as plain JSON
			Dictionary<string, CachedFile> cache = null;
			try { if (File.Exists(cachePath)) cache = JsonConvert.DeserializeObject<Dictionary<string, CachedFile>>(File.ReadAllText(cachePath)); }
			catch (Exception) { cache = null; }
			cache = cache ?? new Dictionary<string, CachedFile>(StringComparer.OrdinalIgnoreCase);
			cache = new Dictionary<string, CachedFile>(cache, StringComparer.OrdinalIgnoreCase);

			var fresh = new ConcurrentDictionary<string, CachedFile>(StringComparer.OrdinalIgnoreCase);
			int read = 0;
			Parallel.ForEach(files, new ParallelOptions { MaxDegreeOfParallelism = Math.Max(1, Environment.ProcessorCount / 2) }, file =>
			{
				var info = new FileInfo(file);
				if (cache.TryGetValue(file, out CachedFile known) && known.Size == info.Length && known.Time == info.LastWriteTimeUtc.Ticks)
				{
					fresh[file] = known;
					return;
				}
				var entry = new CachedFile { Size = info.Length, Time = info.LastWriteTimeUtc.Ticks };
				try
				{
					using (var psarc = new PsarcFile(file))
					{
						// The arrangement manifests (manifests/.../*.json), read as plain JSON: the library's typed reader
						// gives up on a whole file when one field is null (rs1compatibilitydlc_p.psarc has those).
						var byKey = new Dictionary<string, Song>(StringComparer.OrdinalIgnoreCase);
						foreach (var toc in psarc.TOC.Entries)
						{
							if (toc.Path == null || !toc.Path.StartsWith("manifests/", StringComparison.OrdinalIgnoreCase)
								|| !toc.Path.EndsWith(".json", StringComparison.OrdinalIgnoreCase)) continue;
							Newtonsoft.Json.Linq.JObject manifest;
							using (var raw = new MemoryStream())
							{
								psarc.InflateEntry(toc, raw);
								manifest = Newtonsoft.Json.Linq.JObject.Parse(Encoding.UTF8.GetString(raw.ToArray()));
							}
							if (!(manifest["Entries"] is Newtonsoft.Json.Linq.JObject entries)) continue;
							foreach (var item in entries.Properties())
							{
								var a = item.Value["Attributes"];
								var key = (string)a?["SongKey"];
								var title = (string)a?["SongName"];
								if (string.IsNullOrWhiteSpace(key) || string.IsNullOrWhiteSpace(title)) continue;
								if (!byKey.TryGetValue(key, out Song song))
								{
									int.TryParse((string)a["SongYear"] ?? "", out int year);
									song = new Song { Key = key, Title = title, Artist = (string)a["ArtistName"] ?? "", Album = (string)a["AlbumName"] ?? "", Year = year, Arrangements = "", File = Path.GetFileName(file) };
									byKey[key] = song;
								}
								var name = (string)a["ArrangementName"] ?? "";
								if (name.Length > 0 && name != "Vocals" && name != "JVocals" && !song.Arrangements.Split(',').Contains(name))
									song.Arrangements = song.Arrangements.Length == 0 ? name : song.Arrangements + "," + name;
							}
						}
						entry.Songs = byKey.Values.ToList();
					}
				}
				catch (Exception exception)
				{
					Log("song index: " + file + ": " + exception.Message);
					entry.Time = 0;   // not kept: read again next time
				}
				fresh[file] = entry;
				System.Threading.Interlocked.Increment(ref read);
			});

			// One line per song key (a key in several files: the first file's, in name order).
			var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
			var text = new StringBuilder();
			foreach (var file in fresh.Keys.OrderBy(f => f, StringComparer.OrdinalIgnoreCase))
				foreach (var song in fresh[file].Songs)
				{
					if (!seen.Add(song.Key)) continue;
					text.Append(Clean(song.Key)).Append('\t').Append(Clean(song.Title)).Append('\t').Append(Clean(song.Artist)).Append('\t')
						.Append(Clean(song.Album)).Append('\t').Append(song.Year.ToString(CultureInfo.InvariantCulture)).Append('\t')
						.Append(Clean(song.Arrangements)).Append('\t').Append(Clean(song.File)).Append('\n');
				}

			Directory.CreateDirectory(Path.GetDirectoryName(outPath));
			WriteAtomically(outPath, Encoding.UTF8.GetBytes(text.ToString()));
			WriteAtomically(cachePath, Encoding.UTF8.GetBytes(JsonConvert.SerializeObject(new Dictionary<string, CachedFile>(fresh, StringComparer.OrdinalIgnoreCase))));
			Log("song index: " + seen.Count + " songs from " + files.Count + " files (" + read + " read, the rest from the cache)");
		}

		private static string Clean(string text) => (text ?? "").Replace('\t', ' ').Replace('\r', ' ').Replace('\n', ' ').Trim();

		private static void WriteAtomically(string path, byte[] bytes)
		{
			var temp = path + ".tmp";
			File.WriteAllBytes(temp, bytes);
			if (File.Exists(path)) File.Replace(temp, path, null);
			else File.Move(temp, path);
		}

		// ---- Backup and restore ----------------------------------------------------------------------------

		private static string DataFolder()
		{
			return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "RocksmithAudioBridge", "VstHost");
		}

		// Files of the player's setup: tones (with folders), song tones, the current chain, each plugin's settings.
		// Logs, crash dumps and half-written files are left out.
		private static IEnumerable<string> DataFiles(string folder)
		{
			if (!Directory.Exists(folder)) yield break;
			foreach (var file in Directory.GetFiles(folder, "*", SearchOption.AllDirectories))
			{
				var name = Path.GetFileName(file);
				if (name.EndsWith(".dmp", StringComparison.OrdinalIgnoreCase) || name.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)
					|| name.StartsWith("vst-host-log", StringComparison.OrdinalIgnoreCase)) continue;
				yield return file;
			}
		}

		private sealed class SettingValue
		{
			public string Kind;   // "string" or "dword"
			public string Text;
			public long Number;
		}

		private static Dictionary<string, SettingValue> ReadSettings()
		{
			var settings = new Dictionary<string, SettingValue>(StringComparer.OrdinalIgnoreCase);
			using (var key = Registry.CurrentUser.OpenSubKey(SettingsKey))
			{
				if (key == null) return settings;
				foreach (var name in key.GetValueNames())
				{
					if (TransientValues.Contains(name, StringComparer.OrdinalIgnoreCase)) continue;
					switch (key.GetValueKind(name))
					{
						case RegistryValueKind.String:
						case RegistryValueKind.ExpandString:
							settings[name] = new SettingValue { Kind = "string", Text = key.GetValue(name) as string ?? "" };
							break;
						case RegistryValueKind.DWord:
							settings[name] = new SettingValue { Kind = "dword", Number = Convert.ToInt64(key.GetValue(name)) };
							break;
					}
				}
			}
			return settings;
		}

		private static void Backup(string zipPath)
		{
			var folder = DataFolder();
			var files = DataFiles(folder).ToList();
			Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(zipPath)));
			var temp = zipPath + ".tmp";
			using (var stream = new FileStream(temp, FileMode.Create, FileAccess.Write))
			using (var zip = new ZipArchive(stream, ZipArchiveMode.Create))
			{
				foreach (var file in files)
				{
					var relative = file.Substring(folder.Length).TrimStart('\\', '/').Replace('\\', '/');
					var entry = zip.CreateEntry("files/" + relative, CompressionLevel.Optimal);
					entry.LastWriteTime = File.GetLastWriteTime(file);
					// Shared read: the VST host may be writing its autosave meanwhile.
					using (var input = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
					using (var output = entry.Open()) input.CopyTo(output);
				}
				var settings = zip.CreateEntry("settings.json");
				using (var writer = new StreamWriter(settings.Open(), new UTF8Encoding(false)))
					writer.Write(JsonConvert.SerializeObject(ReadSettings(), Formatting.Indented));
				var about = zip.CreateEntry("about.txt");
				using (var writer = new StreamWriter(about.Open(), new UTF8Encoding(false)))
				{
					writer.WriteLine("Rocksmith Audio Bridge, Tone designer backup");
					writer.WriteLine("Made " + DateTime.Now.ToString("yyyy-MM-dd HH:mm"));
					writer.WriteLine(files.Count(f => f.EndsWith(".rabtone", StringComparison.OrdinalIgnoreCase)) + " tone file(s), " + files.Count + " file(s) in all");
					writer.WriteLine("Restore it from the Tone designer page: File, Backups.");
				}
			}
			if (File.Exists(zipPath)) File.Delete(zipPath);
			File.Move(temp, zipPath);
			Log("backup: " + zipPath + " (" + files.Count + " files)");
		}

		private static void AutoBackup(string backupFolder)
		{
			Directory.CreateDirectory(backupFolder);
			if (!DataFiles(DataFolder()).Any()) return;   // nothing to keep yet
			var today = Path.Combine(backupFolder, "Auto " + DateTime.Now.ToString("yyyy-MM-dd") + ".zip");
			if (!File.Exists(today)) Backup(today);
			foreach (var old in Directory.GetFiles(backupFolder, "Auto *.zip").OrderByDescending(f => f, StringComparer.OrdinalIgnoreCase).Skip(7))
				File.Delete(old);
		}

		private static void Restore(string zipPath, string safetyZip)
		{
			using (var stream = new FileStream(zipPath, FileMode.Open, FileAccess.Read, FileShare.Read))
			using (var zip = new ZipArchive(stream, ZipArchiveMode.Read))
			{
				var settingsEntry = zip.GetEntry("settings.json");
				if (settingsEntry == null && !zip.Entries.Any(e => e.FullName.StartsWith("files/", StringComparison.Ordinal)))
					throw new InvalidDataException("Not a Tone designer backup: " + zipPath);

				Backup(safetyZip);   // what is there now, in case this was the wrong backup

				// Replace the setup: the backup's tones, song tones, chain and plugin settings, nothing left over.
				var folder = DataFolder();
				foreach (var file in DataFiles(folder).ToList()) File.Delete(file);
				foreach (var directory in Directory.Exists(folder) ? Directory.GetDirectories(folder, "*", SearchOption.AllDirectories).OrderByDescending(d => d.Length) : Enumerable.Empty<string>())
					if (!Directory.EnumerateFileSystemEntries(directory).Any()) Directory.Delete(directory);
				var root = Path.GetFullPath(folder) + Path.DirectorySeparatorChar;
				foreach (var entry in zip.Entries)
				{
					if (!entry.FullName.StartsWith("files/", StringComparison.Ordinal) || entry.FullName.EndsWith("/", StringComparison.Ordinal)) continue;
					var target = Path.GetFullPath(Path.Combine(folder, entry.FullName.Substring("files/".Length)));
					if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue;   // never outside the data folder
					Directory.CreateDirectory(Path.GetDirectoryName(target));
					using (var input = entry.Open())
					using (var output = new FileStream(target, FileMode.Create, FileAccess.Write)) input.CopyTo(output);
					File.SetLastWriteTime(target, entry.LastWriteTime.LocalDateTime);
				}

				if (settingsEntry != null)
				{
					Dictionary<string, SettingValue> settings;
					using (var reader = new StreamReader(settingsEntry.Open(), Encoding.UTF8))
						settings = JsonConvert.DeserializeObject<Dictionary<string, SettingValue>>(reader.ReadToEnd()) ?? new Dictionary<string, SettingValue>();
					using (var key = Registry.CurrentUser.CreateSubKey(SettingsKey))
					{
						foreach (var name in key.GetValueNames())
							if (!TransientValues.Contains(name, StringComparer.OrdinalIgnoreCase)) key.DeleteValue(name, false);
						foreach (var pair in settings)
						{
							if (TransientValues.Contains(pair.Key, StringComparer.OrdinalIgnoreCase) || pair.Value == null) continue;
							if (pair.Value.Kind == "dword") key.SetValue(pair.Key, unchecked((int)pair.Value.Number), RegistryValueKind.DWord);
							else key.SetValue(pair.Key, pair.Value.Text ?? "", RegistryValueKind.String);
						}
					}
				}
			}
			Log("restore: " + zipPath + " (the setup before it is in " + safetyZip + ")");
		}
	}
}
