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

		// The desktop bridge window is retired, but the game DLL and the overlay still write these values one line
		// at a time, so this window keeps the settings sync: re-read the bridge-owned values whenever the
		// window is activated, so a later full save from here cannot revert what the bridge wrote.
		private void AddAudioRoutingButton()
		{
			Activated += (sender, args) => SyncBridgeOwnedSettingsFromDisk();
		}
	}
}
