using System;
using System.Drawing;
using System.Windows.Forms;
using RSMods.Audio;

namespace RSMods.Controls
{
	internal sealed class ColourPickerDialog : Form
	{
		public Color SelectedColor => wheel.SelectedColor;

		private readonly ColourWheel wheel;
		private readonly Panel preview;
		private readonly Label hexLabel;

		public ColourPickerDialog(string name, Color initialColor)
		{
			if (string.IsNullOrWhiteSpace(name)) throw new ArgumentException("A colour name is required.", nameof(name));
			Text = name + " colour";
			Font = StudioTheme.Body;
			ForeColor = StudioTheme.Ink;
			ClientSize = new Size(296, 366);
			FormBorderStyle = FormBorderStyle.FixedDialog;
			StartPosition = FormStartPosition.CenterParent;
			MinimizeBox = false;
			MaximizeBox = false;
			ShowInTaskbar = false;
			BackColor = StudioTheme.Background;
			wheel = new ColourWheel(initialColor) { Location = new Point(32, 8) };
			wheel.ColorChanged += RefreshPreview;
			Controls.Add(wheel);
			Controls.Add(new Label { Text = "Brightness", Location = new Point(16, 248), AutoSize = true });
			var brightness = new TrackBar
			{
				Location = new Point(94, 239), Size = new Size(186, 36), Minimum = 0, Maximum = 255,
				BackColor = StudioTheme.Background, TickStyle = TickStyle.None, Value = Math.Max(initialColor.R, Math.Max(initialColor.G, initialColor.B)),
				AccessibleName = "Brightness"
			};
			brightness.ValueChanged += (sender, args) => wheel.SetBrightness(brightness.Value / 255.0);
			Controls.Add(brightness);
			preview = new Panel { Location = new Point(20, 286), Size = new Size(42, 22), BorderStyle = BorderStyle.FixedSingle };
			hexLabel = new Label { Location = new Point(74, 289), AutoSize = true };
			Controls.Add(preview);
			Controls.Add(hexLabel);
			var apply = new Button { Text = "Apply", DialogResult = DialogResult.OK, Location = new Point(112, 324), Size = new Size(78, 28) };
			var cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, Location = new Point(198, 324), Size = new Size(78, 28) };
			foreach (var button in new[] { apply, cancel })
			{
				button.FlatStyle = FlatStyle.Flat;
				button.FlatAppearance.BorderColor = StudioTheme.Line;
				button.BackColor = button == apply ? StudioTheme.Accent : StudioTheme.Field;
				button.ForeColor = StudioTheme.Ink;
			}
			Controls.Add(apply);
			Controls.Add(cancel);
			AcceptButton = apply;
			CancelButton = cancel;
			RefreshPreview(null, EventArgs.Empty);
		}

		private void RefreshPreview(object sender, EventArgs args)
		{
			preview.BackColor = SelectedColor;
			hexLabel.Text = "#" + (SelectedColor.ToArgb() & 0xFFFFFF).ToString("X6");
		}
	}
}
