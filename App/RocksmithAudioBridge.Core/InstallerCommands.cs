namespace RocksmithAudioBridge.Core
{
	/// <summary>
	/// The command-line verbs the 4.x RSMods.exe answered for the installer, kept with the same names and exit codes so
	/// Installer/Uninstaller.cs can call either exe:
	///   --install-audio-bridge-driver "&lt;game&gt;" [--elevated]   register the ASIO proxy (+ UsbDk), one UAC prompt
	///   --restore-rs-asio-ini "&lt;game&gt;"                         put the player's own RS_ASIO.ini back
	///   --install-note-by-note-menu / --uninstall-note-by-note-menu "&lt;game&gt;"
	/// The app's own elevated action (Output page, Repair driver) relaunches the exe with the first verb.
	/// </summary>
	public static class InstallerCommands
	{
		public static bool TryRun(string[] arguments, out int exitCode)
		{
			if (AsioProxySetup.TryRunInstallCommand(arguments, out exitCode)) return true;
			if (RsAsioIniSetup.TryRunRestoreCommand(arguments, out exitCode)) return true;
			if (NoteByNoteMenuInstaller.TryRun(arguments, out exitCode)) return true;
			return false;
		}
	}
}
