using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace RocksmithAudioBridge.Helper
{
	// The song tone set (.rabsong, docs: SongToneSet-Format.md): every Tone designer tone a song uses, in one ZIP beside
	// its PSARC, so another player with only the mod hears the same tones. Started by the hidden verbs in
	// ToneDesignerTools (--songtones-plan, --songtones-export, --songtones-import).
	//
	// A tone travels as values: the Stompbox chain of a saved preset, turned into tones.json and back into Stompbox's own
	// preset text on import. Starter files the mod ships are named, never copied; the player's own captures and IRs are
	// embedded once each, named by their SHA-256. There is no rights capture: a tone is settings, and the CDLC's own
	// metadata credits its charter (the draft spec's rights section was dropped).
	internal static class SongToneSet
	{
		// The versions a set records and checks (spec section 8). Both are 1 for the v1.0.0-beta.1 starter release and the
		// pinned Stompbox commit; raise them together with a new tones release or an engine change that sets depend on.
		internal static class Versions
		{
			public const string Format = "rab-songtoneset";
			public const int FormatVersion = 1;
			public const int EngineVersion = 1;
			public const int LibraryVersion = 1;
			public const string EngineBuild = "stompbox 17bc652";
			public const string App = "Rocksmith Audio Bridge";
		}

		public const int ExitDone = 0, ExitError = 1, ExitRefused = 4;

		// Testing only: RAB_TEST_APPDATA stands in for %APPDATA%, as for the backups (ToneDesignerTools.TestRoot).
		private const string DataFolderOverride = "RAB_TEST_APPDATA";

		// The one chain the Tone designer edits (StompboxControl::CHAIN_NAME).
		private const string ChainName = "Main";

		private static readonly string[] ModelExtensions = { ".nam", ".json", ".aidax" };
		private static readonly string[] ImpulseExtensions = { ".wav" };

		// File parameters by pedal, when Catalogue.txt does not say (DefaultPlugins.cpp at the pinned commit).
		private static readonly Dictionary<string, string> KnownFileFolders = new Dictionary<string, string>(StringComparer.Ordinal)
		{
			{ "NAM/Model", "NAM" }, { "NAMMulti/Model", "NAM" }, { "Cabinet/Impulse", "Cabinets" }, { "ConvoReverb/Impulse", "Reverb" },
		};

		private static readonly Regex AssetEntry = new Regex("^assets/([0-9a-f]{64})(\\.nam|\\.json|\\.aidax|\\.wav)$", RegexOptions.CultureInvariant);
		private static readonly Regex LicenceEntry = new Regex("^licenses/([0-9a-f]{64})\\.txt$", RegexOptions.CultureInvariant);
		private static readonly Regex Sha256Text = new Regex("^[0-9a-f]{64}$", RegexOptions.CultureInvariant);
		private const long MaxTonesJson = 1L << 20;
		private const long MaxAsset = 128L << 20;

		private static void Log(string text) => ToneDesignerTools.Log("song tone set: " + text);

		// ---- Data folder ----------------------------------------------------------------------------------------

		private static string DataFolder()
		{
			var custom = Environment.GetEnvironmentVariable(DataFolderOverride);
			var root = string.IsNullOrWhiteSpace(custom) ? Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData) : Path.GetFullPath(custom);
			return Path.Combine(root, "RocksmithAudioBridge", "Stompbox");
		}

		// As StompboxService::SongTonesFile names a song's files.
		private static string SafeKey(string key)
		{
			var text = new StringBuilder();
			foreach (var c in key) text.Append("<>:\"/\\|?*".IndexOf(c) >= 0 || c < 32 ? '_' : c);
			return text.ToString();
		}

		private static string SafeFileName(string name)
		{
			var text = new StringBuilder();
			foreach (var c in name.Trim()) text.Append(Path.GetInvalidFileNameChars().Contains(c) || c == '"' ? '_' : c);
			var result = text.ToString().Trim().TrimEnd('.');
			return result.Length == 0 ? "Tone" : result;
		}

		private static string Sha256(byte[] bytes)
		{
			using (var sha = SHA256.Create()) return string.Concat(sha.ComputeHash(bytes).Select(b => b.ToString("x2")));
		}

		private static string Sha256File(string path)
		{
			using (var sha = SHA256.Create())
			using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
				return string.Concat(sha.ComputeHash(stream).Select(b => b.ToString("x2")));
		}

		private static string[] ExtensionsFor(string folder) => folder == "NAM" ? ModelExtensions : ImpulseExtensions;

		private static string KindFor(string folder) => folder == "NAM" ? "nam" : "ir";

		// Stompbox lists files by name without extension: the file behind a name, if the folder has it.
		private static string FindFile(string data, string folder, string name)
		{
			var directory = Path.Combine(data, folder);
			if (!Directory.Exists(directory)) return null;
			foreach (var extension in ExtensionsFor(folder))
			{
				var path = Path.Combine(directory, name + extension);
				if (File.Exists(path)) return path;
			}
			return null;
		}

		// ---- Starter manifest -----------------------------------------------------------------------------------

		private sealed class StarterFile
		{
			public string Folder, Name, Sha256, Author;
		}

		// StarterTones\manifest.json: the files the mod ships, by "<folder>/<name without extension>".
		private static Dictionary<string, StarterFile> ReadStarterManifest(string data)
		{
			var files = new Dictionary<string, StarterFile>(StringComparer.OrdinalIgnoreCase);
			var path = Path.Combine(data, "StarterTones", "manifest.json");
			if (!File.Exists(path))
			{
				Log("no starter manifest at " + path + ": every file counts as the player's own");
				return files;
			}
			var manifest = JObject.Parse(File.ReadAllText(path, Encoding.UTF8));
			if (!(manifest["files"] is JArray list)) return files;
			foreach (var item in list)
			{
				var filePath = ((string)item["path"] ?? "").Replace('\\', '/');
				var sha = ((string)item["sha256"] ?? "").ToLowerInvariant();
				var slash = filePath.IndexOf('/');
				if (slash <= 0 || sha.Length != 64) continue;
				var folder = filePath.Substring(0, slash);
				if (folder != "NAM" && folder != "Cabinets") continue;
				var name = Path.GetFileNameWithoutExtension(filePath.Substring(slash + 1));
				files[folder + "/" + name] = new StarterFile { Folder = folder, Name = name, Sha256 = sha, Author = (string)item["author"] ?? "" };
			}
			return files;
		}

		// ---- Catalogue (Stompbox DumpConfig, written by the host) -----------------------------------------------

		private sealed class Catalogue
		{
			public readonly Dictionary<string, Dictionary<string, string>> Types = new Dictionary<string, Dictionary<string, string>>(StringComparer.Ordinal);
			public readonly Dictionary<string, string> FileFolders = new Dictionary<string, string>(StringComparer.Ordinal);

			public string TypeOf(string effect, string parameter) =>
				Types.TryGetValue(effect, out var parameters) && parameters.TryGetValue(parameter, out var type) ? type : null;
		}

		private static Catalogue ReadCatalogue(string data)
		{
			var path = Path.Combine(data, "Catalogue.txt");
			if (!File.Exists(path)) return null;
			var catalogue = new Catalogue();
			foreach (var raw in File.ReadAllLines(path, Encoding.UTF8))
			{
				var words = Words(raw);
				if (words.Count >= 2 && words[0] == "PluginConfig")
				{
					if (!catalogue.Types.ContainsKey(words[1])) catalogue.Types[words[1]] = new Dictionary<string, string>(StringComparer.Ordinal);
				}
				else if (words.Count >= 5 && words[0] == "ParameterConfig" && words[3] == "Type")
				{
					if (!catalogue.Types.TryGetValue(words[1], out var parameters))
						catalogue.Types[words[1]] = parameters = new Dictionary<string, string>(StringComparer.Ordinal);
					parameters[words[2]] = words[4];
				}
				else if (words.Count >= 4 && words[0] == "ParameterFileTree")
					catalogue.FileFolders[words[1] + "/" + words[2]] = words[3];
			}
			return catalogue;
		}

		private static string FileFolder(Catalogue catalogue, string effect, string parameter)
		{
			var key = effect + "/" + parameter;
			if (catalogue != null && catalogue.FileFolders.TryGetValue(key, out var folder)) return folder;
			return KnownFileFolders.TryGetValue(key, out folder) ? folder : null;
		}

		// Words as Stompbox's HandleCommand splits them (std::quoted): spaces separate, "..." keeps spaces.
		private static List<string> Words(string line)
		{
			var words = new List<string>();
			int i = 0;
			while (i < line.Length)
			{
				while (i < line.Length && char.IsWhiteSpace(line[i])) i++;
				if (i >= line.Length) break;
				var word = new StringBuilder();
				if (line[i] == '"')
				{
					i++;
					while (i < line.Length && line[i] != '"')
					{
						if (line[i] == '\\' && i + 1 < line.Length) i++;
						word.Append(line[i++]);
					}
					i++;
				}
				else
					while (i < line.Length && !char.IsWhiteSpace(line[i])) word.Append(line[i++]);
				words.Add(word.ToString());
			}
			return words;
		}

		// ---- Presets ---------------------------------------------------------------------------------------------

		private sealed class Pedal
		{
			public string Id, Effect;
			public bool Enabled = true;
			// In file order: parameter name and its value as the preset wrote it (quotes removed).
			public readonly List<KeyValuePair<string, string>> Values = new List<KeyValuePair<string, string>>();
		}

		// A Stompbox preset (DumpProgram text): SetPreset, SetChain Main <ids>, SetParam <id> <name> <value>..., EndProgram.
		// MIDI mappings (MapController) are not part of a tone and are left out.
		private static List<Pedal> ReadPreset(string path)
		{
			var pedals = new List<Pedal>();
			var byId = new Dictionary<string, Pedal>(StringComparer.Ordinal);
			foreach (var raw in File.ReadAllLines(path, Encoding.UTF8))
			{
				var words = Words(raw);
				if (words.Count >= 2 && words[0] == "SetChain" && words[1] == ChainName)
				{
					pedals.Clear();
					byId.Clear();
					foreach (var id in words.Skip(2))
					{
						var underscore = id.IndexOf('_');
						var pedal = new Pedal { Id = id, Effect = underscore < 0 ? id : id.Substring(0, underscore) };
						pedals.Add(pedal);
						byId[id] = pedal;
					}
				}
				else if (words.Count >= 3 && words[0] == "SetParam" && byId.TryGetValue(words[1], out var pedal))
				{
					// An enum or file value of several unquoted words is the rest of the line (as Stompbox joins them).
					var value = string.Join(" ", words.Skip(3));
					if (words[2] == "Enabled") pedal.Enabled = value.Trim() == "1";
					else if (words.Count >= 4)
					{
						pedal.Values.RemoveAll(v => v.Key == words[2]);
						pedal.Values.Add(new KeyValuePair<string, string>(words[2], value));
					}
				}
			}
			return pedals;
		}

		private static string FormatNumber(double value) => value.ToString("R", CultureInfo.InvariantCulture);

		// Stompbox preset text from a pedal list, as SavePreset writes it.
		private static string WritePreset(string name, List<Pedal> pedals)
		{
			var text = new StringBuilder();
			text.Append("SetPreset ").Append(name).Append("\r\n");
			text.Append("SetChain ").Append(ChainName);
			foreach (var pedal in pedals) text.Append(' ').Append(pedal.Id);
			text.Append("\r\n");
			foreach (var pedal in pedals)
			{
				text.Append("SetParam ").Append(pedal.Id).Append(" Enabled ").Append(pedal.Enabled ? "1" : "0").Append("\r\n");
				foreach (var value in pedal.Values)
					text.Append("SetParam ").Append(pedal.Id).Append(' ').Append(value.Key).Append(' ').Append(value.Value).Append("\r\n");
			}
			text.Append("EndProgram\r\n");
			return text.ToString();
		}

		// ---- Song index -------------------------------------------------------------------------------------------

		private sealed class SongInfo
		{
			public string Title = "", Artist = "";
			// Tone key -> arrangements that use it, in index order.
			public readonly Dictionary<string, List<string>> ToneKeys = new Dictionary<string, List<string>>(StringComparer.Ordinal);
		}

		// <index>.tones.tsv (key, arrangement, slot, tone key, tone name) and its <index>.tsv (key, title, artist, ...).
		private static SongInfo ReadSong(string dlcKey, string tonesTsv)
		{
			var song = new SongInfo();
			foreach (var line in File.ReadAllLines(tonesTsv, Encoding.UTF8))
			{
				var columns = line.Split('\t');
				if (columns.Length < 4 || !string.Equals(columns[0], dlcKey, StringComparison.OrdinalIgnoreCase) || columns[3].Length == 0) continue;
				if (!song.ToneKeys.TryGetValue(columns[3], out var arrangements)) song.ToneKeys[columns[3]] = arrangements = new List<string>();
				if (columns[1].Length > 0 && !arrangements.Contains(columns[1])) arrangements.Add(columns[1]);
			}
			var name = Path.GetFileName(tonesTsv);
			if (name.EndsWith(".tones.tsv", StringComparison.OrdinalIgnoreCase))
			{
				var index = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(tonesTsv)), name.Substring(0, name.Length - ".tones.tsv".Length) + ".tsv");
				if (File.Exists(index))
					foreach (var line in File.ReadAllLines(index, Encoding.UTF8))
					{
						var columns = line.Split('\t');
						if (columns.Length < 3 || !string.Equals(columns[0], dlcKey, StringComparison.OrdinalIgnoreCase)) continue;
						song.Title = columns[1];
						song.Artist = columns[2];
						break;
					}
			}
			return song;
		}

		// SongTones\<dlcKey>.txt: tone key, tab, preset name (StompboxService::ReadAssignments).
		private static Dictionary<string, string> ReadAssignments(string data, string dlcKey)
		{
			var tones = new Dictionary<string, string>(StringComparer.Ordinal);
			var path = Path.Combine(data, "SongTones", SafeKey(dlcKey) + ".txt");
			if (!File.Exists(path)) return tones;
			foreach (var line in File.ReadAllLines(path, Encoding.UTF8))
			{
				var tab = line.IndexOf('\t');
				if (tab <= 0 || tab + 1 >= line.Length) continue;
				tones[line.Substring(0, tab)] = line.Substring(tab + 1);
			}
			return tones;
		}

		// ---- Export -------------------------------------------------------------------------------------------

		private sealed class PlannedFile
		{
			public string Kind, Folder, Name, Path, Sha256, Author;
			public long Bytes;
			public bool Starter;
		}

		private sealed class Plan
		{
			public SongInfo Song;
			public readonly List<JObject> PlanTones = new List<JObject>();
			// Tone id -> tones.json tone; tone key -> tone id.
			public readonly Dictionary<string, JObject> Tones = new Dictionary<string, JObject>(StringComparer.Ordinal);
			public readonly Dictionary<string, string> ToneOfKey = new Dictionary<string, string>(StringComparer.Ordinal);
			public readonly Dictionary<string, PlannedFile> Files = new Dictionary<string, PlannedFile>(StringComparer.Ordinal);
			public readonly List<string> Problems = new List<string>();
		}

		// What an export would carry: each assigned tone whose preset and files are all there, every file once.
		// A tone that cannot travel whole is left out and named in Problems (a set never carries part of a tone).
		private static Plan MakePlan(string dlcKey, string tonesTsv)
		{
			var data = DataFolder();
			var plan = new Plan { Song = ReadSong(dlcKey, tonesTsv) };
			var starters = ReadStarterManifest(data);
			var catalogue = ReadCatalogue(data);
			var assignments = ReadAssignments(data, dlcKey);
			if (assignments.Count == 0) plan.Problems.Add("No tones are assigned to this song yet.");
			var toneIdOfPreset = new Dictionary<string, string>(StringComparer.Ordinal);
			var hashes = new Dictionary<string, PlannedFile>(StringComparer.OrdinalIgnoreCase);

			foreach (var assignment in assignments.OrderBy(a => a.Key, StringComparer.Ordinal))
			{
				var toneKey = assignment.Key;
				var preset = assignment.Value;
				if (!plan.Song.ToneKeys.TryGetValue(toneKey, out var arrangements))
				{
					plan.Problems.Add("\"" + toneKey + "\" has a tone assigned but is not one of this song's tones; it is left out.");
					continue;
				}
				if (toneIdOfPreset.TryGetValue(preset, out var sharedId))
				{
					plan.ToneOfKey[toneKey] = sharedId;
					plan.PlanTones.Add(PlanTone(toneKey, arrangements, preset));
					continue;
				}
				var presetPath = Path.Combine(data, "Presets", preset);
				if (preset.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || !File.Exists(presetPath))
				{
					plan.Problems.Add("The tone \"" + preset + "\" for " + toneKey + " no longer exists; it is left out.");
					continue;
				}

				var chain = new JArray();
				var toneFiles = new List<PlannedFile>();
				string problem = null;
				foreach (var pedal in ReadPreset(presetPath))
				{
					var parameters = new JObject();
					foreach (var value in pedal.Values)
					{
						var folder = FileFolder(catalogue, pedal.Effect, value.Key);
						if (folder != null)
						{
							if (folder != "NAM" && folder != "Cabinets" && folder != "Reverb")
							{
								problem = "The tone \"" + preset + "\" uses a " + pedal.Effect + " file, which a song tone set cannot carry; it is left out.";
								break;
							}
							var file = FindFile(data, folder, value.Value);
							if (file == null)
							{
								problem = "The tone \"" + preset + "\" uses \"" + value.Value + "\", which is no longer in " + folder + "; it is left out.";
								break;
							}
							var sha = Sha256File(file);
							var key = folder + "/" + value.Value;
							bool starter = starters.TryGetValue(key, out var shipped) && shipped.Sha256 == sha;
							var planned = new PlannedFile
							{
								Kind = KindFor(folder), Folder = folder, Name = value.Value, Path = file, Sha256 = sha,
								Bytes = new FileInfo(file).Length, Starter = starter, Author = starter ? shipped.Author : "",
							};
							toneFiles.Add(planned);
							parameters[value.Key] = starter ? new JObject { ["builtin"] = value.Value } : new JObject { ["asset"] = sha, ["name"] = value.Value };
						}
						else parameters[value.Key] = ToJsonValue(catalogue?.TypeOf(pedal.Effect, value.Key), value.Value);
					}
					if (problem != null) break;
					chain.Add(new JObject { ["effect"] = pedal.Effect, ["enabled"] = pedal.Enabled, ["params"] = parameters });
				}
				if (problem != null)
				{
					plan.Problems.Add(problem);
					continue;
				}

				var id = ToneId(preset, plan.Tones.Keys);
				plan.Tones[id] = new JObject { ["name"] = preset, ["chain"] = chain };
				toneIdOfPreset[preset] = id;
				plan.ToneOfKey[toneKey] = id;
				plan.PlanTones.Add(PlanTone(toneKey, arrangements, preset));
				foreach (var file in toneFiles)
				{
					// The same bytes under two names (or folders) are embedded once; each starter file is named once.
					var fileKey = file.Starter ? "starter:" + file.Folder + "/" + file.Name : file.Sha256;
					if (!plan.Files.ContainsKey(fileKey)) plan.Files[fileKey] = file;
				}
			}
			return plan;
		}

		private static JObject PlanTone(string toneKey, List<string> arrangements, string preset) =>
			new JObject { ["toneKey"] = toneKey, ["arrangements"] = new JArray(arrangements), ["preset"] = preset };

		private static string ToneId(string preset, IEnumerable<string> taken)
		{
			var slug = Regex.Replace(preset.ToLowerInvariant(), "[^a-z0-9]+", "-").Trim('-');
			if (slug.Length == 0) slug = "tone";
			var used = new HashSet<string>(taken, StringComparer.Ordinal);
			var id = slug;
			for (int n = 2; used.Contains(id); n++) id = slug + "-" + n;
			return id;
		}

		// A preset value as tones.json holds it: a number in Stompbox's units, an enum's value name, a Bool as
		// true/false. Without the catalogue a Bool stays 0 or 1 (Stompbox reads both).
		private static JToken ToJsonValue(string type, string text)
		{
			if (type == "Enum") return text;
			if (type == "Bool") return text.Trim() == "1";
			if (double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var number) && !double.IsNaN(number) && !double.IsInfinity(number))
				return number;
			return text;   // a tempo-synced time ("1/4") or an enum when the catalogue is not there
		}

		public static int WritePlan(string dlcKey, string tonesTsv, string outJson)
		{
			var plan = MakePlan(dlcKey, tonesTsv);
			var files = new JArray();
			foreach (var file in plan.Files.Values.OrderBy(f => f.Starter ? 0 : 1).ThenBy(f => f.Kind).ThenBy(f => f.Name, StringComparer.OrdinalIgnoreCase))
				files.Add(new JObject
				{
					["kind"] = file.Kind, ["name"] = file.Name, ["sha256"] = file.Sha256, ["bytes"] = file.Bytes,
					["status"] = file.Starter ? "starter" : "embedded",
				});
			var result = new JObject
			{
				["dlcKey"] = dlcKey, ["title"] = plan.Song.Title, ["artist"] = plan.Song.Artist,
				["tones"] = new JArray(plan.PlanTones),
				["files"] = files,
				["totals"] = new JObject
				{
					["tones"] = plan.Tones.Count,
					["captures"] = plan.Files.Values.Count(f => f.Kind == "nam"),
					["cabinets"] = plan.Files.Values.Count(f => f.Kind == "ir"),
					["bytes"] = plan.Files.Values.Where(f => !f.Starter).Sum(f => f.Bytes),
				},
				["problems"] = new JArray(plan.Problems),
			};
			WriteText(outJson, result.ToString(Formatting.Indented));
			Log("plan " + dlcKey + ": " + plan.Tones.Count + " tone(s), " + plan.Files.Count + " file(s), " + plan.Problems.Count + " problem(s)");
			return ExitDone;
		}

		public static int Export(string dlcKey, string tonesTsv, string outPath)
		{
			var plan = MakePlan(dlcKey, tonesTsv);
			foreach (var problem in plan.Problems) Log("export " + dlcKey + ": " + problem);
			if (plan.Tones.Count == 0)
			{
				Log("export " + dlcKey + ": nothing to export");
				return ExitError;
			}

			bool anyStarter = plan.Files.Values.Any(f => f.Starter);
			var toneKeys = new JObject();
			foreach (var pair in plan.ToneOfKey.OrderBy(p => p.Key, StringComparer.Ordinal))
				toneKeys[pair.Key] = new JObject { ["tone"] = pair.Value, ["arrangements"] = new JArray(plan.Song.ToneKeys[pair.Key]) };
			var assets = new JObject();
			var embedded = plan.Files.Values.Where(f => !f.Starter).ToList();
			foreach (var file in embedded)
				assets[file.Sha256] = new JObject { ["kind"] = file.Kind, ["bytes"] = file.Bytes, ["fileName"] = Path.GetFileName(file.Path) };
			var song = new JObject { ["dlcKey"] = dlcKey };
			if (plan.Song.Title.Length > 0) song["title"] = plan.Song.Title;
			if (plan.Song.Artist.Length > 0) song["artist"] = plan.Song.Artist;
			// song.author (the charter) is not in the song index; a later version can read it from the CDLC.

			var tones = new JObject
			{
				["format"] = Versions.Format,
				["formatVersion"] = Versions.FormatVersion,
				["createdWith"] = new JObject
				{
					["app"] = Versions.App, ["appVersion"] = AppVersion(),
					["engineVersion"] = Versions.EngineVersion, ["engineBuild"] = Versions.EngineBuild, ["libraryVersion"] = Versions.LibraryVersion,
				},
				// Everything in engine and library 1 is "added in" 1; a set naming no starter file needs no library.
				["requires"] = new JObject { ["engineVersion"] = Versions.EngineVersion, ["libraryVersion"] = anyStarter ? Versions.LibraryVersion : 0 },
				["song"] = song,
				["toneKeys"] = toneKeys,
				["tones"] = new JObject(plan.Tones.Select(t => new JProperty(t.Key, t.Value))),
				["assets"] = assets,
			};

			Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outPath)));
			var temp = outPath + ".tmp";
			using (var stream = new FileStream(temp, FileMode.Create, FileAccess.Write))
			using (var zip = new ZipArchive(stream, ZipArchiveMode.Create))
			{
				// A sidecar is deflated; embedding in a PSARC (stored entries) is not done yet.
				var entry = zip.CreateEntry("tones.json", CompressionLevel.Optimal);
				using (var writer = new StreamWriter(entry.Open(), new UTF8Encoding(false))) writer.Write(tones.ToString(Formatting.Indented));
				foreach (var file in embedded)
				{
					var asset = zip.CreateEntry("assets/" + file.Sha256 + Path.GetExtension(file.Path).ToLowerInvariant(), CompressionLevel.Optimal);
					var bytes = File.ReadAllBytes(file.Path);
					if (Sha256(bytes) != file.Sha256) throw new IOException(file.Path + " changed while it was being exported");
					using (var output = asset.Open()) output.Write(bytes, 0, bytes.Length);
				}
			}
			if (File.Exists(outPath)) File.Delete(outPath);
			File.Move(temp, outPath);
			Log("export " + dlcKey + ": " + outPath + " (" + plan.Tones.Count + " tone(s), " + embedded.Count + " embedded file(s))");
			return ExitDone;
		}

		private static string AppVersion()
		{
			try { return System.Diagnostics.FileVersionInfo.GetVersionInfo(typeof(SongToneSet).Assembly.Location).ProductVersion ?? ""; }
			catch (Exception) { return ""; }
		}

		// ---- Import -------------------------------------------------------------------------------------------

		private sealed class Refusal : Exception
		{
			public Refusal(string reason) : base(reason) { }
		}

		private sealed class ImportAsset
		{
			public string Sha256, Kind, DisplayName, Extension;
			public byte[] Bytes;
			public readonly HashSet<string> Folders = new HashSet<string>(StringComparer.Ordinal);
			public readonly Dictionary<string, string> WrittenAs = new Dictionary<string, string>(StringComparer.Ordinal);   // folder -> name
		}

		private sealed class ImportTone
		{
			public string Id, Name, PresetName;
			public double OutputDb;
			// Per pedal: effect, enabled, and its values; file values as (folder, builtin name or asset hash).
			public readonly List<ImportPedal> Pedals = new List<ImportPedal>();
		}

		private sealed class ImportPedal
		{
			public string Effect;
			public bool Enabled;
			public readonly List<ImportValue> Values = new List<ImportValue>();
		}

		private sealed class ImportValue
		{
			public string Name, Text, Folder, Builtin, Asset;
		}

		public static int Import(string setPath, string dlcKey, string tonesTsv)
		{
			var data = DataFolder();
			var recordPath = Path.Combine(data, "SongTones", SafeKey(dlcKey) + ".rabsong.json");
			var record = ReadRecord(recordPath);
			string setSha = "";
			JObject json = null;
			try
			{
				byte[] setBytes;
				try { setBytes = File.ReadAllBytes(setPath); }
				catch (Exception exception) { throw new Refusal("The song's tone file could not be read (" + exception.Message + ")."); }
				setSha = Sha256(setBytes);

				// Everything is read and checked before anything is written: a refused set changes nothing but its record.
				var assets = new Dictionary<string, ImportAsset>(StringComparer.Ordinal);
				json = ReadSet(setBytes, assets);
				var song = ReadSong(dlcKey, tonesTsv);
				var tones = CheckSet(json, dlcKey, song, assets, data);

				var credits = WriteImport(data, dlcKey, tones, assets, record, out var presetHashes);
				var toneKeys = new JObject();
				foreach (var pair in ((JObject)json["toneKeys"]).Properties())
					toneKeys[pair.Name] = tones[(string)pair.Value["tone"]].PresetName;

				var songJson = json["song"] as JObject;
				SetRecord(record, setPath, setSha, "ready", null,
					(string)songJson?["title"] ?? song.Title, (string)songJson?["artist"] ?? song.Artist, (string)songJson?["author"] ?? "");
				record["toneKeys"] = toneKeys;
				record["credits"] = credits;
				record["presetHashes"] = presetHashes;
				WriteText(recordPath, record.ToString(Formatting.Indented));
				Log("import " + dlcKey + ": " + setPath + ": " + tones.Count + " tone(s) for " + toneKeys.Count + " tone key(s)");
				return ExitDone;
			}
			catch (Refusal refusal)
			{
				var songJson = json?["song"] as JObject;
				SetRecord(record, setPath, setSha, "refused", refusal.Message, (string)songJson?["title"], (string)songJson?["artist"], (string)songJson?["author"]);
				// What an earlier import wrote no longer describes this file.
				record.Remove("toneKeys");
				record.Remove("credits");
				WriteText(recordPath, record.ToString(Formatting.Indented));
				Log("import " + dlcKey + ": " + setPath + " refused: " + refusal.Message);
				return ExitRefused;
			}
		}

		private static JObject ReadRecord(string path)
		{
			try { if (File.Exists(path)) return JObject.Parse(File.ReadAllText(path, Encoding.UTF8)); }
			catch (Exception exception) { Log("import record " + path + " unreadable, starting a new one: " + exception.Message); }
			return new JObject();
		}

		// Sets the record's own fields; anything else in it (the page's fromSong, useMine) is kept as it was.
		private static void SetRecord(JObject record, string setPath, string sha, string state, string reason, string title, string artist, string author)
		{
			record["source"] = Path.GetFullPath(setPath);
			record["sha256"] = sha;
			record["importedAt"] = DateTimeOffset.Now.ToString("o", CultureInfo.InvariantCulture);
			record["state"] = state;
			if (reason != null) record["reason"] = reason;
			else record.Remove("reason");
			if (title != null) record["title"] = title;
			if (artist != null) record["artist"] = artist;
			if (author != null) record["author"] = author;
		}

		// The ZIP: exactly tones.json, assets/<sha256>.<ext> and optional licenses/<sha256>.txt, every asset's hash
		// checked against its bytes.
		private static JObject ReadSet(byte[] setBytes, Dictionary<string, ImportAsset> assets)
		{
			JObject json = null;
			try
			{
				// System.IO.MemoryStream by full name: GUI/MemoryStream.cs defines an RSMods.MemoryStream that ZipArchive cannot read.
				using (var zip = new ZipArchive(new System.IO.MemoryStream(setBytes), ZipArchiveMode.Read))
				{
					var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
					foreach (var entry in zip.Entries)
					{
						var name = entry.FullName;
						if (!names.Add(name)) throw new Refusal("The song's tone file lists \"" + name + "\" twice.");
						if (name == "assets/" || name == "licenses/") continue;
						if (name == "tones.json")
						{
							if (entry.Length > MaxTonesJson) throw new Refusal("The song's tones.json is too large.");
							using (var reader = new StreamReader(entry.Open(), Encoding.UTF8))
							{
								try { json = JObject.Parse(reader.ReadToEnd()); }
								catch (JsonException exception) { throw new Refusal("The song's tones.json is not valid JSON (" + exception.Message + ")."); }
							}
							continue;
						}
						var asset = AssetEntry.Match(name);
						if (asset.Success)
						{
							if (entry.Length > MaxAsset) throw new Refusal("The file " + name + " in the song's tones is too large.");
							byte[] bytes;
							using (var input = entry.Open())
							using (var copy = new System.IO.MemoryStream())
							{
								input.CopyTo(copy);
								bytes = copy.ToArray();
							}
							var sha = asset.Groups[1].Value;
							if (Sha256(bytes) != sha) throw new Refusal("The file " + name + " in the song's tones is damaged: its contents do not match its name.");
							if (assets.ContainsKey(sha)) throw new Refusal("The song's tones hold the file " + sha + " twice.");
							assets[sha] = new ImportAsset { Sha256 = sha, Extension = asset.Groups[2].Value, Bytes = bytes };
							continue;
						}
						if (LicenceEntry.IsMatch(name)) continue;   // allowed by the layout; rights are not used
						throw new Refusal("The song's tone file holds \"" + name + "\", which a song tone set may not contain.");
					}
				}
			}
			catch (InvalidDataException exception)
			{
				throw new Refusal("The song's tone file is not a valid ZIP (" + exception.Message + ").");
			}
			if (json == null) throw new Refusal("The song's tone file has no tones.json.");
			return json;
		}

		// tones.json against the format, the song, the engine and the player's library. Returns the tones by id.
		private static Dictionary<string, ImportTone> CheckSet(JObject json, string dlcKey, SongInfo song, Dictionary<string, ImportAsset> assets, string data)
		{
			if ((string)json["format"] != Versions.Format) throw new Refusal("This is not a song tone set (format \"" + (string)json["format"] + "\").");
			var formatVersion = json["formatVersion"];
			if (formatVersion == null || formatVersion.Type != JTokenType.Integer) throw new Refusal("The song's tones have no format version.");
			if ((int)formatVersion != Versions.FormatVersion)
				throw new Refusal("This song's tones use format version " + (int)formatVersion + "; this version of Rocksmith Audio Bridge reads version " + Versions.FormatVersion + ". Update Rocksmith Audio Bridge to hear them.");
			if (!(json["requires"] is JObject requires)) throw new Refusal("The song's tones do not say which engine and library they need.");
			var needEngine = Whole(requires["engineVersion"], "requires.engineVersion");
			var needLibrary = Whole(requires["libraryVersion"], "requires.libraryVersion");
			if (needEngine > Versions.EngineVersion)
				throw new Refusal("This song's tones need Tone engine " + needEngine + "; you have " + Versions.EngineVersion + ". Update Rocksmith Audio Bridge to hear them.");
			if (needLibrary > Versions.LibraryVersion)
				throw new Refusal("This song's tones need Tone library " + needLibrary + "; you have " + Versions.LibraryVersion + ". Update Rocksmith Audio Bridge to hear them.");

			var setKey = (string)json["song"]?["dlcKey"];
			if (string.IsNullOrEmpty(setKey)) throw new Refusal("The song's tones do not name their song.");
			if (!string.Equals(setKey, dlcKey, StringComparison.OrdinalIgnoreCase))
				throw new Refusal("These tones were made for the song \"" + setKey + "\", not \"" + dlcKey + "\".");

			if (!(json["tones"] is JObject tonesJson)) throw new Refusal("The song's tones.json has no tones.");
			if (!(json["toneKeys"] is JObject toneKeys) || !toneKeys.HasValues) throw new Refusal("The song's tones.json maps no tone keys.");
			var assetsJson = json["assets"] as JObject;
			if (assetsJson == null) throw new Refusal("The song's tones.json has no assets list.");

			// Every listed asset is in the ZIP and every file in the ZIP is listed.
			foreach (var property in assetsJson.Properties())
			{
				if (!Sha256Text.IsMatch(property.Name)) throw new Refusal("The song's tones list an asset with a bad name (\"" + property.Name + "\").");
				if (!assets.TryGetValue(property.Name, out var asset)) throw new Refusal("The song's tones list the file " + property.Name + " but do not hold it.");
				var kind = (string)property.Value["kind"];
				if (kind != "nam" && kind != "ir") throw new Refusal("The song's tones hold a file of an unknown kind (\"" + kind + "\").");
				if ((kind == "nam") != (asset.Extension != ".wav")) throw new Refusal("The file " + property.Name + " in the song's tones is not the kind it says.");
				asset.Kind = kind;
				var fileName = (string)property.Value["fileName"] ?? "";
				asset.DisplayName = Path.GetFileNameWithoutExtension(fileName);
			}
			foreach (var sha in assets.Keys)
				if (assetsJson[sha] == null) throw new Refusal("The song's tones hold the file " + sha + " but do not list it.");

			foreach (var pair in toneKeys.Properties())
			{
				if (!song.ToneKeys.ContainsKey(pair.Name))
					throw new Refusal("These tones were made for another version of this chart: the song has no tone \"" + pair.Name + "\".");
				var tone = (string)pair.Value?["tone"];
				if (tone == null || tonesJson[tone] == null) throw new Refusal("The tone key \"" + pair.Name + "\" points to a tone the set does not have.");
			}

			var catalogue = ReadCatalogue(data);
			if (catalogue == null) Log("no Catalogue.txt in " + data + ": effects and parameters are not checked");
			var starters = ReadStarterManifest(data);
			var checkedBuiltins = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

			// Only the tones a tone key plays are checked and written.
			var used = new HashSet<string>(toneKeys.Properties().Select(p => (string)p.Value["tone"]), StringComparer.Ordinal);
			var tones = new Dictionary<string, ImportTone>(StringComparer.Ordinal);
			foreach (var property in tonesJson.Properties().Where(p => used.Contains(p.Name)))
			{
				var name = ((string)property.Value["name"] ?? "").Trim();
				if (name.Length == 0) throw new Refusal("A tone in the song's tones has no name.");
				if (!(property.Value["chain"] is JArray chain)) throw new Refusal("The tone \"" + name + "\" has no chain.");
				var tone = new ImportTone { Id = property.Name, Name = name };
				var outputDb = property.Value["outputDb"];
				if (outputDb != null && outputDb.Type != JTokenType.Null)
				{
					if (outputDb.Type != JTokenType.Float && outputDb.Type != JTokenType.Integer) throw new Refusal("The tone \"" + name + "\" has a bad level.");
					tone.OutputDb = (double)outputDb;
					if (tone.OutputDb < -40 || tone.OutputDb > 40) throw new Refusal("The tone \"" + name + "\" has a level outside -40 to +40 dB.");
				}
				foreach (var item in chain)
				{
					var effect = (string)item["effect"] ?? "";
					if (!Regex.IsMatch(effect, "^[A-Za-z0-9-]+$")) throw new Refusal("The tone \"" + name + "\" has an effect with a bad name (\"" + effect + "\").");
					if (catalogue != null && !catalogue.Types.ContainsKey(effect))
						throw new Refusal("The tone \"" + name + "\" uses the effect \"" + effect + "\", which this version of the Tone designer does not have.");
					var pedal = new ImportPedal { Effect = effect, Enabled = item["enabled"] == null || (item["enabled"].Type == JTokenType.Boolean ? (bool)item["enabled"] : throw new Refusal("The tone \"" + name + "\" has a bad on/off value for " + effect + ".")) };
					if (item["params"] is JObject parameters)
						foreach (var parameter in parameters.Properties())
						{
							if (parameter.Name == "Enabled") continue;
							if (!Regex.IsMatch(parameter.Name, "^[^\\s\"]+$")) throw new Refusal("The tone \"" + name + "\" has a parameter with a bad name (\"" + parameter.Name + "\").");
							var type = catalogue?.TypeOf(effect, parameter.Name);
							if (catalogue != null && type == null)
								throw new Refusal("The tone \"" + name + "\" sets \"" + parameter.Name + "\" on " + effect + ", which this version of the Tone designer does not have.");
							pedal.Values.Add(ToImportValue(name, effect, parameter, type, catalogue, assets, data, starters, checkedBuiltins));
						}
					else if (item["params"] != null && item["params"].Type != JTokenType.Null) throw new Refusal("The tone \"" + name + "\" has bad settings for " + effect + ".");
					tone.Pedals.Add(pedal);
				}
				tones[tone.Id] = tone;
			}
			return tones;
		}

		private static int Whole(JToken token, string field)
		{
			if (token == null || token.Type != JTokenType.Integer || (long)token < 0) throw new Refusal("The song's tones have a bad " + field + ".");
			return (int)Math.Min((long)token, int.MaxValue);
		}

		private static ImportValue ToImportValue(string tone, string effect, JProperty parameter, string type, Catalogue catalogue,
			Dictionary<string, ImportAsset> assets, string data, Dictionary<string, StarterFile> starters, HashSet<string> checkedBuiltins)
		{
			var value = new ImportValue { Name = parameter.Name };
			var token = parameter.Value;
			if (token is JObject file)
			{
				var folder = FileFolder(catalogue, effect, parameter.Name);
				if (folder == null || (type != null && type != "File"))
					throw new Refusal("The tone \"" + tone + "\" gives a file for \"" + parameter.Name + "\" on " + effect + ", which takes no file.");
				value.Folder = folder;
				if (file["builtin"] != null)
				{
					var builtin = (string)file["builtin"] ?? "";
					if (folder == "Reverb" || builtin.Length == 0 || builtin.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
						throw new Refusal("The tone \"" + tone + "\" names a built-in file the mod does not ship (\"" + builtin + "\").");
					var key = folder + "/" + builtin;
					if (checkedBuiltins.Add(key))
					{
						var path = FindFile(data, folder, builtin);
						if (path == null || !starters.TryGetValue(key, out var shipped))
							throw new Refusal("This song's tones need \"" + builtin + "\" from Tone library " + Versions.LibraryVersion + " or later, which is not installed. Update or repair Rocksmith Audio Bridge to hear them.");
						if (Sha256File(path) != shipped.Sha256)
							throw new Refusal("This song's tones need the built-in \"" + builtin + "\", but your copy is not the one the mod ships. Repair Rocksmith Audio Bridge to hear them.");
					}
					value.Builtin = builtin;
				}
				else
				{
					var sha = ((string)file["asset"] ?? "").ToLowerInvariant();
					if (!assets.TryGetValue(sha, out var asset))
						throw new Refusal("The tone \"" + tone + "\" uses the file " + sha + ", which the set does not hold.");
					if ((asset.Kind == "nam") != (folder == "NAM"))
						throw new Refusal("The tone \"" + tone + "\" puts the file " + sha + " into a pedal of the wrong kind.");
					if (string.IsNullOrWhiteSpace(asset.DisplayName)) asset.DisplayName = ((string)file["name"] ?? "").Trim();
					asset.Folders.Add(folder);
					value.Asset = sha;
				}
				return value;
			}
			switch (token.Type)
			{
				case JTokenType.Boolean: value.Text = (bool)token ? "1" : "0"; break;
				case JTokenType.Integer:
				case JTokenType.Float:
					var number = (double)token;
					if (double.IsNaN(number) || double.IsInfinity(number)) throw new Refusal("The tone \"" + tone + "\" has a bad value for \"" + parameter.Name + "\" on " + effect + ".");
					value.Text = FormatNumber(number);
					break;
				case JTokenType.String:
					var text = (string)token;
					if (text.Length == 0 || text.IndexOfAny(new[] { '\r', '\n', '"' }) >= 0)
						throw new Refusal("The tone \"" + tone + "\" has a bad value for \"" + parameter.Name + "\" on " + effect + ".");
					value.Text = text;
					break;
				default:
					throw new Refusal("The tone \"" + tone + "\" has a bad value for \"" + parameter.Name + "\" on " + effect + ".");
			}
			return value;
		}

		// Writes the assets (NAM\, Cabinets\ or Reverb\), then the presets. Returns the record's credits.
		// Works out every name before writing anything, so a set that cannot be placed is refused with nothing half
		// written. `record` is the song's previous import record: a preset it wrote is replaced only while it is still
		// exactly as written; one the player saved over, or their own of that name, is kept and the set's goes beside it.
		private static JArray WriteImport(string data, string dlcKey, Dictionary<string, ImportTone> tones, Dictionary<string, ImportAsset> assets,
			JObject record, out JObject presetHashes)
		{
			var credits = new JArray();
			var pending = new List<KeyValuePair<string, byte[]>>();   // path, bytes: written once every name is known
			var writtenBefore = record["presetHashes"] as JObject ?? new JObject();
			presetHashes = new JObject();
			var starters = ReadStarterManifest(data);
			foreach (var asset in assets.Values.Where(a => a.Folders.Count > 0))
			{
				var display = SafeFileName(string.IsNullOrWhiteSpace(asset.DisplayName) ? asset.Sha256.Substring(0, 8) : asset.DisplayName);
				foreach (var folder in asset.Folders)
				{
					var directory = Path.Combine(data, folder);
					Directory.CreateDirectory(directory);
					var name = display;
					var existing = FindFile(data, folder, name);
					// Same name and same bytes: reuse it. A different file of that name: keep it, use "<name> (<hash>)".
					if (existing != null && !(Path.GetExtension(existing).Equals(asset.Extension, StringComparison.OrdinalIgnoreCase) && Sha256File(existing) == asset.Sha256))
					{
						name = display + " (" + asset.Sha256.Substring(0, 8) + ")";
						existing = FindFile(data, folder, name);
						if (existing != null && Sha256File(existing) != asset.Sha256)
							throw new Refusal("Two other files in " + folder + " are already called " + display + " and " + name + "; rename one and choose the song again.");
					}
					if (existing == null) pending.Add(new KeyValuePair<string, byte[]>(Path.Combine(directory, name + asset.Extension), asset.Bytes));
					asset.WrittenAs[folder] = name;
				}
				credits.Add(new JObject { ["name"] = asset.WrittenAs.Values.First(), ["kind"] = asset.Kind, ["builtin"] = false });
			}

			var presetNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
			var builtinsCredited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
			var presets = Path.Combine(data, "Presets");
			Directory.CreateDirectory(presets);
			foreach (var tone in tones.Values)
			{
				var baseName = SafeFileName(dlcKey + " - " + tone.Name);
				var presetName = baseName;
				for (int n = 2; !presetNames.Add(presetName) || !IsOurs(Path.Combine(presets, presetName), (string)writtenBefore[presetName]); n++)
					presetName = baseName + " (" + n + ")";
				tone.PresetName = presetName;

				// Instances numbered as the Tone designer page numbers them: Delay, Delay_2, ...
				var pedals = new List<Pedal>();
				var taken = new HashSet<string>(StringComparer.Ordinal);
				foreach (var source in tone.Pedals)
				{
					var id = source.Effect;
					for (int n = 2; !taken.Add(id); n++) id = source.Effect + "_" + n;
					var pedal = new Pedal { Id = id, Effect = source.Effect, Enabled = source.Enabled };
					foreach (var value in source.Values)
					{
						string text = value.Text;
						if (value.Builtin != null)
						{
							text = "\"" + value.Builtin + "\"";
							if (builtinsCredited.Add(value.Folder + "/" + value.Builtin))
								credits.Add(new JObject
								{
									["name"] = value.Builtin, ["kind"] = KindFor(value.Folder), ["builtin"] = true,
									["author"] = starters.TryGetValue(value.Folder + "/" + value.Builtin, out var shipped) ? shipped.Author : "",
								});
						}
						else if (value.Asset != null) text = "\"" + assets[value.Asset].WrittenAs[value.Folder] + "\"";
						pedal.Values.Add(new KeyValuePair<string, string>(value.Name, text));
					}
					pedals.Add(pedal);
				}
				// The tone's level, so a song's tones match: a Level pedal at the end (Stompbox has no preset level).
				if (Math.Abs(tone.OutputDb) > 1e-9)
				{
					var id = "Level";
					for (int n = 2; !taken.Add(id); n++) id = "Level_" + n;
					var level = new Pedal { Id = id, Effect = "Level" };
					level.Values.Add(new KeyValuePair<string, string>("Volume", FormatNumber(tone.OutputDb)));
					pedals.Add(level);
				}
				var presetBytes = new UTF8Encoding(false).GetBytes(WritePreset(presetName, pedals));
				presetHashes[presetName] = Sha256(presetBytes);
				pending.Add(new KeyValuePair<string, byte[]>(Path.Combine(presets, presetName), presetBytes));
			}
			foreach (var write in pending) WriteBytes(write.Key, write.Value);
			return credits;
		}

		// A preset path the import may write: free, or still exactly what an earlier import of this song wrote.
		private static bool IsOurs(string path, string writtenHash)
		{
			if (!File.Exists(path)) return true;
			return !string.IsNullOrEmpty(writtenHash) && Sha256File(path) == writtenHash;
		}

		// ---- Files --------------------------------------------------------------------------------------------

		private static void WriteText(string path, string text) => WriteBytes(path, new UTF8Encoding(false).GetBytes(text));

		private static void WriteBytes(string path, byte[] bytes)
		{
			Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)));
			var temp = path + ".tmp";
			File.WriteAllBytes(temp, bytes);
			if (File.Exists(path)) File.Replace(temp, path, null);
			else File.Move(temp, path);
		}
	}
}
