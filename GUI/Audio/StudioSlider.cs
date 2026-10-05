using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// Horizontal studio slider: the dark-theme replacement for the system TrackBar, whose light track
	/// and thumb do not take a BackColor. Same value contract
	/// as TrackBar (Minimum, Maximum, Value, ValueChanged, Small/LargeChange) so callers only swap the
	/// type; mouse drag, wheel and keyboard all work. MouseUp and KeyUp remain the "commit" signals.
	/// </summary>
	internal sealed class StudioSlider : Control
	{
		private const int TrackHeight = 6;
		private const int ThumbWidth = 14;
		private const int ThumbHeight = 22;

		private int minimum;
		private int maximum = 100;
		private int value;
		private Color tint = StudioTheme.Accent;
		private bool hovered;

		public event EventHandler ValueChanged;
		public event EventHandler Scroll;

		public StudioSlider()
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
			TabStop = true;
			Height = 36;
			Width = 300;
			Cursor = Cursors.Hand;
			AccessibleRole = AccessibleRole.Slider;
			Margin = new Padding(0, 4, 0, 4);
		}

		public int Minimum
		{
			get => minimum;
			set { minimum = value; if (maximum < minimum) maximum = minimum; Value = this.value; Invalidate(); }
		}

		public int Maximum
		{
			get => maximum;
			set { maximum = value; if (minimum > maximum) minimum = maximum; Value = this.value; Invalidate(); }
		}

		public int SmallChange { get; set; } = 1;
		public int LargeChange { get; set; } = 10;

		public int Value
		{
			get => value;
			set
			{
				int clamped = Math.Max(minimum, Math.Min(maximum, value));
				if (clamped == this.value) return;
				this.value = clamped;
				Invalidate();
				ValueChanged?.Invoke(this, EventArgs.Empty);
			}
		}

		public Color Tint
		{
			get => tint;
			set { if (tint == value) return; tint = value; Invalidate(); }
		}

		private int Inset => ThumbWidth / 2 + 2;
		private int Travel => Math.Max(1, Width - Inset * 2);

		private int PositionOf(int level)
		{
			double span = maximum - minimum;
			double fraction = span <= 0 ? 0 : (level - minimum) / span;
			return Inset + (int)Math.Round(fraction * Travel);
		}

		private int LevelAt(int x)
		{
			double fraction = Math.Max(0, Math.Min(1, (x - Inset) / (double)Travel));
			return minimum + (int)Math.Round(fraction * (maximum - minimum));
		}

		protected override void OnPaint(PaintEventArgs args)
		{
			var canvas = args.Graphics;
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			Color live = Enabled ? tint : StudioTheme.Faint;
			int middle = Height / 2;
			var track = new Rectangle(Inset, middle - TrackHeight / 2, Travel, TrackHeight);
			using (var path = StudioTheme.RoundedRectangle(track, TrackHeight / 2))
			{
				using (var brush = new SolidBrush(StudioTheme.Well))
					canvas.FillPath(brush, path);
				int position = PositionOf(value);
				if (Enabled && position > track.X)
				{
					canvas.SetClip(path);
					using (var brush = new LinearGradientBrush(new Rectangle(track.X, track.Y, Math.Max(1, position - track.X), track.Height),
						StudioTheme.Blend(live, StudioTheme.Well, 0.35), StudioTheme.Shift(live, 20), LinearGradientMode.Horizontal))
						canvas.FillRectangle(brush, track.X, track.Y, position - track.X, track.Height);
					canvas.ResetClip();
				}
				using (var pen = new Pen(StudioTheme.Line))
					canvas.DrawPath(pen, path);
			}
			DrawThumb(canvas, PositionOf(value), middle, live);
			if (Focused)
				using (var pen = new Pen(StudioTheme.Blend(StudioTheme.Line, live, 0.7)) { DashStyle = DashStyle.Dot })
					canvas.DrawRectangle(pen, 1, 1, Width - 3, Height - 3);
		}

		private void DrawThumb(Graphics canvas, int position, int middle, Color live)
		{
			var thumb = new Rectangle(position - ThumbWidth / 2, middle - ThumbHeight / 2, ThumbWidth, ThumbHeight);
			using (var shadow = StudioTheme.RoundedRectangle(new Rectangle(thumb.X, thumb.Y + 2, thumb.Width, thumb.Height), 5))
			using (var brush = new SolidBrush(Color.FromArgb(70, 0, 0, 0)))
				canvas.FillPath(brush, shadow);
			using (var path = StudioTheme.RoundedRectangle(thumb, 5))
			{
				using (var brush = new LinearGradientBrush(new Rectangle(thumb.X, thumb.Y, thumb.Width, thumb.Height + 1),
					StudioTheme.Shift(StudioTheme.Elevated, hovered && Enabled ? 26 : 14), StudioTheme.Shift(StudioTheme.Elevated, -10), LinearGradientMode.Vertical))
					canvas.FillPath(brush, path);
				using (var pen = new Pen(Enabled ? StudioTheme.Blend(live, StudioTheme.Ink, hovered ? 0.35 : 0.1) : StudioTheme.Line))
					canvas.DrawPath(pen, path);
			}
			using (var pen = new Pen(Enabled ? StudioTheme.Blend(live, StudioTheme.Ink, 0.25) : StudioTheme.Faint, 2))
				canvas.DrawLine(pen, position, thumb.Y + 6, position, thumb.Bottom - 6);
		}

		protected override void OnMouseDown(MouseEventArgs args)
		{
			base.OnMouseDown(args);
			if (args.Button != MouseButtons.Left || !Enabled) return;
			Focus();
			Capture = true;
			SetFromUser(LevelAt(args.X));
		}

		protected override void OnMouseMove(MouseEventArgs args)
		{
			base.OnMouseMove(args);
			if (Capture && Enabled) SetFromUser(LevelAt(args.X));
		}

		protected override void OnMouseUp(MouseEventArgs args)
		{
			if (args.Button == MouseButtons.Left) Capture = false;
			base.OnMouseUp(args);
		}

		protected override void OnMouseEnter(EventArgs args) { hovered = true; Invalidate(); base.OnMouseEnter(args); }

		protected override void OnMouseLeave(EventArgs args) { hovered = false; Invalidate(); base.OnMouseLeave(args); }

		protected override void OnMouseWheel(MouseEventArgs args)
		{
			base.OnMouseWheel(args);
			if (!Enabled) return;
			SetFromUser(value + Math.Sign(args.Delta) * SmallChange);
			// A wheel step is a discrete commit like a key press, so let key-up style listeners persist it.
			OnKeyUp(new KeyEventArgs(Keys.None));
		}

		protected override bool IsInputKey(Keys keyData)
		{
			return keyData == Keys.Left || keyData == Keys.Right || keyData == Keys.Home || keyData == Keys.End
				|| keyData == Keys.PageUp || keyData == Keys.PageDown || base.IsInputKey(keyData);
		}

		protected override void OnKeyDown(KeyEventArgs args)
		{
			base.OnKeyDown(args);
			if (!Enabled) return;
			int next;
			switch (args.KeyCode)
			{
				case Keys.Right: case Keys.Up: next = value + SmallChange; break;
				case Keys.Left: case Keys.Down: next = value - SmallChange; break;
				case Keys.PageUp: next = value + LargeChange; break;
				case Keys.PageDown: next = value - LargeChange; break;
				case Keys.Home: next = minimum; break;
				case Keys.End: next = maximum; break;
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
			Invalidate();
		}

		protected override void OnGotFocus(EventArgs args) { base.OnGotFocus(args); Invalidate(); }

		protected override void OnLostFocus(EventArgs args) { base.OnLostFocus(args); Invalidate(); }

		private void SetFromUser(int next)
		{
			int clamped = Math.Max(minimum, Math.Min(maximum, next));
			if (clamped == value) return;
			Value = clamped;
			Scroll?.Invoke(this, EventArgs.Empty);
		}
	}
}
