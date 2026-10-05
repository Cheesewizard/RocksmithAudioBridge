using System;
using System.Diagnostics;

namespace RSMods.Audio
{
	/// <summary>
	/// Meter ballistics on a wall clock. The bridge reports a peak only when the window polls it, so a
	/// meter that moves only when a poll lands steps along at the poll rate. This keeps the latest
	/// report as a target and lets the bar and its hold line travel on their own between reports:
	/// a fast rise towards the target, a steady fall away from it, and a peak line that sits for a
	/// moment before sliding down. A target that stops being refreshed decays too, so a stalled
	/// connection never leaves a bar standing.
	/// </summary>
	internal sealed class MeterMotion
	{
		/// <summary>Full-scale fractions per second the bar rises. The whole scale in about 80 ms.</summary>
		private const double RisePerSecond = 12.0;
		/// <summary>Full-scale fractions per second the bar falls. Sixty decibels in about 1.2 s.</summary>
		private const double FallPerSecond = 0.85;
		/// <summary>How long the peak line holds before it starts to slide.</summary>
		private const double HoldSeconds = 1.1;
		private const double HoldFallPerSecond = 0.45;
		/// <summary>A report older than this is treated as silence. Polls arrive every 100 ms while connected.</summary>
		private const double StaleSeconds = 0.4;

		private readonly Stopwatch clock = Stopwatch.StartNew();
		private double target;
		private double targetTaken = double.NegativeInfinity;
		private double holdTaken = double.NegativeInfinity;
		private double lastAdvance;

		/// <summary>Bar height as a fraction of full scale.</summary>
		public double Level { get; private set; }

		/// <summary>Peak line as a fraction of full scale. Never below <see cref="Level"/>.</summary>
		public double Hold { get; private set; }

		/// <summary>True while anything still needs to travel, so a caller can stop its animation clock when idle.</summary>
		public bool IsMoving => Level > 0 || Hold > 0 || target > 0;

		/// <summary>Takes a fresh report from the bridge. Values are clamped to 0..1.</summary>
		public void Feed(double fraction)
		{
			if (double.IsNaN(fraction))
				fraction = 0;
			Advance();
			target = Math.Max(0, Math.Min(1, fraction));
			targetTaken = clock.Elapsed.TotalSeconds;
		}

		/// <summary>Moves the bar and hold line by however much time has passed since the last call.</summary>
		public void Advance()
		{
			double now = clock.Elapsed.TotalSeconds;
			double elapsed = Math.Max(0, Math.Min(0.25, now - lastAdvance));
			lastAdvance = now;
			if (now - targetTaken > StaleSeconds)
				target = Math.Max(0, target - FallPerSecond * elapsed);
			if (target > Level)
				Level = Math.Min(target, Level + RisePerSecond * elapsed);
			else if (target < Level)
				Level = Math.Max(target, Level - FallPerSecond * elapsed);
			if (Level >= Hold)
			{
				Hold = Level;
				holdTaken = now;
			}
			else if (now - holdTaken > HoldSeconds)
				Hold = Math.Max(Level, Hold - HoldFallPerSecond * elapsed);
		}

		public void Reset()
		{
			target = 0;
			Level = 0;
			Hold = 0;
			targetTaken = double.NegativeInfinity;
			holdTaken = double.NegativeInfinity;
			lastAdvance = clock.Elapsed.TotalSeconds;
		}
	}
}
