#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace RocksmithAudioBridge.Setup
{
	/// <summary>
	/// The staged release payload (scripts/Package-Release.ps1 writes it; the installer embeds it as payload.zip). Each
	/// top-level folder maps to one destination, so the plan can list every file it will write before writing any:
	///
	///   native\xinput1_3.dll, xinput1_3.pdb         -> &lt;game&gt; through upstream RsModsInstallation
	///   configurator\RSModsGUI.zip + .manifest.json -> &lt;game&gt;\RSMods through upstream RsModsInstallation
	///   rsmods\mods.manifest.json                   -> &lt;game&gt;\RSMods\mods.manifest.json (loose, after the payload)
	///   game\**                                     -> &lt;game&gt;\ (driver x86/x64, DriverHost64, VstHost, Stompbox, PedalSkin)
	///   helper\**                                   -> &lt;game&gt;\RSMods\AudioBridge\
	///   app\**                                      -> &lt;game&gt;\RocksmithAudioBridge\ (desktop app, self-contained publish)
	///   notices\**                                  -> &lt;game&gt;\RocksmithAudioBridge\ (LICENSE, NOTICE, THIRD-PARTY-NOTICES, licences)
	///   plugins\** (optional)                        -> &lt;game&gt;\RocksmithAudioBridge\Plugins\ (bundled NAM / Airwindows VST3,
	///                                                 bundled-plugins.json, their licences; separate files under their own licences)
	///   tones\StarterTones.zip (optional)           -> %APPDATA%\RocksmithAudioBridge\Stompbox
	/// </summary>
	public sealed class PayloadLayout
	{
		public const string NativeFolder = "native";
		public const string ConfiguratorFolder = "configurator";
		public const string RsModsFolder = "rsmods";
		public const string GameFolder = "game";
		public const string HelperFolder = "helper";
		public const string AppFolder = "app";
		public const string NoticesFolder = "notices";
		public const string TonesFolder = "tones";
		public const string PluginsFolder = "plugins";

		public const string ConfiguratorZip = "RSModsGUI.zip";
		public const string ConfiguratorManifest = "RSModsGUI.manifest.json";
		public const string StarterTonesZip = "StarterTones.zip";

		/// <summary>Payload files without which the install cannot run.</summary>
		public static readonly string[] RequiredFiles =
		{
			NativeFolder + @"\" + ProductLayout.NativeModFile,
			NativeFolder + @"\" + ProductLayout.NativeModPdb,
			ConfiguratorFolder + @"\" + ConfiguratorZip,
			ConfiguratorFolder + @"\" + ConfiguratorManifest,
			RsModsFolder + @"\mods.manifest.json",
			GameFolder + @"\" + ProductLayout.AsioDriver,
			GameFolder + @"\" + ProductLayout.AsioDriver64,
			GameFolder + @"\" + ProductLayout.DriverHost,
			GameFolder + @"\" + ProductLayout.VstHost,
			HelperFolder + @"\RocksmithAudioBridge.Helper.exe",
			AppFolder + @"\" + ProductLayout.AppExeName,
			NoticesFolder + @"\THIRD-PARTY-NOTICES.txt",
		};

		/// <summary>The copy groups: payload folder -> game-relative destination folder.</summary>
		public static readonly (string PayloadFolder, string GameRelativeDestination, string Title, string Replaces4x)[] CopyGroups =
		{
			(GameFolder, "", "Native pieces into the game folder (ASIO driver x86/x64, DriverHost64, VST host, Stompbox, PedalSkin)",
				"DLLStuff.InjectDLL: RocksmithAudioBridgeAsio(64).dll, DriverHost, VstHost, stompbox-capi.dll"),
			(HelperFolder, ProductLayout.HelperFolder, "Helper exe and its runtime (tools, ONNX Runtime, soxr, WindowCapture)",
				"none (new: these jobs ran inside the 4.x RSMods.exe)"),
			(AppFolder, ProductLayout.AppFolder, "Rocksmith Audio Bridge desktop app",
				"DLLStuff.InjectGUI: RSMods.exe (4.x WinForms fork) + RSMods\\RocksmithAudioBridge.dll"),
			(NoticesFolder, ProductLayout.AppFolder, "Licences and third-party notices",
				"DLLStuff.InjectGUI: LICENSE, NOTICE, VstHost/JUCE/Stompbox notices in RSMods\\"),
			(PluginsFolder, ProductLayout.AppFolder + @"\Plugins", "Bundled VST3 plug-ins (NAM, Airwindows) and their licences, when the payload has them",
				"none (new: 4.x had no bundled plug-ins)"),
		};

		public PayloadLayout(string root)
		{
			Root = Path.GetFullPath(root);
		}

		public string Root { get; }

		public string PathOf(string payloadRelative) => Path.Combine(Root, payloadRelative);

		public bool Has(string payloadRelative) => File.Exists(PathOf(payloadRelative));

		public IReadOnlyList<string> MissingRequired() => RequiredFiles.Where(f => !Has(f)).ToList();

		/// <summary>Every file under a payload folder, relative to that folder, sorted. Empty when the folder is missing.</summary>
		public IReadOnlyList<string> FilesUnder(string payloadFolder)
		{
			string folder = PathOf(payloadFolder);
			if (!Directory.Exists(folder)) return Array.Empty<string>();
			return Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories)
				.Select(f => Path.GetRelativePath(folder, f))
				.OrderBy(f => f, StringComparer.OrdinalIgnoreCase)
				.ToList();
		}

		/// <summary>The files each copy group expects at minimum, for a plan made without a payload (dry run from source).</summary>
		public static IEnumerable<string> ExpectedFilesUnder(string payloadFolder) =>
			RequiredFiles.Where(f => f.StartsWith(payloadFolder + @"\", StringComparison.OrdinalIgnoreCase))
				.Select(f => f.Substring(payloadFolder.Length + 1));
	}
}
