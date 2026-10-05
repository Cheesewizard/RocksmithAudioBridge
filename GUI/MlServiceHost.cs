using System;
using System.IO;
using RSModsPlus.MachineLearning;

namespace RSMods
{
	internal static class MlServiceHost
	{
		public static bool TryRun(string[] arguments, out int exitCode)
		{
			exitCode = 0;
			if (arguments.Length == 0 || arguments[0] != "--ml-service") return false;
			var directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RSModsPlus", "Logs");
			StreamWriter log = null;
			try
			{
				Directory.CreateDirectory(directory);
				var path = Path.Combine(directory, "ml-service.log");
				log = new StreamWriter(new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite));
				log.AutoFlush = true;
				if (arguments.Length != 2 || !int.TryParse(arguments[1], out var parentId)) throw new ArgumentException("Usage: RSMods.exe --ml-service <game process id>");
				MlService.Run(parentId, message =>
				{
					if (log.BaseStream.Length > 2 * 1024 * 1024)
					{
						log.Dispose();
						File.Copy(path, path + ".previous", true);
						log = new StreamWriter(new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite));
						log.AutoFlush = true;
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
