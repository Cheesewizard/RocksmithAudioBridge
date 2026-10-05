using System.Collections.Generic;
using System.Drawing;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// A framed panel: 1 px border, a solid header strip carrying the title, content below. Every section of
	/// the bridge sits in one of these so the tabs share one framing language.
	/// </summary>
	internal sealed class StudioCard : Panel
	{
		public const int HeaderHeight = 38;
		private readonly TableLayoutPanel body;
		private readonly List<Label> wrapLabels = new List<Label>();
		private readonly string title;

		public StudioCard(string title = null, bool stretch = false)
		{
			this.title = title;
			BackColor = StudioTheme.Surface;
			int top = string.IsNullOrEmpty(title) ? 16 : HeaderHeight + 14;
			Padding = new Padding(18, top, 18, 16);
			Margin = new Padding(0, 0, 0, 16);
			AutoSize = !stretch;
			AutoSizeMode = AutoSizeMode.GrowAndShrink;
			SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
			body = new TableLayoutPanel
			{
				Dock = DockStyle.Fill,
				ColumnCount = 1,
				AutoSize = !stretch,
				AutoSizeMode = AutoSizeMode.GrowAndShrink
			};
			body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			Controls.Add(body);
		}

		public void Add(Control control, bool fill = false)
		{
			body.RowStyles.Add(fill ? new RowStyle(SizeType.Percent, 100) : new RowStyle(SizeType.AutoSize));
			if (fill)
				control.Dock = DockStyle.Fill;
			else if (control is Label label && label.AutoSize)
			{
				// An AutoSize label lays its text on a single line and gets clipped by the card on a
				// narrow column. To wrap it we have to give it a definite maximum width; that width is
				// the card's, so it is set (and kept in sync) from the card's own size in ApplyWrapWidth.
				label.Anchor = AnchorStyles.Left | AnchorStyles.Right;
				wrapLabels.Add(label);
				ApplyWrapWidth();
			}
			body.Controls.Add(control, 0, body.RowCount++);
		}

		// Cap each wrapping label at the card's inner width so its text wraps to the card and the label
		// grows in height, instead of running off the edge. Re-run whenever the card is resized.
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

		protected override void OnClientSizeChanged(System.EventArgs e)
		{
			base.OnClientSizeChanged(e);
			ApplyWrapWidth();
		}

		protected override void OnPaintBackground(PaintEventArgs e)
		{
			using (var brush = new SolidBrush(Parent?.BackColor ?? StudioTheme.Background))
				e.Graphics.FillRectangle(brush, ClientRectangle);
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			var canvas = e.Graphics;
			canvas.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
			using (var path = StudioTheme.RoundedRectangle(new Rectangle(0, 0, Width - 1, Height - 1), StudioTheme.CornerRadius))
			{
				using (var brush = new SolidBrush(StudioTheme.Surface))
					canvas.FillPath(brush, path);
				using (var pen = new Pen(StudioTheme.Line))
					canvas.DrawPath(pen, path);
			}
			if (!string.IsNullOrEmpty(title))
			{
				using (var pen = new Pen(StudioTheme.Line))
					canvas.DrawLine(pen, 18, HeaderHeight, Width - 19, HeaderHeight);
				TextRenderer.DrawText(canvas, title, StudioTheme.Strong,
					new Rectangle(18, 1, Width - 36, HeaderHeight - 1), StudioTheme.Ink,
					TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.EndEllipsis);
			}
			base.OnPaint(e);
		}
	}
}
