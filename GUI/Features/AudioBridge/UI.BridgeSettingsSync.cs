using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Windows.Forms;
using RSMods.Audio;
using RSMods.Util;

namespace RSMods
{
	public partial class MainForm
	{
		// The audio bridge process owns a set of RSMods.ini values (ReadSettings.BridgeOwnedIdentifiers)
		// and writes them one line at a time, because the shared WriteINI rewrites the whole file from
		// this window's in-memory table. Re-read every bridge-owned value on activation so the next full
		// save from here does not overwrite what the bridge wrote.
		internal static void SyncBridgeOwnedSettingsFromDisk()
		{
			foreach (string identifier in ReadSettings.BridgeOwnedIdentifiers)
			{
				string value = ReadSettings.ProcessSettings(identifier);
				if (!string.IsNullOrWhiteSpace(value))
					SyncBridgeOwnedSetting(identifier, value);
			}
		}

		private static void SyncBridgeOwnedSetting(string identifier, string value)
		{
			foreach (var section in WriteSettings.saveSettingsOrDefaults.Keys)
			{
				if (WriteSettings.saveSettingsOrDefaults[section].ContainsKey(identifier))
				{
					WriteSettings.saveSettingsOrDefaults[section][identifier] = value;
					return;
				}
			}
		}

		// The game DLL and the in-game overlay write these values one line at a time while this window may be open.
		// Re-read them whenever the window is activated (switching back from the game), so a later full save from
		// here cannot revert what the game wrote. Called once from InitializeRsModsPlusPages.
		private void WireBridgeSettingsSync()
		{
			Activated += (sender, args) => SyncBridgeOwnedSettingsFromDisk();
		}
	}
}
