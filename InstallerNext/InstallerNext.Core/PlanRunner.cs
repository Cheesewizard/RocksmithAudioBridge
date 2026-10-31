#nullable enable
using System;
using System.Linq;

namespace RocksmithAudioBridge.Setup
{
	/// <summary>A step's own failure with a message meant for the player (thrown from a step's Run).</summary>
	public sealed class StepFailedException : Exception
	{
		public StepFailedException(string message) : base(message) { }
	}

	/// <summary>
	/// Runs a plan's steps in order through an <see cref="IInstallHost"/>. A plan with blockers does not run, except as a
	/// dry run, which logs every step and every side effect it would have and changes nothing.
	/// </summary>
	public static class PlanRunner
	{
		public static RunResult Run(InstallPlan plan, IInstallHost host, ISystemProbe probe)
		{
			var log = host.Log;
			foreach (string line in plan.Describe()) log(line);
			var result = new RunResult();
			if (plan.Blockers.Count > 0 && !host.IsDryRun)
			{
				result.Error = string.Join(Environment.NewLine, plan.Blockers);
				log("Not started: " + result.Error);
				return result;
			}

			log(host.IsDryRun ? "Dry run: nothing below is done." : "Running.");
			var context = new StepContext(host, new RunState(), probe);
			bool stopped = false;
			foreach (var step in plan.Steps)
			{
				if (stopped && !step.AlwaysRun) continue;
				log("== [" + step.Id + "] " + step.Title);
				try
				{
					step.Run(context);
				}
				catch (OperationCanceledException cancelled)
				{
					log("   cancelled: " + cancelled.Message);
					result.Cancelled = true;
					result.FailedStep ??= step.Id;
					result.Error ??= cancelled.Message;
					if (step.OnFailure == StepFailure.AbortAndRollBack) RollBack(plan, context, result);
					stopped = true;
				}
				catch (Exception error) when (step.OnFailure == StepFailure.Warn)
				{
					log("   warning: " + error.Message);
					context.State.Warnings.Add(step.Title + ": " + error.Message);
				}
				catch (Exception error)
				{
					log("   FAILED: " + error.Message);
					result.FailedStep ??= step.Id;
					result.Error ??= error.Message;
					if (step.OnFailure == StepFailure.AbortAndRollBack) RollBack(plan, context, result);
					stopped = true;
				}
			}

			result.Warnings = context.State.Warnings.ToList();
			result.RestartNeeded = context.State.RestartNeeded;
			result.Succeeded = !stopped;
			log(result.Succeeded ? (host.IsDryRun ? "Dry run complete." : "Done.") : "Stopped at [" + result.FailedStep + "]" + (result.RolledBack ? " and rolled back." : "."));
			foreach (string warning in result.Warnings) log("Warning: " + warning);
			if (result.RestartNeeded) log("Restart Windows before playing (UsbDk was just installed).");
			return result;
		}

		// 4.x (Installer/GUI.cs): a driver install that failed removed the files again, keeping the logs its message
		// points at, and the starter tones.
		private static void RollBack(InstallPlan plan, StepContext context, RunResult result)
		{
			if (plan.RollbackFactory == null) return;
			context.Host.Log("   rolling back");
			foreach (var step in plan.RollbackFactory(true))
			{
				try { step.Run(context); }
				catch (Exception error) { context.Host.Log("   rollback step [" + step.Id + "] failed: " + error.Message); }
			}
			result.RolledBack = true;
		}
	}
}
