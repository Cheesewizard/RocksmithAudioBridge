using RocksmithAudioBridge.Setup;

namespace RocksmithAudioBridge.Setup.Cli;

internal static class Program
{
	public static int Main(string[] args)
	{
		if (args.Length == 0)
		{
			Console.WriteLine(CommandLine.Usage);
			return 64;
		}
		return CommandLine.Run(args, Console.WriteLine);
	}
}
