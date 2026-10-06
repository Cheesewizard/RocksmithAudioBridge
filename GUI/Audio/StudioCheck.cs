using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// Studio check box. The system CheckBox draws a light Windows box that ignores the dark theme;
	/// this one paints a rounded box in the theme colours with an accent fill when checked. Same
	/// members the panel used on CheckBox (Text, Checked, CheckedChanged, Enabled).
	/// </summary>
	internal sealed class StudioCheck : Control
	{
		private const int BoxSize = 18;
		private const int Gap = 9;

		private bool isChecked;
		private bool hovered;

		public event EventHandler CheckedChanged;

		public StudioCheck(string text = "")
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.Selectable | ControlStyles.StandardClick, true);
			Font = StudioTheme.Body;
			ForeColor = StudioTheme.Ink;
			Cursor = Cursors.Hand;
			TabStop = true;
			AccessibleRole = AccessibleRole.CheckButton;
			Margin = new Padding(0, 6, 0, 4);
			// AutoSize + GetPreferredSize so the box and text measure at the live DPI (fixed 96 px sizes leave
			// the box tiny beside scaled text and clip long labels).
			AutoSize = true;
			Text = text;
		}

		public override Size GetPreferredSize(Size proposedSize)
		{
			int boxRight = LogicalToDeviceUnits(1) + LogicalToDeviceUnits(BoxSize) - 1;
			int textWidth = TextRenderer.MeasureText(Text ?? "", Font).Width;
			return new Size(boxRight + LogicalToDeviceUnits(Gap) + textWidth + LogicalToDeviceUnits(6), LogicalToDeviceUnits(26));
		}

		public bool Checked
		{
			get => isChecked;
			set
			{
				if (isChecked == value) return;
				isChecked = value;
				Invalidate();
				CheckedChanged?.Invoke(this, EventArgs.Empty);
			}
		}

		public override string Text
		{
			get => base.Text;
			set
			{
				base.Text = value;
				// AutoSize does not re-query preferred size on a Text change for a custom Control; nudge the layout.
				Parent?.PerformLayout(this, "Text");
				Invalidate();
			}
		}

		protected override void OnFontChanged(EventArgs e)
		{
			base.OnFontChanged(e);
			Parent?.PerformLayout(this, "Font");
			Invalidate();
		}

		protected override void OnClick(EventArgs e)
		{
			if (Enabled) Checked = !Checked;
			base.OnClick(e);
		}

		protected override void OnKeyDown(KeyEventArgs e)
		{
			if (Enabled && e.KeyCode == Keys.Space)
			{
				Checked = !Checked;
				e.Handled = e.SuppressKeyPress = true;
			}
			base.OnKeyDown(e);
		}

		protected override void OnMouseDown(MouseEventArgs e) { Focus(); base.OnMouseDown(e); }

		protected override void OnMouseEnter(EventArgs e) { hovered = true; Invalidate(); base.OnMouseEnter(e); }

		protected override void OnMouseLeave(EventArgs e) { hovered = false; Invalidate(); base.OnMouseLeave(e); }

		protected override void OnEnabledChanged(EventArgs e) { Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); base.OnEnabledChanged(e); }

		protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }

		protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

		protected override void OnPaint(PaintEventArgs e)
		{
			var canvas = e.Graphics;
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			int boxSize = LogicalToDeviceUnits(BoxSize);
			int gap = LogicalToDeviceUnits(Gap);
			var box = new Rectangle(LogicalToDeviceUnits(1), Height / 2 - boxSize / 2, boxSize - 1, boxSize - 1);
			Color fill = isChecked && Enabled ? StudioTheme.Accent : StudioTheme.Field;
			if (hovered && Enabled) fill = StudioTheme.Shift(fill, 14);
			if (!Enabled) fill = StudioTheme.Blend(fill, StudioTheme.Surface, 0.5);
			using (var path = StudioTheme.RoundedRectangle(box, LogicalToDeviceUnits(4)))
			{
				using (var brush = new SolidBrush(fill))
					canvas.FillPath(brush, path);
				Color edge = Focused && Enabled ? StudioTheme.Accent : isChecked && Enabled ? StudioTheme.Shift(StudioTheme.Accent, 30) : StudioTheme.Line;
				using (var pen = new Pen(edge))
					canvas.DrawPath(pen, path);
			}
			if (isChecked)
			{
				float stroke = Math.Max(2f, LogicalToDeviceUnits(2));
				using (var pen = new Pen(Enabled ? Color.White : StudioTheme.Faint, stroke) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
					canvas.DrawLines(pen, new[]
					{
						new PointF(box.X + box.Width * 0.25f, box.Y + box.Height / 2f + 0.5f),
						new PointF(box.X + box.Width / 2f - 0.5f, box.Bottom - box.Height * 0.25f),
						new PointF(box.Right - box.Width * 0.22f, box.Y + box.Height * 0.25f)
					});
			}
			var text = new Rectangle(box.Right + gap, 0, Width - box.Right - gap, Height);
			TextRenderer.DrawText(canvas, Text, Font, text, Enabled ? ForeColor : StudioTheme.Faint,
				TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.EndEllipsis);
		}
	}
}
