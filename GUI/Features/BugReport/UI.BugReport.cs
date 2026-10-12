using System;
using System.Drawing;
using System.Windows.Forms;
using RSMods.Audio;
using RSMods.Util;

namespace RSMods
{
	// The "Report a problem" card on the Audio Bridge tab. The work is in BugReport.cs; this only checks the game is
	// closed, builds the zip and hands it to Explorer and the mail app.
	//
	// Feedback is a status line inside the card (amber = do something first, green = done), like the driver row,
	// not message boxes: grey Windows dialogs looked out of place on the overlay-styled page, and the whole report
	// stays one click with nothing to dismiss.
	public partial class MainForm
	{
		private Label bugReportStatus;
		private Label bugReportNote;

		private void InitializeBugReportSetup(FlowLayoutPanel page)
		{
			var card = new SetupCard("Report a problem", page.LogicalToDeviceUnits(SetupCardWidth));
			var button = CreateSetupButton("Make bug report", card);
			button.Click += MakeBugReport;
			AddSetupRow(card, "Bug report", "Zips your logs and audio settings and opens an email to the developer. Close Rocksmith first.", button);
			bugReportStatus = new Label { Text = "", Font = StudioTheme.Body, AutoSize = true, Visible = false, MaximumSize = new Size(card.InnerWidth, 0), Margin = new Padding(0, card.LogicalToDeviceUnits(10), 0, 0) };
			bugReportNote = new Label { Text = "", Font = StudioTheme.Small, ForeColor = OverlayLook.Muted, AutoSize = true, Visible = false, MaximumSize = new Size(card.InnerWidth, 0), Margin = new Padding(0, card.LogicalToDeviceUnits(2), 0, 0) };
			card.Controls.Add(bugReportStatus);
			card.Controls.Add(bugReportNote);
			page.Controls.Add(card);
		}

		private void ShowBugReportStatus(string text, Color colour, string note = "")
		{
			bugReportStatus.Text = text;
			bugReportStatus.ForeColor = colour;
			bugReportStatus.Visible = true;
			bugReportNote.Text = note;
			bugReportNote.Visible = note.Length > 0;
		}

		private void MakeBugReport(object sender, EventArgs args)
		{
			string gameDirectory = GenUtil.GetRSDirectory();
			if (string.IsNullOrEmpty(gameDirectory) || !System.IO.Directory.Exists(gameDirectory))
			{
				ShowBugReportStatus("The Rocksmith folder was not found.", OverlayLook.Warn);
				return;
			}
			// The mod's log is buffered while the game runs, so the newest lines may not be on disk yet.
			if (new AudioInputModeConfiguration(gameDirectory).IsGameRunning())
			{
				ShowBugReportStatus("Close Rocksmith first so the latest log is complete, then click again.", OverlayLook.Warn);
				return;
			}

			BugReport.Result report;
			try { report = BugReport.Build(gameDirectory); }
			catch (Exception error)
			{
				ShowBugReportStatus("The bug report could not be made: " + error.Message, OverlayLook.Warn);
				return;
			}

			// No log yet: the zip is nearly empty, so ask for a repro instead of opening an email nobody can use.
			if (report.LoggingWasOff)
			{
				ShowBugReportStatus("Logging was off, so there is little to send yet. It is on now: play until the problem happens, close the game and click again.", OverlayLook.Warn);
				return;
			}

			const string privacy = "Contains your logs and audio settings only. Your Windows user name is removed.";
			BugReport.ShowInExplorer(report.ZipPath);
			bool mailOpened = BugReport.OpenEmail(report);
			if (!mailOpened) Clipboard.SetText(BugReport.DevEmail);

			if (mailOpened)
				ShowBugReportStatus("Zip ready: " + System.IO.Path.GetFileName(report.ZipPath) + ". Your email to the developer is open; drag the zip in and send it.", OverlayLook.Good, privacy);
			else
				ShowBugReportStatus("Zip ready: " + System.IO.Path.GetFileName(report.ZipPath) + ". No mail app opened: email it to " + BugReport.DevEmail + " (copied to the clipboard).", OverlayLook.Warn, privacy);
		}
	}
}
