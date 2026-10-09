using Newtonsoft.Json.Linq;
using RocksmithToolkitLib.DLCPackage;
using RSMods.Util;
using SevenZip;
using System;
using System.Diagnostics;
using System.IO;

namespace RSMods
{
	internal static class NoteByNoteMenuInstaller
	{
		private const string INSTALL_ARGUMENT = "--install-note-by-note-menu";
		// Removes ONLY our NoteByNote entry from the CURRENT cache.psarc (used by the installer's Uninstall). A backup
		// is never restored: it predates anything another mod changed in cache.psarc after us, which would be lost.
		private const string UNINSTALL_ARGUMENT = "--uninstall-note-by-note-menu";
		private const string CACHE_DIRECTORY_NAME = "cache_psarc_RS2014_Pc";
		private const string CACHE_ARCHIVE_NAME = "cache7.7z";
		private const string MANIFEST_INTERNAL_PATH = "manifests\\ui_menu_pillar_learnasong.database.json";
		private const string MANIFEST_FILE_NAME = "ui_menu_pillar_learnasong.database.json";

		private const string DUMP_ARGUMENT = "--dump-note-by-note-menu";

		public static bool TryRun(string[] arguments, out int exitCode)
		{
			exitCode = 0;
			if (arguments.Length == 0
				|| (!string.Equals(arguments[0], INSTALL_ARGUMENT, StringComparison.OrdinalIgnoreCase)
					&& !string.Equals(arguments[0], UNINSTALL_ARGUMENT, StringComparison.OrdinalIgnoreCase)
					&& !string.Equals(arguments[0], DUMP_ARGUMENT, StringComparison.OrdinalIgnoreCase)))
			{
				return false;
			}

			try
			{
				if (string.Equals(arguments[0], DUMP_ARGUMENT, StringComparison.OrdinalIgnoreCase))
				{
					// Diagnostic: write the Learn a Song menu manifest out
					// of a cache.psarc so the shipped slider definitions can be compared with the
					// injected NoteByNote entry. Usage: --dump-note-by-note-menu <cache.psarc> <out.json>
					if (arguments.Length != 3)
					{
						throw new ArgumentException($"Usage: {DUMP_ARGUMENT} <cache.psarc> <output.json>");
					}

					DumpManifest(arguments[1], arguments[2]);
					return true;
				}

				if (arguments.Length != 2)
				{
					throw new ArgumentException($"Usage: {arguments[0]} <Rocksmith directory>");
				}

				Apply(arguments[1], add: !string.Equals(arguments[0], UNINSTALL_ARGUMENT, StringComparison.OrdinalIgnoreCase));
			}
			catch (Exception exception)
			{
				exitCode = 1;
				File.WriteAllText(
					Path.Combine(System.Windows.Forms.Application.StartupPath, "NoteByNoteMenuInstall.log"),
					exception.ToString());
			}

			return true;
		}

		// Adds (add = true) or removes our NoteByNote entry. Either way it works on the CURRENT cache.psarc, so whatever
		// other mods changed in it is kept, and it touches nothing but that one entry in one manifest.
		private static void Apply(string rocksmithDirectory, bool add)
		{
			if (Process.GetProcessesByName("Rocksmith2014").Length != 0)
			{
				throw new InvalidOperationException("Rocksmith must be closed before changing the Note by Note menu.");
			}

			var gameDirectory = Path.GetFullPath(rocksmithDirectory);
			var cachePath = Path.Combine(gameDirectory, "cache.psarc");
			if (!File.Exists(cachePath))
			{
				throw new FileNotFoundException("Rocksmith cache.psarc was not found.", cachePath);
			}

			// One quiet copy of the cache before our first-ever patch, only as a recovery file should a repack ever
			// produce a broken archive. Nothing restores it automatically (see UNINSTALL_ARGUMENT).
			var backupPath = Path.Combine(gameDirectory, "cache.pre-note-by-note-menu.psarc");
			if (add && !File.Exists(backupPath))
			{
				File.Copy(cachePath, backupPath);
			}
			var logPath = Path.Combine(System.Windows.Forms.Application.StartupPath, "NoteByNoteMenuInstall.log");

			var installDirectory = Path.Combine(
				Path.GetTempPath(),
				$"RSModsPlus_NoteByNoteMenu_{Guid.NewGuid():N}");
			Directory.CreateDirectory(installDirectory);

			try
			{
				Packer.Unpack(cachePath, installDirectory);
				var unpackedCacheDirectory = Path.Combine(installDirectory, CACHE_DIRECTORY_NAME);
				var cacheArchivePath = Path.Combine(unpackedCacheDirectory, CACHE_ARCHIVE_NAME);
				if (!File.Exists(cacheArchivePath))
				{
					throw new InvalidDataException("The unpacked cache does not contain cache7.7z.");
				}

				if (!ZipUtilities.ExtractSingleFile(
					installDirectory,
					cacheArchivePath,
					MANIFEST_INTERNAL_PATH))
				{
					throw new InvalidDataException("The Learn a Song menu manifest could not be extracted.");
				}

				var manifestPath = Path.Combine(installDirectory, MANIFEST_FILE_NAME);
				if (!add && !HasNoteByNoteToggle(manifestPath))
				{
					File.WriteAllText(logPath, "Note by Note was not in the Riff Repeater menu; cache.psarc left unchanged." + Environment.NewLine);
					return;
				}
				if (add) AddNoteByNoteToggle(manifestPath);
				else RemoveNoteByNoteToggle(manifestPath);
				if (!ZipUtilities.InjectFile(
					manifestPath,
					cacheArchivePath,
					MANIFEST_INTERNAL_PATH,
					OutArchiveFormat.SevenZip,
					CompressionMode.Append))
				{
					throw new InvalidDataException("The Note by Note menu manifest could not be injected.");
				}

				var pendingCachePath = Path.Combine(installDirectory, "cache.note-by-note.pending.psarc");
				Packer.Pack(unpackedCacheDirectory, pendingCachePath);
				if (!File.Exists(pendingCachePath) || new FileInfo(pendingCachePath).Length < 1024 * 1024)
				{
					throw new InvalidDataException("The rebuilt Rocksmith cache is missing or incomplete.");
				}

				File.Copy(pendingCachePath, cachePath, true);
				File.WriteAllText(
					logPath,
					(add ? "Installed Note by Note into Riff Repeater Advanced Settings." : "Removed Note by Note from Riff Repeater Advanced Settings.")
					+ Environment.NewLine
					+ $"Backup (recovery only, never restored automatically): {backupPath}{Environment.NewLine}"
					+ $"Cache size now: {new FileInfo(cachePath).Length}{Environment.NewLine}");
			}
			finally
			{
				Directory.Delete(installDirectory, true);
			}
		}

		private static void DumpManifest(string cachePath, string outputPath)
		{
			cachePath = Path.GetFullPath(cachePath);
			if (!File.Exists(cachePath))
			{
				throw new FileNotFoundException("cache.psarc was not found.", cachePath);
			}

			var workDirectory = Path.Combine(
				Path.GetTempPath(),
				$"RSModsPlus_NoteByNoteMenuDump_{Guid.NewGuid():N}");
			Directory.CreateDirectory(workDirectory);
			try
			{
				Packer.Unpack(cachePath, workDirectory);
				// The unpack folder is named after the archive file, so a backup such as
				// cache.pre-note-by-note-menu.psarc lands somewhere other than CACHE_DIRECTORY_NAME.
				var archives = Directory.GetFiles(workDirectory, CACHE_ARCHIVE_NAME, SearchOption.AllDirectories);
				if (archives.Length == 0)
				{
					throw new InvalidDataException("The unpacked cache does not contain cache7.7z.");
				}
				var cacheArchivePath = archives[0];
				// An output DIRECTORY receives the whole cache7.7z (every UI manifest), so the
				// shipped slider definitions in the other pillar manifests can be compared too.
				if (Directory.Exists(outputPath))
				{
					File.Copy(cacheArchivePath, Path.Combine(Path.GetFullPath(outputPath), CACHE_ARCHIVE_NAME), true);
					return;
				}
				if (!ZipUtilities.ExtractSingleFile(workDirectory, cacheArchivePath, MANIFEST_INTERNAL_PATH))
				{
					throw new InvalidDataException("The Learn a Song menu manifest could not be extracted.");
				}
				File.Copy(Path.Combine(workDirectory, MANIFEST_FILE_NAME), Path.GetFullPath(outputPath), true);
			}
			finally
			{
				Directory.Delete(workDirectory, true);
			}
		}

		private static JObject ReadAdvancedSettingsButtons(string manifestPath, out JObject root)
		{
			root = JObject.Parse(File.ReadAllText(manifestPath));
			var buttons = root["Static"]?["UI"]?["Menus"]?["Entries"]?
				["RiffRepeater_AdvancedSettings"]?["View"]?["Definition"]?["Buttons"] as JObject;
			if (buttons == null)
			{
				throw new InvalidDataException("RiffRepeater_AdvancedSettings.Buttons was not found.");
			}
			return buttons;
		}

		private static bool HasNoteByNoteToggle(string manifestPath)
		{
			var buttons = ReadAdvancedSettingsButtons(manifestPath, out _);
			return buttons.Property("NoteByNote") != null || buttons.Property(FLOW_ID) != null;
		}

		private static void RemoveNoteByNoteToggle(string manifestPath)
		{
			var buttons = ReadAdvancedSettingsButtons(manifestPath, out JObject root);
			buttons.Remove("NoteByNote");
			buttons.Remove(FLOW_ID);
			File.WriteAllText(manifestPath, root.ToString());
		}

		// FLOW MODE row under NOTE BY NOTE: the DLL renumbers it right after NOTE BY NOTE,
		// presets it to the live flow state and sets DisableAccess while Note by Note is off (NoteByNoteMenu.cpp).
		private const string FLOW_ID = "NoteByNoteFlow";

		private static void AddNoteByNoteToggle(string manifestPath)
		{
			var root = JObject.Parse(File.ReadAllText(manifestPath));
			var buttons = root["Static"]?["UI"]?["Menus"]?["Entries"]?
				["RiffRepeater_AdvancedSettings"]?["View"]?["Definition"]?["Buttons"] as JObject;
			if (buttons == null)
			{
				throw new InvalidDataException("RiffRepeater_AdvancedSettings.Buttons was not found.");
			}

			// Place NOTE BY NOTE as the LAST item with a contiguous SortOrder. The native
			// Riff Repeater menu builds its keyboard-navigation index from a contiguous
			// 0..N-1 SortOrder run (as in the shipped pillar manifests); a value outside
			// that run desyncs the cursor from the visible list. max(existing SortOrder)+1
			// appends it at the end and keeps the run contiguous. The existing NoteByNote entry (if any) is skipped so
			// re-installs stay idempotent instead of walking the index forward each run.
			int noteByNoteSortOrder = 0;
			foreach (var property in buttons.Properties())
			{
				if (string.Equals(property.Name, "NoteByNote", StringComparison.Ordinal)
					|| string.Equals(property.Name, FLOW_ID, StringComparison.Ordinal))
				{
					continue;
				}

				if (property.Value is JObject existingButton
					&& existingButton["SortOrder"] is JValue sortValue
					&& sortValue.Type == JTokenType.Integer)
				{
					int existingSortOrder = sortValue.Value<int>();
					if (existingSortOrder + 1 > noteByNoteSortOrder)
					{
						noteByNoteSortOrder = existingSortOrder + 1;
					}
				}
			}

			// The eight shipped Advanced Settings rows are not in this manifest at all: the
			// LAS_RiffRepeater controller creates them in code (this Buttons object is empty).
			// The game draws JSON rows before its code-built ones regardless of SortOrder, and
			// the cursor starts on the JSON row, so the contiguous-run value (0 with no JSON
			// siblings) keeps the visible and navigation orders in agreement.
			const int NATIVE_ADVANCED_SETTINGS_ROWS = 0;
			if (noteByNoteSortOrder < NATIVE_ADVANCED_SETTINGS_ROWS)
			{
				noteByNoteSortOrder = NATIVE_ADVANCED_SETTINGS_ROWS;
			}

			buttons["NoteByNote"] = new JObject
			{
				["ID"] = "NoteByNote",
				["Label"] = "NOTE BY NOTE",
				["State"] = "up",
				["SortOrder"] = noteByNoteSortOrder,
				["Component"] = "RSSlider",
				["Notched"] = true,
				["XScale"] = 100,
				["YScale"] = 80,
				["InitialValue"] = 0,
				["States"] = new JObject
				{
					["Default"] = true
				},
				["AcceptedValues"] = new JObject
				{
					["0"] = "$[23447]Off",
					["1"] = "$[23446]On"
				}
			};

			buttons[FLOW_ID] = new JObject
			{
				["ID"] = FLOW_ID,
				["Label"] = "FLOW MODE",
				["State"] = "up",
				["SortOrder"] = noteByNoteSortOrder + 1,
				["Component"] = "RSSlider",
				["Notched"] = true,
				["XScale"] = 100,
				["YScale"] = 80,
				["InitialValue"] = 1,
				["States"] = new JObject
				{
					["Default"] = true
				},
				["AcceptedValues"] = new JObject
				{
					["0"] = "$[23447]Off",
					["1"] = "$[23446]On"
				}
			};

			File.WriteAllText(manifestPath, root.ToString());
		}
	}
}
