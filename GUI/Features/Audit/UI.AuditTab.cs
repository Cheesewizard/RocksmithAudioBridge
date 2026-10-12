using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Windows.Forms;
using RSMods.Audio;
using RSMods.Util;

namespace RSMods
{
	// The Audit card on the RS_ASIO Settings tab, under upstream's Config card: three lines (Rocksmith settings, RS_ASIO
	// installed, RS_ASIO set up) with the reason in each line's tooltip, one Fix button for whatever is safe to fix
	// (Rocksmith.ini's audio values and/or "Set up for me" for RS_ASIO.ini), and Restore once Set up for me has kept a
	// backup. It sits where its fixes apply and uses only the gap under Config, so the tab keeps upstream's one-screen
	// layout. History: a separate Audit tab, then a Details panel with every finding and a Rocksmith.ini table, both
	// dropped 2026-10-12 as more than players need (see AuditChecks). Feedback is inline (no message boxes).
	public partial class MainForm
	{
		private RsAsioCard auditCard;
		private readonly List<Label> auditLines = new List<Label>();
		private Label auditResult;
		private StudioButton auditFixButton, auditRestoreButton;
		private readonly ToolTip auditTips = new ToolTip();

		private void InitializeAuditTab()
		{
			if (rsAsioConfigCard == null) return;
			tab_RSASIO.SuspendLayout();
			int pad = rsAsioConfigCard.LogicalToDeviceUnits(10);
			auditCard = new RsAsioCard { Left = rsAsioConfigCard.Left, Top = rsAsioConfigCard.Bottom + rsAsioConfigCard.LogicalToDeviceUnits(6), Width = rsAsioConfigCard.Width, Anchor = rsAsioConfigCard.Anchor };
			int inner = auditCard.Width - pad * 2;
			auditCard.Controls.Add(new Label { Text = "AUDIT", Font = StudioTheme.SectionFont, ForeColor = OverlayLook.Muted, BackColor = OverlayLook.Card, AutoSize = true, Location = new Point(pad, auditCard.LogicalToDeviceUnits(8)) });
			for (int i = 0; i < 3; i++)
			{
				var line = new Label { Font = StudioTheme.Small, BackColor = OverlayLook.Card, AutoSize = true, MaximumSize = new Size(inner, 0), Left = pad };
				auditLines.Add(line);
				auditCard.Controls.Add(line);
			}
			auditResult = new Label { Font = StudioTheme.Small, ForeColor = OverlayLook.Text, BackColor = OverlayLook.Card, AutoSize = true, MaximumSize = new Size(inner, 0), Left = pad };
			ShowAuditPart(auditResult, false);
			auditCard.Controls.Add(auditResult);
			auditFixButton = CreateSetupButton("Fix", auditCard, StudioButtonKind.Primary);
			auditRestoreButton = CreateSetupButton("Restore original", auditCard);
			foreach (var button in new[] { auditFixButton, auditRestoreButton })
			{
				button.Height = auditCard.LogicalToDeviceUnits(26);
				button.Width = inner;
				button.Left = pad;
				auditCard.Controls.Add(button);
			}
			auditFixButton.Click += (sender, args) => RunAuditFix();
			auditRestoreButton.Click += (sender, args) =>
			{
				string gameDirectory = GenUtil.GetRSDirectory();
				try { RsAsioIniSetup.Restore(gameDirectory); ShowAuditResult("Your original RS_ASIO.ini is back. It applies when Rocksmith starts."); }
				catch (Exception error) { ShowAuditResult("Could not restore: " + error.Message); }
				ReloadSettingsTabs();
				RefreshAuditSummary();
			};
			tab_RSASIO.Controls.Add(auditCard);
			StudioTheme.StyleTips(auditTips);
			RefreshAuditSummary();
			tab_RSASIO.ResumeLayout(true);
			tab_RSASIO.Enter += (sender, args) => { ShowAuditPart(auditResult, false); RefreshAuditSummary(); };
		}

		private bool auditNeedsRocksmithFix, auditNeedsSetUp;

		private void RefreshAuditSummary()
		{
			if (auditCard == null) return;
			string gameDirectory = GenUtil.GetRSDirectory();
			bool found = !string.IsNullOrEmpty(gameDirectory) && Directory.Exists(gameDirectory);
			bool asio = found && AuditChecks.AsioMode(gameDirectory);
			AuditCheck[] checks = found
				? new[] { AuditChecks.CheckRocksmithSettings(gameDirectory, asio), AuditChecks.CheckRsAsioInstalled(gameDirectory, asio), AuditChecks.CheckRsAsioSetUp(gameDirectory, asio) }
				: new[] { new AuditCheck { Name = "Rocksmith folder not found", State = AuditState.Problem, Detail = "Set the Rocksmith folder in GUI Settings." } };
			for (int i = 0; i < auditLines.Count; i++)
			{
				Label line = auditLines[i];
				ShowAuditPart(line, i < checks.Length);
				if (i >= checks.Length) continue;
				AuditCheck check = checks[i];
				switch (check.State)
				{
					case AuditState.Ok: line.Text = "✓ " + check.Name; line.ForeColor = OverlayLook.Good; break;
					case AuditState.Warning: line.Text = "▲ " + check.Name; line.ForeColor = OverlayLook.Warn; break;
					case AuditState.Problem: line.Text = "● " + check.Name; line.ForeColor = Color.FromArgb(248, 113, 113); break;
					default: line.Text = "– " + check.Name; line.ForeColor = OverlayLook.Faint; break;
				}
				auditTips.SetToolTip(line, check.Detail);
			}
			auditNeedsRocksmithFix = found && checks[0].State == AuditState.Problem;
			auditNeedsSetUp = found && checks.Length > 2 && (checks[2].State == AuditState.Problem || checks[2].State == AuditState.Warning);
			ShowAuditPart(auditFixButton, auditNeedsRocksmithFix || auditNeedsSetUp);
			auditFixButton.Text = auditNeedsSetUp && !auditNeedsRocksmithFix ? "Set up for me" : "Fix";
			auditFixButton.Width = auditRestoreButton.Width;   // setting Text re-measures the button
			auditTips.SetToolTip(auditFixButton, (auditNeedsRocksmithFix ? "Sets Rocksmith.ini's audio values. " : "") + (auditNeedsSetUp ? "Points RS_ASIO.ini at the Rocksmith Audio Bridge, keeping your device and channel picks. " : "") + "Backups are kept.");
			ShowAuditPart(auditRestoreButton, found && RsAsioIniSetup.CanRestore);
			auditTips.SetToolTip(auditRestoreButton, "Puts back the RS_ASIO.ini you had before Set up for me. Backups: " + RsAsioIniSetup.BackupFolder);
			LayoutAuditCard();
		}

		private void RunAuditFix()
		{
			string gameDirectory = GenUtil.GetRSDirectory();
			bool asio = AuditChecks.AsioMode(gameDirectory);
			var said = new List<string>();
			try
			{
				if (auditNeedsRocksmithFix) { AuditChecks.FixRocksmithSettings(gameDirectory, asio); said.Add("Rocksmith.ini updated."); }
				if (auditNeedsSetUp) said.AddRange(RsAsioIniSetup.SetUp(gameDirectory));
			}
			catch (Exception error) { said.Add("The fix failed: " + error.Message); }
			if (new AudioInputModeConfiguration(gameDirectory).IsGameRunning()) said.Add("Applies the next time Rocksmith starts.");
			ShowAuditResult(string.Join(" ", said));
			ReloadSettingsTabs();
			RefreshAuditSummary();
		}

		private void ShowAuditResult(string text)
		{
			auditResult.Text = text;
			ShowAuditPart(auditResult, text.Length > 0);
			LayoutAuditCard();
		}

		// Which parts of the card are meant to show. Control.Visible can't be read back for this: the card is laid out at
		// startup while the RS_ASIO tab is hidden, and Visible reads false for every child of a hidden control, which
		// shrank the card to its title (seen live 2026-10-12).
		private readonly HashSet<Control> auditShown = new HashSet<Control>();

		private void ShowAuditPart(Control control, bool show)
		{
			control.Visible = show;
			if (show) auditShown.Add(control); else auditShown.Remove(control);
		}

		// Stacks the card's contents and fits the card to them. Upstream's credits line sits under the card; if the
		// card needs more room than the designer left, the credits move down instead of being covered.
		private void LayoutAuditCard()
		{
			int y = auditCard.LogicalToDeviceUnits(8 + 15 + 4);
			foreach (Control control in new Control[] { auditLines[0], auditLines[1], auditLines[2], auditResult, auditFixButton, auditRestoreButton })
			{
				if (!auditShown.Contains(control)) continue;
				if (control is StudioButton) y += auditCard.LogicalToDeviceUnits(4);
				control.Top = y;
				y = control.Bottom + auditCard.LogicalToDeviceUnits(2);
			}
			auditCard.Height = y + auditCard.LogicalToDeviceUnits(4);
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
			try { PriorSettings_LoadASIOSettings(); PriorSettings_LoadRocksmithSettings(); }
			catch { }
			finally { AllowSaving = saving; }
		}
	}
}
