using System;
using System.IO;

namespace RocksmithAudioBridge.Helper
{
	// Started by the game DLL with one job per run and no window. The exit code is the result; a job that fails writes
	// its own error file where the caller asked for one.
	internal static class Program
	{
		private const int EXIT_UNKNOWN_COMMAND = 64;

		[STAThread]
		private static int Main(string[] arguments)
		{
			var toolsDirectory = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "tools");

			if (SpeakerModeCacheExtractor.TryRun(arguments, toolsDirectory, out int exitCode))
			{
				return exitCode;
			}

			if (VideoTakeHost.TryRun(arguments, out exitCode))
			{
				return exitCode;
			}

			if (ToneDesignerTools.TryRun(arguments, out exitCode))
			{
				return exitCode;
			}

			if (MlServiceHost.TryRun(arguments, out exitCode))
			{
				return exitCode;
			}

			Console.Error.WriteLine("Unknown command: " + (arguments.Length == 0 ? "(none)" : arguments[0]));
			return EXIT_UNKNOWN_COMMAND;
		}
	}
}
