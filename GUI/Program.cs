using System;
using System.Security.Principal;
using System.Threading;
using System.Windows.Forms;

namespace RSMods
{
	static class Program
	{
		[STAThread]
		static void Main(string[] arguments)
		{
			if (AppDomain.CurrentDomain.GetData("RSMods.GameDomain") is bool)
			{
				Start(arguments);
				return;
			}

			var executable = System.Reflection.Assembly.GetExecutingAssembly().Location;
			var setup = new AppDomainSetup
			{
				ApplicationBase = System.IO.Path.GetDirectoryName(executable),
				ConfigurationFile = AppDomain.CurrentDomain.SetupInformation.ConfigurationFile
			};
			var domain = AppDomain.CreateDomain("RSMods game runtime", null, setup);
			try
			{
				domain.SetData("RSMods.GameDomain", true);
				domain.ExecuteAssembly(executable, arguments);
			}
			finally
			{
				AppDomain.Unload(domain);
			}
		}

		private static void Start(string[] arguments)
		{
			try
			{
				RuntimeBootstrap.Initialize();
				Run(arguments);
			}
			catch (Exception exception)
			{
				Console.Error.WriteLine(exception);
				var logDirectory = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RSModsPlus", "Logs");
				try
				{
					System.IO.Directory.CreateDirectory(logDirectory);
					System.IO.File.WriteAllText(System.IO.Path.Combine(logDirectory, "startup-error.log"), DateTime.UtcNow.ToString("O") + Environment.NewLine + exception);
				}
				catch (Exception loggingException)
				{
					Console.Error.WriteLine(loggingException);
				}
				Environment.ExitCode = 1;
				// Hidden helpers (started by the game or the installer, no window) only log: a modal box there would sit
				// unseen during play while the caller waits on the process. The full error is in startup-error.log.
				if (!IsBackgroundAudioBridge(arguments) && !IsHiddenHelper(arguments))
					MessageBox.Show(exception.ToString(), "RSMods startup error");
			}
		}

		[System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
		private static void Run(string[] arguments)
		{
			// Hidden video take for the in-game overlay and record hotkey (no window, exits when the take ends).
			if (Audio.VideoTakeHost.TryRun(arguments, out int videoExitCode))
			{
				Environment.ExitCode = videoExitCode;
				return;
			}

			if (MlServiceHost.TryRun(arguments, out int serviceExitCode))
			{
				Environment.ExitCode = serviceExitCode;
				return;
			}

			if (MlModelVerifier.TryRun(arguments, out int modelExitCode))
			{
				Environment.ExitCode = modelExitCode;
				return;
			}

			if (NoteByNoteMenuInstaller.TryRun(arguments, out int menuInstallExitCode))
			{
				Environment.ExitCode = menuInstallExitCode;
				return;
			}

			if (NoteByNoteChartExtractor.TryRun(arguments, out int chartExitCode))
			{
				Environment.ExitCode = chartExitCode;
				return;
			}

			if (SpeakerModeCacheExtractor.TryRun(arguments, out int exitCode))
			{
				Environment.ExitCode = exitCode;
				return;
			}

			try
			{
				Application.EnableVisualStyles();
				Application.SetCompatibleTextRenderingDefault(false);

				// The in-game overlay replaces the desktop Audio Bridge window. An older game DLL may still pass
				// --audio-bridge; that launch exits without opening anything.
				if (arguments.Length > 0 && arguments[0] == "--audio-bridge")
					return;

				Application.Run(new MainForm());
			}
			catch (Exception ex)
			{
				if (IsBackgroundAudioBridge(arguments)) throw;
				MessageBox.Show(ex.Message + " " + ex, "Error");
			}
		}

		private static bool IsHiddenHelper(string[] arguments)
		{
			if (arguments.Length == 0) return false;
			switch (arguments[0])
			{
				case "--video-take":
				case "--ml-service":
				case "--speaker-cache-extract":
				case "--install-note-by-note-menu":
				case "--uninstall-note-by-note-menu":
					return true;
				default:
					return false;
			}
		}

		private static bool IsBackgroundAudioBridge(string[] arguments)
		{
			return arguments.Length >= 3 && arguments[0] == "--audio-bridge" && arguments[2] == "--rocksmith-pid";
		}
	}
}
