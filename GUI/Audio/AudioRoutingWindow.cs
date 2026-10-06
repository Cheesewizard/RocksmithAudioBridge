using System;
using System.Diagnostics;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal sealed class AudioRoutingWindow : Form
	{
		private const int TOGGLE_RECORDING_MESSAGE = 0x8421;
		private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;

		[DllImport("dwmapi.dll")]
		private static extern int DwmSetWindowAttribute(IntPtr windowHandle, int attribute, ref int value, int valueSize);

		private readonly AudioRoutingPanel panel;
		private readonly Process rocksmithProcess;
		private readonly Timer rocksmithLifetimeTimer;
		private bool isClosingWithRocksmith;

		public AudioRoutingWindow(string gameDirectory) : this(gameDirectory, 0)
		{
		}

		internal AudioRoutingWindow(string gameDirectory, int rocksmithProcessId, bool showWindow = false)
		{
			Text = AudioBridgeWindow.Title;
			Icon = Properties.Resources.AudioBridgeIcon;
			DoubleBuffered = true;
			// The layout is hand-built in code with 96-DPI pixel constants (ClientSize, column widths, control
			// widths) while StudioTheme uses point-based fonts that render at the real DPI. With the process
			// DPI-aware (app.manifest) and this set to Dpi, WinForms scales every pixel constant by DeviceDpi/96
			// so the layout matches the fonts.
			// The child AudioRoutingPanel is a UserControl left at AutoScaleMode.Inherit, so it scales in this
			// same pass rather than running a second, conflicting scale of its own.
			// This form is built entirely in code, so WinForms has no designer-generated 96-DPI baseline.
			// Without it, AutoScaleMode.Dpi records the current monitor as the design DPI: point-sized text grows,
			// but the form and pixel-based layout constants do not, which clips labels and whole rows on a scaled
			// display. Declare the actual design baseline explicitly before enabling DPI scaling.
			AutoScaleDimensions = new SizeF(96F, 96F);
			AutoScaleMode = AutoScaleMode.Dpi;
			StartPosition = FormStartPosition.CenterScreen;
			// Logical (96-DPI) sizes; WinForms scales them by DeviceDpi/96 via AutoScaleMode.Dpi. The default is
			// deliberately generous, so on a scaled display it can end up taller than the screen; OnShown clamps it
			// to the monitor's work area and the panel's AutoScroll takes care of any overflow. The minimum is kept
			// small enough that its scaled height still fits a 150% laptop panel.
			ClientSize = new Size(1320, 900);
			MinimumSize = new Size(960, 650);
			BackColor = StudioTheme.Background;
			ForeColor = StudioTheme.Ink;
			panel = new AudioRoutingPanel(gameDirectory) { Dock = DockStyle.Fill };
			if (rocksmithProcessId > 0)
			{
				rocksmithProcess = Process.GetProcessById(rocksmithProcessId);
				// Windows never creates a taskbar button for a window whose very first show is already
				// minimised: the button is only created for a window first shown in the normal state.
				// So the form first shows normal but fully transparent (nothing flashes over the game),
				// OnShown drops it to minimised, and the button that first show created survives.
				pendingAutoMinimize = !showWindow;
				if (pendingAutoMinimize)
					Opacity = 0;
				rocksmithLifetimeTimer = new Timer { Interval = 500 };
				rocksmithLifetimeTimer.Tick += CheckRocksmithLifetime;
				rocksmithLifetimeTimer.Start();
			}
			Controls.Add(panel);
			FormClosing += WarnWhileBusy;
			FormClosed += ReleaseRocksmithLifetime;
		}

		protected override void OnHandleCreated(EventArgs e)
		{
			base.OnHandleCreated(e);
			int enabled = 1;
			DwmSetWindowAttribute(Handle, DWMWA_USE_IMMERSIVE_DARK_MODE, ref enabled, sizeof(int));
		}

		private bool clampedToScreen;
		private bool pendingAutoMinimize;

		protected override void OnShown(EventArgs e)
		{
			base.OnShown(e);
			ClampToWorkingArea();
			if (pendingAutoMinimize)
			{
				pendingAutoMinimize = false;
				WindowState = FormWindowState.Minimized;
				BeginInvoke(new Action(() => Opacity = 1));
			}
		}

		protected override void OnResize(EventArgs e)
		{
			base.OnResize(e);
			// OnShown clamps the first show (transparent for the auto-launched bridge, visible for a
			// manual open) once it is laid out and DPI-scaled. This resize pass is the fallback for a
			// restore that lands before that clamp ran: clamp on it, once, rather than fighting later
			// manual resizes.
			if (!clampedToScreen && Visible && WindowState == FormWindowState.Normal)
				ClampToWorkingArea();
		}

		// After DPI scaling the window can be larger than the screen (a 96-DPI 1060x880 becomes ~1590x1320 at
		// 150%). Fit it to the monitor's work area and re-centre; the panel scrolls for anything that does not fit.
		private void ClampToWorkingArea()
		{
			if (WindowState != FormWindowState.Normal)
				return;
			clampedToScreen = true;
			Rectangle work = Screen.FromControl(this).WorkingArea;
			int width = Math.Min(Width, work.Width);
			int height = Math.Min(Height, work.Height);
			if (width == Width && height == Height)
				return;
			Size = new Size(width, height);
			Location = new Point(
				work.Left + Math.Max(0, (work.Width - Width) / 2),
				work.Top + Math.Max(0, (work.Height - Height) / 2));
		}

		protected override void WndProc(ref Message message)
		{
			if (message.Msg == TOGGLE_RECORDING_MESSAGE)
			{
				panel.ToggleRecordingFromHotkey(message.WParam);
				return;
			}
			base.WndProc(ref message);
		}

		private void WarnWhileBusy(object sender, FormClosingEventArgs args)
		{
			if (isClosingWithRocksmith)
				return;
			if (rocksmithProcess != null && !rocksmithProcess.HasExited)
			{
				args.Cancel = true;
				WindowState = FormWindowState.Minimized;
				return;
			}
			if (!panel.IsBusy)
				return;
			args.Cancel = true;
			MessageBox.Show(this, "An audio operation is still running. Finish the take or cancel the buffer test before closing.",
				"Audio bridge", MessageBoxButtons.OK, MessageBoxIcon.Warning);
		}

		private void CheckRocksmithLifetime(object sender, EventArgs args)
		{
			if (!rocksmithProcess.HasExited)
				return;
			isClosingWithRocksmith = true;
			Close();
		}

		private void ReleaseRocksmithLifetime(object sender, FormClosedEventArgs args)
		{
			rocksmithLifetimeTimer?.Stop();
			rocksmithLifetimeTimer?.Dispose();
			rocksmithProcess?.Dispose();
		}
	}
}
