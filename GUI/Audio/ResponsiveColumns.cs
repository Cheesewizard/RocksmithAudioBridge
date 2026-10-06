using System;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// Two sections shown side by side when there is room and stacked one above the other when there is not.
	/// Below a width threshold it switches to a single column, so on a narrow window (or at 150% DPI) each
	/// section still gets the full width its controls need instead of clipping. Children are Dock.Fill and may be AutoSize; the panel is
	/// AutoSize in height, so a stacked pair grows taller and the containing page scrolls if needed.
	/// </summary>
	internal sealed class ResponsiveColumns : TableLayoutPanel
	{
		private readonly Control first;
		private readonly Control second;
		private readonly int minColumnLogical;
		private int mode = -1;   // -1 = not laid out yet, 0 = side by side, 1 = stacked

		public ResponsiveColumns(Control first, Control second, int minColumnLogical = 340)
		{
			this.first = first;
			this.second = second;
			this.minColumnLogical = minColumnLogical;
			Dock = DockStyle.Top;
			AutoSize = true;
			AutoSizeMode = AutoSizeMode.GrowAndShrink;
			Margin = new Padding(0, 0, 0, 14);
			first.Dock = DockStyle.Fill;
			second.Dock = DockStyle.Fill;
			Controls.Add(first, 0, 0);
			Controls.Add(second, 1, 0);
		}

		protected override void OnClientSizeChanged(EventArgs e)
		{
			base.OnClientSizeChanged(e);
			Reflow();
		}

		protected override void OnHandleCreated(EventArgs e)
		{
			base.OnHandleCreated(e);
			Reflow();
		}

		// Only reconfigures the grid when the mode actually flips, so a resize within one mode does not thrash
		// the layout. Widths are taken in device pixels (LogicalToDeviceUnits) because Reflow runs after the
		// control knows its real DPI.
		private void Reflow()
		{
			int width = ClientSize.Width;
			if (width <= 0)
				return;
			int wanted = width < LogicalToDeviceUnits(minColumnLogical * 2 + 14) ? 1 : 0;
			if (wanted == mode)
				return;
			mode = wanted;
			int gap = LogicalToDeviceUnits(7);
			int stackGap = LogicalToDeviceUnits(14);
			SuspendLayout();
			ColumnStyles.Clear();
			RowStyles.Clear();
			if (mode == 0)
			{
				ColumnCount = 2;
				RowCount = 1;
				ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
				ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
				RowStyles.Add(new RowStyle(SizeType.AutoSize));
				SetCellPosition(first, new TableLayoutPanelCellPosition(0, 0));
				SetCellPosition(second, new TableLayoutPanelCellPosition(1, 0));
				first.Margin = new Padding(0, 0, gap, 0);
				second.Margin = new Padding(gap, 0, 0, 0);
			}
			else
			{
				ColumnCount = 1;
				RowCount = 2;
				ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
				RowStyles.Add(new RowStyle(SizeType.AutoSize));
				RowStyles.Add(new RowStyle(SizeType.AutoSize));
				SetCellPosition(first, new TableLayoutPanelCellPosition(0, 0));
				SetCellPosition(second, new TableLayoutPanelCellPosition(0, 1));
				first.Margin = new Padding(0, 0, 0, stackGap);
				second.Margin = new Padding(0, 0, 0, 0);
			}
			ResumeLayout(true);
		}
	}
}
