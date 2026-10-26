using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Windows.Forms;

namespace RS2014_Mod_Installer
{
	// The Tone designer's starter content: amp and pedal captures, cabinet IRs and genre tones from a pinned release of
	// Cheesewizard/RocksmithAudioBridge-Tones (StarterTones.json; AcquireStarterTones.ps1 fetches it at build time),
	// put into the engine's data folder, %APPDATA%\RocksmithAudioBridge\Stompbox.
	//
	// The player's files always win: a file that is already there is never overwritten, so a starter tone saved over
	// in the Tone designer keeps the player's version. The release's manifest, attribution and licence texts go in
	// Stompbox\StarterTones\, which is ours alone. Every file written is recorded with its SHA-256 (RecordName), and
	// Remove deletes only the ones still exactly as installed.
	internal static class StarterTones
	{
		public const string RecordName = "StarterTones-installed.txt";
		private const string InfoFolder = "StarterTones";

		private static readonly string[] EngineFolders = { "NAM", "Cabinets", "Presets" };

		public static string EngineDataFolder => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "RocksmithAudioBridge", "Stompbox");

		private static string RecordPath => Path.Combine(EngineDataFolder, RecordName);

		public static bool Install()
		{
			try
			{
				var record = ReadRecord();
				using (var archive = new ZipArchive(new MemoryStream(Properties.Resources.StarterTones), ZipArchiveMode.Read))
				{
					foreach (var entry in archive.Entries)
					{
						if (entry.FullName.EndsWith("/")) continue;   // a folder
						var relative = TargetFor(entry.FullName);
						var target = Path.Combine(EngineDataFolder, relative);
						bool isEngineFile = EngineFolders.Contains(relative.Split('\\')[0], StringComparer.OrdinalIgnoreCase);
						if (isEngineFile && File.Exists(target)) continue;
						byte[] data;
						using (var stream = entry.Open())
						using (var buffer = new MemoryStream())
						{
							stream.CopyTo(buffer);
							data = buffer.ToArray();
						}
						Directory.CreateDirectory(Path.GetDirectoryName(target));
						File.WriteAllBytes(target, data);
						record[relative] = Sha256(data);
					}
				}
				WriteRecord(record);
				return true;
			}
			catch (UnauthorizedAccessException ex)
			{
				MessageBox.Show("Windows refused to write the Tone designer's starter tones (" + ex.Message + ").\nIf your antivirus blocked it, allow the installer and press this button again.", "Error: access denied", MessageBoxButtons.OK, MessageBoxIcon.Error);
				return false;
			}
			catch (IOException ex)
			{
				MessageBox.Show("The Tone designer's starter tones could not be written (" + ex.Message + ").\nClose Rocksmith and the Rocksmith Audio Bridge host, then press this button again.", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
				return false;
			}
		}

		// Deletes the starter files that are still exactly as installed and returns the ones that could not be removed.
		// A file the player changed stays.
		public static List<string> Remove()
		{
			var failed = new List<string>();
			var record = ReadRecord();
			foreach (var file in record)
			{
				var full = Path.Combine(EngineDataFolder, file.Key);
				try
				{
					if (File.Exists(full) && Sha256(File.ReadAllBytes(full)) == file.Value) File.Delete(full);
				}
				catch (IOException) { failed.Add(full); }
				catch (UnauthorizedAccessException) { failed.Add(full); }
			}
			try { if (File.Exists(RecordPath)) File.Delete(RecordPath); } catch (IOException) { failed.Add(RecordPath); }

			// Only folders that are now empty: the player's own models, IRs, tones and song tones stay.
			foreach (var folder in new[] { Path.Combine(InfoFolder, "LICENSES"), InfoFolder }.Concat(EngineFolders).Concat(new[] { "" }))
			{
				var full = Path.Combine(EngineDataFolder, folder);
				try { if (Directory.Exists(full) && !Directory.EnumerateFileSystemEntries(full).Any()) Directory.Delete(full); }
				catch (IOException) { }
				catch (UnauthorizedAccessException) { }
			}
			return failed;
		}

		// "NAM/Plexi Crunch.nam" stays where the engine reads it; everything else (manifest.json, ATTRIBUTION.md,
		// LICENSES/, README.md) goes in StarterTones\.
		private static string TargetFor(string entryName)
		{
			if (entryName.Contains("..") || Path.IsPathRooted(entryName)) throw new InvalidDataException("The starter tones archive has an unsafe path: " + entryName);
			var relative = entryName.Replace('/', '\\');
			return EngineFolders.Contains(relative.Split('\\')[0], StringComparer.OrdinalIgnoreCase) ? relative : Path.Combine(InfoFolder, relative);
		}

		private static Dictionary<string, string> ReadRecord()
		{
			var record = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
			if (!File.Exists(RecordPath)) return record;
			foreach (var line in File.ReadAllLines(RecordPath))
			{
				var tab = line.LastIndexOf('\t');
				if (tab > 0) record[line.Substring(0, tab)] = line.Substring(tab + 1);
			}
			return record;
		}

		private static void WriteRecord(Dictionary<string, string> record)
		{
			Directory.CreateDirectory(EngineDataFolder);
			File.WriteAllLines(RecordPath, record.OrderBy(file => file.Key, StringComparer.OrdinalIgnoreCase).Select(file => file.Key + "\t" + file.Value));
		}

		private static string Sha256(byte[] data)
		{
			using (var sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(data)).Replace("-", "");
		}
	}
}
