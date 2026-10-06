using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Controls
{
	internal sealed class ColourWheel : Control
	{
		public Color SelectedColor => CreateColor(hue, saturation, brightness);

		private readonly Bitmap wheelImage;
		private double hue;
		private double saturation;
		private double brightness = 1;

		public event EventHandler ColorChanged;

		public ColourWheel(Color color)
		{
			Size = new Size(232, 232);
			DoubleBuffered = true;
			TabStop = true;
			AccessibleName = "Colour wheel. Arrow keys change hue and saturation.";
			SetStyle(ControlStyles.Selectable, true);
			hue = color.GetHue();
			var maximum = Math.Max(color.R, Math.Max(color.G, color.B));
			var minimum = Math.Min(color.R, Math.Min(color.G, color.B));
			brightness = maximum / 255.0;
			saturation = maximum == 0 ? 0 : 1 - minimum / (double)maximum;
			wheelImage = new Bitmap(224, 224);
			for (var y = 0; y < 224; y++)
			{
				for (var x = 0; x < 224; x++)
				{
					var dx = x - 111.5;
					var dy = y - 111.5;
					var radius = Math.Sqrt(dx * dx + dy * dy) / 111.5;
					if (radius <= 1)
					{
						wheelImage.SetPixel(x, y, CreateColor((Math.Atan2(dy, dx) * 180 / Math.PI + 360) % 360, radius, 1));
					}
				}
			}
		}

		public void SetBrightness(double value)
		{
			if (double.IsNaN(value) || value < 0 || value > 1) throw new ArgumentOutOfRangeException(nameof(value));
			brightness = value;
			NotifyColorChanged();
		}

		protected override void OnPaint(PaintEventArgs args)
		{
			base.OnPaint(args);
			args.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
			args.Graphics.DrawImageUnscaled(wheelImage, 4, 4);
			var angle = hue * Math.PI / 180;
			var x = 115.5f + (float)(Math.Cos(angle) * saturation * 111.5);
			var y = 115.5f + (float)(Math.Sin(angle) * saturation * 111.5);
			using (var dark = new Pen(Color.Black, 3)) args.Graphics.DrawEllipse(dark, x - 5, y - 5, 10, 10);
			using (var light = new Pen(Color.White, 1)) args.Graphics.DrawEllipse(light, x - 5, y - 5, 10, 10);
			if (Focused) ControlPaint.DrawFocusRectangle(args.Graphics, ClientRectangle);
		}

		protected override void OnMouseDown(MouseEventArgs args)
		{
			base.OnMouseDown(args);
			if (args.Button != MouseButtons.Left) return;
			Focus();
			Capture = true;
			SelectPoint(args.Location);
		}

		protected override void OnMouseMove(MouseEventArgs args)
		{
			base.OnMouseMove(args);
			if (Capture && args.Button == MouseButtons.Left) SelectPoint(args.Location);
		}

		protected override void OnMouseUp(MouseEventArgs args)
		{
			base.OnMouseUp(args);
			Capture = false;
		}

		protected override bool IsInputKey(Keys keyData)
		{
			var key = keyData & Keys.KeyCode;
			return key == Keys.Left || key == Keys.Right || key == Keys.Up || key == Keys.Down || base.IsInputKey(keyData);
		}

		protected override void OnKeyDown(KeyEventArgs args)
		{
			base.OnKeyDown(args);
			switch (args.KeyCode)
			{
				case Keys.Left: hue = (hue + 355) % 360; break;
				case Keys.Right: hue = (hue + 5) % 360; break;
				case Keys.Up: saturation = Math.Min(1, saturation + 0.02); break;
				case Keys.Down: saturation = Math.Max(0, saturation - 0.02); break;
				default: return;
			}
			args.Handled = true;
			NotifyColorChanged();
		}

		private void SelectPoint(Point point)
		{
			var dx = point.X - 115.5;
			var dy = point.Y - 115.5;
			hue = (Math.Atan2(dy, dx) * 180 / Math.PI + 360) % 360;
			saturation = Math.Min(1, Math.Sqrt(dx * dx + dy * dy) / 111.5);
			NotifyColorChanged();
		}

		private void NotifyColorChanged()
		{
			Invalidate();
			ColorChanged?.Invoke(this, EventArgs.Empty);
		}

		private static Color CreateColor(double hue, double saturation, double brightness)
		{
			var chroma = brightness * saturation;
			var secondary = chroma * (1 - Math.Abs(hue / 60 % 2 - 1));
			var minimum = brightness - chroma;
			double red = 0, green = 0, blue = 0;
			if (hue < 60)
			{
				red = chroma;
				green = secondary;
			}
			else if (hue < 120)
			{
				red = secondary;
				green = chroma;
			}
			else if (hue < 180)
			{
				green = chroma;
				blue = secondary;
			}
			else if (hue < 240)
			{
				green = secondary;
				blue = chroma;
			}
			else if (hue < 300)
			{
				red = secondary;
				blue = chroma;
			}
			else
			{
				red = chroma;
				blue = secondary;
			}
			return Color.FromArgb((int)Math.Round((red + minimum) * 255),
				(int)Math.Round((green + minimum) * 255), (int)Math.Round((blue + minimum) * 255));
		}

		protected override void Dispose(bool disposing)
		{
			if (disposing) wheelImage.Dispose();
			base.Dispose(disposing);
		}
	}
}
