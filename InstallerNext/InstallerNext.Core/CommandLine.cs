#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;

namespace RocksmithAudioBridge.Setup
{
	/// <summary>
	/// The command line both shells share (InstallerNext.Cli, and the wizard exe when given arguments):
	///
	///   --dry-run --game &lt;folder&gt; [--uninstall] [--payload &lt;dir&gt;]   log the whole plan and every action, change nothing
	///   --install --game &lt;folder&gt; [--payload &lt;dir&gt;] [--replace-native-mod | --keep-native-mod] [--no-driver]
	///             [--no-menu] [--no-start-menu] [--desktop-shortcut] [--launch]
	///   --uninstall --game &lt;folder&gt; [--remove-settings] [--remove-usbdk | --keep-usbdk] [--keep-rsmods]
	///   --validate-manifest &lt;mods.manifest.json&gt;                    CI: exit 1 when the manifest would not work
	///   --print-plan ...                                              like --dry-run, plan only
	///
	/// Without --game the folder is detected (App's GameFolder search). Without --payload: a payload\ folder beside the
	/// exe, else the payload.zip embedded in the exe (extracted to %TEMP%).
	/// Exit codes: 0 ok, 1 failed, 2 cancelled (admin prompt declined), 3 blocked (plan not started), 64 usage.
	/// </summary>
	public static class CommandLine
	{
		public static bool IsCommand(string[] args) => args.Any(a => a.StartsWith("--", StringComparison.Ordinal));

		public static int Run(string[] args, Action<string> output, ISystemProbe? probe = null)
		{
			var flags = new HashSet<string>(args.Where(a => a.StartsWith("--")), StringComparer.OrdinalIgnoreCase);
			string? Value(string name)
			{
				int index = Array.FindIndex(args, a => a.Equals(name, StringComparison.OrdinalIgnoreCase));
				return index >= 0 && index + 1 < args.Length && !args[index + 1].StartsWith("--") ? args[index + 1] : null;
			}

			try
			{
				if (flags.Contains("--validate-manifest"))
				{
					string? path = Value("--validate-manifest");
					if (path == null) { output("Usage: --validate-manifest <mods.manifest.json>"); return 64; }
					var result = ModManifestValidator.Validate(File.ReadAllText(path));
					foreach (string warning in result.Warnings) output("warning: " + warning);
					foreach (string error in result.Errors) output("error: " + error);
					output((result.Ok ? "OK: " : "INVALID: ") + path + " (" + result.SettingCount + " settings, " + result.OurSettingCount + " of ours)");
					return result.Ok ? 0 : 1;
				}

				bool dryRun = flags.Contains("--dry-run") || flags.Contains("--print-plan");
				bool uninstall = flags.Contains("--uninstall");
				bool install = flags.Contains("--install") || (dryRun && !uninstall);
				if (!install && !uninstall) { output(Usage); return 64; }

				probe ??= new SystemProbe();
				string? game = Value("--game") ?? RocksmithAudioBridge.Core.GameFolder.Detect();
				if (string.IsNullOrWhiteSpace(game)) { output("Rocksmith 2014 was not found; pass --game <folder>."); return 64; }

				InstallPlan plan;
				using var payload = uninstall ? null : PayloadSource.Resolve(Value("--payload"), output);
				if (uninstall)
				{
					plan = new InstallPlanner(probe).PlanUninstall(new UninstallOptions
					{
						GameFolder = game,
						RemoveSettings = flags.Contains("--remove-settings"),
						RemoveUsbDk = flags.Contains("--remove-usbdk") ? true : flags.Contains("--keep-usbdk") ? false : null,
						KeepRsModsConfigurator = flags.Contains("--keep-rsmods"),
					});
				}
				else
				{
					plan = new InstallPlanner(probe).PlanInstall(new InstallOptions
					{
						GameFolder = game,
						Payload = payload!.Layout,
						ReplaceForeignNativeMod = flags.Contains("--replace-native-mod") ? true : flags.Contains("--keep-native-mod") ? false : null,
						RegisterDriver = !flags.Contains("--no-driver"),
						InstallMenuRow = !flags.Contains("--no-menu"),
						CreateStartMenuShortcut = !flags.Contains("--no-start-menu"),
						CreateDesktopShortcut = flags.Contains("--desktop-shortcut"),
						LaunchApp = flags.Contains("--launch"),
					});
				}

				if (flags.Contains("--print-plan"))
				{
					foreach (string line in plan.Describe()) output(line);
					return plan.Blockers.Count == 0 ? 0 : 3;
				}

				IInstallHost host = dryRun ? new DryRunInstallHost(output) : new SystemInstallHost(output);
				RunResult run = PlanRunner.Run(plan, host, probe);
				output(Summary(plan, run, dryRun));
				if (!dryRun && plan.Blockers.Count > 0) return 3;
				return run.Succeeded ? 0 : run.Cancelled ? 2 : 1;
			}
			catch (Exception error)
			{
				output("error: " + error.Message);
				return 1;
			}
		}

		public static string Summary(InstallPlan plan, RunResult run, bool dryRun) =>
			"Summary: " + plan.Operation + (dryRun ? " (dry run)" : "") + ", " + plan.Steps.Count + " steps, " + plan.Blockers.Count + " blocker(s), "
			+ run.Warnings.Count + " warning(s)" + (run.Succeeded ? ", completed" : ", stopped at " + run.FailedStep) + (run.RestartNeeded ? ", restart needed" : "") + ".";

		public const string Usage =
			"Rocksmith Audio Bridge setup\n" +
			"  --dry-run --game <folder> [--uninstall] [--payload <dir>]\n" +
			"  --install --game <folder> [--payload <dir>] [--replace-native-mod|--keep-native-mod] [--no-driver] [--no-menu] [--no-start-menu] [--desktop-shortcut] [--launch]\n" +
			"  --uninstall --game <folder> [--remove-settings] [--remove-usbdk|--keep-usbdk] [--keep-rsmods]\n" +
			"  --validate-manifest <mods.manifest.json>";
	}

	/// <summary>Where the payload comes from: a folder, or the payload.zip embedded in the exe extracted to %TEMP%
	/// (deleted on Dispose).</summary>
	public sealed class PayloadSource : IDisposable
	{
		public const string EmbeddedResourceName = "payload.zip";
		private readonly string? _temporary;

		private PayloadSource(string root, string? temporary)
		{
			Layout = new PayloadLayout(root);
			_temporary = temporary;
		}

		public PayloadLayout Layout { get; }

		public static PayloadSource Resolve(string? explicitFolder, Action<string> log)
		{
			if (!string.IsNullOrWhiteSpace(explicitFolder)) return new PayloadSource(explicitFolder, null);
			string beside = Path.Combine(AppContext.BaseDirectory, "payload");
			if (Directory.Exists(beside)) return new PayloadSource(beside, null);

			var assembly = Assembly.GetEntryAssembly();
			using Stream? embedded = assembly?.GetManifestResourceStream(EmbeddedResourceName);
			if (embedded != null)
			{
				string temporary = Path.Combine(Path.GetTempPath(), "RocksmithAudioBridgeSetup_" + Guid.NewGuid().ToString("N"));
				log("Unpacking the payload to " + temporary);
				using (var archive = new ZipArchive(embedded, ZipArchiveMode.Read)) archive.ExtractToDirectory(temporary);
				return new PayloadSource(temporary, temporary);
			}
			log("No payload: none given (--payload), none beside the exe, none embedded. The plan lists what it expects.");
			return new PayloadSource(beside, null);
		}

		public void Dispose()
		{
			if (_temporary == null) return;
			try { Directory.Delete(_temporary, recursive: true); } catch { }
		}
	}
}
