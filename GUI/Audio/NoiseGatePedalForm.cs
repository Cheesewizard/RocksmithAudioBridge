using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>Text for the noise gate values, shared by the settings row summary and the pedal knobs.</summary>
	internal static class NoiseGateFormat
	{
		public static string Threshold(double db) => db.ToString("0.#", CultureInfo.InvariantCulture) + " dB";
		public static string Reduction(double db) => db.ToString("0", CultureInfo.InvariantCulture) + " dB";
		public static string Decay(double ms) => ms >= 1000
			? (ms / 1000.0).ToString("0.0", CultureInfo.InvariantCulture) + " s"
			: ms.ToString("0", CultureInfo.InvariantCulture) + " ms";
		public static string Attack(double ms) => ms < 1
			? ms.ToString("0.0", CultureInfo.InvariantCulture) + " ms"
			: ms.ToString("0.#", CultureInfo.InvariantCulture) + " ms";

		/// <summary>Detector level in dB from the status GateMeter word, or null when the gate reports no data.</summary>
		public static double? MeterLevelDb(uint meter)
		{
			if ((meter & 0x40000000u) == 0) return null;
			return ((int)(meter & 0xFFFFu) - 2000) / 10.0;
		}

		public static bool MeterOpen(uint meter) => (meter & 0x40000000u) != 0 && (meter & 0x80000000u) != 0;
	}

	internal sealed class GatePedalChangeEventArgs : EventArgs
	{
		public GatePedalChangeEventArgs(bool isThreshold, bool committed)
		{
			IsThreshold = isThreshold;
			Committed = committed;
		}

		/// <summary>The threshold knob moved (op 19); otherwise decay, reduction or attack (op 31).</summary>
		public bool IsThreshold { get; }
		/// <summary>The gesture ended (release, key up, reset): persist now.</summary>
		public bool Committed { get; }
	}

	/// <summary>A small round status light. Green while lit, a dim green-grey otherwise.</summary>
	internal sealed class PedalLed : Control
	{
		public static readonly Color OnColor = ColorTranslator.FromHtml("#97C459");
		public static readonly Color OffColor = ColorTranslator.FromHtml("#33473f");
		private bool lit;

		public PedalLed(int diameter)
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
			SetStyle(ControlStyles.Selectable, false);
			TabStop = false;
			Size = new Size(diameter, diameter);
			AccessibleRole = AccessibleRole.Indicator;
			AccessibleName = "Noise gate open light";
		}

		public bool Lit
		{
			get => lit;
			set
			{
				if (lit == value) return;
				lit = value;
				AccessibleDescription = lit ? "Open" : "Closed";
				Invalidate();
			}
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
			e.Graphics.Clear(BackColor);
			int size = Math.Min(Width, Height) - 2;
			var dot = new Rectangle((Width - size) / 2, (Height - size) / 2, size, size);
			NoiseGatePedalForm.PaintLed(e.Graphics, dot, lit);
		}
	}

	/// <summary>The round footswitch at the bottom of the pedal. Click, Space or Enter toggles the gate.</summary>
	internal sealed class PedalFootswitch : Control
	{
		private bool on;
		private bool hovered;
		private bool pressed;

		public event EventHandler Toggled;

		public PedalFootswitch()
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
			TabStop = true;
			Cursor = Cursors.Hand;
			BackColor = PedalKnob.Body;
			AccessibleRole = AccessibleRole.CheckButton;
			AccessibleName = "Footswitch, noise gate on or off";
		}

		public bool On
		{
			get => on;
			set
			{
				if (on == value) return;
				on = value;
				AccessibleDescription = on ? "On" : "Off";
				AccessibilityNotifyClients(AccessibleEvents.StateChange, -1);
				Invalidate();
			}
		}

		private void Press()
		{
			On = !On;
			Toggled?.Invoke(this, EventArgs.Empty);
		}

		protected override void OnMouseDown(MouseEventArgs e)
		{
			base.OnMouseDown(e);
			if (e.Button != MouseButtons.Left) return;
			Focus();
			pressed = true;
			Invalidate();
		}

		protected override void OnMouseUp(MouseEventArgs e)
		{
			base.OnMouseUp(e);
			if (e.Button != MouseButtons.Left || !pressed) return;
			pressed = false;
			Invalidate();
			if (ClientRectangle.Contains(e.Location)) Press();
		}

		protected override void OnKeyDown(KeyEventArgs e)
		{
			base.OnKeyDown(e);
			if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter)
			{
				Press();
				e.Handled = e.SuppressKeyPress = true;
			}
		}

		protected override bool IsInputKey(Keys keyData) => (keyData & Keys.KeyCode) == Keys.Enter || base.IsInputKey(keyData);

		protected override void OnMouseEnter(EventArgs e) { hovered = true; Invalidate(); base.OnMouseEnter(e); }
		protected override void OnMouseLeave(EventArgs e) { hovered = false; pressed = false; Invalidate(); base.OnMouseLeave(e); }
		protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
		protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

		protected override void OnPaint(PaintEventArgs e)
		{
			var canvas = e.Graphics;
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			canvas.Clear(BackColor);
			int size = Math.Min(Width, Height) - LogicalToDeviceUnits(4);
			var outer = new Rectangle((Width - size) / 2, (Height - size) / 2, size, size);
			// Hex-nut style collar, then the domed metal button, pushed down a touch while pressed.
			using (var brush = new SolidBrush(ColorTranslator.FromHtml("#14403a")))
				canvas.FillEllipse(brush, outer);
			using (var pen = new Pen(Focused ? PedalKnob.Fill : PedalKnob.Border, Focused ? Math.Max(2f, LogicalToDeviceUnits(2)) : 1.5f))
				canvas.DrawEllipse(pen, outer);
			int inset = LogicalToDeviceUnits(pressed ? 9 : 7);
			var dome = Rectangle.Inflate(outer, -inset, -inset);
			Color top = StudioTheme.Shift(Color.FromArgb(214, 222, 226), hovered ? 10 : 0);
			Color bottom = Color.FromArgb(120, 132, 138);
			using (var brush = new LinearGradientBrush(new Rectangle(dome.X, dome.Y, dome.Width, dome.Height + 1), pressed ? bottom : top, pressed ? top : bottom, LinearGradientMode.Vertical))
				canvas.FillEllipse(brush, dome);
			using (var pen = new Pen(Color.FromArgb(90, 0, 0, 0)))
				canvas.DrawEllipse(pen, dome);
			var shine = new Rectangle(dome.X + dome.Width / 4, dome.Y + dome.Height / 8, dome.Width / 2, dome.Height / 4);
			using (var brush = new SolidBrush(Color.FromArgb(pressed ? 30 : 70, 255, 255, 255)))
				canvas.FillEllipse(brush, shine);
		}
	}

	/// <summary>
	/// The noise gate pop-out: a non-modal tool window painted as a stomp box. Teal enclosure, NOISE GATE lettering,
	/// an open/closed LED, four knobs (threshold, decay, reduction, attack), the live detector meter with the threshold
	/// as a yellow line, and a footswitch. It holds no pipe of its own: AudioRoutingPanel owns the values and the
	/// connection, pushes the latest GateMeter in through SetMeter on every status poll, and listens to GateToggled
	/// and SettingChanged to send op 19 / op 31 and persist.
	/// </summary>
	internal sealed class NoiseGatePedalForm : Form
	{
		private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
		private const double MeterFloorDb = -90.0;

		[DllImport("dwmapi.dll")]
		private static extern int DwmSetWindowAttribute(IntPtr windowHandle, int attribute, ref int value, int valueSize);

		public static readonly Color MeterTrack = ColorTranslator.FromHtml("#123a35");
		public static readonly Color MeterFill = ColorTranslator.FromHtml("#5DCAA5");
		public static readonly Color ThresholdLine = ColorTranslator.FromHtml("#FAC775");
		private const string MeterCaption = "Yellow line = threshold. Set it just above where the meter rests when you're not playing.";

		private readonly PedalBody body;
		private readonly PedalKnob threshold = new PedalKnob("THRESHOLD") { Minimum = -80, Maximum = -20, Step = 0.5, DefaultValue = -62, Formatter = NoiseGateFormat.Threshold };
		private readonly PedalKnob decay = new PedalKnob("DECAY") { Minimum = 50, Maximum = 2000, Step = 1, DefaultValue = 200, LogTaper = true, Formatter = NoiseGateFormat.Decay };
		private readonly PedalKnob reduction = new PedalKnob("REDUCTION") { Minimum = -80, Maximum = -10, Step = 1, DefaultValue = -60, Formatter = NoiseGateFormat.Reduction };
		private readonly PedalKnob attack = new PedalKnob("ATTACK") { Minimum = 0.1, Maximum = 20, Step = 0.1, DefaultValue = 0.5, LogTaper = true, Formatter = NoiseGateFormat.Attack };
		private readonly PedalFootswitch footswitch = new PedalFootswitch();
		private readonly ToolTip tips = new ToolTip { AutoPopDelay = 12000, InitialDelay = 400, ReshowDelay = 120 };
		private uint meter;

		/// <summary>The footswitch was pressed. Read GateOn for the new state.</summary>
		public event EventHandler GateToggled;
		/// <summary>A knob moved (live) or a knob gesture ended (Committed).</summary>
		public event EventHandler<GatePedalChangeEventArgs> SettingChanged;

		public NoiseGatePedalForm()
		{
			Text = "Noise gate · Player 1";
			AutoScaleDimensions = new SizeF(96F, 96F);
			AutoScaleMode = AutoScaleMode.Dpi;
			FormBorderStyle = FormBorderStyle.FixedToolWindow;
			MaximizeBox = false;
			MinimizeBox = false;
			ShowInTaskbar = false;
			StartPosition = FormStartPosition.Manual;
			KeyPreview = false;
			BackColor = StudioTheme.Background;
			ForeColor = StudioTheme.Ink;
			Font = StudioTheme.Body;
			ClientSize = new Size(300, 512);
			DoubleBuffered = true;

			body = new PedalBody(this) { Dock = DockStyle.Fill };
			body.Controls.Add(threshold);
			body.Controls.Add(decay);
			body.Controls.Add(reduction);
			body.Controls.Add(attack);
			body.Controls.Add(footswitch);
			Controls.Add(body);

			foreach (var knob in new[] { threshold, decay, reduction, attack })
			{
				var current = knob;
				bool isThreshold = current == threshold;
				current.ValueChanged += (sender, args) =>
				{
					if (isThreshold) body.Invalidate(body.MeterBounds);
					SettingChanged?.Invoke(this, new GatePedalChangeEventArgs(isThreshold, false));
				};
				current.ValueCommitted += (sender, args) => SettingChanged?.Invoke(this, new GatePedalChangeEventArgs(isThreshold, true));
			}
			footswitch.Toggled += (sender, args) => GateToggled?.Invoke(this, EventArgs.Empty);

			StudioTheme.StyleTips(tips);
			const string turn = " Drag up or down, scroll, or use the arrow keys. Double-click to reset.";
			tips.SetToolTip(threshold, "Threshold from -80 to -20 dB: where the gate opens. It listens to the guitar's own range, not the hiss or hum, and fades each note out smoothly. Lower keeps quieter note tails; if noise gets through between notes, raise it a few dB." + turn);
			tips.SetToolTip(decay, "Decay from 50 ms to 2 s: how long a note takes to fade once the gate closes." + turn);
			tips.SetToolTip(reduction, "Reduction from -80 to -10 dB: how far the gate turns the noise down. -60 is near silence; around -20 leaves a faint natural floor." + turn);
			tips.SetToolTip(attack, "Attack from 0.1 to 20 ms: how fast the gate opens. Keep it short so your pick attack stays sharp." + turn);
			tips.SetToolTip(footswitch, "Switches the noise gate on or off.");
		}

		public bool GateOn => footswitch.On;
		public int ThresholdTenths => (int)Math.Round(threshold.Value * 10.0);
		public int DecayMs => (int)Math.Round(decay.Value);
		public int ReductionDb => (int)Math.Round(reduction.Value);
		public int AttackTenths => Math.Max(1, Math.Min(200, (int)Math.Round(attack.Value * 10.0)));

		/// <summary>Load every value from the panel without raising any event.</summary>
		public void SetState(bool on, int thresholdTenths, int decayMs, int reductionDb, int attackTenths)
		{
			footswitch.On = on;
			threshold.SetValue(thresholdTenths / 10.0);
			decay.SetValue(decayMs);
			reduction.SetValue(reductionDb);
			attack.SetValue(attackTenths / 10.0);
			body.Invalidate();
		}

		/// <summary>Mirror the settings row's check onto the footswitch without raising GateToggled.</summary>
		public void SetGateOn(bool on)
		{
			footswitch.On = on;
		}

		/// <summary>The latest status GateMeter word (0 = no data). Drives the LED and the meter.</summary>
		public void SetMeter(uint value)
		{
			if (meter == value) return;
			bool ledChanged = NoiseGateFormat.MeterOpen(meter) != NoiseGateFormat.MeterOpen(value);
			meter = value;
			body.Invalidate(body.MeterBounds);
			if (ledChanged) body.Invalidate(body.LedBounds);
		}

		protected override void OnHandleCreated(EventArgs e)
		{
			base.OnHandleCreated(e);
			int enabled = 1;
			DwmSetWindowAttribute(Handle, DWMWA_USE_IMMERSIVE_DARK_MODE, ref enabled, sizeof(int));
		}

		protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
		{
			if (keyData == Keys.Escape)
			{
				Close();
				return true;
			}
			return base.ProcessCmdKey(ref msg, keyData);
		}

		protected override void Dispose(bool disposing)
		{
			if (disposing) tips.Dispose();
			base.Dispose(disposing);
		}

		internal static void PaintLed(Graphics canvas, Rectangle dot, bool lit)
		{
			if (lit)
			{
				var glow = Rectangle.Inflate(dot, Math.Max(2, dot.Width / 3), Math.Max(2, dot.Height / 3));
				using (var path = new GraphicsPath())
				{
					path.AddEllipse(glow);
					using (var brush = new PathGradientBrush(path) { CenterColor = Color.FromArgb(110, PedalLed.OnColor), SurroundColors = new[] { Color.FromArgb(0, PedalLed.OnColor) } })
						canvas.FillEllipse(brush, glow);
				}
			}
			using (var brush = new SolidBrush(lit ? PedalLed.OnColor : PedalLed.OffColor))
				canvas.FillEllipse(brush, dot);
			using (var pen = new Pen(Color.FromArgb(lit ? 60 : 110, 0, 0, 0)))
				canvas.DrawEllipse(pen, dot);
			var shine = new Rectangle(dot.X + dot.Width / 4, dot.Y + dot.Height / 6, Math.Max(1, dot.Width / 3), Math.Max(1, dot.Height / 4));
			using (var brush = new SolidBrush(Color.FromArgb(lit ? 120 : 40, 255, 255, 255)))
				canvas.FillEllipse(brush, shine);
		}

		/// <summary>
		/// The enclosure. Paints the window background, the rounded teal box, the lettering, the LED, the meter and its
		/// caption, and lays out the four knobs and the footswitch on it. All geometry is in logical (96 DPI) pixels
		/// through LogicalToDeviceUnits so it scales with the window.
		/// </summary>
		private sealed class PedalBody : Control
		{
			private static readonly Font LetteringFont = new Font(StudioTheme.Title.FontFamily, 13F, FontStyle.Bold);
			private readonly NoiseGatePedalForm owner;

			public PedalBody(NoiseGatePedalForm owner)
			{
				this.owner = owner;
				SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
				SetStyle(ControlStyles.Selectable, false);
				BackColor = StudioTheme.Background;
			}

			private int L(int logical) => LogicalToDeviceUnits(logical);

			private Rectangle Enclosure => new Rectangle(L(12), L(12), Math.Max(1, Width - L(24)), Math.Max(1, Height - L(24)));

			public Rectangle LedBounds
			{
				get
				{
					var box = Enclosure;
					int size = L(14);
					return Rectangle.Inflate(new Rectangle(box.Right - L(20) - size, box.Y + L(18), size, size), L(6), L(6));
				}
			}

			private Rectangle LedDot
			{
				get
				{
					var box = Enclosure;
					int size = L(14);
					return new Rectangle(box.Right - L(20) - size, box.Y + L(18), size, size);
				}
			}

			private int KnobsTop => Enclosure.Y + L(54);
			private int KnobHeight => L(124);

			/// <summary>The meter bar, with room above and below for the threshold line's overhang.</summary>
			public Rectangle MeterBounds
			{
				get
				{
					var bar = MeterBar;
					return new Rectangle(bar.X - L(4), bar.Y - L(6), bar.Width + L(8), bar.Height + L(12));
				}
			}

			private Rectangle MeterBar
			{
				get
				{
					var box = Enclosure;
					int top = KnobsTop + KnobHeight * 2 + L(14);
					return new Rectangle(box.X + L(20), top, Math.Max(1, box.Width - L(40)), L(10));
				}
			}

			private Rectangle CaptionBounds
			{
				get
				{
					var bar = MeterBar;
					return new Rectangle(bar.X, bar.Bottom + L(8), bar.Width, L(50));
				}
			}

			protected override void OnLayout(LayoutEventArgs levent)
			{
				base.OnLayout(levent);
				var box = Enclosure;
				int column = (box.Width - L(16)) / 2;
				int knobWidth = Math.Min(L(124), column);
				var knobs = new[] { owner.threshold, owner.decay, owner.reduction, owner.attack };
				for (int i = 0; i < knobs.Length; i++)
				{
					int col = i % 2;
					int row = i / 2;
					int x = box.X + L(8) + col * column + (column - knobWidth) / 2;
					int y = KnobsTop + row * KnobHeight;
					knobs[i].Bounds = new Rectangle(x, y, knobWidth, KnobHeight - L(4));
				}
				int switchSize = L(64);
				owner.footswitch.Bounds = new Rectangle(box.X + (box.Width - switchSize) / 2, box.Bottom - L(18) - switchSize, switchSize, switchSize);
			}

			protected override void OnPaint(PaintEventArgs e)
			{
				var canvas = e.Graphics;
				canvas.SmoothingMode = SmoothingMode.AntiAlias;
				canvas.Clear(BackColor);
				var box = Enclosure;

				using (var shadow = StudioTheme.RoundedRectangle(new Rectangle(box.X, box.Y + L(3), box.Width, box.Height), L(10)))
				using (var brush = new SolidBrush(Color.FromArgb(90, 0, 0, 0)))
					canvas.FillPath(brush, shadow);
				using (var path = StudioTheme.RoundedRectangle(box, L(10)))
				{
					using (var brush = new SolidBrush(PedalKnob.Body))
						canvas.FillPath(brush, path);
					using (var pen = new Pen(PedalKnob.Border, Math.Max(2f, L(2))))
						canvas.DrawPath(pen, path);
				}

				TextRenderer.DrawText(canvas, "NOISE GATE", LetteringFont, new Point(box.X + L(18), box.Y + L(14)), PedalKnob.Lettering,
					TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine);
				PaintLed(canvas, LedDot, NoiseGateFormat.MeterOpen(owner.meter));

				// Meter: detector level from -90 dB (empty) to 0 dB (full), threshold as a yellow line.
				var bar = MeterBar;
				using (var path = StudioTheme.RoundedRectangle(bar, bar.Height / 2))
				{
					using (var brush = new SolidBrush(MeterTrack))
						canvas.FillPath(brush, path);
					double? level = NoiseGateFormat.MeterLevelDb(owner.meter);
					if (level.HasValue)
					{
						int fill = (int)Math.Round(bar.Width * Fraction(level.Value));
						if (fill > 0)
						{
							canvas.SetClip(path);
							using (var brush = new SolidBrush(MeterFill))
								canvas.FillRectangle(brush, bar.X, bar.Y, fill, bar.Height);
							canvas.ResetClip();
						}
					}
				}
				int line = bar.X + (int)Math.Round(bar.Width * Fraction(owner.threshold.Value));
				using (var pen = new Pen(ThresholdLine, Math.Max(2f, L(2))))
					canvas.DrawLine(pen, line, bar.Y - L(4), line, bar.Bottom + L(4));

				TextRenderer.DrawText(canvas, MeterCaption, StudioTheme.Small, CaptionBounds, PedalKnob.Lettering,
					TextFormatFlags.Left | TextFormatFlags.Top | TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix);
			}

			private static double Fraction(double db) => Math.Max(0, Math.Min(1, (db - MeterFloorDb) / (0 - MeterFloorDb)));
		}
	}
}
