using System;
using System.Drawing;
using System.Reflection;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal static class StudioTheme
	{
		public static readonly Color Background = Color.FromArgb(12, 18, 28);
		public static readonly Color Surface = Color.FromArgb(20, 29, 42);
		public static readonly Color Elevated = Color.FromArgb(29, 40, 56);
		public static readonly Color Well = Color.FromArgb(8, 13, 21);
		public static readonly Color Field = Color.FromArgb(31, 45, 63);
		public static readonly Color Line = Color.FromArgb(57, 73, 94);
		public static readonly Color Ink = Color.FromArgb(233, 237, 243);
		public static readonly Color Muted = Color.FromArgb(174, 187, 203);
		public static readonly Color Faint = Color.FromArgb(126, 143, 163);
		public static readonly Color Record = Color.FromArgb(233, 76, 82);
		public static readonly Color Positive = Color.FromArgb(60, 205, 140);
		public static readonly Color Warning = Color.FromArgb(240, 172, 52);
		public static readonly Color Accent = Color.FromArgb(88, 168, 255);
		public static readonly Color Violet = Color.FromArgb(150, 132, 250);
		public static readonly Color Teal = Color.FromArgb(70, 190, 205);
		public static readonly Color Neutral = Color.FromArgb(48, 64, 84);
		/// <summary>Header strip on framed panels (cards, list headers).</summary>
		public static readonly Color Header = Color.FromArgb(24, 35, 50);

		/// <summary>
		/// Corner radius every studio shape is clamped to. 0 = square-cut frames everywhere: one switch, so
		/// the whole window shares one edge language instead of each control choosing its own rounding.
		/// </summary>
		public const int CornerRadius = 10;

		private static readonly string UiFont = ResolveUiFont();
		public static readonly Font PageTitle = new Font(UiFont, 20F, FontStyle.Bold);
		public static readonly Font Display = new Font(UiFont, 20F, FontStyle.Regular);
		public static readonly Font Title = new Font(UiFont, 14F, FontStyle.Bold);
		public static readonly Font Timecode = new Font(UiFont, 28F, FontStyle.Regular);
		public static readonly Font Mono = new Font(UiFont, 10.5F, FontStyle.Regular);
		public static readonly Font MonoSmall = new Font(UiFont, 9F, FontStyle.Regular);
		public static readonly Font Body = new Font(UiFont, 9.75F, FontStyle.Regular);
		public static readonly Font Strong = new Font(UiFont, 9.75F, FontStyle.Bold);
		public static readonly Font Small = new Font(UiFont, 8.75F, FontStyle.Regular);
		public static readonly Font Readout = new Font(UiFont, 11F, FontStyle.Bold);
		public static readonly Font SectionFont = new Font(UiFont, 9F, FontStyle.Bold);

		private static string ResolveUiFont()
		{
			using (var fonts = new System.Drawing.Text.InstalledFontCollection())
			{
				foreach (var family in fonts.Families)
					if (string.Equals(family.Name, "Segoe UI Variable", StringComparison.OrdinalIgnoreCase))
						return family.Name;
			}
			return "Segoe UI";
		}

		public static Label Text(string text)
		{
			return new Label { Text = text, AutoSize = true, UseMnemonic = false, Font = Body, ForeColor = Ink, Margin = new Padding(0, 4, 0, 4) };
		}

		/// <summary>Guidance under a control. Body size so it reads as copy rather than fine print; cards wrap it to their width.</summary>
		public static Label Hint(string text)
		{
			return new Label { Text = text, AutoSize = true, UseMnemonic = false, Font = Body, ForeColor = Muted, Margin = new Padding(0, 4, 0, 6) };
		}

		public static Label Section(string text)
		{
			return new Label { Text = text, AutoSize = true, UseMnemonic = false, Font = SectionFont, ForeColor = Muted, Margin = new Padding(0, 12, 0, 6) };
		}

		public static void StyleField(TextBox field)
		{
			field.ReadOnly = true;
			field.Cursor = Cursors.Arrow;
			field.BackColor = Field;
			field.ForeColor = Ink;
			field.Font = Body;
			field.BorderStyle = BorderStyle.FixedSingle;
			field.Margin = new Padding(0, 2, 0, 6);
		}

		/// <summary>Caption above a control inside a card row (the same small-caps voice as a section heading).</summary>
		public static Label Caption(string text)
		{
			return new Label { Text = text, AutoSize = true, UseMnemonic = false, Font = SectionFont, ForeColor = Faint, Margin = new Padding(0, 0, 0, 2) };
		}

		/// <summary>
		/// Dark tooltips. The default ToolTip paints the light system bubble, which is the one place the
		/// theme visibly broke on hover; owner-drawing it keeps the bubble in the studio colours.
		/// </summary>
		public static void StyleTips(ToolTip tips)
		{
			tips.OwnerDraw = true;
			tips.BackColor = Elevated;
			tips.ForeColor = Ink;
			tips.Draw += (sender, args) =>
			{
				args.DrawBackground();
				using (var pen = new Pen(Line))
					args.Graphics.DrawRectangle(pen, 0, 0, args.Bounds.Width - 1, args.Bounds.Height - 1);
				TextRenderer.DrawText(args.Graphics, args.ToolTipText, Body, args.Bounds, Ink,
					TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix);
			};
			tips.Popup += (sender, args) =>
			{
				var size = TextRenderer.MeasureText(tips.GetToolTip(args.AssociatedControl), Body, new Size(420, 0),
					TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix);
				args.ToolTipSize = new Size(size.Width + 16, size.Height + 10);
			};
		}

		public static void StyleSelector(ComboBox selector)
		{
			selector.BackColor = Field;
			selector.ForeColor = Ink;
			selector.Font = Body;
			selector.FlatStyle = FlatStyle.Flat;
			selector.DropDownStyle = ComboBoxStyle.DropDownList;
			selector.ItemHeight = 22;
			selector.DrawMode = DrawMode.OwnerDrawFixed;
			selector.Margin = new Padding(0, 2, 0, 6);
			selector.DrawItem += DrawSelectorItem;
		}

		/// <summary>
		/// Turns on double buffering for the plain containers in a tree. Panels and layout panels
		/// paint straight to the screen by default, so a window of custom-drawn controls tears and
		/// flickers no matter how well each control buffers itself.
		/// </summary>
		public static void EnableDoubleBuffering(Control root)
		{
			if (root == null)
				return;
			var buffered = typeof(Control).GetProperty("DoubleBuffered", BindingFlags.Instance | BindingFlags.NonPublic);
			if (buffered == null)
				return;
			ApplyDoubleBuffering(root, buffered);
		}

		private static void ApplyDoubleBuffering(Control control, PropertyInfo buffered)
		{
			if (control is Panel || control is TableLayoutPanel || control is FlowLayoutPanel || control is TabPage || control is UserControl || control is Form)
				buffered.SetValue(control, true, null);
			foreach (Control child in control.Controls)
				ApplyDoubleBuffering(child, buffered);
		}

		public static GraphicsPath RoundedRectangle(Rectangle bounds, int radius)
		{
			radius = Math.Min(radius, CornerRadius);
			int diameter = radius * 2;
			var path = new GraphicsPath();
			if (diameter <= 0 || bounds.Width <= diameter || bounds.Height <= diameter)
			{
				path.AddRectangle(bounds);
				return path;
			}
			path.AddArc(bounds.X, bounds.Y, diameter, diameter, 180, 90);
			path.AddArc(bounds.Right - diameter - 1, bounds.Y, diameter, diameter, 270, 90);
			path.AddArc(bounds.Right - diameter - 1, bounds.Bottom - diameter - 1, diameter, diameter, 0, 90);
			path.AddArc(bounds.X, bounds.Bottom - diameter - 1, diameter, diameter, 90, 90);
			path.CloseFigure();
			return path;
		}

		public static Color Shift(Color color, int amount)
		{
			return Color.FromArgb(color.A, Clamp(color.R + amount), Clamp(color.G + amount), Clamp(color.B + amount));
		}

		public static Color Blend(Color color, Color towards, double weight)
		{
			return Color.FromArgb(
				(int)(color.R + (towards.R - color.R) * weight),
				(int)(color.G + (towards.G - color.G) * weight),
				(int)(color.B + (towards.B - color.B) * weight));
		}

		private static int Clamp(int value)
		{
			return value < 0 ? 0 : value > 255 ? 255 : value;
		}

		private static void DrawSelectorItem(object sender, DrawItemEventArgs args)
		{
			var selector = (ComboBox)sender;
			bool highlighted = (args.State & DrawItemState.Selected) != 0 && (args.State & DrawItemState.ComboBoxEdit) == 0;
			using (var background = new SolidBrush(highlighted ? Blend(Field, Accent, 0.45) : Field))
				args.Graphics.FillRectangle(background, args.Bounds);
			if (args.Index >= 0)
			{
				var text = new Rectangle(args.Bounds.X + 6, args.Bounds.Y, args.Bounds.Width - 12, args.Bounds.Height);
				TextRenderer.DrawText(args.Graphics, selector.Items[args.Index].ToString(), selector.Font, text, Ink,
					TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
			}
		}
	}
}
