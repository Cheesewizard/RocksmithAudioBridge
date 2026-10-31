using Avalonia;
using RocksmithAudioBridge.App.Views;
using RocksmithAudioBridge.Core;

namespace RocksmithAudioBridge.App;

internal static class Program
{
	[STAThread]
	public static int Main(string[] args)
	{
		// The installer's verbs (driver registration, RS_ASIO.ini restore, Note by Note menu) run without a window,
		// with the same names and exit codes RSMods.exe 4.x used.
		if (InstallerCommands.TryRun(args, out int exitCode)) return exitCode;

		ScreenshotMode.Parse(args);
		return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
	}

	public static AppBuilder BuildAvaloniaApp() =>
		AppBuilder.Configure<App>()
			.UsePlatformDetect()
			.WithInterFont()
			.LogToTrace();
}
