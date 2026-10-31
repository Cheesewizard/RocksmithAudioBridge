using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using SevenZip;

namespace RocksmithAudioBridge.Core
{
	// The two SevenZipSharp helpers the Note by Note menu installer uses, ported from GUI/Util/ZipUtils.cs (originally
	// written by cozy1). The native 7z64.dll is copied beside the app from upstream's RuntimeAssets.
	internal static class ZipUtilities
	{
		private static readonly object setupLock = new();
		private static bool initialized;

		/// <summary>Extracts one file of the archive into targetDir (by its file name). False on any failure.</summary>
		public static bool ExtractSingleFile(string targetDir, string zipFile, string fileName)
		{
			SetupLibrary();
			try
			{
				using var extractor = new SevenZipExtractor(zipFile);
				var filePath = Path.Combine(targetDir, Path.GetFileName(fileName));
				using var fs = new FileStream(filePath, FileMode.Create);
				extractor.ExtractFile(fileName, fs);
			}
			catch
			{
				return false;
			}
			return true;
		}

		/// <summary>Replaces (or adds) internalArchivePath in the archive with sourceFilePath. Throws IOException on failure.</summary>
		public static bool InjectFile(string sourceFilePath, string archiveName, string internalArchivePath = "", OutArchiveFormat archiveFormat = OutArchiveFormat.Zip, CompressionMode compressionMode = CompressionMode.Append)
		{
			SetupLibrary();
			try
			{
				if (string.IsNullOrEmpty(internalArchivePath))
					internalArchivePath = Path.GetFileName(sourceFilePath);

				// Find and delete the existing file in the archive.
				ReadOnlyCollection<string> names;
				using (var extractor = new SevenZipExtractor(archiveName))
					names = extractor.ArchiveFileNames;

				var updateFile = new Dictionary<int, string>();
				if (names != null)
				{
					for (int index = 0; index < names.Count; index++)
						if (names[index].Equals(internalArchivePath))
						{
							updateFile.Add(index, null);
							break;
						}
				}

				var compressor = new SevenZipCompressor
				{
					ArchiveFormat = archiveFormat,
					CompressionMode = compressionMode,
					TempFolderPath = Path.GetTempPath(),
				};
				if (updateFile.Count == 1)
					compressor.ModifyArchive(archiveName, updateFile);

				compressor.CompressFileDictionary(new Dictionary<string, string> { [internalArchivePath] = sourceFilePath }, archiveName);
			}
			catch (Exception ex)
			{
				throw new IOException($"File injection failed: {ex.Message}", ex);
			}
			return true;
		}

		private static void SetupLibrary()
		{
			lock (setupLock)
			{
				if (initialized) return;
				SevenZipBase.SetLibraryPath(Path.Combine(AppContext.BaseDirectory, "7z64.dll"));
				initialized = true;
			}
		}
	}
}
