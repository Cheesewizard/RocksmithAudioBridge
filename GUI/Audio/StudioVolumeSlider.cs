using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// Vertical channel fader: filled travel, a capped thumb and wheel, keyboard and double click
	/// control. The trough, its scale and the fill gradient are rendered once into cached bitmaps
	/// and blitted, because painting a tall anti-aliased gradient on every value change makes a
	/// drag across seven faders far too slow.
	/// </summary>
	internal sealed class StudioVolumeSlider : Control
	{
		private const int TroughWidth = 9;
		private const int ThumbWidth = 30;
		private const int ThumbHeight = 14;

		public int Value
		{
			get => value;
			set
			{
				if (value < 0 || value > 100) throw new ArgumentOutOfRangeException(nameof(value));
				if (this.value == value) return;
				int previous = this.value;
				this.value = value;
				InvalidateTravel(previous, value);
			}
		}

		public Color Tint
		{
			get => tint;
			set { if (tint == value) return; tint = value; DiscardCache(); Invalidate(); }
		}

		private int value;
		private Color tint = StudioTheme.Accent;
		private bool hovered;
		private Bitmap trough;
		private Bitmap fill;
		private Size cachedFor;
		private bool cachedEnabled;

		public event EventHandler Scroll;

		public StudioVolumeSlider()
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
			TabStop = true;
			AccessibleRole = AccessibleRole.Slider;
			Cursor = Cursors.Hand;
		}

		private int Inset => Math.Max(10, DeviceDpi / 10);

		private int Travel => Math.Max(1, Height - Inset * 2);

		private int PositionOf(int level) => Inset + (int)Math.Round((100 - level) / 100.0 * Travel);

		/// <summary>Repaints only the band the thumb moved through, plus the fill it uncovered.</summary>
		private void InvalidateTravel(int from, int to)
		{
			if (!IsHandleCreated) return;
			int top = Math.Min(PositionOf(from), PositionOf(to)) - ThumbHeight;
			int bottom = Math.Max(PositionOf(from), PositionOf(to)) + ThumbHeight;
			Invalidate(new Rectangle(0, Math.Max(0, top), Width, Math.Min(Height, bottom) - Math.Max(0, top) + 1));
		}

		private void DiscardCache()
		{
			trough?.Dispose();
			fill?.Dispose();
			trough = null;
			fill = null;
			cachedFor = Size.Empty;
		}

		private void EnsureCache()
		{
			if (trough != null && fill != null && cachedFor == Size && cachedEnabled == Enabled)
				return;
			DiscardCache();
			cachedFor = Size;
			cachedEnabled = Enabled;
			if (Width <= 0 || Height <= 0)
				return;

			int centre = Width / 2;
			int inset = Inset;
			int length = Travel;
			var bounds = new Rectangle(centre - TroughWidth / 2, inset, TroughWidth, length);
			Color live = Enabled ? tint : StudioTheme.Faint;

			trough = new Bitmap(Width, Height);
			using (var canvas = Graphics.FromImage(trough))
			{
				canvas.SmoothingMode = SmoothingMode.AntiAlias;
				DrawScale(canvas, centre, inset, length);
				using (var path = StudioTheme.RoundedRectangle(bounds, TroughWidth / 2))
				{
					using (var brush = new SolidBrush(StudioTheme.Well))
						canvas.FillPath(brush, path);
					using (var pen = new Pen(StudioTheme.Line))
						canvas.DrawPath(pen, path);
				}
			}

			fill = new Bitmap(Width, Height);
			using (var canvas = Graphics.FromImage(fill))
			{
				canvas.SmoothingMode = SmoothingMode.AntiAlias;
				using (var path = StudioTheme.RoundedRectangle(bounds, TroughWidth / 2))
				{
					canvas.SetClip(path);
					using (var brush = new LinearGradientBrush(
						new Rectangle(bounds.X, inset, bounds.Width, length + 1),
						StudioTheme.Shift(live, 26), StudioTheme.Blend(live, StudioTheme.Well, 0.45), LinearGradientMode.Vertical))
						canvas.FillRectangle(brush, bounds);
					canvas.ResetClip();
				}
			}
		}

		protected override void OnPaint(PaintEventArgs args)
		{
			var canvas = args.Graphics;
			using (var background = new SolidBrush(BackColor.A == 0 ? Parent?.BackColor ?? StudioTheme.Surface : BackColor))
				canvas.FillRectangle(background, args.ClipRectangle);
			EnsureCache();
			if (trough == null)
				return;

			int position = PositionOf(value);
			canvas.DrawImageUnscaled(trough, 0, 0);
			if (Enabled && value > 0)
			{
				var band = new Rectangle(0, position, Width, Height - position);
				if (band.Height > 0)
					canvas.DrawImage(fill, band, band, GraphicsUnit.Pixel);
			}
			DrawThumb(canvas, Width / 2, position, Enabled ? tint : StudioTheme.Faint);
			if (Focused)
				using (var pen = new Pen(StudioTheme.Blend(StudioTheme.Line, Enabled ? tint : StudioTheme.Faint, 0.7)) { DashStyle = DashStyle.Dot })
					canvas.DrawRectangle(pen, 1, 1, Width - 3, Height - 3);
		}

		private void DrawScale(Graphics canvas, int centre, int inset, int length)
		{
			using (var pen = new Pen(StudioTheme.Blend(StudioTheme.Line, StudioTheme.Background, 0.25)))
			{
				for (int index = 0; index <= 4; index++)
				{
					int y = inset + index * length / 4;
					int reach = index == 0 || index == 4 || index == 2 ? 9 : 6;
					canvas.DrawLine(pen, centre - TroughWidth / 2 - reach, y, centre - TroughWidth / 2 - 3, y);
					canvas.DrawLine(pen, centre + TroughWidth / 2 + 3, y, centre + TroughWidth / 2 + reach, y);
				}
			}
		}

		private void DrawThumb(Graphics canvas, int centre, int position, Color live)
		{
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			var thumb = new Rectangle(centre - ThumbWidth / 2, position - ThumbHeight / 2, ThumbWidth, ThumbHeight);
			using (var shadow = StudioTheme.RoundedRectangle(new Rectangle(thumb.X, thumb.Y + 2, thumb.Width, thumb.Height), 5))
			using (var brush = new SolidBrush(Color.FromArgb(70, 0, 0, 0)))
				canvas.FillPath(brush, shadow);
			using (var path = StudioTheme.RoundedRectangle(thumb, 5))
			{
				using (var brush = new LinearGradientBrush(
					new Rectangle(thumb.X, thumb.Y, thumb.Width, thumb.Height + 1),
					StudioTheme.Shift(StudioTheme.Elevated, hovered && Enabled ? 26 : 14), StudioTheme.Shift(StudioTheme.Elevated, -10), LinearGradientMode.Vertical))
					canvas.FillPath(brush, path);
				using (var pen = new Pen(Enabled ? StudioTheme.Blend(live, StudioTheme.Ink, hovered ? 0.35 : 0.1) : StudioTheme.Line))
					canvas.DrawPath(pen, path);
			}
			using (var pen = new Pen(Enabled ? StudioTheme.Blend(live, StudioTheme.Ink, 0.25) : StudioTheme.Faint, 2))
				canvas.DrawLine(pen, thumb.X + 7, position, thumb.Right - 7, position);
			canvas.SmoothingMode = SmoothingMode.None;
		}

		protected override void OnMouseDown(MouseEventArgs args)
		{
			base.OnMouseDown(args);
			if (args.Button != MouseButtons.Left) return;
			Focus();
			Capture = true;
			SetFromPointer(args.Y);
		}

		protected override void OnMouseMove(MouseEventArgs args)
		{
			base.OnMouseMove(args);
			if (Capture) SetFromPointer(args.Y);
		}

		protected override void OnMouseUp(MouseEventArgs args)
		{
			base.OnMouseUp(args);
			if (args.Button == MouseButtons.Left) Capture = false;
		}

		protected override void OnMouseEnter(EventArgs args)
		{
			base.OnMouseEnter(args);
			hovered = true;
			InvalidateTravel(value, value);
		}

		protected override void OnMouseLeave(EventArgs args)
		{
			base.OnMouseLeave(args);
			hovered = false;
			InvalidateTravel(value, value);
		}

		protected override void OnMouseWheel(MouseEventArgs args)
		{
			base.OnMouseWheel(args);
			SetFromUser(value + Math.Sign(args.Delta) * 2);
		}

		protected override void OnDoubleClick(EventArgs args)
		{
			base.OnDoubleClick(args);
			SetFromUser(100);
		}

		protected override bool IsInputKey(Keys keyData)
		{
			return keyData == Keys.Up || keyData == Keys.Down || keyData == Keys.Home || keyData == Keys.End || keyData == Keys.PageUp || keyData == Keys.PageDown || base.IsInputKey(keyData);
		}

		protected override void OnKeyDown(KeyEventArgs args)
		{
			base.OnKeyDown(args);
			int next;
			switch (args.KeyCode)
			{
				case Keys.Up: next = value + 1; break;
				case Keys.Down: next = value - 1; break;
				case Keys.PageUp: next = value + 5; break;
				case Keys.PageDown: next = value - 5; break;
				case Keys.Home: next = 100; break;
				case Keys.End: next = 0; break;
				default: return;
			}
			SetFromUser(next);
			args.Handled = args.SuppressKeyPress = true;
		}

		protected override void OnEnabledChanged(EventArgs args)
		{
			base.OnEnabledChanged(args);
			if (!Enabled) Capture = false;
			Cursor = Enabled ? Cursors.Hand : Cursors.Default;
			DiscardCache();
			Invalidate();
		}

		protected override void OnResize(EventArgs args)
		{
			base.OnResize(args);
			DiscardCache();
		}

		protected override void OnGotFocus(EventArgs args)
		{
			base.OnGotFocus(args);
			Invalidate();
		}

		protected override void OnLostFocus(EventArgs args)
		{
			base.OnLostFocus(args);
			Invalidate();
		}

		protected override void Dispose(bool disposing)
		{
			if (disposing) DiscardCache();
			base.Dispose(disposing);
		}

		private void SetFromPointer(int y)
		{
			SetFromUser((int)Math.Round(100 - (y - Inset) * 100.0 / Travel));
		}

		private void SetFromUser(int next)
		{
			if (!Enabled) return;
			next = Math.Max(0, Math.Min(100, next));
			if (next == value) return;
			Value = next;
			Scroll?.Invoke(this, EventArgs.Empty);
		}
	}
}
