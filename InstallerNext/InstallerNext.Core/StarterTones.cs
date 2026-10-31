#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;

namespace RocksmithAudioBridge.Setup
{
	/// <summary>
	/// The Tone designer's starter content (Installer/StarterTones.cs in 4.x, same behaviour and file names): captures,
	/// cabinet IRs and genre tones from the pinned tones release, put into the engine's data folder
	/// %APPDATA%\RocksmithAudioBridge\Stompbox. The player's files always win: an engine file already there is never
	/// overwritten. Every file written is recorded with its SHA-256 in StarterTones-installed.txt, and Remove deletes only
	/// the ones still exactly as installed.
	/// </summary>
	public static class StarterTones
	{
		public const string RecordName = "StarterTones-installed.txt";
		private const string InfoFolder = "StarterTones";
		private static readonly string[] EngineFolders = { "NAM", "Cabinets", "Presets" };

		public static string DefaultEngineDataFolder =>
			Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "RocksmithAudioBridge", "Stompbox");

		/// <summary>Installs the archive; in a dry run only logs what it would write. Returns the number of files written.</summary>
		public static int Install(string zipPath, string engineDataFolder, IInstallHost host)
		{
			var record = ReadRecord(engineDataFolder);
			int written = 0, kept = 0;
			using (var archive = ZipFile.OpenRead(zipPath))
			{
				foreach (var entry in archive.Entries)
				{
					if (entry.FullName.EndsWith("/")) continue;
					string relative = TargetFor(entry.FullName);
					string target = Path.Combine(engineDataFolder, relative);
					bool isEngineFile = EngineFolders.Contains(relative.Split('\\')[0], StringComparer.OrdinalIgnoreCase);
					if (isEngineFile && File.Exists(target)) { kept++; continue; }
					written++;
					if (host.IsDryRun) continue;
					byte[] data;
					using (var stream = entry.Open())
					using (var buffer = new MemoryStream())
					{
						stream.CopyTo(buffer);
						data = buffer.ToArray();
					}
					Directory.CreateDirectory(Path.GetDirectoryName(target)!);
					File.WriteAllBytes(target, data);
					record[relative] = Sha256(data);
				}
			}
			if (host.IsDryRun) host.Log("    [dry run] write " + written + " starter files into " + engineDataFolder + " (" + kept + " already there are kept)");
			else WriteRecord(engineDataFolder, record);
			return written;
		}

		/// <summary>Deletes the starter files still exactly as installed and returns the ones that could not be removed.</summary>
		public static List<string> Remove(string engineDataFolder, IInstallHost host)
		{
			var failed = new List<string>();
			var record = ReadRecord(engineDataFolder);
			foreach (var file in record)
			{
				string full = Path.Combine(engineDataFolder, file.Key);
				try
				{
					if (File.Exists(full) && Sha256(File.ReadAllBytes(full)) == file.Value) host.DeleteFile(full);
				}
				catch (Exception error) when (error is IOException or UnauthorizedAccessException) { failed.Add(full); }
			}
			try { host.DeleteFile(Path.Combine(engineDataFolder, RecordName)); }
			catch (Exception error) when (error is IOException or UnauthorizedAccessException) { failed.Add(Path.Combine(engineDataFolder, RecordName)); }

			// Only folders that are now empty: the player's own models, IRs, tones and song tones stay.
			foreach (string folder in new[] { Path.Combine(InfoFolder, "LICENSES"), InfoFolder }.Concat(EngineFolders).Concat(new[] { "" }))
			{
				try { host.DeleteDirectory(Path.Combine(engineDataFolder, folder), onlyIfEmpty: true); }
				catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
			}
			return failed;
		}

		// "NAM/Plexi Crunch.nam" stays where the engine reads it; everything else (manifest.json, ATTRIBUTION.md,
		// LICENSES/, README.md) goes in StarterTones\.
		internal static string TargetFor(string entryName)
		{
			if (entryName.Contains("..") || Path.IsPathRooted(entryName)) throw new InvalidDataException("The starter tones archive has an unsafe path: " + entryName);
			string relative = entryName.Replace('/', '\\');
			return EngineFolders.Contains(relative.Split('\\')[0], StringComparer.OrdinalIgnoreCase) ? relative : Path.Combine(InfoFolder, relative);
		}

		private static Dictionary<string, string> ReadRecord(string engineDataFolder)
		{
			var record = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
			string path = Path.Combine(engineDataFolder, RecordName);
			if (!File.Exists(path)) return record;
			foreach (string line in File.ReadAllLines(path))
			{
				int tab = line.LastIndexOf('\t');
				if (tab > 0) record[line.Substring(0, tab)] = line.Substring(tab + 1);
			}
			return record;
		}

		private static void WriteRecord(string engineDataFolder, Dictionary<string, string> record)
		{
			Directory.CreateDirectory(engineDataFolder);
			File.WriteAllLines(Path.Combine(engineDataFolder, RecordName),
				record.OrderBy(file => file.Key, StringComparer.OrdinalIgnoreCase).Select(file => file.Key + "\t" + file.Value));
		}

		private static string Sha256(byte[] data) => Convert.ToHexString(SHA256.HashData(data));
	}
}
