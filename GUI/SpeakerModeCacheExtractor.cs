using Rocksmith2014PsarcLib.Psarc;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;

namespace RSMods
{
	internal static class SpeakerModeCacheExtractor
	{
		private const string COMMAND = "--speaker-cache-extract";

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
				Extract(
					arguments[1],
					arguments[2],
					arguments[3]);
				DeleteIfPresent(errorPath);
			}
			catch (Exception exception)
			{
				var errorDirectory = Path.GetDirectoryName(errorPath);
				if (!string.IsNullOrEmpty(errorDirectory))
				{
					Directory.CreateDirectory(errorDirectory);
				}

				File.WriteAllText(errorPath, exception.ToString());
				exitCode = 1;
			}

			return true;
		}

		private static void Extract(
			string rocksmithDirectory,
			string bankName,
			string wavePath)
		{
			if (!Directory.Exists(rocksmithDirectory))
			{
				throw new DirectoryNotFoundException($"Rocksmith directory does not exist: {rocksmithDirectory}");
			}

			var source = FindSource(rocksmithDirectory, bankName);
			var outputDirectory = Path.GetDirectoryName(wavePath);
			if (string.IsNullOrEmpty(outputDirectory))
			{
				throw new ArgumentException("The decoded WAV path must have a parent directory.", nameof(wavePath));
			}

			Directory.CreateDirectory(outputDirectory);
			var temporaryPrefix = Path.Combine(outputDirectory, Guid.NewGuid().ToString("N"));
			var wemPath = temporaryPrefix + ".wem";
			var cleanedWemPath = temporaryPrefix + ".clean.wem";
			var oggPath = temporaryPrefix + ".ogg";
			var repairedOggPath = temporaryPrefix + ".fixed.ogg";
			var temporaryWavePath = temporaryPrefix + ".wav";

			try
			{
				uint wwiseFrameCount;
				using (var archive = new PsarcFile(source.ArchivePath))
				using (var wemOutput = File.Create(wemPath))
				{
					var wemEntry = archive.TOC.Entries.Single(entry => entry.Path == source.WemEntryPath);
					archive.InflateEntry(wemEntry, wemOutput);
				}
				wwiseFrameCount = ReadWwiseFrameCount(wemPath);

				// ww2ogg rejects any RIFF chunk it does not recognize ("Parse error: unknown chunk
				// type"). DLC Builder on Wwise 2022/2023 adds newer metadata chunks (e.g. "akd "),
				// which the shipped decoder does not know, so Speaker Mode fails on those songs even
				// though Rocksmith plays them. Rewrite the WEM keeping only the chunks ww2ogg reads
				// (its complete recognized set), dropping the rest. No behavioural change for legacy
				// WEMs, which carry only known chunks.
				WriteWw2oggCompatibleWem(wemPath, cleanedWemPath);

				var toolsDirectory = Path.Combine(RuntimeBootstrap.DirectoryPath, "tools");
				RunTool(
					Path.Combine(toolsDirectory, "ww2ogg.exe"),
					$"\"{cleanedWemPath}\" -o \"{oggPath}\" --pcb \"{Path.Combine(toolsDirectory, "packed_codebooks_aoTuV_603.bin")}\"");
				RunTool(
					Path.Combine(toolsDirectory, "revorb.exe"),
					$"\"{oggPath}\" \"{repairedOggPath}\"");
				RunTool(
					Path.Combine(toolsDirectory, "oggdec.exe"),
					$"-Q -b 16 -e 0 -s 1 -o \"{temporaryWavePath}\" \"{repairedOggPath}\"");
				TrimWaveToFrames(temporaryWavePath, wwiseFrameCount);

				CopyOver(temporaryWavePath, wavePath);
			}
			finally
			{
				DeleteIfPresent(wemPath);
				DeleteIfPresent(cleanedWemPath);
				DeleteIfPresent(oggPath);
				DeleteIfPresent(repairedOggPath);
				DeleteIfPresent(temporaryWavePath);
			}
		}

		private static uint ReadWwiseFrameCount(string wemPath)
		{
			using (var stream = File.OpenRead(wemPath))
			using (var reader = new BinaryReader(stream))
			{
				if (new string(reader.ReadChars(4)) != "RIFF")
				{
					throw new InvalidDataException("Speaker Mode WEM is not a RIFF file.");
				}

				reader.ReadUInt32();
				if (new string(reader.ReadChars(4)) != "WAVE")
				{
					throw new InvalidDataException("Speaker Mode WEM is not a WAVE container.");
				}

				while (stream.Position + 8 <= stream.Length)
				{
					var chunkId = new string(reader.ReadChars(4));
					var chunkSize = reader.ReadUInt32();
					var chunkStart = stream.Position;
					if (chunkId == "fmt ")
					{
						if (chunkSize < 28)
						{
							throw new InvalidDataException("Speaker Mode WEM format chunk has no Wwise frame count.");
						}

						stream.Position = chunkStart + 24;
						var frameCount = reader.ReadUInt32();
						if (frameCount == 0)
						{
							throw new InvalidDataException("Speaker Mode WEM reports zero frames.");
						}

						return frameCount;
					}

					stream.Position = chunkStart + chunkSize + (chunkSize & 1);
				}
			}

			throw new InvalidDataException("Speaker Mode WEM has no format chunk.");
		}

		// Rewrites a WEM keeping only the RIFF chunks the shipped ww2ogg recognizes, dropping any
		// newer Wwise metadata chunks that would abort its parse. Chunk bytes are copied verbatim
		// (contents unchanged); only the top-level chunk set and the RIFF size are rebuilt, which is
		// all ww2ogg walks. A legacy WEM (only known chunks) is reproduced unchanged apart from a
		// recomputed RIFF size.
		private static void WriteWw2oggCompatibleWem(string sourceWemPath, string destinationWemPath)
		{
			var known = new HashSet<string>(StringComparer.Ordinal)
			{
				"fmt ", "cue ", "LIST", "smpl", "vorb", "data"
			};

			byte[] bytes = File.ReadAllBytes(sourceWemPath);
			if (bytes.Length < 12
				|| System.Text.Encoding.ASCII.GetString(bytes, 0, 4) != "RIFF"
				|| System.Text.Encoding.ASCII.GetString(bytes, 8, 4) != "WAVE")
			{
				throw new InvalidDataException("Speaker Mode WEM is not a RIFF/WAVE container.");
			}

			var kept = new List<byte[]>();
			long position = 12;
			while (position + 8 <= bytes.Length)
			{
				string chunkId = System.Text.Encoding.ASCII.GetString(bytes, (int)position, 4);
				uint chunkSize = BitConverter.ToUInt32(bytes, (int)position + 4);
				long dataStart = position + 8;
				if (chunkSize > bytes.Length - dataStart)
				{
					// A truncated trailing chunk: clamp so a malformed tail cannot walk off the end.
					chunkSize = (uint)(bytes.Length - dataStart);
				}

				long advance = 8 + chunkSize + (chunkSize & 1); // chunks are word-aligned
				if (known.Contains(chunkId))
				{
					int total = (int)Math.Min(advance, bytes.Length - position);
					var chunk = new byte[total];
					Array.Copy(bytes, (int)position, chunk, 0, total);
					kept.Add(chunk);
				}

				position += advance;
			}

			if (!kept.Any(chunk => System.Text.Encoding.ASCII.GetString(chunk, 0, 4) == "data"))
			{
				throw new InvalidDataException("Speaker Mode WEM has no audio data chunk after cleaning.");
			}

			long body = 4; // "WAVE"
			foreach (var chunk in kept)
			{
				body += chunk.Length;
			}

			using (var output = File.Create(destinationWemPath))
			using (var writer = new BinaryWriter(output))
			{
				writer.Write(System.Text.Encoding.ASCII.GetBytes("RIFF"));
				writer.Write((uint)body);
				writer.Write(System.Text.Encoding.ASCII.GetBytes("WAVE"));
				foreach (var chunk in kept)
				{
					writer.Write(chunk);
				}
			}
		}

		private static void TrimWaveToFrames(string wavePath, uint frameCount)
		{
			using (var stream = File.Open(wavePath, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
			using (var reader = new BinaryReader(stream, System.Text.Encoding.ASCII, true))
			using (var writer = new BinaryWriter(stream, System.Text.Encoding.ASCII, true))
			{
				if (new string(reader.ReadChars(4)) != "RIFF")
				{
					throw new InvalidDataException("Decoded Speaker Mode audio is not RIFF WAVE.");
				}

				reader.ReadUInt32();
				if (new string(reader.ReadChars(4)) != "WAVE")
				{
					throw new InvalidDataException("Decoded Speaker Mode audio is not RIFF WAVE.");
				}

				ushort channels = 0;
				ushort bitsPerSample = 0;
				long dataSizeOffset = 0;
				long dataOffset = 0;
				uint decodedDataSize = 0;
				while (stream.Position + 8 <= stream.Length)
				{
					var chunkId = new string(reader.ReadChars(4));
					var chunkSize = reader.ReadUInt32();
					var chunkStart = stream.Position;
					if (chunkId == "fmt ")
					{
						var format = reader.ReadUInt16();
						channels = reader.ReadUInt16();
						reader.ReadUInt32();
						reader.ReadUInt32();
						reader.ReadUInt16();
						bitsPerSample = reader.ReadUInt16();
						if (format != 1 || channels != 2 || bitsPerSample != 16)
						{
							throw new InvalidDataException("Speaker Mode decoder did not produce stereo 16-bit PCM.");
						}
					}
					else if (chunkId == "data")
					{
						dataSizeOffset = chunkStart - 4;
						dataOffset = chunkStart;
						decodedDataSize = chunkSize;
						break;
					}

					stream.Position = chunkStart + chunkSize + (chunkSize & 1);
				}

				if (channels == 0 || bitsPerSample == 0 || dataOffset == 0)
				{
					throw new InvalidDataException("Decoded Speaker Mode WAV has incomplete chunks.");
				}

				var expectedDataSize = checked(frameCount * channels * bitsPerSample / 8);
				if (decodedDataSize < expectedDataSize)
				{
					throw new InvalidDataException(
						$"Speaker Mode decoder produced {decodedDataSize} bytes; Wwise requires {expectedDataSize}.");
				}

				stream.Position = dataSizeOffset;
				writer.Write(expectedDataSize);
				stream.SetLength(dataOffset + expectedDataSize);
				stream.Position = 4;
				writer.Write(checked((uint)(stream.Length - 8)));
			}
		}

		private static SpeakerCacheSource FindSource(string rocksmithDirectory, string bankName)
		{
			var archivePaths = Directory
				.EnumerateFiles(Path.Combine(rocksmithDirectory, "dlc"), "*_p.psarc", SearchOption.AllDirectories)
				.ToList();
			var songsArchive = Path.Combine(rocksmithDirectory, "songs.psarc");
			if (File.Exists(songsArchive))
			{
				archivePaths.Add(songsArchive);
			}

			foreach (var archivePath in archivePaths)
			{
				using (var archive = new PsarcFile(archivePath))
				{
					var bankEntry = archive.TOC.Entries.FirstOrDefault(entry =>
						PsarcEntryPath.TryGetFileName(entry.Path, out string entryFileName)
						&& string.Equals(
							entryFileName,
							bankName,
							StringComparison.OrdinalIgnoreCase));
					if (bankEntry == null)
					{
						continue;
					}

					byte[] bankBytes;
					using (var bankStream = new MemoryStream())
					{
						archive.InflateEntry(bankEntry, bankStream);
						bankBytes = bankStream.ToArray();
					}

					var matchingWems = archive.TOC.Entries.Where(entry =>
					{
						if (!PsarcEntryPath.TryGetWemMediaId(entry.Path, out uint mediaId))
						{
							return false;
						}

						return Contains(bankBytes, BitConverter.GetBytes(mediaId));
					}).ToList();

					if (matchingWems.Count != 1)
					{
						throw new InvalidDataException(
							$"Bank {bankName} maps to {matchingWems.Count} WEM entries in {archivePath}; expected exactly one.");
					}

					return new SpeakerCacheSource(archivePath, matchingWems[0]);
				}
			}

			throw new FileNotFoundException($"Could not find Wwise bank {bankName} in the installed Rocksmith archives.");
		}

		private static bool Contains(byte[] source, byte[] value)
		{
			for (var sourceIndex = 0; sourceIndex <= source.Length - value.Length; sourceIndex++)
			{
				var matches = true;
				for (var valueIndex = 0; valueIndex < value.Length; valueIndex++)
				{
					if (source[sourceIndex + valueIndex] == value[valueIndex])
					{
						continue;
					}

					matches = false;
					break;
				}

				if (matches)
				{
					return true;
				}
			}

			return false;
		}

		private static void RunTool(string executablePath, string arguments)
		{
			if (!File.Exists(executablePath))
			{
				throw new FileNotFoundException("Speaker Mode decode tool is missing.", executablePath);
			}

			var startInfo = new ProcessStartInfo
			{
				FileName = executablePath,
				Arguments = arguments,
				CreateNoWindow = true,
				UseShellExecute = false,
				RedirectStandardError = true,
				RedirectStandardOutput = true
			};

			using (var process = Process.Start(startInfo))
			{
				if (process == null)
				{
					throw new InvalidOperationException($"Could not start {Path.GetFileName(executablePath)}.");
				}

				var standardOutput = process.StandardOutput.ReadToEnd();
				var standardError = process.StandardError.ReadToEnd();
				process.WaitForExit();
				if (process.ExitCode != 0)
				{
					throw new InvalidOperationException(
						$"{Path.GetFileName(executablePath)} failed with exit code {process.ExitCode}. {standardOutput} {standardError}");
				}
			}
		}

		private static void CopyOver(string sourcePath, string destinationPath)
		{
			File.Copy(sourcePath, destinationPath, true);
		}

		private static void DeleteIfPresent(string path)
		{
			if (File.Exists(path))
			{
				File.Delete(path);
			}
		}
	}

	internal sealed class SpeakerCacheSource
	{
		public string ArchivePath { get; }
		public string WemEntryPath { get; }

		public SpeakerCacheSource(string archivePath, PsarcTOCEntry wemEntry)
		{
			ArchivePath = Path.GetFullPath(archivePath);
			WemEntryPath = wemEntry.Path;
		}
	}
}
