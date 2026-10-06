using System;
using System.Drawing;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// Left navigation column: one flat list of sections. The selected row is a raised block with an accent bar
	/// on its left edge; a hairline down the right edge separates the column from the page.
	/// </summary>
	internal sealed class StudioNav : Control
	{
		private const int RowHeight = 42;
		private readonly string[] items;
		private int selectedIndex;
		private int hoveredIndex = -1;

		public event EventHandler SelectedIndexChanged;

		public StudioNav(params string[] items)
		{
			if (items == null || items.Length == 0)
				throw new ArgumentException("The navigation needs at least one section.", nameof(items));
			this.items = items;
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
			Font = StudioTheme.Strong;
			Width = 196;
			Cursor = Cursors.Hand;
			TabStop = true;
			AccessibleRole = AccessibleRole.List;
		}

		public int SelectedIndex
		{
			get => selectedIndex;
			set
			{
				int clamped = Math.Max(0, Math.Min(items.Length - 1, value));
				if (clamped == selectedIndex) return;
				selectedIndex = clamped;
				Invalidate();
				SelectedIndexChanged?.Invoke(this, EventArgs.Empty);
			}
		}

		private int IndexAt(int y)
		{
			int index = y / RowHeight;
			return index >= 0 && index < items.Length ? index : -1;
		}

		protected override void OnMouseMove(MouseEventArgs e)
		{
			int index = IndexAt(e.Y);
			if (index != hoveredIndex) { hoveredIndex = index; Invalidate(); }
			base.OnMouseMove(e);
		}

		protected override void OnMouseLeave(EventArgs e) { hoveredIndex = -1; Invalidate(); base.OnMouseLeave(e); }

		protected override void OnMouseDown(MouseEventArgs e)
		{
			Focus();
			if (Enabled && e.Button == MouseButtons.Left)
			{
				int index = IndexAt(e.Y);
				if (index >= 0) SelectedIndex = index;
			}
			base.OnMouseDown(e);
		}

		protected override bool IsInputKey(Keys keyData)
		{
			return keyData == Keys.Up || keyData == Keys.Down || keyData == Keys.Home || keyData == Keys.End || base.IsInputKey(keyData);
		}

		protected override void OnKeyDown(KeyEventArgs e)
		{
			if (Enabled && e.KeyCode == Keys.Up) { SelectedIndex = selectedIndex - 1; e.Handled = true; }
			else if (Enabled && e.KeyCode == Keys.Down) { SelectedIndex = selectedIndex + 1; e.Handled = true; }
			else if (Enabled && e.KeyCode == Keys.Home) { SelectedIndex = 0; e.Handled = true; }
			else if (Enabled && e.KeyCode == Keys.End) { SelectedIndex = items.Length - 1; e.Handled = true; }
			base.OnKeyDown(e);
		}

		protected override void OnEnabledChanged(EventArgs e) { Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); base.OnEnabledChanged(e); }

		protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }

		protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

		protected override void OnPaint(PaintEventArgs e)
		{
			var canvas = e.Graphics;
			canvas.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
			for (int index = 0; index < items.Length; index++)
			{
				var row = new Rectangle(8, index * RowHeight + 2, Width - 17, RowHeight - 4);
				bool selected = index == selectedIndex;
				if (selected)
				{
					using (var path = StudioTheme.RoundedRectangle(row, 9))
					using (var brush = new SolidBrush(Enabled ? StudioTheme.Blend(StudioTheme.Accent, StudioTheme.Surface, 0.72) : StudioTheme.Elevated))
						canvas.FillPath(brush, path);
					using (var brush = new SolidBrush(Enabled ? StudioTheme.Accent : StudioTheme.Faint))
						canvas.FillEllipse(brush, row.X + 12, row.Y + 15, 7, 7);
				}
				else if (index == hoveredIndex && Enabled)
				{
					using (var path = StudioTheme.RoundedRectangle(row, 9))
					using (var brush = new SolidBrush(StudioTheme.Shift(StudioTheme.Background, 8)))
						canvas.FillPath(brush, path);
				}
				Color ink = !Enabled ? StudioTheme.Faint : selected || index == hoveredIndex ? StudioTheme.Ink : StudioTheme.Muted;
				TextRenderer.DrawText(canvas, items[index], selected ? StudioTheme.Strong : StudioTheme.Body,
					new Rectangle(row.X + 30, row.Y, row.Width - 38, row.Height), ink,
					TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.EndEllipsis);
			}
			using (var pen = new Pen(StudioTheme.Line))
				canvas.DrawLine(pen, Width - 1, 0, Width - 1, Height);
			if (Focused && Enabled)
				using (var pen = new Pen(StudioTheme.Blend(StudioTheme.Line, StudioTheme.Accent, 0.6)))
					canvas.DrawRectangle(pen, 7, selectedIndex * RowHeight + 1, Width - 16, RowHeight);
		}
	}
}
