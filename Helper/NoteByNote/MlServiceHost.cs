using System;
using System.IO;
using RSModsPlus.MachineLearning;

namespace RocksmithAudioBridge.Helper
{
	// --ml-service <game process id>: Note by Note's FretNet ML service (RSModsPlus.MachineLearning.MlService). Runs until
	// the game process exits. Logs to %LOCALAPPDATA%\Rocksmith Audio Bridge\Logs\ml-service.log. Exit code 0 on a clean
	// stop, 1 on any failure. The 4.x WinForms app compiles this file too (linked from GUI.csproj).
	internal static class MlServiceHost
	{
		// The helper cannot see the 4.x app's ProductInfo, so the log header text lives here.
		// Keep in step with GUI/App/ProductInfo.cs DISPLAY_NAME (PRODUCT_NAME + " " + VERSION).
		private const string DISPLAY_NAME = "Rocksmith Audio Bridge 4.3";

		public static bool TryRun(string[] arguments, out int exitCode)
		{
			exitCode = 0;
			if (arguments.Length == 0 || arguments[0] != "--ml-service") return false;
			var directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Rocksmith Audio Bridge", "Logs");
			StreamWriter log = null;
			try
			{
				Directory.CreateDirectory(directory);
				var path = Path.Combine(directory, "ml-service.log");
				log = new StreamWriter(new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite));
				log.AutoFlush = true;
				log.WriteLine(DISPLAY_NAME + " Note by Note ML service");
				if (arguments.Length != 2 || !int.TryParse(arguments[1], out var parentId)) throw new ArgumentException("Usage: " + AppDomain.CurrentDomain.FriendlyName + " --ml-service <game process id>");
				MlService.Run(parentId, message =>
				{
					if (log.BaseStream.Length > 2 * 1024 * 1024)
					{
						log.Dispose();
						File.Copy(path, path + ".previous", true);
						log = new StreamWriter(new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite));
						log.AutoFlush = true;
						log.WriteLine(DISPLAY_NAME + " Note by Note ML service");
					}
					log.WriteLine(DateTime.UtcNow.ToString("O") + " " + message);
				});
			}
			catch (Exception exception)
			{
				Console.Error.WriteLine(exception);
				log?.WriteLine(exception);
				exitCode = 1;
			}
			finally
			{
				log?.Dispose();
			}
			return true;
		}
	}
}
