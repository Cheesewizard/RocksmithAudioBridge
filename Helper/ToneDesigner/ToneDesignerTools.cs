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

namespace RocksmithAudioBridge.Helper
{
	// Hidden helpers for the in-game Tone designer page (DLL/ToneDesigner/SongLibrary.cpp and ToneBackup.cpp start them; no
	// window, exit code 0 = done):
	//   --song-index <Rocksmith folder> <out.tsv>   every song in songs.psarc and dlc\*_p.psarc, one line each:
	//                                                key, title, artist, album, year, arrangements, file (tab separated,
	//                                                UTF-8). Each .psarc is read once: <out.tsv>.cache.v3.json keeps what
	//                                                was found per file (by size and time), so later runs only read new
	//                                                or changed files. <out.tsv minus .tsv>.tones.tsv lists every
	//                                                arrangement's tones: song key, arrangement, slot (Base, A-D),
	//                                                tone key, tone name (the Tone designer's song tones).
	//   --tone-backup <out.zip>                     the Tone designer's data (tones, song tones, plugin settings) and its
	//                                                settings (HKCU\Software\RSMods\VstHost) in one zip
	//   --tone-backup-auto <folder>                 one "Auto yyyy-MM-dd.zip" a day in that folder, the newest 7 kept
	//   --tone-restore <in.zip> <safety.zip>        backs the current data up to safety.zip, then puts the zip's back
	// The VST host must not be running during a restore (it autosaves its chain); the page closes it first.
	// Song tone sets (.rabsong, SongToneSet.cs; the Tone designer page starts these):
	//   --songtones-plan <dlcKey> <tones.tsv> <out.json>       what an export of the song would carry (tones, files,
	//                                                           problems)
	//   --songtones-export <dlcKey> <tones.tsv> <out.rabsong>  the song's assigned tones and the player's own files they use
	//   --songtones-import <in.rabsong> <dlcKey> <tones.tsv>   checks the set, writes its presets and files and the record
	//                                                           SongTones\<dlcKey>.rabsong.json; exit 4 = refused (the
	//                                                           reason is in the record, nothing else is written)
	internal static class ToneDesignerTools
	{
		private const string SettingsKey = @"Software\RSMods\VstHost";
		// Values that describe this run, not the player's setup: never backed up or restored.
		// GuitarTone (the Guitar Tone page's tab) is this PC's choice of what plays, read by the game only at start: a
		// restore or an import from another PC must neither change it under the running game nor delete it.
		private static readonly string[] TransientValues = { "LastCrash", "LastCrashAt", "LastCrashModule", "LastCrashLoading", "LastCrashGraphics", "LastCrashWindows", "PreloadingNow", "PreloadFailed", "PreloadFailedWhy", "GuitarTone" };

		public static bool IsCommand(string argument)
		{
			return argument == "--song-index" || argument == "--tone-backup" || argument == "--tone-backup-auto" || argument == "--tone-restore"
				|| argument == "--tone-import"
				|| argument == "--songtones-plan" || argument == "--songtones-export" || argument == "--songtones-import";
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
					case "--tone-import" when arguments.Length == 4: Import(arguments[1], arguments[2], arguments[3]); break;   // <in.zip> <safety.zip> <report.txt>
					case "--songtones-plan" when arguments.Length == 4: exitCode = SongToneSet.WritePlan(arguments[1], arguments[2], arguments[3]); break;
					case "--songtones-export" when arguments.Length == 4: exitCode = SongToneSet.Export(arguments[1], arguments[2], arguments[3]); break;
					case "--songtones-import" when arguments.Length == 4: exitCode = SongToneSet.Import(arguments[1], arguments[2], arguments[3]); break;
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

		internal static void Log(string text)
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
			public List<SongTone> Tones = new List<SongTone>();
		}

		// One of an arrangement's tones, from its manifest: Tone_Base and Tone_A..D name tone keys, its Tones list the
		// names players see. The game switches between them at the chart's tone changes.
		private sealed class SongTone
		{
			public string Arrangement, Slot, Key, Name;
		}

		private static readonly string[] ToneSlots = { "Tone_Base", "Tone_A", "Tone_B", "Tone_C", "Tone_D" };

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

			var cachePath = outPath + ".cache.v3.json";   // v3: each arrangement's tones; v2: manifests read as plain JSON
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
								AddTones(song, name, a);
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

			// Every arrangement's tones, for the songs listed above.
			var tones = new StringBuilder();
			var listed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
			foreach (var file in fresh.Keys.OrderBy(f => f, StringComparer.OrdinalIgnoreCase))
				foreach (var song in fresh[file].Songs)
				{
					if (!listed.Add(song.Key)) continue;
					foreach (var tone in song.Tones)
						tones.Append(Clean(song.Key)).Append('\t').Append(Clean(tone.Arrangement)).Append('\t').Append(Clean(tone.Slot)).Append('\t')
							.Append(Clean(tone.Key)).Append('\t').Append(Clean(tone.Name)).Append('\n');
				}

			Directory.CreateDirectory(Path.GetDirectoryName(outPath));
			WriteAtomically(outPath, Encoding.UTF8.GetBytes(text.ToString()));
			WriteAtomically(TonesPath(outPath), Encoding.UTF8.GetBytes(tones.ToString()));
			WriteAtomically(cachePath, Encoding.UTF8.GetBytes(JsonConvert.SerializeObject(new Dictionary<string, CachedFile>(fresh, StringComparer.OrdinalIgnoreCase))));
			Log("song index: " + seen.Count + " songs from " + files.Count + " files (" + read + " read, the rest from the cache)");
		}

		private static string TonesPath(string indexPath) =>
			Path.Combine(Path.GetDirectoryName(indexPath), Path.GetFileNameWithoutExtension(indexPath) + ".tones.tsv");

		// The arrangement's Tone_Base / Tone_A..D keys, named from its Tones list (the key itself when it has no name).
		private static void AddTones(Song song, string arrangement, Newtonsoft.Json.Linq.JToken attributes)
		{
			if (arrangement.Length == 0 || arrangement == "Vocals" || arrangement == "JVocals") return;
			var names = new Dictionary<string, string>(StringComparer.Ordinal);
			if (attributes["Tones"] is Newtonsoft.Json.Linq.JArray list)
				foreach (var tone in list)
				{
					var key = (string)tone["Key"];
					if (!string.IsNullOrEmpty(key) && !names.ContainsKey(key)) names[key] = (string)tone["Name"] ?? key;
				}
			foreach (var slot in ToneSlots)
			{
				var key = (string)attributes[slot];
				if (string.IsNullOrWhiteSpace(key)) continue;
				if (song.Tones.Any(t => t.Arrangement == arrangement && t.Key == key)) continue;   // Tone_A repeating Tone_Base
				song.Tones.Add(new SongTone
				{
					Arrangement = arrangement,
					Slot = slot == "Tone_Base" ? "Base" : slot.Substring(5),
					Key = key,
					Name = names.TryGetValue(key, out var name) && !string.IsNullOrWhiteSpace(name) ? name : key,
				});
			}
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

		// Test only: RAB_TEST_APPDATA stands in for %APPDATA% (both data folders) and moves the settings to
		// HKCU\Software\RSMods\VstHost-BackupTest, so backup and restore can be tried without touching the player's setup.
		private static string TestRoot => Environment.GetEnvironmentVariable("RAB_TEST_APPDATA");

		private static string AppDataRoot => string.IsNullOrEmpty(TestRoot) ? Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData) : TestRoot;

		private static string BackupSettingsKey => string.IsNullOrEmpty(TestRoot) ? SettingsKey : SettingsKey + "-BackupTest";

		private static string DataFolder()
		{
			return Path.Combine(AppDataRoot, "RocksmithAudioBridge", "VstHost");
		}

		// The built-in Tone designer (Stompbox engine) keeps its captures, cabinets, presets and song tones here.
		private static string StompboxFolder()
		{
			return Path.Combine(AppDataRoot, "RocksmithAudioBridge", "Stompbox");
		}

		// Only the player's material. StarterTones\ and StarterTones-installed.txt belong to the installer, Catalogue.txt
		// is rewritten by the host at every start.
		private static readonly string[] StompboxFolders = { "NAM", "Cabinets", "Presets", "SongTones" };
		private const string StompboxPrefix = "stompbox/";
		// Its presence marks a backup that carries the Tone designer's data (older ones do not).
		private const string StompboxListEntry = "stompbox.txt";
		// Settings the built-in Tone designer added; an older backup has no say over them.
		private static readonly string[] StompboxSettings = { "ToneDesignerEngine", "ToneDesignerInputTrim" };

		// Starter files by path relative to the data folder ("NAM\Plexi Crunch.nam"), with the SHA-256 the installer wrote
		// (Installer/StarterTones.cs: path, tab, upper-case hex, one per line).
		private static Dictionary<string, string> InstalledStarterFiles(string folder)
		{
			var record = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
			var path = Path.Combine(folder, "StarterTones-installed.txt");
			if (!File.Exists(path)) return record;
			foreach (var line in File.ReadAllLines(path))
			{
				var tab = line.LastIndexOf('\t');
				if (tab > 0) record[line.Substring(0, tab).Replace('/', '\\')] = line.Substring(tab + 1).Trim();
			}
			return record;
		}

		private static string Sha256Of(string file)
		{
			using (var sha = System.Security.Cryptography.SHA256.Create())
			using (var input = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
				return BitConverter.ToString(sha.ComputeHash(input)).Replace("-", "");
		}

		// The Tone designer files worth keeping, relative to its data folder with back slashes. A starter file still as
		// installed is left out (the installer puts it back); one the player saved over is kept.
		private static List<string> StompboxFiles(string folder)
		{
			var files = new List<string>();
			if (!Directory.Exists(folder)) return files;
			var starters = InstalledStarterFiles(folder);
			foreach (var sub in StompboxFolders)
			{
				var path = Path.Combine(folder, sub);
				if (!Directory.Exists(path)) continue;
				foreach (var file in Directory.GetFiles(path, "*", SearchOption.AllDirectories))
				{
					if (file.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)) continue;
					var relative = file.Substring(folder.Length).TrimStart('\\', '/');
					if (starters.TryGetValue(relative, out var hash) && string.Equals(Sha256Of(file), hash, StringComparison.OrdinalIgnoreCase)) continue;
					files.Add(relative);
				}
			}
			return files;
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
			using (var key = Registry.CurrentUser.OpenSubKey(BackupSettingsKey))
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
			var stompboxFolder = StompboxFolder();
			var stompboxFiles = StompboxFiles(stompboxFolder);
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
				foreach (var relative in stompboxFiles)
				{
					var file = Path.Combine(stompboxFolder, relative);
					var entry = zip.CreateEntry(StompboxPrefix + relative.Replace('\\', '/'), CompressionLevel.Optimal);
					entry.LastWriteTime = File.GetLastWriteTime(file);
					using (var input = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
					using (var output = entry.Open()) input.CopyTo(output);
				}
				var list = zip.CreateEntry(StompboxListEntry);
				using (var writer = new StreamWriter(list.Open(), new UTF8Encoding(false)))
					foreach (var relative in stompboxFiles) writer.WriteLine(relative);
				var settings = zip.CreateEntry("settings.json");
				using (var writer = new StreamWriter(settings.Open(), new UTF8Encoding(false)))
					writer.Write(JsonConvert.SerializeObject(ReadSettings(), Formatting.Indented));
				var about = zip.CreateEntry("about.txt");
				using (var writer = new StreamWriter(about.Open(), new UTF8Encoding(false)))
				{
					writer.WriteLine("Rocksmith Audio Bridge, Tone designer backup");
					writer.WriteLine("Made " + DateTime.Now.ToString("yyyy-MM-dd HH:mm"));
					writer.WriteLine(files.Count(f => f.EndsWith(".rabtone", StringComparison.OrdinalIgnoreCase)) + " tone file(s), " + files.Count + " file(s) in all");
					writer.WriteLine(stompboxFiles.Count + " Tone designer file(s) (captures, cabinets, presets, song tones; starter files as installed are left out)");
					writer.WriteLine("Restore it from the Tone designer page: File, Backups.");
				}
			}
			if (File.Exists(zipPath)) File.Delete(zipPath);
			File.Move(temp, zipPath);
			Log("backup: " + zipPath + " (" + files.Count + " files, " + stompboxFiles.Count + " Tone designer files)");
		}

		private static void AutoBackup(string backupFolder)
		{
			Directory.CreateDirectory(backupFolder);
			if (!DataFiles(DataFolder()).Any() && StompboxFiles(StompboxFolder()).Count == 0) return;   // nothing to keep yet
			var today = Path.Combine(backupFolder, "Auto " + DateTime.Now.ToString("yyyy-MM-dd") + ".zip");
			if (!File.Exists(today)) Backup(today);
			foreach (var old in Directory.GetFiles(backupFolder, "Auto *.zip").OrderByDescending(f => f, StringComparer.OrdinalIgnoreCase).Skip(7))
				File.Delete(old);
		}

		// Checks that a zip is one of ours; the zip is left open for the caller.
		private static void CheckBackup(ZipArchive zip, string zipPath)
		{
			if (zip.GetEntry("settings.json") == null && zip.GetEntry(StompboxListEntry) == null
				&& !zip.Entries.Any(e => e.FullName.StartsWith("files/", StringComparison.Ordinal)))
				throw new InvalidDataException("Not a Tone designer backup: " + zipPath);
		}

		// Writes a zip's entries under prefix into folder, over any file of the same name. Never outside the folder; with
		// allowedFolders, only into those top-level folders. Returns the relative paths (forward slashes) written.
		private static List<string> ExtractEntries(ZipArchive zip, string prefix, string folder, string[] allowedFolders)
		{
			var written = new List<string>();
			var root = Path.GetFullPath(folder) + Path.DirectorySeparatorChar;
			foreach (var entry in zip.Entries)
			{
				if (!entry.FullName.StartsWith(prefix, StringComparison.Ordinal) || entry.FullName.EndsWith("/", StringComparison.Ordinal)) continue;
				var relative = entry.FullName.Substring(prefix.Length);
				var target = Path.GetFullPath(Path.Combine(folder, relative));
				if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue;   // never outside the data folder
				// Checked on the resolved path: "NAM/../StarterTones/manifest.json" names NAM but lands elsewhere.
				if (allowedFolders != null && !allowedFolders.Contains(target.Substring(root.Length).Split(Path.DirectorySeparatorChar)[0], StringComparer.OrdinalIgnoreCase)) continue;
				Directory.CreateDirectory(Path.GetDirectoryName(target));
				using (var input = entry.Open())
				using (var output = new FileStream(target, FileMode.Create, FileAccess.Write)) input.CopyTo(output);
				File.SetLastWriteTime(target, entry.LastWriteTime.LocalDateTime);
				written.Add(relative);
			}
			return written;
		}

		private static Dictionary<string, SettingValue> BackupSettings(ZipArchive zip)
		{
			var entry = zip.GetEntry("settings.json");
			if (entry == null) return null;
			using (var reader = new StreamReader(entry.Open(), Encoding.UTF8))
				return JsonConvert.DeserializeObject<Dictionary<string, SettingValue>>(reader.ReadToEnd()) ?? new Dictionary<string, SettingValue>();
		}

		private static void SetSetting(RegistryKey key, string name, SettingValue value)
		{
			if (value.Kind == "dword") key.SetValue(name, unchecked((int)value.Number), RegistryValueKind.DWord);
			else key.SetValue(name, value.Text ?? "", RegistryValueKind.String);
		}

		// Restoring a backup from the list (File, Backups) takes the setup back to that moment: the VST rig's files are
		// replaced, nothing left over, and its settings too. The Tone designer's files are only put back (see below).
		private static void Restore(string zipPath, string safetyZip)
		{
			using (var stream = new FileStream(zipPath, FileMode.Open, FileAccess.Read, FileShare.Read))
			using (var zip = new ZipArchive(stream, ZipArchiveMode.Read))
			{
				CheckBackup(zip, zipPath);
				var hasStompbox = zip.GetEntry(StompboxListEntry) != null;

				Backup(safetyZip);   // what is there now, in case this was the wrong backup

				// Replace the setup: the backup's tones, song tones, chain and plugin settings, nothing left over.
				var folder = DataFolder();
				foreach (var file in DataFiles(folder).ToList()) File.Delete(file);
				foreach (var directory in Directory.Exists(folder) ? Directory.GetDirectories(folder, "*", SearchOption.AllDirectories).OrderByDescending(d => d.Length) : Enumerable.Empty<string>())
					if (!Directory.EnumerateFileSystemEntries(directory).Any()) Directory.Delete(directory);
				ExtractEntries(zip, "files/", folder, null);

				// The Tone designer's files are put back over the player's versions, but nothing is deleted: its captures can be
				// large, starter files are never in a backup, and a backup that predates a capture should not take it away.
				var stompboxCount = ExtractEntries(zip, StompboxPrefix, StompboxFolder(), StompboxFolders).Count;
				if (stompboxCount > 0) Log("restore: " + stompboxCount + " Tone designer file(s) put back");

				var settings = BackupSettings(zip);
				if (settings != null)
				{
					using (var key = Registry.CurrentUser.CreateSubKey(BackupSettingsKey))
					{
						// A backup from before the built-in Tone designer leaves its settings as they are.
						foreach (var name in key.GetValueNames())
							if (!TransientValues.Contains(name, StringComparer.OrdinalIgnoreCase)
								&& (hasStompbox || !StompboxSettings.Contains(name, StringComparer.OrdinalIgnoreCase))) key.DeleteValue(name, false);
						foreach (var pair in settings)
							if (!TransientValues.Contains(pair.Key, StringComparer.OrdinalIgnoreCase) && pair.Value != null) SetSetting(key, pair.Key, pair.Value);
					}
				}
			}
			Log("restore: " + zipPath + " (the setup before it is in " + safetyZip + ")");
		}

		// ---- Import a setup (a new or rebuilt PC) ------------------------------------------------------------

		// Settings that list plugin files, folders or tones (separated by ;): an import adds to this PC's list.
		private static readonly string[] ListSettings = { "ScanFolders", "FavouritePlugins", "FavouriteTones", "KeepLoadedPlugins", "RecentPlugins" };
		// Settings that hold plugin paths, fixed when a plugin sits in another folder on this PC.
		private static readonly string[] PluginPathSettings = { "LastPlugin", "FavouritePlugins", "KeepLoadedPlugins", "RecentPlugins" };

		private static List<string> SplitList(string text) => (text ?? "").Split(';').Where(item => item.Length > 0).ToList();

		// Like a restore, but for moving to another PC: everything in the file is added, or written over the file of the
		// same name, and nothing the file does not mention is deleted, for the VST rig and the Tone designer alike.
		// Afterwards the imported tones' plugins are looked up on this PC. The report gets one line per plugin that is not
		// here ("missing", tab, plugin name, tab, the tones that use it) or that was found in another folder ("moved",
		// tab, plugin name, tab, its path here; the tones were changed to use it).
		private static void Import(string zipPath, string safetyZip, string reportPath)
		{
			var report = new List<string>();
			using (var stream = new FileStream(zipPath, FileMode.Open, FileAccess.Read, FileShare.Read))
			using (var zip = new ZipArchive(stream, ZipArchiveMode.Read))
			{
				CheckBackup(zip, zipPath);
				Backup(safetyZip);

				var folder = DataFolder();
				var vstFiles = ExtractEntries(zip, "files/", folder, null);
				var stompboxCount = ExtractEntries(zip, StompboxPrefix, StompboxFolder(), StompboxFolders).Count;

				var settings = BackupSettings(zip);
				if (settings != null)
				{
					using (var key = Registry.CurrentUser.CreateSubKey(BackupSettingsKey))
					{
						foreach (var pair in settings)
						{
							if (TransientValues.Contains(pair.Key, StringComparer.OrdinalIgnoreCase) || pair.Value == null) continue;
							if (pair.Value.Kind != "dword" && ListSettings.Contains(pair.Key, StringComparer.OrdinalIgnoreCase))
							{
								var merged = SplitList(pair.Value.Text);
								foreach (var mine in SplitList(key.GetValue(pair.Key) as string))
									if (!merged.Contains(mine, StringComparer.OrdinalIgnoreCase)) merged.Add(mine);
								key.SetValue(pair.Key, string.Join(";", merged), RegistryValueKind.String);
							}
							else SetSetting(key, pair.Key, pair.Value);
						}
					}
				}

				var tones = vstFiles.Where(f => f.EndsWith(".rabtone", StringComparison.OrdinalIgnoreCase))
					.Select(f => Path.Combine(folder, f.Replace('/', '\\'))).ToList();
				report.AddRange(CheckPlugins(tones, folder));
				Log("import: " + zipPath + " (" + vstFiles.Count + " VST rig files, " + stompboxCount + " Tone designer files; the setup before it is in " + safetyZip + ")");
			}
			Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(reportPath)));
			File.WriteAllLines(reportPath, report, new UTF8Encoding(false));
		}

		// Plugin file name (AmpliTube 5.vst3) to where it is on this PC: Common Files\VST3 and the player's added folders.
		private static Dictionary<string, string> InstalledPlugins()
		{
			var common = Environment.GetEnvironmentVariable("CommonProgramW6432");
			if (string.IsNullOrEmpty(common)) common = Environment.GetFolderPath(Environment.SpecialFolder.CommonProgramFiles);
			var folders = new List<string> { Path.Combine(common, "VST3") };
			using (var key = Registry.CurrentUser.OpenSubKey(BackupSettingsKey))
				folders.AddRange(SplitList(key?.GetValue("ScanFolders") as string));
			var found = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
			var pending = new Stack<string>(folders.Where(Directory.Exists));
			while (pending.Count > 0)
			{
				var current = pending.Pop();
				try
				{
					foreach (var file in Directory.GetFiles(current, "*.vst3"))
						if (!found.ContainsKey(Path.GetFileName(file))) found[Path.GetFileName(file)] = file;
					foreach (var directory in Directory.GetDirectories(current))
					{
						// A .vst3 folder is a plugin bundle: the plugin itself, not a folder to look inside.
						if (!directory.EndsWith(".vst3", StringComparison.OrdinalIgnoreCase)) pending.Push(directory);
						else if (!found.ContainsKey(Path.GetFileName(directory))) found[Path.GetFileName(directory)] = directory;
					}
				}
				catch (UnauthorizedAccessException) { }
				catch (IOException) { }
			}
			return found;
		}

		private static List<string> CheckPlugins(List<string> tones, string folder)
		{
			var users = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);   // plugin path -> tone names
			var toneSlots = new Dictionary<string, List<string>>();
			foreach (var tone in tones)
			{
				var plugins = ToneFile.Plugins(tone);
				if (plugins == null) continue;
				toneSlots[tone] = plugins;
				var name = Path.ChangeExtension(tone.Substring(folder.Length).TrimStart('\\'), null);
				if (name.StartsWith("Tones\\", StringComparison.OrdinalIgnoreCase)) name = name.Substring("Tones\\".Length);
				foreach (var plugin in plugins.Distinct(StringComparer.OrdinalIgnoreCase))
				{
					if (!users.TryGetValue(plugin, out var list)) users[plugin] = list = new List<string>();
					list.Add(name.Replace('\\', '/'));
				}
			}
			var report = new List<string>();
			var moved = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
			Dictionary<string, string> installed = null;
			foreach (var pair in users.OrderBy(p => Path.GetFileName(p.Key), StringComparer.OrdinalIgnoreCase))
			{
				if (File.Exists(pair.Key) || Directory.Exists(pair.Key)) continue;
				if (installed == null) installed = InstalledPlugins();
				var display = Path.GetFileNameWithoutExtension(pair.Key);
				if (installed.TryGetValue(Path.GetFileName(pair.Key), out var here))
				{
					moved[pair.Key] = here;
					report.Add("moved\t" + display + "\t" + here);
				}
				else report.Add("missing\t" + display + "\t" + string.Join(", ", pair.Value.OrderBy(t => t, StringComparer.OrdinalIgnoreCase)));
			}
			if (moved.Count == 0) return report;

			// The host loads a tone's plugins by their full path, so the tones and plugin lists point at where they are here.
			foreach (var pair in toneSlots)
				if (pair.Value.Any(moved.ContainsKey)) ToneFile.ReplacePlugins(pair.Key, moved);
			using (var key = Registry.CurrentUser.CreateSubKey(BackupSettingsKey))
				foreach (var name in PluginPathSettings)
				{
					if (!(key.GetValue(name) is string text) || text.Length == 0) continue;
					var items = SplitList(text).Select(item => moved.TryGetValue(item, out var here) ? here : item).Distinct(StringComparer.OrdinalIgnoreCase);
					key.SetValue(name, string.Join(";", items), RegistryValueKind.String);
				}
			return report;
		}

		// The VST host's tone file (VstHost/Source/Chain.cpp, SaveTone): "RABTONE2" (or "RABTONE1"), two int32 trims, a
		// uint32 slot count, then per slot a blob (uint32 length + bytes) with the plugin path in UTF-16, a bypass byte
		// and one blob of plugin settings (two in version 1).
		private static class ToneFile
		{
			private const int MaxSlots = 8;   // Chain::kMaxSlots

			private sealed class Slot
			{
				public string Path;
				public byte Bypass;
				public List<byte[]> States = new List<byte[]>();
			}

			private static bool Read(string file, out byte[] header, out List<Slot> slots)
			{
				header = null;
				slots = new List<Slot>();
				try
				{
					using (var reader = new BinaryReader(new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)))
					{
						header = reader.ReadBytes(16);
						if (header.Length != 16) return false;
						var magic = Encoding.ASCII.GetString(header, 0, 8);
						if (magic != "RABTONE2" && magic != "RABTONE1") return false;
						var states = magic == "RABTONE1" ? 2 : 1;
						var count = reader.ReadUInt32();
						if (count > MaxSlots) return false;
						for (var i = 0; i < count; ++i)
						{
							var slot = new Slot { Path = Encoding.Unicode.GetString(Blob(reader)), Bypass = reader.ReadByte() };
							for (var j = 0; j < states; ++j) slot.States.Add(Blob(reader));
							slots.Add(slot);
						}
						return true;
					}
				}
				catch (EndOfStreamException) { return false; }
				catch (InvalidDataException) { return false; }
				catch (IOException) { return false; }
			}

			private static byte[] Blob(BinaryReader reader)
			{
				var size = reader.ReadUInt32();
				if (size > (256u << 20)) throw new InvalidDataException("A block in the tone file is too large.");
				var bytes = reader.ReadBytes((int)size);
				if (bytes.Length != size) throw new EndOfStreamException();
				return bytes;
			}

			public static List<string> Plugins(string file) => Read(file, out _, out var slots) ? slots.Select(s => s.Path).ToList() : null;

			// Writes the tone again with the moved plugins' new paths; everything else stays byte for byte.
			public static void ReplacePlugins(string file, Dictionary<string, string> moved)
			{
				if (!Read(file, out var header, out var slots)) return;
				using (var buffer = new MemoryStream())
				using (var writer = new BinaryWriter(buffer))
				{
					writer.Write(header);
					writer.Write((uint)slots.Count);
					foreach (var slot in slots)
					{
						var path = Encoding.Unicode.GetBytes(moved.TryGetValue(slot.Path, out var here) ? here : slot.Path);
						writer.Write((uint)path.Length);
						writer.Write(path);
						writer.Write(slot.Bypass);
						foreach (var state in slot.States) { writer.Write((uint)state.Length); writer.Write(state); }
					}
					writer.Flush();
					WriteAtomically(file, buffer.ToArray());
				}
			}
		}
	}
}
