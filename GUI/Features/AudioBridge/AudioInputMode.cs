using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;

namespace RSMods.Audio
{
	internal enum AudioInputMode
	{
		Cable,
		Asio,
		Unavailable
	}

	internal sealed class AudioInputModeConfiguration
	{
		private static readonly string[] asioFileNames = { "avrt.dll", "RS_ASIO.dll" };

		private readonly string gameDirectory;
		private readonly Func<bool> isGameRunning;

		public AudioInputModeConfiguration(string gameDirectory, Func<bool> isGameRunning = null)
		{
			if (string.IsNullOrWhiteSpace(gameDirectory)) throw new ArgumentException("Rocksmith folder is required.", nameof(gameDirectory));
			this.gameDirectory = Path.GetFullPath(gameDirectory);
			this.isGameRunning = isGameRunning ?? DetectGameRunning;
		}

		public AudioInputMode ReadMode()
		{
			int enabledFiles = 0;
			int disabledFiles = 0;
			foreach (string fileName in asioFileNames)
			{
				if (File.Exists(Path.Combine(gameDirectory, fileName))) enabledFiles++;
				if (File.Exists(Path.Combine(gameDirectory, fileName + ".disabled"))) disabledFiles++;
			}
			if (enabledFiles == 0 && disabledFiles == 0) return AudioInputMode.Unavailable;
			if ((enabledFiles != asioFileNames.Length || disabledFiles != 0)
				&& (disabledFiles != asioFileNames.Length || enabledFiles != 0))
				throw new InvalidOperationException("ASIO files are incomplete or conflicting. Expected both enabled files or both .disabled files.");
			return enabledFiles == asioFileNames.Length ? AudioInputMode.Asio : AudioInputMode.Cable;
		}

		public bool IsGameRunning()
		{
			return isGameRunning();
		}

		private bool DetectGameRunning()
		{
			foreach (var process in Process.GetProcessesByName("Rocksmith2014"))
			{
				using (process)
				{
					if (IsProcessInGameDirectory(process)) return true;
				}
			}
			return false;
		}

		private bool IsProcessInGameDirectory(Process process)
		{
			try
			{
				return string.Equals(Path.GetDirectoryName(process.MainModule.FileName), gameDirectory.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase);
			}
			catch (InvalidOperationException)
			{
				return false;
			}
			catch (Win32Exception error)
			{
				// ERROR_PARTIAL_COPY is the normal race when Rocksmith exits between enumeration and
				// MainModule. For any other inspection failure, keep the running-game protection active.
				return error.NativeErrorCode != 299;
			}
		}

		public void SetAsioEnabled(bool enabled)
		{
			using (var modeLock = new FileStream(Path.Combine(gameDirectory, ".rsmods-audio-mode.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
			{
				if (isGameRunning()) throw new InvalidOperationException("Close Rocksmith before changing ASIO / Cable mode.");
				var current = ReadMode();
				if (current == AudioInputMode.Unavailable)
					throw new InvalidOperationException("Install RS_ASIO before selecting an input mode.");
				if ((current == AudioInputMode.Asio) == enabled) return;

				string sourceSuffix = enabled ? ".disabled" : "";
				string destinationSuffix = enabled ? "" : ".disabled";
				var movedFiles = new List<KeyValuePair<string, string>>();
				try
				{
					foreach (string fileName in asioFileNames)
					{
						string source = Path.Combine(gameDirectory, fileName + sourceSuffix);
						string destination = Path.Combine(gameDirectory, fileName + destinationSuffix);
						File.Move(source, destination);
						movedFiles.Add(new KeyValuePair<string, string>(source, destination));
					}
				}
				catch
				{
					for (int index = movedFiles.Count - 1; index >= 0; --index)
						File.Move(movedFiles[index].Value, movedFiles[index].Key);
					throw;
				}
			}
		}

	}
}
