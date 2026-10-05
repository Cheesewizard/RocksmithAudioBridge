using System.Drawing;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal enum ChipTone
	{
		Idle,
		Good,
		Warn,
		Bad,
		Info
	}

	/// <summary>Square status tag: a framed field with a colour bar on its left edge saying the tone.</summary>
	internal sealed class StatusChip : Control
	{
		private ChipTone tone = ChipTone.Idle;

		public StatusChip(string text = "")
		{
			Font = StudioTheme.Small;
			// AutoSize + GetPreferredSize so the chip measures itself at the live DPI; a Width cached at 96 DPI
			// lets scaled text run past the box.
			AutoSize = true;
			Margin = new Padding(0, 3, 8, 3);
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
			Text = text;
		}

		public override Size GetPreferredSize(Size proposedSize)
		{
			int textWidth = TextRenderer.MeasureText(Text ?? "", Font).Width;
			return new Size(textWidth + LogicalToDeviceUnits(26), LogicalToDeviceUnits(24));
		}

		public ChipTone Tone
		{
			get { return tone; }
			set { if (tone != value) { tone = value; Invalidate(); } }
		}

		public void Set(string text, ChipTone chipTone)
		{
			Tone = chipTone;
			Text = text;
		}

		public override string Text
		{
			get { return base.Text; }
			set
			{
				if (base.Text == value)
					return;
				base.Text = value;
				if (AutoSize)
					Size = GetPreferredSize(Size.Empty);
				// AutoSize does not re-query preferred size on a Text change for a custom Control, and the chip
				// lives in a FlowLayoutPanel that has to reflow its neighbours, so ask the parent to lay out again.
				Parent?.PerformLayout(this, "Text");
				Invalidate();
			}
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			e.Graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
			Color accent = ToneColor();
			var bounds = new Rectangle(0, 0, Width - 1, Height - 1);
			using (var path = StudioTheme.RoundedRectangle(bounds, LogicalToDeviceUnits(9)))
			using (var brush = new SolidBrush(StudioTheme.Blend(StudioTheme.Field, accent, tone == ChipTone.Idle ? 0.0 : 0.12)))
				e.Graphics.FillPath(brush, path);
			using (var path = StudioTheme.RoundedRectangle(bounds, LogicalToDeviceUnits(9)))
			using (var pen = new Pen(StudioTheme.Blend(StudioTheme.Line, accent, tone == ChipTone.Idle ? 0.0 : 0.4)))
				e.Graphics.DrawPath(pen, path);
			int dotSize = LogicalToDeviceUnits(6);
			int dotLeft = LogicalToDeviceUnits(9);
			using (var brush = new SolidBrush(tone == ChipTone.Idle ? StudioTheme.Faint : accent))
				e.Graphics.FillEllipse(brush, dotLeft, Height / 2 - dotSize / 2, dotSize, dotSize);
			int textLeft = LogicalToDeviceUnits(20);
			TextRenderer.DrawText(e.Graphics, Text, Font, new Rectangle(textLeft, 0, Width - textLeft - LogicalToDeviceUnits(4), Height),
				tone == ChipTone.Idle ? StudioTheme.Muted : StudioTheme.Ink,
				TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
		}

		private Color ToneColor()
		{
			switch (tone)
			{
				case ChipTone.Good: return StudioTheme.Positive;
				case ChipTone.Warn: return StudioTheme.Warning;
				case ChipTone.Bad: return StudioTheme.Record;
				case ChipTone.Info: return StudioTheme.Accent;
				default: return StudioTheme.Faint;
			}
		}
	}
}
