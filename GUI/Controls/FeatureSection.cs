using System.Drawing;
using System.Windows.Forms;
using RSMods.Audio;

namespace RSMods.Controls
{
	internal sealed class FeatureSection : GroupBox
	{
		public FeatureSection()
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
		}

		protected override void OnPaint(PaintEventArgs args)
		{
			args.Graphics.Clear(StudioTheme.Surface);
			using (var border = new Pen(StudioTheme.Line))
			{
				args.Graphics.DrawRectangle(border, 0, 0, Width - 1, Height - 1);
			}
			TextRenderer.DrawText(args.Graphics, Text.ToUpperInvariant(), StudioTheme.SectionFont,
				new Rectangle(16, 6, Width - 32, 16), StudioTheme.Muted,
				TextFormatFlags.Left | TextFormatFlags.NoPrefix | TextFormatFlags.EndEllipsis);
		}
	}
}
