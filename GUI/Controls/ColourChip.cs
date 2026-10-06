using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;
using RSMods.Audio;

namespace RSMods.Controls
{
	internal sealed class ColourChip : Button
	{
		public Color SelectedColor
		{
			get => selectedColor;
			set
			{
				selectedColor = value;
				Text = "#" + (value.ToArgb() & 0xFFFFFF).ToString("X6");
				Invalidate();
			}
		}

		private Color selectedColor = Color.White;
		private bool hovered;

		public ColourChip()
		{
			Size = new Size(174, 36);
			Font = StudioTheme.Strong;
			Cursor = Cursors.Hand;
			AccessibleName = "Edit selected colour";
			AccessibleDescription = "Opens the colour wheel. The displayed value is the current colour.";
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
			SelectedColor = Color.White;
		}

		protected override void OnMouseEnter(EventArgs args)
		{
			hovered = true;
			Invalidate();
			base.OnMouseEnter(args);
		}

		protected override void OnMouseLeave(EventArgs args)
		{
			hovered = false;
			Invalidate();
			base.OnMouseLeave(args);
		}

		protected override void OnPaint(PaintEventArgs args)
		{
			args.Graphics.Clear(Parent.BackColor);
			args.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
			using (var frame = StudioTheme.RoundedRectangle(new Rectangle(0, 0, Width - 1, Height - 1), 6))
			{
				using (var fill = new SolidBrush(hovered ? StudioTheme.Elevated : StudioTheme.Field)) args.Graphics.FillPath(fill, frame);
				using (var edge = new Pen(Focused || hovered ? StudioTheme.Accent : StudioTheme.Line)) args.Graphics.DrawPath(edge, frame);
			}
			var swatch = new Rectangle(12, (Height - 18) / 2, 18, 18);
			using (var fill = new SolidBrush(selectedColor)) args.Graphics.FillEllipse(fill, swatch);
			using (var edge = new Pen(StudioTheme.Muted)) args.Graphics.DrawEllipse(edge, swatch);
			TextRenderer.DrawText(args.Graphics, Text, Font, new Rectangle(40, 0, Width - 65, Height), StudioTheme.Ink,
				TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
			using (var pen = new Pen(StudioTheme.Muted, 1.6f))
			{
				var x = Width - 22;
				var y = Height / 2;
				args.Graphics.DrawLine(pen, x, y + 5, x + 8, y - 3);
				args.Graphics.DrawLine(pen, x, y + 5, x + 4, y + 4);
			}
		}
	}
}
