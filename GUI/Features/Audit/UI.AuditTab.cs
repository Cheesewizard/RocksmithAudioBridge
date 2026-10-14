using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Windows.Forms;
using RSMods.Audio;
using RSMods.Util;

namespace RSMods
{
	// The Audit tab: basic checks for the guitar input mode that is selected (Guitar input on the Rocksmith Audio
	// Bridge tab), because the Real Tone Cable and an ASIO interface fail in different ways. Both
	// modes: Rocksmith.ini's audio values with Use recommended. Cable: the cable seen by Windows and Windows' microphone
	// access. ASIO: RS_ASIO installed, RS_ASIO.ini set up (Set up for me / Restore), the bridge's interface and whether
	// it opened last launch. The RS_ASIO Settings tab keeps a small card (mode, worst result, Open audit) under
	// upstream's Config card, and the Rocksmith Settings tab an Open audit button, so the audit is one click from both
	// files it changes.
	// Rebuilt on every visit and after every change, so it
	// shows the files as they are now. Feedback is inline (no message boxes).
	public partial class MainForm
	{
		private const int AuditCardWidth = 900;   // logical px
		private TabPage tabAudit;
		private FlowLayoutPanel auditPage;
		private string auditResult = "";
		private readonly ToolTip auditTips = new ToolTip();

		// the small card on the RS_ASIO tab
		private RsAsioCard auditCard;
		private Label auditCardTitle, auditCardSummary;
		private StudioButton auditCardOpen;

		private void InitializeAuditTab()
		{
			tabAudit = new TabPage("Audit") { Name = "tab_Audit", BackColor = OverlayLook.Window, ForeColor = OverlayLook.Text };
			// TabPages.Insert does nothing until the TabControl has its window handle (a WinForms quirk), and this runs
			// at startup before it does: without this the tab was silently never added.
			_ = TabController.Handle;
			int after = TabController.TabPages.IndexOf(tab_RSModsPlus);
			TabController.TabPages.Insert(after < 0 ? TabController.TabPages.Count : after + 1, tabAudit);
			auditPage = new FlowLayoutPanel
			{
				Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true,
				BackColor = OverlayLook.Window, ForeColor = OverlayLook.Text, Font = StudioTheme.Body
			};
			auditPage.Padding = new Padding(auditPage.LogicalToDeviceUnits(28), auditPage.LogicalToDeviceUnits(18), auditPage.LogicalToDeviceUnits(12), 0);
			tabAudit.Controls.Add(auditPage);
			tabAudit.Enter += (sender, args) => RefreshAudit();
			StudioTheme.StyleTips(auditTips);

			InitializeRsAsioAuditCard();
			InitializeRocksmithAuditButton();
		}

		private void OpenAudit() => TabController.SelectedTab = tabAudit;

		// ---- the checks for the selected mode --------------------------------------------------------------------
		private static List<AuditCheck> AuditChecksForMode(string gameDirectory, AudioInputMode mode)
		{
			bool asio = mode == AudioInputMode.Asio;
			var checks = new List<AuditCheck> { AuditChecks.CheckRocksmithSettings(gameDirectory, asio) };
			if (asio)
			{
				checks.Add(AuditChecks.CheckRsAsioInstalled(gameDirectory));
				AuditCheck setUp = AuditChecks.CheckRsAsioSetUp(gameDirectory);
				if (setUp.State == AuditState.Problem || setUp.State == AuditState.Warning) setUp.FixLabel = "Set up for me";
				checks.Add(setUp);
				checks.Add(AuditChecks.CheckBridgeInterface());
			}
			else
			{
				checks.Add(AuditChecks.CheckCable());
				checks.Add(AuditChecks.CheckMicrophoneAccess());
			}
			return checks;
		}

		private static string ModeName(AudioInputMode mode) => mode == AudioInputMode.Asio ? "ASIO interface" : "Real Tone Cable";

		// ---- the tab ----------------------------------------------------------------------------------------------
		private void RefreshAudit()
		{
			string gameDirectory = GenUtil.GetRSDirectory();
			tabAudit.SuspendLayout();
			auditPage.SuspendLayout();
			while (auditPage.Controls.Count > 0) auditPage.Controls[0].Dispose();
			int width = auditPage.LogicalToDeviceUnits(AuditCardWidth);

			var header = new FlowLayoutPanel { FlowDirection = FlowDirection.LeftToRight, AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, auditPage.LogicalToDeviceUnits(12)) };
			header.Controls.Add(new Label { Text = "Checks the basics that most often cost the guitar or the sound, for the guitar input you use.", Font = StudioTheme.Body, ForeColor = OverlayLook.Muted, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, auditPage.LogicalToDeviceUnits(8), auditPage.LogicalToDeviceUnits(16), 0) });
			var again = CreateSetupButton("Check again", auditPage);
			again.Click += (sender, args) => { auditResult = ""; RefreshAudit(); };
			header.Controls.Add(again);
			auditPage.Controls.Add(header);

			if (string.IsNullOrEmpty(gameDirectory) || !Directory.Exists(gameDirectory))
				auditPage.Controls.Add(new Label { Text = "The Rocksmith folder was not found. Set it in GUI Settings.", ForeColor = OverlayLook.Warn, AutoSize = true });
			else
			{
				AudioInputMode mode = AuditChecks.Mode(gameDirectory);
				bool asio = mode == AudioInputMode.Asio;
				List<AuditCheck> checks = AuditChecksForMode(gameDirectory, mode);

				// Guitar input: which mode is being checked, and where to change it.
				var modeCard = new SetupCard("Guitar input", width);
				var change = CreateSetupButton("Change", modeCard);
				change.Click += (sender, args) => TabController.SelectedTab = tab_RSModsPlus;
				string modeNote = asio ? "The guitar comes in through RS_ASIO and the Rocksmith Audio Bridge driver."
					: mode == AudioInputMode.Cable ? "RS_ASIO is installed but off. Switch to ASIO interface on the Rocksmith Audio Bridge tab to use it."
					: "RS_ASIO is not installed; it is only needed for an audio interface.";
				var modeRow = AddSetupRow(modeCard, ModeName(mode), modeNote, change);
				((Label)((FlowLayoutPanel)((TableLayoutPanel)modeRow).GetControlFromPosition(0, 0)).Controls[0]).ForeColor = OverlayLook.Accent;
				auditPage.Controls.Add(modeCard);

				// Rocksmith settings: each value this mode needs, yours beside it, one button for all.
				auditPage.Controls.Add(AuditRocksmithCard(gameDirectory, asio, checks[0], width));

				// The mode's own checks.
				var modeChecks = new SetupCard(asio ? "ASIO interface" : "Real Tone Cable", width);
				for (int i = 1; i < checks.Count; i++)
				{
					if (i > 1) AddSetupDivider(modeChecks);
					AddAuditCheckRow(modeChecks, checks[i], gameDirectory);
				}
				if (asio && RsAsioIniSetup.CanRestore)
				{
					AddSetupDivider(modeChecks);
					var restore = CreateSetupButton("Restore", modeChecks);
					restore.Click += (sender, args) =>
					{
						try { RsAsioIniSetup.Restore(gameDirectory); auditResult = "Your original RS_ASIO.ini is back. It applies when Rocksmith starts."; }
						catch (Exception error) { auditResult = "Could not restore: " + error.Message; }
						ReloadSettingsTabs();
						RefreshAudit();
					};
					AddSetupRow(modeChecks, "Your original RS_ASIO.ini", "Puts back the file you had before Set up for me. Backups: " + RsAsioIniSetup.BackupFolder, restore);
				}
				auditPage.Controls.Add(modeChecks);
			}

			if (auditResult.Length > 0)
				auditPage.Controls.Add(new Label { Text = auditResult, Font = StudioTheme.Body, ForeColor = OverlayLook.Text, AutoSize = true, MaximumSize = new Size(width, 0), Margin = new Padding(0, auditPage.LogicalToDeviceUnits(4), 0, 0) });
			// A FlowLayoutPanel's scroll range ignores its bottom padding; an empty spacer is counted.
			auditPage.Controls.Add(new Panel { Size = new Size(1, auditPage.LogicalToDeviceUnits(24)), Margin = Padding.Empty });
			auditPage.ResumeLayout(true);
			tabAudit.ResumeLayout(true);
			RefreshAuditSummary();
		}

		private SetupCard AuditRocksmithCard(string gameDirectory, bool asio, AuditCheck check, int width)
		{
			var card = new SetupCard("Rocksmith settings", width);
			var ini = File.Exists(AuditChecks.RocksmithIniPath(gameDirectory)) ? IniText.Load(AuditChecks.RocksmithIniPath(gameDirectory)) : null;
			bool first = true;
			foreach (var (key, value) in AuditChecks.RocksmithWanted(asio))
			{
				if (!first) AddSetupDivider(card);
				first = false;
				string current = ini?.Get("Audio", key);
				bool off = !string.IsNullOrEmpty(current) && current != value;
				var values = new Label { Text = off ? current + "   →   " + value : "✓ " + value, Font = StudioTheme.Strong, ForeColor = off ? OverlayLook.Warn : OverlayLook.Good, AutoSize = true };
				AddSetupRow(card, key, off ? "Yours, then recommended for the " + ModeName(asio ? AudioInputMode.Asio : AudioInputMode.Cable) + "." : "Recommended for the " + ModeName(asio ? AudioInputMode.Asio : AudioInputMode.Cable) + ".", values);
			}
			card.Controls.Add(new Label { Text = check.Detail, Font = StudioTheme.Small, ForeColor = OverlayLook.Muted, AutoSize = true, MaximumSize = new Size(card.InnerWidth, 0), Margin = new Padding(0, card.LogicalToDeviceUnits(10), 0, 0) });
			if (check.State == AuditState.Problem)
			{
				var use = CreateSetupButton("Use recommended", card, StudioButtonKind.Primary);
				use.Margin = new Padding(0, card.LogicalToDeviceUnits(10), 0, 0);
				use.Click += (sender, args) =>
				{
					try { AuditChecks.FixRocksmithSettings(gameDirectory, asio); auditResult = "Rocksmith.ini updated (a backup is kept)." + AppliesNextStart(gameDirectory); }
					catch (Exception error) { auditResult = "Could not change Rocksmith.ini: " + error.Message; }
					ReloadSettingsTabs();
					RefreshAudit();
				};
				card.Controls.Add(use);
			}
			return card;
		}

		private void AddAuditCheckRow(SetupCard card, AuditCheck check, string gameDirectory)
		{
			Control fix = new Label { AutoSize = true, Text = "" };
			if (check.FixLabel != null)
			{
				var button = CreateSetupButton(check.FixLabel, card, check.Fix == null ? StudioButtonKind.Primary : StudioButtonKind.Ghost);
				button.Click += (sender, args) =>
				{
					try
					{
						if (check.Fix != null) check.Fix();
						else { auditResult = string.Join(" ", RsAsioIniSetup.SetUp(gameDirectory)) + AppliesNextStart(gameDirectory); ReloadSettingsTabs(); }
					}
					catch (Exception error) { auditResult = "That did not work: " + error.Message; }
					RefreshAudit();
				};
				fix = button;
			}
			var row = AddSetupRow(card, AuditMark(check.State) + check.Name, check.Detail, fix);
			((Label)((FlowLayoutPanel)((TableLayoutPanel)row).GetControlFromPosition(0, 0)).Controls[0]).ForeColor = AuditColour(check.State);
		}

		private static string AppliesNextStart(string gameDirectory) =>
			new AudioInputModeConfiguration(gameDirectory).IsGameRunning() ? " Applies the next time Rocksmith starts." : "";

		private static string AuditMark(AuditState state) =>
			state == AuditState.Ok ? "✓ " : state == AuditState.Warning ? "▲ " : state == AuditState.Problem ? "● " : "– ";

		private static Color AuditColour(AuditState state) =>
			state == AuditState.Ok ? OverlayLook.Good : state == AuditState.Warning ? OverlayLook.Warn : state == AuditState.Problem ? Color.FromArgb(248, 113, 113) : OverlayLook.Faint;

		// ---- the small card on the RS_ASIO tab, and the button on the Rocksmith Settings tab -------------------------
		private void InitializeRsAsioAuditCard()
		{
			if (rsAsioConfigCard == null) return;
			int pad = rsAsioConfigCard.LogicalToDeviceUnits(10);
			auditCard = new RsAsioCard { Left = rsAsioConfigCard.Left, Top = rsAsioConfigCard.Bottom + rsAsioConfigCard.LogicalToDeviceUnits(6), Width = rsAsioConfigCard.Width, Anchor = rsAsioConfigCard.Anchor };
			int inner = auditCard.Width - pad * 2;
			auditCardTitle = new Label { Text = "AUDIT", Font = StudioTheme.SectionFont, ForeColor = OverlayLook.Muted, BackColor = OverlayLook.Card, AutoSize = true, Location = new Point(pad, auditCard.LogicalToDeviceUnits(8)) };
			auditCardSummary = new Label { Font = StudioTheme.Small, BackColor = OverlayLook.Card, AutoSize = true, MaximumSize = new Size(inner, 0), Location = new Point(pad, auditCardTitle.Bottom + auditCard.LogicalToDeviceUnits(4)) };
			auditCardOpen = CreateSetupButton("Open audit", auditCard);
			auditCardOpen.Height = auditCard.LogicalToDeviceUnits(26);
			auditCardOpen.Width = inner;
			auditCardOpen.Left = pad;
			auditCardOpen.Click += (sender, args) => OpenAudit();
			auditCard.Controls.Add(auditCardTitle);
			auditCard.Controls.Add(auditCardSummary);
			auditCard.Controls.Add(auditCardOpen);
			tab_RSASIO.Controls.Add(auditCard);
			RefreshAuditSummary();
			tab_RSASIO.Enter += (sender, args) => RefreshAuditSummary();
		}

		// Upstream's light tab: a plain button beside the Audio settings group, in the empty space on its right.
		private void InitializeRocksmithAuditButton()
		{
			var button = new Button { Text = "Open audit", AutoSize = true, Anchor = AnchorStyles.Top | AnchorStyles.Left };
			button.Location = new Point(groupBox_Rocksmith_AudioSettings.Right + groupBox_Rocksmith_AudioSettings.LogicalToDeviceUnits(12), groupBox_Rocksmith_AudioSettings.Top + groupBox_Rocksmith_AudioSettings.LogicalToDeviceUnits(8));
			button.Click += (sender, args) => OpenAudit();
			auditTips.SetToolTip(button, "Checks Rocksmith's audio settings and your guitar input, with the recommended values.");
			tab_Rocksmith.Controls.Add(button);
		}

		// The RS_ASIO card's lines: the mode, and the worst result for it. Laid out by hand and sized to its contents;
		// Control.Visible is not read back (it is false while the tab is hidden at startup, which once shrank this card
		// to its title).
		private void RefreshAuditSummary()
		{
			if (auditCard == null) return;
			string gameDirectory = GenUtil.GetRSDirectory();
			if (string.IsNullOrEmpty(gameDirectory) || !Directory.Exists(gameDirectory))
			{
				auditCardTitle.Text = "AUDIT";
				auditCardSummary.Text = "Rocksmith folder not found";
				auditCardSummary.ForeColor = OverlayLook.Warn;
			}
			else
			{
				AudioInputMode mode = AuditChecks.Mode(gameDirectory);
				auditCardTitle.Text = mode == AudioInputMode.Asio ? "AUDIT · ASIO MODE" : "AUDIT · CABLE MODE";
				int problems = 0, warnings = 0;
				try
				{
					foreach (AuditCheck check in AuditChecksForMode(gameDirectory, mode))
					{
						if (check.State == AuditState.Problem) problems++;
						else if (check.State == AuditState.Warning) warnings++;
					}
				}
				catch { warnings++; }
				if (problems > 0) { auditCardSummary.Text = "● " + problems + (problems == 1 ? " problem" : " problems"); auditCardSummary.ForeColor = AuditColour(AuditState.Problem); }
				else if (warnings > 0) { auditCardSummary.Text = "▲ " + warnings + (warnings == 1 ? " warning" : " warnings"); auditCardSummary.ForeColor = OverlayLook.Warn; }
				else { auditCardSummary.Text = "✓ No problems found"; auditCardSummary.ForeColor = OverlayLook.Good; }
				auditTips.SetToolTip(auditCardSummary, mode == AudioInputMode.Cable ? "Checked for the Real Tone Cable. RS_ASIO is installed but off." : "Checked for the " + ModeName(mode) + ".");
			}
			auditCardOpen.Top = auditCardSummary.Bottom + auditCard.LogicalToDeviceUnits(6);
			auditCard.Height = auditCardOpen.Bottom + auditCard.LogicalToDeviceUnits(8);
			int gap = auditCard.LogicalToDeviceUnits(4);
			if (label_ASIO_Credits.Parent == tab_RSASIO && label_ASIO_Credits.Left < auditCard.Right && label_ASIO_Credits.Top < auditCard.Bottom + gap)
				label_ASIO_Credits.Top = auditCard.Bottom + gap;
		}

		// The RS_ASIO Settings and Rocksmith Settings tabs show the files as read at startup; after the audit changes
		// one, read them again. Saving is held off meanwhile: upstream's loaders set the controls, and their change
		// handlers would write the files straight back (which also makes a fresh setup read as "edited by hand").
		private void ReloadSettingsTabs()
		{
			bool saving = AllowSaving;
			AllowSaving = false;
			try { RefreshUpstreamIniCopies(); PriorSettings_LoadASIOSettings(); PriorSettings_LoadRocksmithSettings(); }
			catch { }
			finally { AllowSaving = saving; }
		}

		// Upstream's writers (ASIO\WriteSettings.cs, Rocksmith\WriteSettings.cs) keep every value of their file in a static
		// dictionary filled once at startup and rewrite the whole file from it on each edit. After the audit writes a file,
		// those dictionaries are read again from disk, or the next edit on the upstream tab would put the old values back.
		private static void RefreshUpstreamIniCopies()
		{
			ASIO.ReadSettings.DisabledOutput = ASIO.ReadSettings.DisabledInput0 = ASIO.ReadSettings.DisabledInput1 = ASIO.ReadSettings.DisabledInputMic = false;
			foreach (ASIO.ReadSettings.Sections section in Enum.GetValues(typeof(ASIO.ReadSettings.Sections)))
			{
				if (!ASIO.WriteSettings.newSettings.TryGetValue(ASIO.ReadSettings.SectionToName(section), out var values)) continue;
				foreach (string key in new List<string>(values.Keys))
					if (!key.StartsWith(";")) values[key] = ASIO.ReadSettings.ProcessSettings(key, section);
			}

			if (!Rocksmith.ReadSettings.DoesSettingsINIExist()) return;
			foreach (var values in Rocksmith.WriteSettings.settings.Values)
				foreach (string key in new List<string>(values.Keys))
				{
					// A key missing from the file keeps the value upstream would write for it.
					string value = Rocksmith.ReadSettings.ProcessSettings(key);
					if (value.Length > 0) values[key] = value;
				}
		}
	}
}
