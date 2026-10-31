#nullable enable
using System;
using System.Collections.Generic;
using System.Linq;
using RSMods.Core.Settings;

namespace RocksmithAudioBridge.Setup
{
	/// <summary>
	/// Checks a dumped mods.manifest.json (rundll32 xinput1_3.dll,DumpManifest) before it ships. The loose manifest
	/// REPLACES the configurator's built-in one, and upstream's ManifestService silently falls back to the built-in one
	/// when the loose file does not parse or is empty, so a bad dump would hide every setting of ours without an error.
	/// Parsed with upstream's own model and options (GUI.Core ManifestService, linked into App/RocksmithAudioBridge.Core),
	/// so "parses here" means "parses in the configurator".
	/// </summary>
	public static class ModManifestValidator
	{
		/// <summary>The external mods that declare settings today (OurModsManifest's comment lists them). A missing one
		/// means the dump came from a DLL without our mods compiled in.</summary>
		public static readonly string[] RequiredMods =
		{
			"AudioBridgeShellMod", "AudioBridgeRoutingMod", "AudioRecorderMod", "DropPedalMod", "InputConditionerMod", "NoteByNoteMod",
		};

		public sealed class Result
		{
			public List<string> Errors { get; } = new();
			public List<string> Warnings { get; } = new();
			public int SettingCount { get; set; }
			public int OurSettingCount { get; set; }
			public bool Ok => Errors.Count == 0;
		}

		public static Result Validate(string json, IEnumerable<string>? requiredMods = null)
		{
			var result = new Result();
			IReadOnlyList<SettingDescriptor> settings;
			try
			{
				settings = new ManifestService(json).AllSettings;
			}
			catch (Exception error)
			{
				result.Errors.Add("does not parse with upstream's ManifestService: " + error.Message);
				return result;
			}
			result.SettingCount = settings.Count;
			if (settings.Count == 0)
			{
				result.Errors.Add("has no settings (the configurator would fall back to its built-in manifest)");
				return result;
			}

			for (int i = 0; i < settings.Count; i++)
			{
				var s = settings[i];
				string where = "setting #" + i + (string.IsNullOrWhiteSpace(s?.Key) ? "" : " (" + s!.Key + ")");
				if (s == null) { result.Errors.Add(where + " is null"); continue; }
				if (string.IsNullOrWhiteSpace(s.Key)) result.Errors.Add(where + " has no key");
				if (s.Ini == null) result.Errors.Add(where + " has no ini location");
				else
				{
					if (string.IsNullOrWhiteSpace(s.Ini.Section)) result.Errors.Add(where + " has no ini section");
					if (string.IsNullOrWhiteSpace(s.Ini.Name)) result.Errors.Add(where + " has no ini name");
				}
				if (!Enum.IsDefined(typeof(SettingType), s.Type)) result.Errors.Add(where + " has an unknown type " + s.Type);
				if (s.Type == SettingType.Enum && (s.Choices == null || s.Choices.Count == 0) && string.IsNullOrWhiteSpace(s.ChoicesSource))
					result.Errors.Add(where + " is an Enum with no choices and no choicesSource");
				if (s.Type == SettingType.Int && s.Min.HasValue && s.Max.HasValue && s.Min > s.Max)
					result.Errors.Add(where + " has min " + s.Min + " > max " + s.Max);
			}

			// The configurator looks keys up case-insensitively (ManifestService._byKey), so a case-only duplicate is one too.
			foreach (var duplicate in settings.Where(s => !string.IsNullOrWhiteSpace(s?.Key)).GroupBy(s => s.Key, StringComparer.OrdinalIgnoreCase).Where(g => g.Count() > 1))
				result.Errors.Add("key \"" + duplicate.Key + "\" is declared " + duplicate.Count() + " times");

			foreach (var shared in settings.Where(s => s?.Ini != null).GroupBy(s => s.Ini.Section + "]" + s.Ini.Name, StringComparer.OrdinalIgnoreCase).Where(g => g.Count() > 1))
				result.Warnings.Add("ini [" + shared.First().Ini.Section + "] " + shared.First().Ini.Name + " is written by " + string.Join(", ", shared.Select(s => s.Key)));

			var keys = new HashSet<string>(settings.Where(s => s?.Key != null).Select(s => s.Key), StringComparer.OrdinalIgnoreCase);
			foreach (var s in settings.Where(s => s?.VisibleWhen != null && !keys.Contains(s.VisibleWhen!.Key)))
				result.Warnings.Add(s.Key + " is visible when \"" + s.VisibleWhen!.Key + "\", which is not a setting");

			var mods = new HashSet<string>(settings.Where(s => s?.Mod != null).Select(s => s.Mod!), StringComparer.Ordinal);
			foreach (string mod in requiredMods ?? RequiredMods)
				if (!mods.Contains(mod)) result.Errors.Add("our mod " + mod + " declares no settings: was the DLL built with Mod/ compiled in?");
			result.OurSettingCount = settings.Count(s => s != null && RocksmithAudioBridge.Core.OurModsManifest.IsOurs(s));
			return result;
		}
	}
}
