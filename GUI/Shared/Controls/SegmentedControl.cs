using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal sealed class SegmentedControl : Control
	{
		public event EventHandler SelectedIndexChanged;
		private readonly string[] segments;
		private readonly int[] widths;
		private int selectedIndex;
		private int hoveredIndex = -1;

		// The last DeviceDpi the segment widths were measured at. The control is AutoSize and measures itself
		// from the live DPI (see GetPreferredSize/Measure), rather than caching a 96-DPI size in the constructor
		// and leaving AutoScaleMode.Dpi to stretch the outer box while the segment boxes and paddings stay at
		// 96 px, which would clip the rendered (DPI-scaled) text.
		private int metricsDpi = -1;

		public SegmentedControl(params string[] segments)
		{
			if (segments == null || segments.Length < 2)
				throw new ArgumentException("A segmented control needs at least two segments.", nameof(segments));
			this.segments = segments;
			widths = new int[segments.Length];
			Font = StudioTheme.Body;
			AutoSize = true;
			Margin = new Padding(0, 4, 0, 4);
			Cursor = Cursors.Hand;
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
		}

		public override Size GetPreferredSize(Size proposedSize)
		{
			Measure();
			int total = LogicalToDeviceUnits(8);
			foreach (int width in widths)
				total += width;
			return new Size(total, LogicalToDeviceUnits(34));
		}

		public int SelectedIndex
		{
			get { return selectedIndex; }
			set
			{
				int clamped = Math.Max(0, Math.Min(segments.Length - 1, value));
				if (clamped == selectedIndex)
					return;
				selectedIndex = clamped;
				Invalidate();
				SelectedIndexChanged?.Invoke(this, EventArgs.Empty);
			}
		}

		// Recompute the per-segment box widths for the current DPI. Measured with the bold (active) font so the
		// box never shrinks when a segment is selected. Cheap and cached: only re-measures when the DPI changes.
		private void Measure()
		{
			if (metricsDpi == DeviceDpi)
				return;
			metricsDpi = DeviceDpi;
			int pad = LogicalToDeviceUnits(30);
			for (int index = 0; index < segments.Length; index++)
				widths[index] = TextRenderer.MeasureText(segments[index], StudioTheme.Strong).Width + pad;
		}

		protected override void OnMouseMove(MouseEventArgs e)
		{
			int index = IndexAt(e.X);
			if (index != hoveredIndex)
			{
				hoveredIndex = index;
				Invalidate();
			}
			base.OnMouseMove(e);
		}

		protected override void OnMouseLeave(EventArgs e)
		{
			hoveredIndex = -1;
			Invalidate();
			base.OnMouseLeave(e);
		}

		protected override void OnMouseDown(MouseEventArgs e)
		{
			if (Enabled)
			{
				int index = IndexAt(e.X);
				if (index >= 0)
					SelectedIndex = index;
			}
			base.OnMouseDown(e);
		}

		protected override void OnEnabledChanged(EventArgs e)
		{
			Cursor = Enabled ? Cursors.Hand : Cursors.Default;
			Invalidate();
			base.OnEnabledChanged(e);
		}

		private int IndexAt(int x)
		{
			Measure();
			int offset = LogicalToDeviceUnits(4);
			for (int index = 0; index < segments.Length; index++)
			{
				if (x >= offset && x < offset + widths[index])
					return index;
				offset += widths[index];
			}
			return -1;
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			Measure();
			e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
			var outer = new Rectangle(0, 0, Width - 1, Height - 1);
			using (var path = StudioTheme.RoundedRectangle(outer, LogicalToDeviceUnits(9)))
			using (var brush = new SolidBrush(StudioTheme.Field))
				e.Graphics.FillPath(brush, path);
			int start = LogicalToDeviceUnits(4);
			int inset = LogicalToDeviceUnits(3);
			int top = LogicalToDeviceUnits(1);
			int offset = start;
			for (int index = 0; index < segments.Length; index++)
			{
				var bounds = new Rectangle(offset - inset, top, widths[index], Height - top * 2);
				bool active = index == selectedIndex;
				if (active)
					using (var path = StudioTheme.RoundedRectangle(bounds, LogicalToDeviceUnits(7)))
					using (var brush = new SolidBrush(Enabled ? StudioTheme.Accent : StudioTheme.Neutral))
						e.Graphics.FillPath(brush, path);
				else if (Enabled && index == hoveredIndex)
					using (var brush = new SolidBrush(StudioTheme.Shift(StudioTheme.Field, 10)))
						e.Graphics.FillRectangle(brush, bounds);
				if (index > 0)
					using (var pen = new Pen(StudioTheme.Line))
						e.Graphics.DrawLine(pen, bounds.X, top, bounds.X, Height - top * 2);
				Color ink = !Enabled ? StudioTheme.Faint : active ? Color.White : StudioTheme.Muted;
				TextRenderer.DrawText(e.Graphics, segments[index], active ? StudioTheme.Strong : StudioTheme.Body, bounds, ink,
					TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
				offset += widths[index];
			}
			using (var pen = new Pen(Focused ? StudioTheme.Accent : StudioTheme.Line, Focused ? 1.5f : 1f))
			using (var path = StudioTheme.RoundedRectangle(outer, LogicalToDeviceUnits(9)))
				e.Graphics.DrawPath(pen, path);
		}
	}
}
