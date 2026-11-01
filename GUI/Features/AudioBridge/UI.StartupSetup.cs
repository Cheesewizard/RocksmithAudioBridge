using System;
using System.Drawing;
using System.Text;
using System.Windows.Forms;
using RSMods.Audio;

namespace RSMods
{
    public partial class MainForm
    {
        private ComboBox startupMode;
        private ComboBox startupProfile;
        private Label startupHint;
        private bool syncingStartup;

        private sealed class StartupProfileChoice
        {
            public bool Automatic;
            public string Name;
            public string Caption;
            public override string ToString() => Caption;
        }

        private void InitializeStartupSetup(FlowLayoutPanel page)
        {
            var card = new SetupCard("Game startup", page.LogicalToDeviceUnits(SetupCardWidth));
            startupMode = CreateStartupPicker(card, "Ubisoft at startup");
            startupMode.Items.AddRange(new object[] { "Normal sign-in", "Skip sign-in", "Start offline" });
            AddSetupRow(card, "Ubisoft at startup", "Choose how Rocksmith opens. Changes apply next launch.", startupMode);
            AddSetupDivider(card);
            startupProfile = CreateStartupPicker(card, "Startup profile");
            AddSetupRow(card, "Profile", "Choose in game, continue automatically, or pick a saved profile.", startupProfile);
            AddSetupDivider(card);
            startupHint = new Label {
                AutoSize = true, MaximumSize = new Size(card.InnerWidth, 0),
                Font = StudioTheme.Small, ForeColor = OverlayLook.Muted, Margin = Padding.Empty,
                UseMnemonic = false
            };
            card.Controls.Add(startupHint);
            page.Controls.Add(card);

            startupMode.SelectedIndexChanged += (sender, args) => SaveStartupMode();
            startupProfile.SelectedIndexChanged += (sender, args) => SaveStartupProfile();
            page.VisibleChanged += (sender, args) => { if (page.Visible) RefreshStartupSetup(); };
            Activated += (sender, args) => { if (page.Visible) RefreshStartupSetup(); };
            RefreshStartupSetup();
        }

        private static ComboBox CreateStartupPicker(Control scale, string accessibleName)
        {
            return new ComboBox {
                DropDownStyle = ComboBoxStyle.DropDownList, Font = StudioTheme.Body,
                Width = scale.LogicalToDeviceUnits(280), IntegralHeight = false,
                DropDownHeight = scale.LogicalToDeviceUnits(220),
                AccessibleName = accessibleName, Margin = Padding.Empty
            };
        }

        private static string ReadStartupValue(string section, string key, string fallback)
        {
            var value = new StringBuilder(1024);
            GetPrivateProfileStringW(section, key, fallback, value, value.Capacity, BridgeSettingsPath);
            return value.ToString();
        }

        private void RefreshStartupSetup()
        {
            if (startupMode == null) return;
            syncingStartup = true;
            try
            {
                bool offline = ReadStartupValue("Startup", "OfflineStartup", "off").Equals("on", StringComparison.OrdinalIgnoreCase);
                bool skip = ReadStartupValue("Startup", "SkipOptionalUbisoftLogin", "off").Equals("on", StringComparison.OrdinalIgnoreCase);
                startupMode.SelectedIndex = offline ? 2 : skip ? 1 : 0;

                bool automatic = ReadStartupValue("Toggle Switches", "ForceProfileLoad", "off").Equals("on", StringComparison.OrdinalIgnoreCase);
                string selectedName = ReadStartupValue("Toggle Switches", "ProfileToLoad", "");
                startupProfile.Items.Clear();
                startupProfile.Items.Add(new StartupProfileChoice { Caption = "Choose in game" });
                startupProfile.Items.Add(new StartupProfileChoice { Automatic = true, Name = "", Caption = "Continue with selected profile" });
                int selection = automatic ? 1 : 0;
                foreach (object item in listBox_AutoLoadProfiles.Items)
                {
                    string name = item.ToString();
                    startupProfile.Items.Add(new StartupProfileChoice { Automatic = true, Name = name, Caption = name });
                    if (automatic && name == selectedName) selection = startupProfile.Items.Count - 1;
                }
                if (automatic && selectedName.Length > 0 && selection == 1)
                {
                    startupProfile.Items.Add(new StartupProfileChoice { Automatic = true, Name = selectedName, Caption = selectedName + " (not found)" });
                    selection = startupProfile.Items.Count - 1;
                }
                startupProfile.SelectedIndex = selection;
                startupProfile.Enabled = !new AudioInputModeConfiguration(RSMods.Util.GenUtil.GetRSDirectory()).IsGameRunning();
                UpdateStartupHint();
            }
            finally { syncingStartup = false; }
        }

        private void UpdateStartupHint()
        {
            startupHint.Text = startupMode.SelectedIndex == 2
                ? "Offline skips Ubisoft sign-in and the automatic connection attempt. Local songs and profiles still load."
                : startupMode.SelectedIndex == 1
                    ? "Skips the optional Ubisoft sign-in screen. Automatic connection attempts are unchanged."
                    : "Uses Rocksmith's original Ubisoft sign-in and connection flow.";
            startupHint.Text += " Native startup options currently support the December 2024 Learn & Play build.";
            if (!startupProfile.Enabled)
                startupHint.Text += " Close Rocksmith before changing the automatic profile.";
        }

        private void SaveStartupMode()
        {
            if (syncingStartup || !AllowSaving || startupMode.SelectedIndex < 0) return;
            int mode = startupMode.SelectedIndex;
            bool saved = WritePrivateProfileStringW("Startup", "SkipOptionalUbisoftLogin", mode > 0 ? "on" : "off", BridgeSettingsPath);
            saved = WritePrivateProfileStringW("Startup", "OfflineStartup", mode == 2 ? "on" : "off", BridgeSettingsPath) && saved;
            RefreshStartupSetup();
            if (saved) SaveSettings_ShowLabel();
            else startupHint.Text = "Could not save startup settings. Check that RSMods.ini is writable and try again.";
        }

        private void SaveStartupProfile()
        {
            if (syncingStartup || !AllowSaving || new AudioInputModeConfiguration(RSMods.Util.GenUtil.GetRSDirectory()).IsGameRunning()) return;
            var choice = startupProfile.SelectedItem as StartupProfileChoice;
            if (choice == null) return;
            if (choice.Automatic && !string.IsNullOrEmpty(choice.Name) && !listBox_AutoLoadProfiles.Items.Contains(choice.Name))
            {
                startupHint.Text = "That saved profile was not found. Choose another profile or select Choose in game.";
                return;
            }
            // Reuse the existing controls and save handlers, keeping both views in
            // sync and preserving the inherited auto-load behavior.
            if (choice.Automatic)
            {
                if (string.IsNullOrEmpty(choice.Name)) listBox_AutoLoadProfiles.ClearSelected();
                else listBox_AutoLoadProfiles.SelectedItem = choice.Name;
            }
            checkBox_AutoLoadProfile.Checked = choice.Automatic;
            RefreshStartupSetup();
        }
    }
}
