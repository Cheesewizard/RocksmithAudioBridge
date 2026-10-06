using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>Small speaker toggle under each fader. Muting parks the level at zero and remembers where it was.</summary>
	internal sealed class MuteButton : Control
	{
		private bool muted;
		private bool hovered;

		public MuteButton()
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
			Cursor = Cursors.Hand;
			TabStop = true;
			Size = new Size(52, 26);
			AccessibleRole = AccessibleRole.CheckButton;
		}

		public Color Tint { get; set; } = StudioTheme.Accent;

		public bool Muted
		{
			get => muted;
			set { if (muted == value) return; muted = value; Invalidate(); }
		}

		protected override void OnPaint(PaintEventArgs args)
		{
			var canvas = args.Graphics;
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			Color accent = !Enabled ? StudioTheme.Faint : muted ? StudioTheme.Record : StudioTheme.Muted;
			var bounds = new Rectangle(0, 0, Width - 1, Height - 1);
			using (var path = StudioTheme.RoundedRectangle(bounds, 6))
			{
				Color fill = muted && Enabled
					? StudioTheme.Blend(StudioTheme.Elevated, StudioTheme.Record, 0.28)
					: StudioTheme.Blend(StudioTheme.Elevated, StudioTheme.Background, 0.35);
				if (hovered && Enabled)
					fill = StudioTheme.Shift(fill, 12);
				using (var brush = new SolidBrush(fill))
					canvas.FillPath(brush, path);
				using (var pen = new Pen(Focused && Enabled ? StudioTheme.Blend(StudioTheme.Line, Tint, 0.8) : StudioTheme.Line))
					canvas.DrawPath(pen, path);
			}
			float left = Width / 2f - 8;
			float top = Height / 2f - 7;
			using (var pen = new Pen(accent, 1.7f) { StartCap = LineCap.Round, EndCap = LineCap.Round })
			using (var brush = new SolidBrush(accent))
			{
				canvas.FillPolygon(brush, new[]
				{
					new PointF(left, top + 5), new PointF(left + 3, top + 5), new PointF(left + 7, top + 1),
					new PointF(left + 7, top + 13), new PointF(left + 3, top + 9), new PointF(left, top + 9)
				});
				if (muted)
					canvas.DrawLine(pen, left + 10, top + 2, left + 17, top + 12);
				else
					canvas.DrawArc(pen, left + 8, top + 3, 7, 8, -60, 120);
			}
		}

		protected override void OnMouseEnter(EventArgs args) { hovered = true; Invalidate(); base.OnMouseEnter(args); }

		protected override void OnMouseLeave(EventArgs args) { hovered = false; Invalidate(); base.OnMouseLeave(args); }

		protected override void OnMouseDown(MouseEventArgs args) { Focus(); base.OnMouseDown(args); }

		protected override void OnKeyDown(KeyEventArgs args)
		{
			if (args.KeyCode == Keys.Space || args.KeyCode == Keys.Enter)
			{
				args.Handled = true;
				OnClick(EventArgs.Empty);
			}
			base.OnKeyDown(args);
		}

		protected override void OnEnabledChanged(EventArgs args) { Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); base.OnEnabledChanged(args); }

		protected override void OnGotFocus(EventArgs args) { Invalidate(); base.OnGotFocus(args); }

		protected override void OnLostFocus(EventArgs args) { Invalidate(); base.OnLostFocus(args); }
	}

	/// <summary>One console channel: icon, name, fader, level readout and mute. The master strip also carries an output meter.</summary>
	internal sealed class MixerStrip : UserControl
	{
		public const int NormalWidth = 108;
		public const int MasterWidth = 132;

		public MixerChannel Channel { get; }

		public int Value => slider.Value;

		public bool IsAdjusting => slider.Capture;

		public event EventHandler VolumeChanged;

		private readonly StudioVolumeSlider slider = new StudioVolumeSlider { Dock = DockStyle.Fill };
		private readonly ChannelMeter meter;
		private readonly MuteButton mute = new MuteButton();
		private readonly Label readout = new Label
		{
			Text = "--",
			Dock = DockStyle.Fill,
			TextAlign = ContentAlignment.MiddleCenter,
			ForeColor = StudioTheme.Ink,
			Font = StudioTheme.Readout,
			UseMnemonic = false
		};
		private readonly Color tint;
		private readonly Color face;
		private readonly ChannelGlyph glyph;
		private bool available = true;
		private bool paintedAvailable = true;
		private int restoreValue = 100;

		public MixerStrip(MixerChannel channel, bool withMeter = false)
		{
			Channel = channel;
			tint = ChannelStyle.Tint(channel);
			face = StudioTheme.Elevated;
			glyph = ChannelStyle.Glyph(channel);
			meter = withMeter ? new ChannelMeter { Dock = DockStyle.Fill, BackColor = face } : null;
			Width = withMeter ? MasterWidth : NormalWidth;
			Margin = new Padding(4, 0, 4, 0);
			Padding = new Padding(6, 8, 6, 8);
			BackColor = StudioTheme.Surface;
			SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
			slider.BackColor = face;
			mute.BackColor = face;
			readout.BackColor = face;

			slider.Tint = tint;
			slider.AccessibleName = ChannelStyle.Name(channel) + " playback volume";
			slider.AccessibleDescription = ChannelStyle.Description(channel) + " Zero to one hundred percent.";
			mute.Tint = tint;
			mute.AccessibleName = "Mute " + ChannelStyle.Name(channel);

			var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 4, Margin = new Padding(0), BackColor = face };
			layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 54));
			layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
			layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 28));
			layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));

			var heading = new Panel { Dock = DockStyle.Fill, Margin = new Padding(0), BackColor = face };
			heading.Paint += DrawHeading;
			layout.Controls.Add(heading, 0, 0);
			layout.Controls.Add(BuildTravel(), 0, 1);
			layout.Controls.Add(readout, 0, 2);
			layout.Controls.Add(BuildMuteRow(), 0, 3);
			Controls.Add(layout);

			slider.Scroll += HandleScroll;
			mute.Click += ToggleMute;
		}

		private Control BuildTravel()
		{
			if (meter == null)
			{
				var solo = new Panel { Dock = DockStyle.Fill, Margin = new Padding(0), BackColor = face };
				solo.Controls.Add(slider);
				return solo;
			}
			var travel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Margin = new Padding(0), BackColor = face };
			travel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			travel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 22));
			travel.Controls.Add(slider, 0, 0);
			travel.Controls.Add(meter, 1, 0);
			return travel;
		}

		private Control BuildMuteRow()
		{
			var row = new Panel { Dock = DockStyle.Fill, Margin = new Padding(0), BackColor = face };
			mute.Location = new Point(0, 3);
			row.Controls.Add(mute);
			row.Resize += (sender, args) => mute.Location = new Point(Math.Max(0, (row.Width - mute.Width) / 2), 3);
			return row;
		}

		public void SetVolume(float volume)
		{
			if (float.IsNaN(volume) || float.IsInfinity(volume) || volume < 0 || volume > 100)
				throw new ArgumentOutOfRangeException(nameof(volume));
			available = true;
			slider.Value = (int)Math.Round(volume);
			slider.Visible = true;
			slider.Enabled = true;
			mute.Enabled = true;
			if (slider.Value > 0)
				restoreValue = slider.Value;
			UpdateReadout();
		}

		public void SetUnavailable()
		{
			available = false;
			Enabled = true;
			slider.Enabled = false;
			slider.Visible = true;
			mute.Enabled = false;
			UpdateReadout();
		}

		public void SetLevel(int peak)
		{
			meter?.SetLevel(peak);
		}

		public void ResetLevel()
		{
			meter?.Reset();
		}

		private void UpdateReadout()
		{
			readout.Text = available ? slider.Value + "%" : "n/a";
			readout.ForeColor = !available || slider.Value == 0 ? StudioTheme.Faint : StudioTheme.Ink;
			mute.Muted = available && slider.Value == 0;
			if (available == paintedAvailable)
				return;
			paintedAvailable = available;
			Invalidate();
		}

		private void HandleScroll(object sender, EventArgs args)
		{
			if (slider.Value > 0)
				restoreValue = slider.Value;
			UpdateReadout();
			VolumeChanged?.Invoke(this, EventArgs.Empty);
		}

		private void ToggleMute(object sender, EventArgs args)
		{
			if (!available || !slider.Enabled)
				return;
			int next = slider.Value > 0 ? 0 : restoreValue > 0 ? restoreValue : 100;
			if (slider.Value > 0)
				restoreValue = slider.Value;
			slider.Value = next;
			UpdateReadout();
			VolumeChanged?.Invoke(this, EventArgs.Empty);
		}

		private void DrawHeading(object sender, PaintEventArgs args)
		{
			var host = (Control)sender;
			var canvas = args.Graphics;
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			Color live = available ? tint : StudioTheme.Faint;
			ChannelStyle.Draw(canvas, glyph, new PointF(host.Width / 2f - 11, 1), 0.92f, live);
			TextRenderer.DrawText(canvas, ChannelStyle.Name(Channel), StudioTheme.Body,
				new Rectangle(2, 29, host.Width - 4, 23), available ? StudioTheme.Ink : StudioTheme.Faint,
				TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
		}

		protected override void OnPaintBackground(PaintEventArgs args)
		{
			using (var brush = new SolidBrush(Parent?.BackColor ?? StudioTheme.Background))
				args.Graphics.FillRectangle(brush, ClientRectangle);
		}

		protected override void OnPaint(PaintEventArgs args)
		{
			var canvas = args.Graphics;
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			var bounds = new Rectangle(0, 0, Width - 1, Height - 1);
			using (var path = StudioTheme.RoundedRectangle(bounds, 9))
			{
				using (var brush = new SolidBrush(face))
					canvas.FillPath(brush, path);
				using (var pen = new Pen(StudioTheme.Line))
					canvas.DrawPath(pen, path);
			}
			using (var brush = new SolidBrush(StudioTheme.Blend(StudioTheme.Line, tint, available ? 0.85 : 0.0)))
				canvas.FillRectangle(brush, 0, 0, Width, 3);
			base.OnPaint(args);
		}
	}
}
