using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal sealed class RockerToggle : Control
	{
		public bool IsOn
		{
			get { return isOn; }
			set
			{
				if (isOn == value) return;
				isOn = value;
				AccessibleName = "Audio bridge " + (isOn ? "on" : "off");
				Invalidate();
				Toggled?.Invoke(this, EventArgs.Empty);
			}
		}

		private bool isOn;
		private bool isHovered;

		public event EventHandler Toggled;

		public RockerToggle()
		{
			Width = 148;
			Height = 40;
			Margin = new Padding(0);
			Font = StudioTheme.Strong;
			Cursor = Cursors.Hand;
			TabStop = true;
			AccessibleName = "Audio bridge off";
			AccessibleRole = AccessibleRole.CheckButton;
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer
				| ControlStyles.ResizeRedraw | ControlStyles.Selectable | ControlStyles.StandardClick, true);
		}

		protected override void OnClick(EventArgs e)
		{
			if (Enabled) IsOn = !IsOn;
			base.OnClick(e);
		}

		protected override void OnEnter(EventArgs e)
		{
			Invalidate();
			base.OnEnter(e);
		}

		protected override void OnKeyDown(KeyEventArgs e)
		{
			if (Enabled && (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter))
			{
				IsOn = !IsOn;
				e.Handled = true;
				e.SuppressKeyPress = true;
			}
			base.OnKeyDown(e);
		}

		protected override void OnLeave(EventArgs e)
		{
			Invalidate();
			base.OnLeave(e);
		}

		protected override void OnMouseEnter(EventArgs e)
		{
			isHovered = true;
			Invalidate();
			base.OnMouseEnter(e);
		}

		protected override void OnMouseLeave(EventArgs e)
		{
			isHovered = false;
			Invalidate();
			base.OnMouseLeave(e);
		}

		protected override void OnMouseDown(MouseEventArgs e)
		{
			Focus();
			base.OnMouseDown(e);
		}

		protected override void OnEnabledChanged(EventArgs e)
		{
			Cursor = Enabled ? Cursors.Hand : Cursors.Default;
			Invalidate();
			base.OnEnabledChanged(e);
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
			var outerBounds = new Rectangle(0, 0, Width - 1, Height - 1);
			using (var outerPath = StudioTheme.RoundedRectangle(outerBounds, 10))
			{
				using (var background = new SolidBrush(Enabled && isHovered ? StudioTheme.Shift(StudioTheme.Field, 7) : StudioTheme.Field))
					e.Graphics.FillPath(background, outerPath);
				using (var border = new Pen(Focused ? StudioTheme.Accent : StudioTheme.Line, Focused ? 2F : 1F))
					e.Graphics.DrawPath(border, outerPath);
			}

			int halfWidth = (Width - 8) / 2;
			var activeBounds = new Rectangle(IsOn ? 4 + halfWidth : 4, 4, halfWidth, Height - 9);
			using (var activePath = StudioTheme.RoundedRectangle(activeBounds, 7))
			using (var active = new LinearGradientBrush(
				activeBounds,
				IsOn ? StudioTheme.Blend(StudioTheme.Positive, StudioTheme.Elevated, 0.35) : StudioTheme.Neutral,
				IsOn ? StudioTheme.Blend(StudioTheme.Positive, StudioTheme.Field, 0.65) : StudioTheme.Blend(StudioTheme.Neutral, StudioTheme.Well, 0.35),
				LinearGradientMode.Vertical))
			{
				e.Graphics.FillPath(active, activePath);
				using (var edge = new Pen(IsOn ? StudioTheme.Positive : StudioTheme.Faint))
					e.Graphics.DrawPath(edge, activePath);
			}

			var offBounds = new Rectangle(4, 4, halfWidth, Height - 9);
			var onBounds = new Rectangle(4 + halfWidth, 4, halfWidth, Height - 9);
			Color inactiveText = Enabled ? StudioTheme.Muted : StudioTheme.Faint;
			TextRenderer.DrawText(e.Graphics, "Off", IsOn ? StudioTheme.Body : StudioTheme.Strong, offBounds,
				!IsOn && Enabled ? Color.White : inactiveText,
				TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
			TextRenderer.DrawText(e.Graphics, "On", IsOn ? StudioTheme.Strong : StudioTheme.Body, onBounds,
				IsOn ? StudioTheme.Positive : inactiveText,
				TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
		}
	}
}
