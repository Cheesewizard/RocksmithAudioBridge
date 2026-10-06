using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// Drop-down list that paints its whole face itself. The system combo box, even flat-styled, draws a
	/// light inner edge and its own arrow that the dark theme cannot recolour, so after the base paint the
	/// entire client area is repainted: field, text, arrow and a 1 px frame.
	/// </summary>
	internal sealed class StudioSelector : ComboBox
	{
		private const int WM_PAINT = 0x000F;
		private const int WM_PRINT = 0x0317;
		private const int WM_PRINT_CLIENT = 0x0318;
		private const int ARROW_WIDTH = 28;

		public StudioSelector()
		{
			StudioTheme.StyleSelector(this);
		}

		protected override void WndProc(ref Message message)
		{
			base.WndProc(ref message);
			if ((message.Msg != WM_PAINT && message.Msg != WM_PRINT && message.Msg != WM_PRINT_CLIENT) || !IsHandleCreated)
				return;
			using (var canvas = message.Msg == WM_PAINT ? Graphics.FromHwnd(Handle) : Graphics.FromHdc(message.WParam))
			{
				canvas.SmoothingMode = SmoothingMode.AntiAlias;
				Color field = Enabled ? StudioTheme.Field : StudioTheme.Blend(StudioTheme.Field, StudioTheme.Surface, 0.5);
				using (var brush = new SolidBrush(Parent?.BackColor ?? StudioTheme.Surface))
					canvas.FillRectangle(brush, 0, 0, Width, Height);
				using (var path = StudioTheme.RoundedRectangle(new Rectangle(0, 0, Width - 1, Height - 1), LogicalToDeviceUnits(8)))
				using (var brush = new SolidBrush(field))
					canvas.FillPath(brush, path);
				string text = SelectedItem?.ToString() ?? Text ?? "";
				TextRenderer.DrawText(canvas, text, Font, new Rectangle(8, 0, Width - ARROW_WIDTH - 12, Height),
					Enabled ? StudioTheme.Ink : StudioTheme.Faint,
					TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
				canvas.SmoothingMode = SmoothingMode.AntiAlias;
				using (var pen = new Pen(Enabled ? StudioTheme.Muted : StudioTheme.Faint, 1.8f) { StartCap = LineCap.Round, EndCap = LineCap.Round })
				{
					float centre = Width - ARROW_WIDTH / 2f;
					float middle = Height / 2f;
					canvas.DrawLines(pen, new[]
					{
						new PointF(centre - 4, middle - 2), new PointF(centre, middle + 2), new PointF(centre + 4, middle - 2)
					});
				}
				canvas.SmoothingMode = SmoothingMode.None;
				using (var pen = new Pen(StudioTheme.Line))
					canvas.DrawLine(pen, Width - ARROW_WIDTH, 1, Width - ARROW_WIDTH, Height - 2);
				using (var pen = new Pen(Focused ? StudioTheme.Accent : StudioTheme.Line, Focused ? 1.5f : 1f))
				using (var path = StudioTheme.RoundedRectangle(new Rectangle(0, 0, Width - 1, Height - 1), LogicalToDeviceUnits(8)))
					canvas.DrawPath(pen, path);
			}
		}
	}
}
