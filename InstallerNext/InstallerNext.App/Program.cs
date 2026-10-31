using System.Runtime.InteropServices;
using Avalonia;

namespace RocksmithAudioBridge.Setup.App;

internal static class Program
{
	[STAThread]
	public static int Main(string[] args)
	{
		// --dry-run, --install, --uninstall, --validate-manifest: the console runner, printing into the console this
		// GUI-subsystem exe was started from.
		if (CommandLine.IsCommand(args))
		{
			AttachConsole(-1);
			return CommandLine.Run(args, Console.WriteLine);
		}
		return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
	}

	public static AppBuilder BuildAvaloniaApp() =>
		AppBuilder.Configure<App>()
			.UsePlatformDetect()
			.WithInterFont()
			.LogToTrace();

	[DllImport("kernel32.dll")]
	private static extern bool AttachConsole(int processId);
}
