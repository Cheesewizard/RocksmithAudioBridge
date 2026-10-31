#nullable enable
using System;
using System.Collections.Generic;

namespace RocksmithAudioBridge.Setup
{
	public enum PlanOperation { Install, Uninstall }

	/// <summary>What happens when a step throws or reports failure.</summary>
	public enum StepFailure
	{
		/// <summary>Stop; the steps after it do not run (AlwaysRun steps still do).</summary>
		Abort,
		/// <summary>Stop and remove what the install wrote so far, keeping the logs the error points at (4.x: a driver
		/// install that failed rolled the whole install back).</summary>
		AbortAndRollBack,
		/// <summary>Note it and carry on.</summary>
		Warn,
	}

	public sealed class InstallOptions
	{
		public string GameFolder { get; set; } = "";
		public PayloadLayout Payload { get; set; } = new PayloadLayout(".");
		/// <summary>Replace an xinput1_3.dll that belongs to another program or to an RSMods build with other external
		/// mods. null = not decided yet: the plan carries a blocker until the UI asks or the CLI passes a flag.</summary>
		public bool? ReplaceForeignNativeMod { get; set; }
		public bool RegisterDriver { get; set; } = true;
		public bool InstallMenuRow { get; set; } = true;
		public bool CreateStartMenuShortcut { get; set; } = true;
		public bool CreateDesktopShortcut { get; set; }
		public bool LaunchApp { get; set; }
		/// <summary>Tests only: skip the Rocksmith2014.exe hash check.</summary>
		public bool SkipCompatibilityCheck { get; set; }
	}

	public sealed class UninstallOptions
	{
		public string GameFolder { get; set; } = "";
		/// <summary>Also remove RSMods.ini (and a 4.0 AudioRouting.ini), the app's logs and the bridge's HKCU settings.</summary>
		public bool RemoveSettings { get; set; }
		/// <summary>null = the 4.x default: yes when this installer put UsbDk on the PC, no when it was already there.</summary>
		public bool? RemoveUsbDk { get; set; }
		/// <summary>Leave upstream's configurator (RSMods\RSMods.exe and its payload) in place. Our DLL is removed either way.</summary>
		public bool KeepRsModsConfigurator { get; set; }
	}

	/// <summary>Mutable state the steps share while a plan runs.</summary>
	public sealed class RunState
	{
		/// <summary>Game-relative paths written by this run (for the install record).</summary>
		public HashSet<string> Written { get; } = new(StringComparer.OrdinalIgnoreCase);
		public List<string> Warnings { get; } = new();
		public bool RestartNeeded { get; set; }
		/// <summary>Uninstall: our Riff Repeater row is gone, so cache.pre-note-by-note-menu.psarc and its log can go too.</summary>
		public bool MenuEntryRemoved { get; set; }
	}

	public sealed class StepContext
	{
		public StepContext(IInstallHost host, RunState state, ISystemProbe probe)
		{
			Host = host;
			State = state;
			Probe = probe;
		}

		public IInstallHost Host { get; }
		public RunState State { get; }
		public ISystemProbe Probe { get; }
	}

	public sealed class PlannedStep
	{
		public PlannedStep(string id, string title, string replaces4x, Action<StepContext> run)
		{
			Id = id;
			Title = title;
			Replaces4x = replaces4x;
			Run = run;
		}

		public string Id { get; }
		public string Title { get; }
		/// <summary>The 4.x installer step (Installer/*.cs) this one replaces, for review against the old installer.</summary>
		public string Replaces4x { get; }
		/// <summary>Planned detail lines (files, commands) shown in the plan before anything runs.</summary>
		public List<string> Details { get; } = new();
		public Action<StepContext> Run { get; }
		public StepFailure OnFailure { get; init; } = StepFailure.Abort;
		/// <summary>Runs even after an earlier step aborted (the install record, so Uninstall knows every file written).</summary>
		public bool AlwaysRun { get; init; }
	}

	public sealed class InstallPlan
	{
		public InstallPlan(PlanOperation operation, string gameFolder)
		{
			Operation = operation;
			GameFolder = gameFolder;
		}

		public PlanOperation Operation { get; }
		public string GameFolder { get; }
		/// <summary>Read-only checks done while planning, with their result.</summary>
		public List<string> Checks { get; } = new();
		/// <summary>Reasons the plan must not run. A dry run still shows the steps.</summary>
		public List<string> Blockers { get; } = new();
		public List<PlannedStep> Steps { get; } = new();
		/// <summary>Uninstall only: the steps that undo a failed install (also used as the install's rollback).</summary>
		public Func<bool, IEnumerable<PlannedStep>>? RollbackFactory { get; set; }

		public IEnumerable<string> Describe()
		{
			yield return Operation + " plan for " + GameFolder;
			foreach (string check in Checks) yield return "  check: " + check;
			foreach (string blocker in Blockers) yield return "  BLOCKER: " + blocker;
			int number = 1;
			foreach (var step in Steps)
			{
				yield return $"  {number++,2}. [{step.Id}] {step.Title}";
				yield return "      replaces 4.x: " + step.Replaces4x;
				foreach (string detail in step.Details) yield return "      - " + detail;
			}
		}
	}

	public sealed class RunResult
	{
		public bool Succeeded { get; set; }
		public bool Cancelled { get; set; }
		public bool RolledBack { get; set; }
		public string? Error { get; set; }
		public string? FailedStep { get; set; }
		public List<string> Warnings { get; set; } = new();
		public bool RestartNeeded { get; set; }
	}
}
