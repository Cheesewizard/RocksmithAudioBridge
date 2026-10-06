using System;
using System.Drawing;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>A labelled bank of channel strips, so the console reads as master, players and game audio.</summary>
	internal sealed class MixerGroup : Panel
	{
		public MixerGroup(string title, params MixerStrip[] strips)
		{
			if (string.IsNullOrWhiteSpace(title))
				throw new ArgumentException("A group title is required.", nameof(title));
			if (strips == null || strips.Length == 0)
				throw new ArgumentException("A group needs at least one strip.", nameof(strips));
			BackColor = StudioTheme.Background;
			Margin = new Padding(0);
			Dock = DockStyle.Fill;

			var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = strips.Length, RowCount = 1, Margin = new Padding(0, 8, 0, 0), BackColor = StudioTheme.Background };
			int width = 0;
			foreach (var strip in strips)
			{
				int cell = strip.Width + strip.Margin.Horizontal;
				layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, cell));
				width += cell;
			}
			for (int index = 0; index < strips.Length; index++)
			{
				strips[index].Dock = DockStyle.Fill;
				layout.Controls.Add(strips[index], index, 0);
			}

			var heading = new Label
			{
				Text = title,
				Dock = DockStyle.Top,
				Height = 28,
				Font = StudioTheme.SectionFont,
				ForeColor = StudioTheme.Faint,
				TextAlign = ContentAlignment.MiddleLeft,
				Padding = new Padding(8, 0, 0, 0),
				UseMnemonic = false,
				BackColor = StudioTheme.Background
			};

			Controls.Add(layout);
			Controls.Add(heading);
			Width = width;
			MinimumSize = new Size(width, 0);
		}
	}

	/// <summary>Hairline that separates two banks of strips.</summary>
	internal sealed class MixerDivider : Control
	{
		public MixerDivider()
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
			BackColor = StudioTheme.Surface;
			Width = 21;
			Margin = new Padding(0);
			Dock = DockStyle.Fill;
			TabStop = false;
		}

		protected override void OnPaint(PaintEventArgs args)
		{
			int x = Width / 2;
			using (var pen = new Pen(StudioTheme.Line))
				args.Graphics.DrawLine(pen, x, 26, x, Height - 10);
		}
	}
}
