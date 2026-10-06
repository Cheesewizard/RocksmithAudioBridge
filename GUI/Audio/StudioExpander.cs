using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// A collapsible rounded section for the Setup accordion. The header strip is one clickable row: a
	/// chevron, the title, a one-line summary underneath and, on the right, either the current value of the
	/// setting or a small on/off pill. Clicking it shows or hides the body below. It shares the square frame
	/// and header colours of <see cref="StudioCard"/>, so the accordion reads as the same framing language as
	/// the cards on the other tabs, and it sizes itself from the fonts (not fixed pixels) so it scales with DPI.
	/// </summary>
	internal sealed class StudioExpander : Panel
	{
		private readonly TableLayoutPanel body;
		private readonly List<Label> wrapLabels = new List<Label>();
		private readonly string title;
		private readonly int headerHeight;
		private readonly int titleTop;
		private readonly int titleLineHeight;
		private readonly int summaryTop;
		private readonly int summaryLineHeight;
		private bool expanded;
		private bool hoverHeader;

		private string summary = "";
		private string valueText;               // right-side muted value (for a chosen setting)
		private string pillText;                // right-side filled pill (for an on/off feature)
		private Color pillColor = StudioTheme.Neutral;

		private const int Pad = 17;             // matches StudioCard's inner padding, so bodies line up
		private const int ChevronBox = 18;      // width reserved for the chevron at the left of the header

		public event EventHandler ExpandedChanged;

		public StudioExpander(string title)
		{
			this.title = title ?? "";
			BackColor = StudioTheme.Surface;
			Margin = new Padding(0, 0, 0, 12);
			AutoSize = true;
			AutoSizeMode = AutoSizeMode.GrowAndShrink;
			SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);

			// Header height is measured from the two fonts so it grows with the display scale instead of
			// clipping the summary line at 150%.
			titleLineHeight = TextRenderer.MeasureText("Ag", StudioTheme.Strong).Height;
			summaryLineHeight = TextRenderer.MeasureText("Ag", StudioTheme.Small).Height;
			titleTop = 12;
			summaryTop = titleTop + titleLineHeight + 2;
			headerHeight = summaryTop + summaryLineHeight + 12;

			body = new TableLayoutPanel
			{
				Dock = DockStyle.Fill,
				ColumnCount = 1,
				AutoSize = true,
				AutoSizeMode = AutoSizeMode.GrowAndShrink
			};
			body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			Controls.Add(body);
			ApplyExpandedState();
		}

		public bool Expanded
		{
			get => expanded;
			set
			{
				if (expanded == value) return;
				expanded = value;
				ApplyExpandedState();
				ExpandedChanged?.Invoke(this, EventArgs.Empty);
			}
		}

		/// <summary>The one-line teaser shown under the title while collapsed.</summary>
		public void SetSummary(string text) { summary = text ?? ""; Invalidate(); }

		/// <summary>Show the current value of a setting on the right of the header (e.g. the chosen device).</summary>
		public void SetValue(string text)
		{
			if (valueText == text && pillText == null) return;
			valueText = text;
			pillText = null;
			Invalidate();
		}

		/// <summary>Show an on/off pill on the right of the header (e.g. protection ON).</summary>
		public void SetPill(string text, Color color)
		{
			if (pillText == text && pillColor == color && valueText == null) return;
			pillText = text;
			pillColor = color;
			valueText = null;
			Invalidate();
		}

		public void Add(Control control, bool fill = false)
		{
			body.RowStyles.Add(fill ? new RowStyle(SizeType.Percent, 100) : new RowStyle(SizeType.AutoSize));
			if (fill)
				control.Dock = DockStyle.Fill;
			else if (control is Label label && label.AutoSize)
			{
				// Same trick as StudioCard: an AutoSize label needs a definite maximum width to wrap, and that
				// width is the expander's inner width, kept in sync from the control's own size.
				label.Anchor = AnchorStyles.Left | AnchorStyles.Right;
				wrapLabels.Add(label);
				ApplyWrapWidth();
			}
			body.Controls.Add(control, 0, body.RowCount++);
		}

		private void ApplyExpandedState()
		{
			body.Visible = expanded;
			// The body fills the padding box under the header. Collapsed, it is hidden and takes no space, so the
			// panel auto-sizes down to just the header strip.
			Padding = new Padding(Pad, headerHeight + (expanded ? 10 : 0), Pad, expanded ? 14 : 0);
			Invalidate();
		}

		private void ApplyWrapWidth()
		{
			int available = ClientSize.Width - Padding.Horizontal;
			if (available <= 1) return;
			foreach (var label in wrapLabels)
			{
				int width = available - label.Margin.Horizontal;
				if (width > 1) label.MaximumSize = new Size(width, 0);
			}
		}

		protected override void OnClientSizeChanged(EventArgs e)
		{
			base.OnClientSizeChanged(e);
			ApplyWrapWidth();
		}

		private bool InHeader(int y) => y <= headerHeight;

		protected override void OnMouseMove(MouseEventArgs e)
		{
			base.OnMouseMove(e);
			bool over = InHeader(e.Y);
			Cursor = over ? Cursors.Hand : Cursors.Default;
			if (over != hoverHeader) { hoverHeader = over; Invalidate(); }
		}

		protected override void OnMouseLeave(EventArgs e)
		{
			base.OnMouseLeave(e);
			if (hoverHeader) { hoverHeader = false; Invalidate(); }
		}

		protected override void OnMouseUp(MouseEventArgs e)
		{
			base.OnMouseUp(e);
			if (e.Button == MouseButtons.Left && InHeader(e.Y)) Expanded = !expanded;
		}

		protected override void OnPaintBackground(PaintEventArgs e)
		{
			using (var brush = new SolidBrush(Parent?.BackColor ?? StudioTheme.Background))
				e.Graphics.FillRectangle(brush, ClientRectangle);
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			base.OnPaint(e);
			var g = e.Graphics;

			g.SmoothingMode = SmoothingMode.AntiAlias;
			var outerBounds = new Rectangle(0, 0, Width - 1, Height - 1);
			using (var outerPath = StudioTheme.RoundedRectangle(outerBounds, LogicalToDeviceUnits(10)))
			using (var background = new SolidBrush(StudioTheme.Surface))
				g.FillPath(background, outerPath);

			// Header strip, lit a touch on hover so the whole row reads as one button.
			Color strip = hoverHeader ? StudioTheme.Shift(StudioTheme.Header, 8) : StudioTheme.Header;
			var headerBounds = new Rectangle(1, 1, Width - 2, headerHeight - 1);
			using (var headerPath = StudioTheme.RoundedRectangle(headerBounds, LogicalToDeviceUnits(9)))
			using (var brush = new SolidBrush(strip))
				g.FillPath(brush, headerPath);
			if (expanded)
				using (var brush = new SolidBrush(strip))
					g.FillRectangle(brush, 1, headerHeight / 2, Width - 2, headerHeight / 2);

			// Chevron: points right while collapsed, down while open.
			g.SmoothingMode = SmoothingMode.AntiAlias;
			float cx = Pad + ChevronBox / 2f - 4;
			float cy = headerHeight / 2f;
			using (var pen = new Pen(hoverHeader ? StudioTheme.Ink : StudioTheme.Muted, 1.8f) { StartCap = LineCap.Round, EndCap = LineCap.Round })
			{
				var chevron = expanded
					? new[] { new PointF(cx - 4, cy - 2), new PointF(cx, cy + 2), new PointF(cx + 4, cy - 2) }
					: new[] { new PointF(cx - 2, cy - 4), new PointF(cx + 2, cy), new PointF(cx - 2, cy + 4) };
				g.DrawLines(pen, chevron);
			}
			g.SmoothingMode = SmoothingMode.None;

			int textLeft = Pad + ChevronBox + 8;

			// Right side: an on/off pill or the setting's current value, vertically centred against the title line.
			int rightEdge = Width - Pad;
			int midTitle = titleTop + titleLineHeight / 2;
			if (!string.IsNullOrEmpty(pillText))
			{
				var size = TextRenderer.MeasureText(pillText, StudioTheme.Small);
				int pw = size.Width + 16;
				int ph = size.Height + 6;
				var pill = new Rectangle(rightEdge - pw, midTitle - ph / 2, pw, ph);
				using (var path = StudioTheme.RoundedRectangle(pill, LogicalToDeviceUnits(7)))
				using (var brush = new SolidBrush(StudioTheme.Blend(StudioTheme.Surface, pillColor, 0.28)))
					g.FillPath(brush, path);
				using (var path = StudioTheme.RoundedRectangle(pill, LogicalToDeviceUnits(7)))
				using (var pen = new Pen(StudioTheme.Blend(StudioTheme.Line, pillColor, 0.5)))
					g.DrawPath(pen, path);
				TextRenderer.DrawText(g, pillText, StudioTheme.Small, pill, pillColor,
					TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
				rightEdge = pill.X - 10;
			}
			else if (!string.IsNullOrEmpty(valueText))
			{
				var region = new Rectangle(textLeft, titleTop, rightEdge - textLeft, titleLineHeight);
				int vw = Math.Min(TextRenderer.MeasureText(valueText, StudioTheme.Body).Width, region.Width * 3 / 5);
				var box = new Rectangle(rightEdge - vw, titleTop, vw, titleLineHeight);
				TextRenderer.DrawText(g, valueText, StudioTheme.Body, box, StudioTheme.Muted,
					TextFormatFlags.Right | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
				rightEdge = box.X - 10;
			}

			// Title on the top line, summary underneath, both clipped short of the right-side value/pill.
			var titleRect = new Rectangle(textLeft, titleTop, Math.Max(10, rightEdge - textLeft), titleLineHeight);
			TextRenderer.DrawText(g, title, StudioTheme.Strong, titleRect, StudioTheme.Ink,
				TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
			if (!string.IsNullOrEmpty(summary))
			{
				var summaryRect = new Rectangle(textLeft, summaryTop, Math.Max(10, (Width - Pad) - textLeft), summaryLineHeight);
				TextRenderer.DrawText(g, summary, StudioTheme.Small, summaryRect, StudioTheme.Faint,
					TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
			}

			// Divider under the header only while open (so the collapsed row stays a single clean band), then the frame.
			if (expanded)
				using (var pen = new Pen(StudioTheme.Line))
					g.DrawLine(pen, 1, headerHeight, Width - 2, headerHeight);
			g.SmoothingMode = SmoothingMode.AntiAlias;
			using (var pen = new Pen(StudioTheme.Line))
			using (var outerPath = StudioTheme.RoundedRectangle(outerBounds, LogicalToDeviceUnits(10)))
				g.DrawPath(pen, outerPath);
		}
	}
}
