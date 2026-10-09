using Newtonsoft.Json;
using Rocksmith2014PsarcLib.Psarc;
using RocksmithToolkitLib;
using RocksmithToolkitLib.Sng2014HSL;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace RSMods
{
	internal static class NoteByNoteChartExtractor
	{
		private const string COMMAND = "--note-by-note-chart-extract";

		public static bool TryRun(string[] arguments, out int exitCode)
		{
			exitCode = 0;
			if (arguments.Length == 0 || !string.Equals(arguments[0], COMMAND, StringComparison.Ordinal))
			{
				return false;
			}

			if (arguments.Length != 5)
			{
				exitCode = 2;
				return true;
			}

			var errorPath = arguments[4];
			try
			{
				Extract(arguments[1], arguments[2], arguments[3]);
				DeleteIfPresent(errorPath);
			}
			catch (Exception exception)
			{
				EnsureParentDirectory(errorPath);
				File.WriteAllText(errorPath, exception.ToString());
				exitCode = 1;
			}

			return true;
		}

		private static void Extract(string rocksmithDirectory, string songKey, string outputPath)
		{
			if (!Directory.Exists(rocksmithDirectory))
			{
				throw new DirectoryNotFoundException($"Rocksmith directory does not exist: {rocksmithDirectory}");
			}

			if (string.IsNullOrWhiteSpace(songKey))
			{
				throw new ArgumentException("A song key is required.", nameof(songKey));
			}

			var archivePaths = Directory
				.EnumerateFiles(Path.Combine(rocksmithDirectory, "dlc"), "*_p.psarc", SearchOption.AllDirectories)
				.ToList();
			var songsArchive = Path.Combine(rocksmithDirectory, "songs.psarc");
			if (File.Exists(songsArchive))
			{
				archivePaths.Add(songsArchive);
			}

			var matchingDocuments = new List<NoteByNoteChartDocument>();
			foreach (var archivePath in archivePaths)
			{
				using (var archive = new PsarcFile(archivePath))
				{
					var matchingEntries = archive.TOC.Entries
						.Where(entry => IsArrangementEntry(entry.Path, songKey))
						.ToList();
					if (matchingEntries.Count == 0)
					{
						continue;
					}

					matchingDocuments.Add(new NoteByNoteChartDocument
					{
						SongKey = songKey,
						ArchivePath = archivePath,
						Arrangements = matchingEntries
							.Select(entry => ExtractArrangement(archive, entry.Path, songKey))
							.OrderBy(arrangement => arrangement.Name, StringComparer.Ordinal)
							.ToList()
					});
				}
			}

			if (matchingDocuments.Count == 0)
			{
				throw new FileNotFoundException($"Could not find playable SNG arrangements for song key {songKey}.");
			}

			var arrangementSignature = JsonConvert.SerializeObject(
				matchingDocuments[0].Arrangements,
				Formatting.None);
			if (matchingDocuments.Skip(1).Any(candidate =>
				!string.Equals(
					arrangementSignature,
					JsonConvert.SerializeObject(candidate.Arrangements, Formatting.None),
					StringComparison.Ordinal)))
			{
				throw new InvalidDataException(
					$"Song key {songKey} has conflicting arrangements in more than one Rocksmith archive.");
			}

			var document = matchingDocuments
				.OrderBy(candidate => Path.GetFullPath(candidate.ArchivePath).Length)
				.ThenBy(candidate => candidate.ArchivePath, StringComparer.OrdinalIgnoreCase)
				.First();

			EnsureParentDirectory(outputPath);
			File.WriteAllText(outputPath, JsonConvert.SerializeObject(document, Formatting.None));
		}

		private static NoteByNoteArrangementChart ExtractArrangement(
			PsarcFile archive,
			string entryPath,
			string songKey)
		{
			var entry = archive.TOC.Entries.Single(candidate => candidate.Path == entryPath);
			using (var stream = new MemoryStream())
			{
				archive.InflateEntry(entry, stream);
				stream.Position = 0;
				var sng = Sng2014File.ReadSng(
					stream,
					new Platform(GamePlatform.Pc, GameVersion.RS2014));

				return new NoteByNoteArrangementChart
				{
					Name = GetArrangementName(entryPath, songKey),
					StringCount = sng.Metadata.StringCount,
					Tuning = sng.Metadata.Tuning,
					CapoFret = sng.Metadata.CapoFretId == byte.MaxValue ? 0 : sng.Metadata.CapoFretId,
					MaxDifficulty = sng.Metadata.MaxDifficulty,
					MaxDifficultyEvents = BuildMaxDifficultyEvents(sng),
					Chords = sng.Chords.Chords
						.Select((chord, id) => new NoteByNoteChordTemplate
						{
							Id = id,
							Frets = chord.Frets,
							MidiNotes = chord.Notes
						})
						.ToList(),
					Difficulties = sng.Arrangements.Arrangements
						.Select(arrangement => new NoteByNoteDifficultyChart
						{
							Difficulty = arrangement.Difficulty,
							Events = arrangement.Notes.Notes
								.Select(CreateChartEvent)
								.ToList()
						})
						.OrderBy(difficulty => difficulty.Difficulty)
						.ToList()
				};
			}
		}

		private static List<NoteByNoteChartEvent> BuildMaxDifficultyEvents(Sng2014File sng)
		{
			var events = new List<NoteByNoteChartEvent>();
			for (var iterationId = 0; iterationId < sng.PhraseIterations.PhraseIterations.Length; iterationId++)
			{
				var iteration = sng.PhraseIterations.PhraseIterations[iterationId];
				var phrase = sng.Phrases.Phrases[iteration.PhraseId];
				var arrangement = sng.Arrangements.Arrangements.SingleOrDefault(
					candidate => candidate.Difficulty == phrase.MaxDifficulty);
				if (arrangement == null)
				{
					throw new InvalidDataException(
						$"Phrase {iteration.PhraseId} requires missing difficulty {phrase.MaxDifficulty}.");
				}

				events.AddRange(arrangement.Notes.Notes
					.Where(note => note.PhraseIterationId == iterationId)
					.Select(CreateChartEvent));
			}

			return events.OrderBy(note => note.Time).ToList();
		}

		private static NoteByNoteChartEvent CreateChartEvent(Notes note)
		{
			return new NoteByNoteChartEvent
			{
				Time = note.Time,
				PhraseIterationId = note.PhraseIterationId,
				StringIndex = note.StringIndex,
				Fret = note.FretId,
				ChordId = note.ChordId,
				ChordNotesId = note.ChordNotesId,
				NoteMask = note.NoteMask,
				Sustain = note.Sustain
			};
		}

		private static bool IsArrangementEntry(string entryPath, string songKey)
		{
			if (!PsarcEntryPath.TryGetFileName(entryPath, out string fileName)) return false;
			if (!fileName.EndsWith(".sng", StringComparison.OrdinalIgnoreCase)) return false;

			var prefix = songKey + "_";
			return fileName.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)
				&& !fileName.EndsWith("_vocals.sng", StringComparison.OrdinalIgnoreCase)
				&& entryPath.StartsWith("songs/bin/generic/", StringComparison.OrdinalIgnoreCase);
		}

		private static string GetArrangementName(string entryPath, string songKey)
		{
			if (!PsarcEntryPath.TryGetFileName(entryPath, out string fileName))
			{
				throw new InvalidDataException($"SNG entry has no file name: {entryPath}");
			}

			var prefixLength = songKey.Length + 1;
			return fileName.Substring(prefixLength, fileName.Length - prefixLength - ".sng".Length);
		}

		private static void EnsureParentDirectory(string path)
		{
			var directory = Path.GetDirectoryName(path);
			if (string.IsNullOrEmpty(directory))
			{
				throw new ArgumentException("Output paths must have a parent directory.", nameof(path));
			}

			Directory.CreateDirectory(directory);
		}

		private static void DeleteIfPresent(string path)
		{
			if (File.Exists(path))
			{
				File.Delete(path);
			}
		}
	}

	internal sealed class NoteByNoteChartDocument
	{
		public string SongKey { get; set; }
		public string ArchivePath { get; set; }
		public List<NoteByNoteArrangementChart> Arrangements { get; set; }
	}

	internal sealed class NoteByNoteArrangementChart
	{
		public string Name { get; set; }
		public int StringCount { get; set; }
		public short[] Tuning { get; set; }
		public int CapoFret { get; set; }
		public int MaxDifficulty { get; set; }
		public List<NoteByNoteChartEvent> MaxDifficultyEvents { get; set; }
		public List<NoteByNoteChordTemplate> Chords { get; set; }
		public List<NoteByNoteDifficultyChart> Difficulties { get; set; }
	}

	internal sealed class NoteByNoteChordTemplate
	{
		public int Id { get; set; }
		public byte[] Frets { get; set; }
		public int[] MidiNotes { get; set; }
	}

	internal sealed class NoteByNoteDifficultyChart
	{
		public int Difficulty { get; set; }
		public List<NoteByNoteChartEvent> Events { get; set; }
	}

	internal sealed class NoteByNoteChartEvent
	{
		public float Time { get; set; }
		public int PhraseIterationId { get; set; }
		public byte StringIndex { get; set; }
		public byte Fret { get; set; }
		public int ChordId { get; set; }
		public int ChordNotesId { get; set; }
		public uint NoteMask { get; set; }
		public float Sustain { get; set; }
	}
}
