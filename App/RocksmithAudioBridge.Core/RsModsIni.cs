#nullable enable
using System;
using System.IO;
using RSMods;
using RSMods.Util;

namespace RocksmithAudioBridge.Core
{
	/// <summary>
	/// Our settings in &lt;game&gt;\RSMods.ini, read and written with upstream's IniManager (GUI.Core), which keeps every
	/// other line of the file as it was. Opened with fillDefaults off: the game DLL and upstream's configurator own the
	/// file too, so reading never seeds defaults and only a value the player changes is written.
	///
	/// Each write reloads the file first, sets the one key and saves, so a change the overlay made meanwhile is kept
	/// (the 4.x GUI used WritePrivateProfileString per key for the same reason). After a save the running game is asked
	/// to re-read its settings ("update all" over WM_COPYDATA, as upstream's configurator does); nothing happens when the
	/// game is closed.
	/// </summary>
	public sealed class RsModsIni
	{
		private readonly bool notifyGame;

		/// <param name="notifyGame">False keeps a save from messaging a running game (tests).</param>
		public RsModsIni(string gameDirectory, bool notifyGame = true)
		{
			FilePath = Path.Combine(gameDirectory, "RSMods.ini");
			this.notifyGame = notifyGame;
		}

		public string FilePath { get; }

		public bool Exists => File.Exists(FilePath);

		private static string Section(string section) => section.StartsWith('[') ? section : "[" + section + "]";

		/// <summary>A fresh read of one value; the default when the file, section or key is missing.</summary>
		public string Get(string section, string key, string defaultValue = "")
		{
			var ini = new IniManager(FilePath, fillDefaults: false);
			ini.Load();
			return ini.TryGetString(Section(section), key, out string value) ? value : defaultValue;
		}

		/// <summary>A snapshot for reading many values at once (the Settings page).</summary>
		public IniManager Snapshot()
		{
			var ini = new IniManager(FilePath, fillDefaults: false);
			ini.Load();
			return ini;
		}

		public void Set(string section, string key, string value) => SetMany((section, key, value));

		/// <summary>Writes the values in one save. Throws IOException when the file can't be written.</summary>
		public void SetMany(params (string Section, string Key, string Value)[] values)
		{
			var ini = new IniManager(FilePath, fillDefaults: false);
			ini.Load();
			if (ini.LoadError != null) throw new IOException("RSMods.ini could not be read, so nothing was written: " + ini.LoadError.Message, ini.LoadError);
			foreach (var (section, key, value) in values) ini.SetString(Section(section), key, value);
			if (ini.Save() && notifyGame) NotifyGame();
		}

		/// <summary>Asks a running Rocksmith to re-read RSMods.ini. Safe to call when the game is closed.</summary>
		public static void NotifyGame()
		{
			try { WinMsgUtil.SendMsgToRS("update all"); } catch { /* the game is not there or did not answer */ }
		}
	}

	/// <summary>[Audio Bridge] keys the manifest does not describe (the DLL reads them with GetPrivateProfile*).</summary>
	public static class BridgeSettings
	{
		public const string Section = "Audio Bridge";

		/// <summary>Bridge power. Mod/AudioBridge/Cable/CableInput.cpp: anything but 1 is off. Default on.</summary>
		public static bool ReadPower(RsModsIni ini) => ini.Get(Section, "MasterEnabled", "1") == "1";

		/// <summary>As the 4.x GUI: on writes MasterEnabled=1; off also clears the engine's Enabled flag.</summary>
		public static void WritePower(RsModsIni ini, bool enabled)
		{
			if (enabled) ini.Set(Section, "MasterEnabled", "1");
			else ini.SetMany((Section, "Enabled", "0"), (Section, "MasterEnabled", "0"));
		}

		/// <summary>Mod/Recorder/TakeRecorder.cpp Folder(): RecordingDirectory, else &lt;game&gt;\Recordings.</summary>
		public static string ReadRecordingFolder(RsModsIni ini, string gameDirectory)
		{
			string value = ini.Get(Section, "RecordingDirectory", "");
			return value.Length > 0 ? value : Path.Combine(gameDirectory, "Recordings");
		}

		public static void WriteRecordingFolder(RsModsIni ini, string folder) => ini.Set(Section, "RecordingDirectory", folder);

		/// <summary>TakeRecorder.cpp PreferVideo(): anything but "Audio" records video.</summary>
		public static bool ReadPreferVideo(RsModsIni ini) => !string.Equals(ini.Get(Section, "CaptureMode", "Video"), "Audio", StringComparison.OrdinalIgnoreCase);

		public static void WritePreferVideo(RsModsIni ini, bool video) => ini.Set(Section, "CaptureMode", video ? "Video" : "Audio");

		public static readonly string[] VideoQualities = { "High", "Standard", "Small" };

		/// <summary>TakeRecorder.cpp VideoQuality(): High, Standard or Small; anything else reads as High.</summary>
		public static string ReadVideoQuality(RsModsIni ini)
		{
			string value = ini.Get(Section, "VideoQuality", "High");
			foreach (string quality in VideoQualities)
				if (string.Equals(quality, value, StringComparison.OrdinalIgnoreCase)) return quality;
			return "High";
		}

		public static void WriteVideoQuality(RsModsIni ini, string quality) => ini.Set(Section, "VideoQuality", quality);
	}
}
