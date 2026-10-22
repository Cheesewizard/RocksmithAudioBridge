using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// A stomp-box knob for the noise gate pedal: a custom-painted, double-buffered dial with a 270 degree
	/// sweep, its caption above and its formatted value below. Drag up or down to turn it (about 150 px for the
	/// full range, Ctrl for fine), scroll for steps, arrow keys when focused, double-click to reset to the
	/// default. Value is in display units (dB, ms); LogTaper spreads a range like 50..2000 ms evenly over the
	/// sweep. ValueChanged fires live while turning; ValueCommitted fires once a gesture ends (mouse release,
	/// key release, a pause after scrolling, a reset), which is when callers persist.
	/// </summary>
	internal sealed class PedalKnob : Control
	{
		private const int DragTravel = 150;     // logical px for the full sweep
		private const int DialSize = 58;        // logical px
		private const float StartAngle = 135f;  // GDI degrees: bottom-left, then clockwise over the top
		private const float SweepAngle = 270f;
		private const int WheelNotches = 40;    // log-taper knobs: notches for the full sweep

		public static readonly Color Body = ColorTranslator.FromHtml("#1f5b54");
		public static readonly Color Border = ColorTranslator.FromHtml("#2d7a70");
		public static readonly Color Track = ColorTranslator.FromHtml("#123a35");
		public static readonly Color Fill = ColorTranslator.FromHtml("#5DCAA5");
		public static readonly Color Lettering = ColorTranslator.FromHtml("#E4F4EF");

		private static readonly Font CaptionFont = new Font(StudioTheme.SectionFont.FontFamily, 8.25F, FontStyle.Bold);

		private double minimum;
		private double maximum = 1;
		private double value;
		private bool dragging;
		private bool hovered;
		private int dragStartY;
		private double dragStartPosition;
		private bool dragMoved;
		private bool keyHeld;
		private readonly Timer wheelCommit = new Timer { Interval = 400 };

		public event EventHandler ValueChanged;
		public event EventHandler ValueCommitted;

		public PedalKnob(string caption)
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.Selectable | ControlStyles.StandardDoubleClick, true);
			Caption = caption;
			Text = caption;
			BackColor = Body;
			ForeColor = Lettering;
			Font = StudioTheme.Readout;
			TabStop = true;
			Cursor = Cursors.SizeNS;
			AccessibleRole = AccessibleRole.Dial;
			AccessibleName = caption.Substring(0, 1) + caption.Substring(1).ToLowerInvariant();
			wheelCommit.Tick += (sender, args) => { wheelCommit.Stop(); ValueCommitted?.Invoke(this, EventArgs.Empty); };
		}

		/// <summary>Upper-case stomp-box lettering above the dial.</summary>
		public string Caption { get; }
		public double Minimum { get => minimum; set { minimum = value; Invalidate(); } }
		public double Maximum { get => maximum; set { maximum = value; Invalidate(); } }
		/// <summary>Values snap to multiples of this from user input (0 = no snapping).</summary>
		public double Step { get; set; }
		public double DefaultValue { get; set; }
		public bool LogTaper { get; set; }
		public Func<double, string> Formatter { get; set; }

		public double Value => value;

		/// <summary>A gesture is in progress: a drag, a held key, or scrolling that has not settled into ValueCommitted.</summary>
		public bool IsAdjusting => dragging || keyHeld || wheelCommit.Enabled;

		/// <summary>Ends any gesture without raising ValueCommitted (the value it was editing no longer applies).</summary>
		public void CancelGesture()
		{
			wheelCommit.Stop();
			keyHeld = false;
			if (!dragging) return;
			dragging = false;
			dragMoved = false;
			Capture = false;
		}

		public string DisplayValue => Formatter != null ? Formatter(value) : value.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture);

		/// <summary>Set from code (settings load, the panel's own state). Raises no events.</summary>
		public void SetValue(double next)
		{
			next = Clamp(next);
			if (next == value) return;
			value = next;
			Invalidate();
			AccessibilityNotifyClients(AccessibleEvents.ValueChange, -1);
		}

		public override Size GetPreferredSize(Size proposedSize) => new Size(LogicalToDeviceUnits(120), LogicalToDeviceUnits(124));

		private double Clamp(double v) => Math.Max(minimum, Math.Min(maximum, v));

		private double PositionOf(double v)
		{
			if (maximum <= minimum) return 0;
			if (LogTaper && minimum > 0)
				return Math.Log(Clamp(v) / minimum) / Math.Log(maximum / minimum);
			return (Clamp(v) - minimum) / (maximum - minimum);
		}

		private double ValueAt(double position)
		{
			position = Math.Max(0, Math.Min(1, position));
			if (LogTaper && minimum > 0)
				return minimum * Math.Pow(maximum / minimum, position);
			return minimum + position * (maximum - minimum);
		}

		private double Snap(double v)
		{
			if (Step > 0) v = Math.Round(v / Step) * Step;
			// Kill binary noise (0.30000000000000004) so formatting and equality stay clean.
			return Clamp(Math.Round(v, 6));
		}

		private void SetFromUser(double next)
		{
			next = Snap(next);
			if (next == value) return;
			value = next;
			Invalidate();
			AccessibilityNotifyClients(AccessibleEvents.ValueChange, -1);
			ValueChanged?.Invoke(this, EventArgs.Empty);
		}

		/// <summary>One wheel notch or arrow press. Linear knobs move by Step; log knobs by a fixed share of the
		/// sweep, but always by at least one Step so a notch is never swallowed by snapping.</summary>
		private void Nudge(int notches)
		{
			if (notches == 0) return;
			double next;
			if (LogTaper)
			{
				next = ValueAt(PositionOf(value) + notches / (double)WheelNotches);
				double snapped = Snap(next);
				if (snapped == value && Step > 0) next = value + Math.Sign(notches) * Step;
			}
			else
			{
				next = value + notches * (Step > 0 ? Step : (maximum - minimum) / 100.0);
			}
			SetFromUser(next);
		}

		protected override void OnMouseDown(MouseEventArgs e)
		{
			base.OnMouseDown(e);
			if (e.Button != MouseButtons.Left || !Enabled) return;
			Focus();
			Capture = true;
			dragging = true;
			dragMoved = false;
			dragStartY = e.Y;
			dragStartPosition = PositionOf(value);
		}

		protected override void OnMouseMove(MouseEventArgs e)
		{
			base.OnMouseMove(e);
			if (!dragging) return;
			double travel = Math.Max(1, LogicalToDeviceUnits(DragTravel));
			if ((ModifierKeys & Keys.Control) != 0) travel *= 5;   // fine adjust
			double delta = (dragStartY - e.Y) / travel;
			if (delta != 0) dragMoved = true;
			SetFromUser(ValueAt(dragStartPosition + delta));
		}

		protected override void OnMouseUp(MouseEventArgs e)
		{
			base.OnMouseUp(e);
			if (e.Button != MouseButtons.Left || !dragging) return;
			dragging = false;
			Capture = false;
			if (dragMoved) ValueCommitted?.Invoke(this, EventArgs.Empty);
		}

		protected override void OnMouseCaptureChanged(EventArgs e)
		{
			base.OnMouseCaptureChanged(e);
			if (dragging && !Capture)
			{
				dragging = false;
				if (dragMoved) ValueCommitted?.Invoke(this, EventArgs.Empty);
			}
		}

		protected override void OnDoubleClick(EventArgs e)
		{
			base.OnDoubleClick(e);
			if (!Enabled) return;
			dragging = false;
			Capture = false;
			SetFromUser(DefaultValue);
			ValueCommitted?.Invoke(this, EventArgs.Empty);
		}

		protected override void OnMouseWheel(MouseEventArgs e)
		{
			base.OnMouseWheel(e);
			if (!Enabled) return;
			if (e is HandledMouseEventArgs handled) handled.Handled = true;
			Nudge(Math.Sign(e.Delta));
			wheelCommit.Stop();
			wheelCommit.Start();
		}

		protected override bool IsInputKey(Keys keyData)
		{
			switch (keyData & Keys.KeyCode)
			{
				case Keys.Up: case Keys.Down: case Keys.Left: case Keys.Right:
				case Keys.PageUp: case Keys.PageDown: case Keys.Home: case Keys.End:
					return true;
			}
			return base.IsInputKey(keyData);
		}

		protected override void OnKeyDown(KeyEventArgs e)
		{
			base.OnKeyDown(e);
			if (!Enabled) return;
			switch (e.KeyCode)
			{
				case Keys.Up: case Keys.Right: Nudge(1); break;
				case Keys.Down: case Keys.Left: Nudge(-1); break;
				case Keys.PageUp: Nudge(5); break;
				case Keys.PageDown: Nudge(-5); break;
				case Keys.Home: SetFromUser(minimum); break;
				case Keys.End: SetFromUser(maximum); break;
				case Keys.Delete: case Keys.Back: SetFromUser(DefaultValue); break;
				default: return;
			}
			keyHeld = true;
			e.Handled = e.SuppressKeyPress = true;
		}

		protected override void OnKeyUp(KeyEventArgs e)
		{
			base.OnKeyUp(e);
			switch (e.KeyCode)
			{
				case Keys.Up: case Keys.Down: case Keys.Left: case Keys.Right:
				case Keys.PageUp: case Keys.PageDown: case Keys.Home: case Keys.End:
				case Keys.Delete: case Keys.Back:
					keyHeld = false;
					ValueCommitted?.Invoke(this, EventArgs.Empty);
					break;
			}
		}

		protected override void OnMouseEnter(EventArgs e) { hovered = true; Invalidate(); base.OnMouseEnter(e); }
		protected override void OnMouseLeave(EventArgs e) { hovered = false; Invalidate(); base.OnMouseLeave(e); }
		protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
		protected override void OnLostFocus(EventArgs e) { keyHeld = false; Invalidate(); base.OnLostFocus(e); }
		protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }

		protected override void OnPaint(PaintEventArgs e)
		{
			var canvas = e.Graphics;
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			canvas.Clear(BackColor);

			int captionHeight = LogicalToDeviceUnits(18);
			int valueHeight = LogicalToDeviceUnits(22);
			TextRenderer.DrawText(canvas, Caption, CaptionFont, new Rectangle(0, 0, Width, captionHeight), Lettering,
				TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine);

			int dial = Math.Min(LogicalToDeviceUnits(DialSize), Math.Min(Width - LogicalToDeviceUnits(24), Height - captionHeight - valueHeight - LogicalToDeviceUnits(16)));
			dial = Math.Max(LogicalToDeviceUnits(20), dial);
			int centerX = Width / 2;
			int centerY = captionHeight + LogicalToDeviceUnits(8) + (Height - captionHeight - valueHeight - LogicalToDeviceUnits(8)) / 2;
			float ringWidth = Math.Max(3f, LogicalToDeviceUnits(4));
			int ringRadius = dial / 2 + LogicalToDeviceUnits(7);
			var ring = new RectangleF(centerX - ringRadius, centerY - ringRadius, ringRadius * 2, ringRadius * 2);
			double position = PositionOf(value);

			using (var pen = new Pen(Track, ringWidth) { StartCap = LineCap.Round, EndCap = LineCap.Round })
				canvas.DrawArc(pen, ring, StartAngle, SweepAngle);
			if (position > 0.001)
				using (var pen = new Pen(Enabled ? Fill : StudioTheme.Blend(Fill, Body, 0.6), ringWidth) { StartCap = LineCap.Round, EndCap = LineCap.Round })
					canvas.DrawArc(pen, ring, StartAngle, (float)(SweepAngle * position));

			// Knob cap: a darker disc with a soft top highlight, the enclosure border colour as its rim.
			var cap = new Rectangle(centerX - dial / 2, centerY - dial / 2, dial, dial);
			using (var shadow = new SolidBrush(Color.FromArgb(70, 0, 0, 0)))
				canvas.FillEllipse(shadow, cap.X, cap.Y + LogicalToDeviceUnits(2), cap.Width, cap.Height);
			Color capTop = StudioTheme.Shift(ColorTranslator.FromHtml("#163f3a"), hovered && Enabled ? 18 : 8);
			Color capBottom = ColorTranslator.FromHtml("#0e2b27");
			using (var brush = new LinearGradientBrush(new Rectangle(cap.X, cap.Y, cap.Width, cap.Height + 1), capTop, capBottom, LinearGradientMode.Vertical))
				canvas.FillEllipse(brush, cap);
			using (var pen = new Pen(Focused ? Fill : Border, Focused ? Math.Max(2f, LogicalToDeviceUnits(2)) : 1.5f))
				canvas.DrawEllipse(pen, cap);

			// Pointer from near the centre to the rim, at the value's angle.
			double radians = (StartAngle + SweepAngle * position) * Math.PI / 180.0;
			float inner = dial * 0.14f;
			float outer = dial / 2f - LogicalToDeviceUnits(5);
			using (var pen = new Pen(Enabled ? Lettering : StudioTheme.Faint, Math.Max(2.5f, LogicalToDeviceUnits(3))) { StartCap = LineCap.Round, EndCap = LineCap.Round })
				canvas.DrawLine(pen,
					(float)(centerX + Math.Cos(radians) * inner), (float)(centerY + Math.Sin(radians) * inner),
					(float)(centerX + Math.Cos(radians) * outer), (float)(centerY + Math.Sin(radians) * outer));

			TextRenderer.DrawText(canvas, DisplayValue, Font, new Rectangle(0, Height - valueHeight, Width, valueHeight), Enabled ? Lettering : StudioTheme.Faint,
				TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine);
		}

		protected override AccessibleObject CreateAccessibilityInstance() => new KnobAccessibleObject(this);

		protected override void Dispose(bool disposing)
		{
			if (disposing) wheelCommit.Dispose();
			base.Dispose(disposing);
		}

		private sealed class KnobAccessibleObject : ControlAccessibleObject
		{
			private readonly PedalKnob knob;

			public KnobAccessibleObject(PedalKnob owner) : base(owner) { knob = owner; }

			public override AccessibleRole Role => AccessibleRole.Dial;

			public override string Value
			{
				get => knob.DisplayValue;
				set { }
			}
		}
	}
}
