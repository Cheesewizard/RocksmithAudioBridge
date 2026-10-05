using System;
using System.Drawing;
using System.Globalization;
using System.Windows.Forms;
using RSMods.Audio;

namespace RSMods.Controls
{
	internal sealed class FeatureColourSettings : UserControl
	{
		private readonly CheckBox customColours;
		private readonly Panel colourRow;
		private readonly ComboBox element;
		private readonly ColourChip colourChip;
		private readonly Label stateLabel;
		private readonly string overrideIdentifier;
		private readonly string[] identifiers;
		private readonly string[] defaults;
		private readonly Func<string, string> readSetting;
		private readonly Action<string, string> saveSetting;
		private bool loading;

		public FeatureColourSettings(string overrideIdentifier, string[] identifiers, string[] names,
			string[] defaults, Func<string, string> readSetting, Action<string, string> saveSetting)
		{
			if (string.IsNullOrWhiteSpace(overrideIdentifier)) throw new ArgumentException("An override setting is required.", nameof(overrideIdentifier));
			if (identifiers == null) throw new ArgumentNullException(nameof(identifiers));
			if (names == null) throw new ArgumentNullException(nameof(names));
			if (defaults == null) throw new ArgumentNullException(nameof(defaults));
			if (identifiers.Length == 0 || identifiers.Length != names.Length || identifiers.Length != defaults.Length)
				throw new ArgumentException("Colour settings, labels and defaults must have the same nonzero length.");
			this.overrideIdentifier = overrideIdentifier;
			this.identifiers = (string[])identifiers.Clone();
			this.defaults = (string[])defaults.Clone();
			this.readSetting = readSetting ?? throw new ArgumentNullException(nameof(readSetting));
			this.saveSetting = saveSetting ?? throw new ArgumentNullException(nameof(saveSetting));
			Size = new Size(780, 38);
			Font = StudioTheme.Body;
			ForeColor = StudioTheme.Ink;
			BackColor = StudioTheme.Background;
			Margin = new Padding(0, 4, 0, 18);
			customColours = new CheckBox { Text = "Custom colours", Font = StudioTheme.Strong, AutoSize = true, Location = new Point(0, 3) };
			customColours.CheckedChanged += ChangeOverride;
			Controls.Add(customColours);
			stateLabel = new Label { AutoSize = true, ForeColor = StudioTheme.Muted, Font = StudioTheme.Small, Location = new Point(168, 7) };
			Controls.Add(stateLabel);
			colourRow = new Panel { Location = new Point(0, 38), Size = new Size(650, 88), BackColor = StudioTheme.Surface, Visible = false };
			colourRow.Controls.Add(new Label { Text = "DISPLAY ELEMENT", Font = StudioTheme.SectionFont, ForeColor = StudioTheme.Muted, AutoSize = true, Location = new Point(16, 12) });
			colourRow.Controls.Add(new Label { Text = "COLOUR", Font = StudioTheme.SectionFont, ForeColor = StudioTheme.Muted, AutoSize = true, Location = new Point(240, 12) });
			element = new StudioSelector { Location = new Point(16, 38), Size = new Size(204, 30), AccessibleName = "Display element" };
			element.Items.AddRange(names);
			element.SelectedIndexChanged += RefreshColour;
			colourChip = new ColourChip { Location = new Point(240, 32) };
			colourChip.Click += ChooseColour;
			var reset = new LinkLabel { Text = "Reset colours", Font = StudioTheme.Small, LinkColor = StudioTheme.Muted, ActiveLinkColor = StudioTheme.Accent,
				LinkBehavior = LinkBehavior.HoverUnderline, AutoSize = true, Location = new Point(462, 44) };
			reset.LinkClicked += ResetColours;
			colourRow.Controls.AddRange(new Control[] { element, colourChip, reset });
			Controls.Add(colourRow);
		}

		public void LoadSettings()
		{
			loading = true;
			try
			{
				customColours.Checked = readSetting(overrideIdentifier) == "on";
				if (element.SelectedIndex < 0) element.SelectedIndex = 0;
				UpdateExpansion();
				RefreshColour(null, EventArgs.Empty);
			}
			finally
			{
				loading = false;
			}
		}

		private void ChangeOverride(object sender, EventArgs args)
		{
			if (!loading) saveSetting(overrideIdentifier, customColours.Checked ? "on" : "off");
			UpdateExpansion();
		}

		private void UpdateExpansion()
		{
			colourRow.Visible = customColours.Checked;
			Height = customColours.Checked ? 130 : 38;
			stateLabel.Text = customColours.Checked ? "Your colours are active" : "Using default colours";
		}

		private void RefreshColour(object sender, EventArgs args)
		{
			if (element.SelectedIndex < 0) return;
			var value = readSetting(identifiers[element.SelectedIndex]);
			if (value.Length != 6 || !int.TryParse(value, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var rgb))
			{
				colourChip.Text = "Invalid colour";
				return;
			}
			colourChip.SelectedColor = Color.FromArgb(255, Color.FromArgb(rgb));
		}

		private void ChooseColour(object sender, EventArgs args)
		{
			using (var dialog = new ColourPickerDialog(element.Text, colourChip.SelectedColor))
			{
				if (dialog.ShowDialog(this) != DialogResult.OK) return;
				saveSetting(identifiers[element.SelectedIndex], (dialog.SelectedColor.ToArgb() & 0xFFFFFF).ToString("X6"));
				RefreshColour(null, EventArgs.Empty);
			}
		}

		private void ResetColours(object sender, LinkLabelLinkClickedEventArgs args)
		{
			for (var index = 0; index < identifiers.Length; index++) saveSetting(identifiers[index], defaults[index]);
			RefreshColour(null, EventArgs.Empty);
		}
	}
}
