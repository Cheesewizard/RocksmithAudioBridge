using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Windows.Forms;
using RSMods.Audio;
using RSMods.Util;

namespace RSMods
{
	// The Audit tab: "Set up for me" for RS_ASIO.ini (RsAsioIniSetup), the list of problems AuditChecks finds with a
	// Fix button where one is safe, and Rocksmith.ini's settings beside the game's defaults. Built the first time the
	// tab is opened and rebuilt after every change, so it always shows the files as they are now and costs nothing at
	// startup. Feedback is inline (no message boxes), like the Audio Bridge tab.
	public partial class MainForm
	{
		private const int AuditCardWidth = 900;   // logical px
		private TabPage tabAudit;
		private FlowLayoutPanel auditPage;
		private List<string> auditSetupResult = new List<string>();

		private void InitializeAuditTab()
		{
			tabAudit = new TabPage("Audit") { Name = "tab_Audit", BackColor = OverlayLook.Window, ForeColor = OverlayLook.Text };
			int after = TabController.TabPages.IndexOf(tab_RSASIO);
			if (after < 0) after = TabController.TabPages.IndexOf(tab_RSModsPlus);
			TabController.TabPages.Insert(after < 0 ? TabController.TabPages.Count : after + 1, tabAudit);
			tabAudit.Enter += (sender, args) => RefreshAudit();
		}

		private void RefreshAudit()
		{
			string gameDirectory = GenUtil.GetRSDirectory();
			tabAudit.SuspendLayout();
			if (auditPage == null)
			{
				auditPage = new FlowLayoutPanel
				{
					Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true,
					BackColor = OverlayLook.Window, ForeColor = OverlayLook.Text, Font = StudioTheme.Body
				};
				auditPage.Padding = new Padding(auditPage.LogicalToDeviceUnits(28), auditPage.LogicalToDeviceUnits(18), auditPage.LogicalToDeviceUnits(12), auditPage.LogicalToDeviceUnits(18));
				tabAudit.Controls.Add(auditPage);
			}
			auditPage.SuspendLayout();
			while (auditPage.Controls.Count > 0) auditPage.Controls[0].Dispose();
			int width = auditPage.LogicalToDeviceUnits(AuditCardWidth);

			var header = new FlowLayoutPanel { FlowDirection = FlowDirection.LeftToRight, AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, auditPage.LogicalToDeviceUnits(12)) };
			header.Controls.Add(new Label { Text = "Checks your RS_ASIO and Rocksmith settings for the mistakes that most often cost the guitar or the sound.", Font = StudioTheme.Body, ForeColor = OverlayLook.Muted, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, auditPage.LogicalToDeviceUnits(8), auditPage.LogicalToDeviceUnits(16), 0) });
			var again = CreateSetupButton("Check again", auditPage);
			again.Click += (sender, args) => { auditSetupResult.Clear(); RefreshAudit(); };
			header.Controls.Add(again);
			auditPage.Controls.Add(header);

			if (string.IsNullOrEmpty(gameDirectory) || !Directory.Exists(gameDirectory))
				auditPage.Controls.Add(new Label { Text = "The Rocksmith folder was not found.", ForeColor = OverlayLook.Warn, AutoSize = true });
			else
			{
				bool asio = false;
				try { asio = new AudioInputModeConfiguration(gameDirectory).ReadMode() == AudioInputMode.Asio; } catch { }
				auditPage.Controls.Add(AuditSetupCard(gameDirectory, width));
				auditPage.Controls.Add(AuditFindingsCard(gameDirectory, width));
				auditPage.Controls.Add(AuditRocksmithIniCard(gameDirectory, asio, width));
			}
			auditPage.ResumeLayout(true);
			tabAudit.ResumeLayout(true);
		}

		// ---- RS_ASIO setup ---------------------------------------------------------------------------------------
		private SetupCard AuditSetupCard(string gameDirectory, int width)
		{
			var card = new SetupCard("RS_ASIO setup", width);
			RsAsioIniSetup.State state = RsAsioIniSetup.GetState(gameDirectory);
			string status;
			Color colour = OverlayLook.Muted;
			switch (state)
			{
				case RsAsioIniSetup.State.NoRsAsio: status = "RS_ASIO is not installed, so there is nothing to set up. The guitar uses the Real Tone Cable."; break;
				case RsAsioIniSetup.State.Managed: status = "Set up for the Rocksmith Audio Bridge."; colour = OverlayLook.Good; break;
				case RsAsioIniSetup.State.EditedByHand: status = "Set up earlier, then edited. It is left as it is."; break;
				default:
					status = AsioProxySetup.IsLinked(gameDirectory)
						? "Already uses the Rocksmith Audio Bridge (set up by hand). Set up for me checks the rest and keeps a backup you can restore."
						: "Not set up yet. Set up for me points RS_ASIO.ini at the Rocksmith Audio Bridge and keeps your device and channel picks.";
					break;
			}
			var buttons = new FlowLayoutPanel { FlowDirection = FlowDirection.LeftToRight, AutoSize = true, WrapContents = false, Margin = new Padding(0) };
			if (state != RsAsioIniSetup.State.NoRsAsio)
			{
				var setUp = CreateSetupButton("Set up for me", card, StudioButtonKind.Primary);
				setUp.Margin = new Padding(0, 0, card.LogicalToDeviceUnits(10), 0);
				setUp.Click += (sender, args) => RunAuditSetUp(gameDirectory);
				buttons.Controls.Add(setUp);
			}
			if (RsAsioIniSetup.CanRestore)
			{
				var restore = CreateSetupButton("Restore my original RS_ASIO.ini", card);
				restore.Click += (sender, args) =>
				{
					try { RsAsioIniSetup.Restore(gameDirectory); auditSetupResult = new List<string> { "Your original RS_ASIO.ini is back. It applies when Rocksmith starts." }; }
					catch (Exception error) { auditSetupResult = new List<string> { "Could not restore: " + error.Message }; }
					ReloadRsAsioTab();
					RefreshAudit();
				};
				buttons.Controls.Add(restore);
			}
			AddSetupRow(card, "RS_ASIO.ini", status, buttons);
			((Label)((FlowLayoutPanel)((TableLayoutPanel)card.Controls[card.Controls.Count - 1]).GetControlFromPosition(0, 0)).Controls[1]).ForeColor = colour;
			foreach (string line in auditSetupResult)
				card.Controls.Add(new Label { Text = line, Font = StudioTheme.Small, ForeColor = OverlayLook.Text, AutoSize = true, MaximumSize = new Size(card.InnerWidth, 0), Margin = new Padding(0, card.LogicalToDeviceUnits(4), 0, 0) });
			if (RsAsioIniSetup.CanRestore)
				card.Controls.Add(new Label { Text = "Backups: " + RsAsioIniSetup.BackupFolder, Font = StudioTheme.Small, ForeColor = OverlayLook.Faint, AutoSize = true, MaximumSize = new Size(card.InnerWidth, 0), Margin = new Padding(0, card.LogicalToDeviceUnits(8), 0, 0) });
			return card;
		}

		private void RunAuditSetUp(string gameDirectory)
		{
			try { auditSetupResult = RsAsioIniSetup.SetUp(gameDirectory); }
			catch (Exception error) { auditSetupResult = new List<string> { "Set up failed: " + error.Message }; }
			ReloadRsAsioTab();
			RefreshAudit();
		}

		// The RS_ASIO Settings and Rocksmith Settings tabs show the files as read at startup; after the Audit changes
		// one, read them again. Saving is held off meanwhile: upstream's loaders set the controls, and their change
		// handlers would write the files straight back (which also makes a fresh setup read as "edited by hand").
		private void ReloadSettingsTabs()
		{
			bool saving = AllowSaving;
			AllowSaving = false;
			try { PriorSettings_LoadASIOSettings(); PriorSettings_LoadRocksmithSettings(); }
			catch { }
			finally { AllowSaving = saving; }
		}

		private void ReloadRsAsioTab() => ReloadSettingsTabs();

		// ---- findings --------------------------------------------------------------------------------------------
		private SetupCard AuditFindingsCard(string gameDirectory, int width)
		{
			var card = new SetupCard("Findings", width);
			List<AuditFinding> findings;
			try { findings = AuditChecks.Run(gameDirectory, () => RunAuditSetUp(gameDirectory)); }
			catch (Exception error) { findings = new List<AuditFinding> { new AuditFinding { Severity = AuditSeverity.Warning, Title = "The check could not finish", Detail = error.Message } }; }
			findings.Sort((a, b) => a.Severity.CompareTo(b.Severity));
			bool any = false;
			foreach (AuditFinding finding in findings)
			{
				if (any) AddSetupDivider(card);
				any = true;
				Control fix = null;
				if (finding.Fix != null)
				{
					var button = CreateSetupButton(finding.FixLabel, card);
					AuditFinding captured = finding;
					button.Click += (sender, args) =>
					{
						try { captured.Fix(); }
						catch (Exception error) { auditSetupResult = new List<string> { "The fix failed: " + error.Message }; }
						ReloadSettingsTabs();
						RefreshAudit();
					};
					fix = button;
				}
				var row = AddSetupRow(card, finding.Title, finding.Detail, fix ?? new Label { AutoSize = true, Text = "" });
				var title = (Label)((FlowLayoutPanel)((TableLayoutPanel)row).GetControlFromPosition(0, 0)).Controls[0];
				title.ForeColor = finding.Severity == AuditSeverity.Problem ? Color.FromArgb(248, 113, 113) : finding.Severity == AuditSeverity.Warning ? OverlayLook.Warn : OverlayLook.Text;
				title.Text = (finding.Severity == AuditSeverity.Problem ? "● " : finding.Severity == AuditSeverity.Warning ? "▲ " : "○ ") + title.Text;
			}
			if (!any)
				card.Controls.Add(new Label { Text = "No problems found.", Font = StudioTheme.Strong, ForeColor = OverlayLook.Good, AutoSize = true });
			return card;
		}

		// ---- Rocksmith.ini table ---------------------------------------------------------------------------------
		private SetupCard AuditRocksmithIniCard(string gameDirectory, bool asio, int width)
		{
			var card = new SetupCard("Rocksmith.ini", width);
			card.Controls.Add(new Label { Text = "Each setting beside Rocksmith's default. A value that differs is in amber: not wrong, just changed. Changing one saves a backup first.", Font = StudioTheme.Small, ForeColor = OverlayLook.Muted, AutoSize = true, MaximumSize = new Size(card.InnerWidth, 0), Margin = new Padding(0, 0, 0, card.LogicalToDeviceUnits(10)) });
			var grid = new TableLayoutPanel { AutoSize = true, ColumnCount = 5, Margin = new Padding(0) };
			foreach (int column in new[] { 260, 110, 130, 130, 150 })
				grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, card.LogicalToDeviceUnits(column)));
			void Cell(Control control, int column, int row) { control.Margin = new Padding(0, card.LogicalToDeviceUnits(3), 0, card.LogicalToDeviceUnits(3)); control.Anchor = AnchorStyles.Left; grid.Controls.Add(control, column, row); }
			Label Text(string text, Color colour, Font font = null) => new Label { Text = text, ForeColor = colour, Font = font ?? StudioTheme.Body, AutoSize = true };

			string[] headings = { "SETTING", "CURRENT", "ROCKSMITH DEFAULT", "RECOMMENDED", "" };
			for (int c = 0; c < headings.Length; c++) Cell(Text(headings[c], OverlayLook.Faint, StudioTheme.SectionFont), c, 0);
			int r = 1;

			var tips = new ToolTip();
			StudioTheme.StyleTips(tips);
			string lastSection = null;
			foreach (RocksmithIniSetting setting in AuditChecks.RocksmithIni(gameDirectory, asio))
			{
				if (setting.Section != lastSection)
				{
					lastSection = setting.Section;
					var section = Text("[" + setting.Section + "]", OverlayLook.Muted, StudioTheme.SectionFont);
					Cell(section, 0, r);
					grid.SetColumnSpan(section, 5);
					r++;
				}
				var name = Text(setting.Key, OverlayLook.Text);
				if (setting.Why != null) tips.SetToolTip(name, setting.Why);
				Cell(name, 0, r);
				bool off = setting.Recommended != null && setting.Current != null && !string.Equals(setting.Current, setting.Recommended, StringComparison.OrdinalIgnoreCase);
				Cell(Text(setting.Current ?? "(not set)", setting.DiffersFromDefault || off ? OverlayLook.Warn : OverlayLook.Text), 1, r);
				Cell(Text(setting.Default.Length == 0 ? "(empty)" : setting.Default, OverlayLook.Muted), 2, r);
				Cell(Text(setting.Recommended ?? "", OverlayLook.Accent), 3, r);
				// Only where there is a recommendation: a blind "use default" would undo the player's own choices
				// (screen size, quality), which are theirs to make on the Rocksmith Settings tab.
				string target = setting.Recommended;
				if (target != null && !string.Equals(setting.Current ?? "", target, StringComparison.OrdinalIgnoreCase))
				{
					var button = CreateSetupButton("Use recommended", card);
					button.Height = card.LogicalToDeviceUnits(28);
					RocksmithIniSetting captured = setting;
					button.Click += (sender, args) =>
					{
						try { AuditChecks.SetRocksmithValue(gameDirectory, captured.Section, captured.Key, target); }
						catch (Exception error) { auditSetupResult = new List<string> { "Could not change Rocksmith.ini: " + error.Message }; }
						ReloadSettingsTabs();
						RefreshAudit();
					};
					Cell(button, 4, r);
				}
				r++;
			}
			grid.RowCount = r;
			card.Controls.Add(grid);
			return card;
		}
	}
}
