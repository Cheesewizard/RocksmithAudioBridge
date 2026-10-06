using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// Slim vertical output meter that sits beside the master fader. The bridge is polled a few times a
	/// second, so the meter runs its own 30 fps clock and lets <see cref="MeterMotion"/> carry the bar
	/// between reports; it repaints only when the bar or the peak line lands on a new pixel row.
	/// </summary>
	internal sealed class ChannelMeter : Control
	{
		private const int BarWidth = 10;
		private readonly MeterMotion motion = new MeterMotion();
		private readonly Timer animation = new Timer { Interval = 33 };
		private int paintedTop = -1;
		private int paintedHold = -1;

		public ChannelMeter()
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
			Width = 18;
			animation.Tick += (sender, args) => Animate();
		}

		public void SetLevel(int peak)
		{
			motion.Feed(LevelMeter.Fraction(peak));
			if (!animation.Enabled)
				animation.Start();
			Animate();
		}

		public void Reset()
		{
			animation.Stop();
			motion.Reset();
			InvalidateWhenMoved();
		}

		private void Animate()
		{
			motion.Advance();
			if (!motion.IsMoving)
				animation.Stop();
			InvalidateWhenMoved();
		}

		/// <summary>Repaints only when the bar or its peak hold lands on a different pixel row.</summary>
		private void InvalidateWhenMoved()
		{
			int top = (int)Math.Round(motion.Level * Math.Max(0, Height));
			int peak = (int)Math.Round(motion.Hold * Math.Max(0, Height));
			if (top == paintedTop && peak == paintedHold)
				return;
			paintedTop = top;
			paintedHold = peak;
			Invalidate();
		}

		protected override void OnPaint(PaintEventArgs args)
		{
			base.OnPaint(args);
			var canvas = args.Graphics;
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			double level = motion.Level;
			double hold = motion.Hold;
			int inset = Math.Max(10, DeviceDpi / 10);
			var bar = new Rectangle(Width / 2 - BarWidth / 2, inset, BarWidth, Math.Max(1, Height - inset * 2));
			using (var path = StudioTheme.RoundedRectangle(bar, BarWidth / 2))
			{
				using (var brush = new SolidBrush(StudioTheme.Well))
					canvas.FillPath(brush, path);
				canvas.SetClip(path);
				if (level > 0.001)
				{
					int top = bar.Bottom - (int)Math.Round(bar.Height * level);
					using (var brush = new LinearGradientBrush(new Rectangle(bar.X, bar.Y, bar.Width, bar.Height + 1),
						StudioTheme.Record, StudioTheme.Positive, LinearGradientMode.Vertical))
					{
						brush.InterpolationColors = new ColorBlend
						{
							Colors = new[] { StudioTheme.Record, StudioTheme.Warning, StudioTheme.Positive, StudioTheme.Positive },
							Positions = new[] { 0f, 0.12f, 0.28f, 1f }
						};
						canvas.FillRectangle(brush, bar.X, top, bar.Width, bar.Bottom - top);
					}
				}
				if (hold > 0.01)
				{
					float y = bar.Bottom - (float)(bar.Height * hold);
					using (var brush = new SolidBrush(Color.FromArgb(205, 255, 255, 255)))
						canvas.FillRectangle(brush, bar.X, y - 1, bar.Width, 2);
				}
				canvas.ResetClip();
				using (var pen = new Pen(StudioTheme.Line))
					canvas.DrawPath(pen, path);
			}
		}

		protected override void Dispose(bool disposing)
		{
			if (disposing)
				animation.Dispose();
			base.Dispose(disposing);
		}
	}
}
