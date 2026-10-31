#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using RSMods.Core.Settings;

namespace RocksmithAudioBridge.Core
{
	/// <summary>
	/// Our mods' settings out of the mod manifest the game DLL dumps (rundll32 xinput1_3.dll,DumpManifest, written by
	/// Mod/Build-ModDll.ps1 to build\mod\mods.manifest.json). Parsed with upstream's own model (GUI.Core
	/// ManifestService / SettingDescriptor), then cut down to the settings whose owning "mod" is one of ours. Upstream's
	/// configurator shows the rest.
	/// </summary>
	public sealed class OurModsManifest
	{
		/// <summary>Owning-mod prefixes of Rocksmith Audio Bridge's external mods. Checked against the manifest: today it
		/// names AudioBridgeShellMod, AudioBridgeRoutingMod, AudioRecorderMod, DropPedalMod, InputConditionerMod and
		/// NoteByNoteMod; ToneDesigner, ExternalAmp and ProductWatermark declare no settings yet.</summary>
		public static readonly string[] OurModPrefixes =
		{
			"AudioBridge", "DropPedal", "NoteByNote", "ToneDesigner", "AudioRecorder", "ExternalAmp", "InputConditioner", "ProductWatermark",
		};

		public static bool IsOurs(SettingDescriptor setting) =>
			setting.Mod != null && OurModPrefixes.Any(prefix => setting.Mod.StartsWith(prefix, StringComparison.Ordinal));

		public string? SourcePath { get; }
		public IReadOnlyList<SettingDescriptor> Settings { get; }
		public string? LoadError { get; }

		private OurModsManifest(string? sourcePath, IReadOnlyList<SettingDescriptor> settings, string? loadError)
		{
			SourcePath = sourcePath;
			Settings = settings;
			LoadError = loadError;
		}

		public static OurModsManifest Parse(string json, string? sourcePath = null)
		{
			var all = new ManifestService(json).AllSettings;
			return new OurModsManifest(sourcePath, all.Where(IsOurs).ToList(), null);
		}

		/// <summary>The first manifest found: beside the app, then build\mod\ of a source checkout above it.</summary>
		public static OurModsManifest Load(params string?[] extraCandidates)
		{
			foreach (string path in Candidates(extraCandidates))
			{
				if (!File.Exists(path)) continue;
				try { return Parse(File.ReadAllText(path), path); }
				catch (Exception error) { return new OurModsManifest(path, Array.Empty<SettingDescriptor>(), error.Message); }
			}
			return new OurModsManifest(null, Array.Empty<SettingDescriptor>(), "No mods.manifest.json was found beside the app.");
		}

		private static IEnumerable<string> Candidates(string?[] extra)
		{
			foreach (string? path in extra)
				if (!string.IsNullOrWhiteSpace(path)) yield return path;
			yield return Path.Combine(AppContext.BaseDirectory, "mods.manifest.json");
			for (var folder = new DirectoryInfo(AppContext.BaseDirectory); folder != null; folder = folder.Parent)
				yield return Path.Combine(folder.FullName, "build", "mod", "mods.manifest.json");
		}

		public IEnumerable<SettingDescriptor> ForMod(string modPrefix) =>
			Settings.Where(s => s.Mod != null && s.Mod.StartsWith(modPrefix, StringComparison.Ordinal));
	}
}
