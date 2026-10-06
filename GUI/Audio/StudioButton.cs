using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal enum StudioButtonKind
	{
		Ghost,
		Primary,
		Record
	}

	internal sealed class StudioButton : Control
	{
		private StudioButtonKind kind;
		private readonly bool showDot;
		private bool hovered;
		private bool held;

		public StudioButton(string text, StudioButtonKind kind = StudioButtonKind.Ghost, bool showDot = false)
		{
			this.kind = kind;
			this.showDot = showDot;
			Font = StudioTheme.Strong;
			Height = 36;
			Margin = new Padding(0, 4, 8, 4);
			TabStop = true;
			Cursor = Cursors.Hand;
			// No transparent BackColor: the control inherits its parent's opaque colour (ambient BackColor), so
			// the parent never has to repaint underneath it. Transparent children make the window visibly
			// paint control by control.
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
			MinimumTextWidth = 96;
			Text = text;
		}

		public int MinimumTextWidth { get; set; }

		// Settable so one button can switch look with its action (e.g. Install = Primary, Uninstall = Ghost).
		public StudioButtonKind Kind
		{
			get { return kind; }
			set { if (kind == value) return; kind = value; Invalidate(); }
		}

		public override string Text
		{
			get { return base.Text; }
			set { base.Text = value; ResizeToText(); Invalidate(); }
		}

		private void ResizeToText()
		{
			if (Font == null)
				return;
			int text = TextRenderer.MeasureText(Text ?? "", Font).Width;
			Width = Math.Max(MinimumTextWidth, text + (showDot ? 54 : 36));
		}

		protected override void OnMouseEnter(EventArgs e)
		{
			hovered = true;
			Invalidate();
			base.OnMouseEnter(e);
		}

		protected override void OnMouseLeave(EventArgs e)
		{
			hovered = false;
			held = false;
			Invalidate();
			base.OnMouseLeave(e);
		}

		protected override void OnMouseDown(MouseEventArgs e)
		{
			held = e.Button == MouseButtons.Left;
			Focus();
			Invalidate();
			base.OnMouseDown(e);
		}

		protected override void OnMouseUp(MouseEventArgs e)
		{
			held = false;
			Invalidate();
			base.OnMouseUp(e);
		}

		protected override void OnKeyDown(KeyEventArgs e)
		{
			if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter)
			{
				e.Handled = true;
				OnClick(EventArgs.Empty);
			}
			base.OnKeyDown(e);
		}

		protected override void OnEnabledChanged(EventArgs e)
		{
			Cursor = Enabled ? Cursors.Hand : Cursors.Default;
			Invalidate();
			base.OnEnabledChanged(e);
		}

		protected override void OnGotFocus(EventArgs e)
		{
			Invalidate();
			base.OnGotFocus(e);
		}

		protected override void OnLostFocus(EventArgs e)
		{
			Invalidate();
			base.OnLostFocus(e);
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
			var bounds = new Rectangle(0, 0, Width - 1, Height - 1);
			Color fill = BaseFill();
			Color ink = kind == StudioButtonKind.Ghost ? StudioTheme.Ink : Color.White;
			if (!Enabled)
			{
				fill = StudioTheme.Blend(fill, StudioTheme.Surface, 0.62);
				ink = StudioTheme.Faint;
			}
			else if (held)
				fill = StudioTheme.Shift(fill, -16);
			else if (hovered)
				fill = StudioTheme.Shift(fill, 18);
			using (var path = StudioTheme.RoundedRectangle(bounds, LogicalToDeviceUnits(9)))
			using (var brush = new SolidBrush(fill))
				e.Graphics.FillPath(brush, path);
			Color edge = Focused && Enabled ? StudioTheme.Ink
				: kind == StudioButtonKind.Ghost ? StudioTheme.Line : StudioTheme.Shift(fill, -30);
			using (var path = StudioTheme.RoundedRectangle(bounds, LogicalToDeviceUnits(9)))
			using (var pen = new Pen(Enabled ? edge : StudioTheme.Blend(StudioTheme.Line, StudioTheme.Surface, 0.5), Focused && Enabled ? 1.5f : 1f))
				e.Graphics.DrawPath(pen, path);
			e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
			var text = ClientRectangle;
			if (showDot)
			{
				int dotSize = LogicalToDeviceUnits(8);
				var dot = new Rectangle(LogicalToDeviceUnits(14), Height / 2 - dotSize / 2, dotSize, dotSize);
				using (var brush = new SolidBrush(ink))
					e.Graphics.FillEllipse(brush, dot);
				text = new Rectangle(dot.Right + 4, 0, Width - dot.Right - 12, Height);
			}
			TextRenderer.DrawText(e.Graphics, Text, Font, text, ink,
				TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
		}

		private Color BaseFill()
		{
			switch (kind)
			{
				case StudioButtonKind.Record: return StudioTheme.Record;
				case StudioButtonKind.Primary: return StudioTheme.Accent;
				default: return StudioTheme.Field;
			}
		}
	}
}
