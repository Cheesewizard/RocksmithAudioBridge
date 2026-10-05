using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
using RSMods.Audio;
using RSMods.Util;

namespace RSMods
{
	// The audio setup that cannot live in the in-game overlay, the whole of the Rocksmith Audio Bridge tab: bridge
	// power, input mode and the ASIO bridge driver (all game-closed), plus the recordings folder (a folder dialog
	// cannot show over fullscreen Rocksmith). Everything used while playing - mixer, guitar input, output
	// switching, recording, Note by Note, Drop Pedal - is the in-game overlay.
	//
	// The driver section is a single Install / Uninstall button; the interface the bridge wraps is worked out,
	// never asked (ResolveBridgeInterface). The row is hidden in Real Tone Cable mode unless the driver is installed.
	//
	// The tab is drawn like the in-game overlay (DLL/Overlay/OverlayUi.hpp palette): a header
	// strip with the Rocksmith Audio Bridge logo, rounded cards, and rows with the label on the left and the
	// control on the right. OverlayLook mirrors the overlay's Color:: constants; keep the two in step.
	public partial class MainForm
	{
		[DllImport("kernel32", CharSet = CharSet.Unicode)]
		private static extern int GetPrivateProfileStringW(string section, string key, string def, StringBuilder ret, int size, string path);
		[DllImport("kernel32", CharSet = CharSet.Unicode)]
		private static extern bool WritePrivateProfileStringW(string section, string key, string val, string path);

		private SegmentedControl audioSetupPower;
		private SegmentedControl audioSetupInput;
		private Label audioSetupStatus;
		private PathField recordingFolderLabel;
		private Control asioDriverRow, asioDriverDivider;
		private Label asioDriverStatus;
		private StudioButton asioDriverButton;
		private bool asioDriverRegistered;
		private string asioResolvedDriver;
		private bool syncingAudioSetup;

		// 0 = Real Tone Cable, 1 = ASIO interface. Kept in one place so the rocker and the read/write paths agree.
		private const int InputSegmentCable = 0;
		private const int InputSegmentAsio = 1;

		private static string AudioRoutingIniPath => Path.Combine(GenUtil.GetRSDirectory(), "AudioRouting.ini");

		// The overlay's palette (DLL/Overlay/OverlayUi.hpp, Color::*).
		internal static class OverlayLook
		{
			public static readonly Color Window = Color.FromArgb(11, 17, 30);
			public static readonly Color Header = Color.FromArgb(14, 21, 38);
			public static readonly Color Card = Color.FromArgb(21, 30, 50);
			public static readonly Color CardEdge = Color.FromArgb(34, 46, 72);
			public static readonly Color Track = Color.FromArgb(36, 48, 74);
			public static readonly Color Text = Color.FromArgb(230, 234, 242);
			public static readonly Color Muted = Color.FromArgb(138, 151, 173);
			public static readonly Color Faint = Color.FromArgb(91, 104, 128);
			public static readonly Color Accent = Color.FromArgb(96, 165, 250);
			public static readonly Color Good = Color.FromArgb(52, 211, 153);
			public static readonly Color Warn = Color.FromArgb(251, 191, 36);
		}

		private const int SetupCardWidth = 760;   // logical px
		private const int SetupLabelWidth = 330;  // logical px, left column of a row

		private void InitializeAudioBridgeSetup(FlowLayoutPanel page)
		{
			audioSetupStatus = new Label { Text = "", Font = StudioTheme.Strong, ForeColor = OverlayLook.Warn, AutoSize = true, MaximumSize = new Size(page.LogicalToDeviceUnits(SetupCardWidth), 0), Margin = new Padding(0, 0, 0, page.LogicalToDeviceUnits(10)) };
			page.Controls.Add(audioSetupStatus);

			var setup = new SetupCard("Audio bridge", page.LogicalToDeviceUnits(SetupCardWidth));
			audioSetupPower = new SegmentedControl("Off", "On") { Margin = new Padding(0), AccessibleName = "Audio bridge power" };
			audioSetupPower.SelectedIndexChanged += (sender, args) => { if (!syncingAudioSetup) SetAudioBridgePower(audioSetupPower.SelectedIndex == 1); };
			AddSetupRow(setup, "Power", "Note by Note works either way.", audioSetupPower);

			audioSetupInput = new SegmentedControl("Real Tone Cable", "ASIO interface") { Margin = new Padding(0), AccessibleName = "Guitar input" };
			audioSetupInput.SelectedIndexChanged += (sender, args) => { if (!syncingAudioSetup) SetAudioInputMode(audioSetupInput.SelectedIndex == InputSegmentAsio); };
			AddSetupDivider(setup);
			AddSetupRow(setup, "Guitar input", "Takes effect next time Rocksmith starts.", audioSetupInput);

			InitializeAsioDriverSetup(setup);
			page.Controls.Add(setup);

			InitializeRecordingFolderSetup(page);

			page.VisibleChanged += (sender, args) => { if (page.Visible) RefreshAudioBridgeSetup(); };
			// The window can sit open while Rocksmith starts or stops; re-check whenever it comes back to the front.
			Activated += (sender, args) => { if (page.Visible) RefreshAudioBridgeSetup(); };
			RefreshAudioBridgeSetup();
		}

		// A row inside a card: title and a one-line caption on the left, the control on the right, vertically centred.
		private static Control AddSetupRow(SetupCard card, string title, string caption, Control control)
		{
			var row = new TableLayoutPanel { AutoSize = true, ColumnCount = 2, RowCount = 1, Margin = new Padding(0), Padding = new Padding(0) };
			row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, card.LogicalToDeviceUnits(SetupLabelWidth)));
			row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			row.RowStyles.Add(new RowStyle(SizeType.AutoSize));
			var text = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = new Padding(0), Anchor = AnchorStyles.Left };
			text.Controls.Add(new Label { Text = title, Font = StudioTheme.Strong, ForeColor = OverlayLook.Text, AutoSize = true, Margin = new Padding(0, 0, 0, 2) });
			if (!string.IsNullOrEmpty(caption))
				text.Controls.Add(new Label { Text = caption, Font = StudioTheme.Small, ForeColor = OverlayLook.Muted, AutoSize = true, MaximumSize = new Size(card.LogicalToDeviceUnits(SetupLabelWidth - 16), 0), Margin = new Padding(0) });
			control.Anchor = AnchorStyles.Left;
			row.Controls.Add(text, 0, 0);
			row.Controls.Add(control, 1, 0);
			card.Controls.Add(row);
			return row;
		}

		private static Control AddSetupDivider(SetupCard card)
		{
			var line = new Panel { Height = 1, Width = card.InnerWidth, BackColor = OverlayLook.CardEdge, Margin = new Padding(0, card.LogicalToDeviceUnits(12), 0, card.LogicalToDeviceUnits(12)) };
			card.Controls.Add(line);
			return line;
		}

		private static StudioButton CreateSetupButton(string text, Control scale, StudioButtonKind kind = StudioButtonKind.Ghost)
		{
			var button = new StudioButton(text, kind) { Margin = new Padding(0), MinimumTextWidth = scale.LogicalToDeviceUnits(96) };
			button.Height = scale.LogicalToDeviceUnits(36);
			button.Text = text;   // re-measure at the scaled minimum width
			return button;
		}

		// The overlay's BeginCard/EndCard: a rounded Card-coloured box with a CardEdge outline, 16 px padding and a
		// small-caps title. Fixed width; height follows the contents.
		internal sealed class SetupCard : FlowLayoutPanel
		{
			public SetupCard(string title, int width)
			{
				FlowDirection = FlowDirection.TopDown;
				WrapContents = false;
				AutoSize = true;
				AutoSizeMode = AutoSizeMode.GrowAndShrink;
				int pad = LogicalToDeviceUnits(18);
				Padding = new Padding(pad);
				MinimumSize = new Size(width, 0);
				MaximumSize = new Size(width, 0);
				Margin = new Padding(0, 0, 0, LogicalToDeviceUnits(14));
				BackColor = OverlayLook.Card;
				ForeColor = OverlayLook.Text;
				DoubleBuffered = true;
				ResizeRedraw = true;
				if (!string.IsNullOrEmpty(title))
					Controls.Add(new Label { Text = title.ToUpperInvariant(), Font = StudioTheme.SectionFont, ForeColor = OverlayLook.Muted, AutoSize = true, Margin = new Padding(0, 0, 0, LogicalToDeviceUnits(12)) });
			}

			public int InnerWidth => MinimumSize.Width - Padding.Horizontal;

			protected override void OnPaintBackground(PaintEventArgs e)
			{
				e.Graphics.Clear(Parent?.BackColor ?? OverlayLook.Window);
				e.Graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
				var bounds = new Rectangle(0, 0, Width - 1, Height - 1);
				using (var path = StudioTheme.RoundedRectangle(bounds, LogicalToDeviceUnits(6)))
				using (var fill = new SolidBrush(OverlayLook.Card))
				using (var edge = new Pen(OverlayLook.CardEdge))
				{
					e.Graphics.FillPath(fill, path);
					e.Graphics.DrawPath(edge, path);
				}
			}
		}

		// The header strip across the top of the tab, same as the overlay's: logo, name, one line of context.
		internal sealed class BrandHeader : Control
		{
			private static Bitmap logo;

			public BrandHeader()
			{
				SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
				BackColor = OverlayLook.Header;
				Height = LogicalToDeviceUnits(84);
			}

			// GUI/Assets/AudioBridge256.ico, embedded - the same art the overlay header uses (DLL/Overlay/AudioBridgeLogo.h).
			private static Bitmap Logo()
			{
				if (logo != null) return logo;
				// Icon.ToBitmap cannot decode the PNG-compressed 256 px frame, so read the ICO directory and decode the
				// largest frame directly (PNG frames via Image.FromStream, older BMP frames via Icon).
				try
				{
					using (var stream = typeof(BrandHeader).Assembly.GetManifestResourceStream("RSMods.AudioBridgeLogo.ico"))
					{
						if (stream == null) return null;
						var data = new byte[stream.Length];
						stream.Read(data, 0, data.Length);
						int count = BitConverter.ToUInt16(data, 4), best = -1, bestSize = 0;
						for (int index = 0; index < count; index++)
						{
							int entry = 6 + index * 16;
							int size = data[entry] == 0 ? 256 : data[entry];
							if (size > bestSize) { bestSize = size; best = entry; }
						}
						if (best < 0) return null;
						int length = BitConverter.ToInt32(data, best + 8), offset = BitConverter.ToInt32(data, best + 12);
						bool png = data[offset] == 0x89 && data[offset + 1] == 0x50;
						if (png)
							using (var frame = new System.IO.MemoryStream(data, offset, length))
								logo = new Bitmap(Image.FromStream(frame));
						else
							using (var whole = new System.IO.MemoryStream(data))
							using (var icon = new Icon(whole, new Size(bestSize, bestSize)))
								logo = icon.ToBitmap();
					}
				}
				catch { logo = null; }
				return logo;
			}

			protected override void OnPaint(PaintEventArgs e)
			{
				var g = e.Graphics;
				g.Clear(BackColor);
				using (var edge = new Pen(OverlayLook.CardEdge)) g.DrawLine(edge, 0, Height - 1, Width, Height - 1);
				int left = LogicalToDeviceUnits(28);
				int mark = LogicalToDeviceUnits(52);
				var image = Logo();
				if (image != null)
				{
					g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
					g.DrawImage(image, new Rectangle(left, (Height - mark) / 2, mark, mark));
					left += mark + LogicalToDeviceUnits(16);
				}
				int titleHeight = TextRenderer.MeasureText("Ag", StudioTheme.Title).Height;
				int subHeight = TextRenderer.MeasureText("Ag", StudioTheme.Body).Height;
				int top = (Height - titleHeight - subHeight) / 2;
				TextRenderer.DrawText(g, "Rocksmith Audio Bridge", StudioTheme.Title, new Point(left, top), OverlayLook.Text, TextFormatFlags.NoPrefix);
				TextRenderer.DrawText(g, "Setup. Change these with Rocksmith closed. Everything else is in the in-game overlay (press \\).",
					StudioTheme.Body, new Rectangle(left, top + titleHeight, Math.Max(0, Width - left - LogicalToDeviceUnits(20)), subHeight),
					OverlayLook.Muted, TextFormatFlags.NoPrefix | TextFormatFlags.EndEllipsis | TextFormatFlags.SingleLine);
			}
		}

		// Re-read the on-disk state so the controls reflect what is actually set, and disable them while the game
		// is running (these are game-closed settings).
		private void RefreshAudioBridgeSetup()
		{
			if (audioSetupPower == null) return;
			syncingAudioSetup = true;
			try
			{
				var config = new AudioInputModeConfiguration(GenUtil.GetRSDirectory());
				bool gameRunning = config.IsGameRunning();

				var power = new StringBuilder(16);
				GetPrivateProfileStringW("Audio", "MasterEnabled", "1", power, power.Capacity, AudioRoutingIniPath);
				audioSetupPower.SelectedIndex = power.ToString() == "1" ? 1 : 0;
				audioSetupPower.Enabled = !gameRunning;

				AudioInputMode mode;
				try { mode = config.ReadMode(); } catch { mode = AudioInputMode.Unavailable; }
				audioSetupInput.Enabled = mode != AudioInputMode.Unavailable && !gameRunning;
				audioSetupInput.SelectedIndex = mode == AudioInputMode.Asio ? InputSegmentAsio : InputSegmentCable;

				RefreshAsioDriverSetup(gameRunning, mode == AudioInputMode.Asio);

				audioSetupStatus.Text = gameRunning
					? "Rocksmith is running. Close it to change these."
					: mode == AudioInputMode.Unavailable ? "RS_ASIO is not installed, so the guitar input cannot be switched here." : "";
				audioSetupStatus.Visible = audioSetupStatus.Text.Length > 0;
			}
			finally { syncingAudioSetup = false; }
		}

		// ---- ASIO bridge driver ---------------------------------------------------------------------------------
		// One-time, game-closed setup of the "Rocksmith Audio Bridge ASIO" proxy: register it with Windows (one admin
		// prompt) and record which real interface it wraps. RS_ASIO.ini is never written or offered for editing;
		// RS_ASIO setup is the user's.

		private static string ProxyDllPath => Path.Combine(GenUtil.GetRSDirectory(), "RocksmithAudioBridgeAsio.dll");

		// Which interface the bridge wraps; the user is never asked. The saved HKCU target wins (even before the
		// driver is registered: an earlier install or the old bridge window set it), then the interface RS_ASIO.ini
		// names directly, then the first installed driver. Every ASIO driver counts (ASIO4ALL, FlexASIO, Voicemeeter
		// included): the bridge does not judge the user's setup, and a driver that does not work is theirs to fix.
		// null only when no ASIO driver exists.
		private static string ResolveBridgeInterface(List<string> real, string target, string iniDriver)
		{
			bool Installed(string name) => !string.IsNullOrEmpty(name) && real.Exists(r => string.Equals(r, name, StringComparison.OrdinalIgnoreCase));
			if (Installed(target)) return target;
			if (Installed(iniDriver)) return iniDriver;
			// Only one ASIO driver: that is the interface. With several and none named, do not guess: wrapping the first
			// alphabetically (often ASIO4ALL or a DAW's driver) leaves the player with no sound from their interface.
			return real.Count == 1 ? real[0] : null;
		}

		private void InitializeAsioDriverSetup(SetupCard card)
		{
			asioDriverDivider = AddSetupDivider(card);
			var right = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = new Padding(0) };
			asioDriverButton = CreateSetupButton("Install", card, StudioButtonKind.Primary);
			asioDriverButton.Click += (sender, args) => { if (asioDriverRegistered) RemoveAsioDriver(); else InstallAsioDriver(); };
			right.Controls.Add(asioDriverButton);
			asioDriverStatus = new Label { Text = "", Font = StudioTheme.Small, ForeColor = OverlayLook.Muted, AutoSize = true, MaximumSize = new Size(card.LogicalToDeviceUnits(SetupCardWidth - SetupLabelWidth - 40), 0), Margin = new Padding(0, card.LogicalToDeviceUnits(6), 0, 0) };
			right.Controls.Add(asioDriverStatus);
			asioDriverRow = AddSetupRow(card, "ASIO bridge driver", "Needed to record and switch output under ASIO.", right);
		}

		private void RefreshAsioDriverSetup(bool gameRunning, bool asioMode)
		{
			if (asioDriverButton == null) return;
			string target = AsioProxySetup.ReadTarget();
			string iniDriver = AsioProxySetup.ReadOutputDriver(GenUtil.GetRSDirectory());
			bool registered = AsioProxySetup.IsProxyRegistered();
			// Registered is not enough: the entry must resolve to the shipped proxy DLL. A stale entry (the pre-rename
			// RocksmithAudioBridge.dll, now the managed library) makes RS_ASIO find no device at all, so it is shown
			// as needing repair and the one button re-registers instead of offering Uninstall.
			bool broken = registered && !AsioProxySetup.IsProxyRegistrationCurrent(ProxyDllPath);
			// Repair re-registers the shipped DLL, so it needs the file. With the file gone (antivirus quarantine, a game
			// reinstall) the one useful action is removing the dangling entry, which Unregister does without the DLL.
			bool repairable = broken && File.Exists(ProxyDllPath);
			bool linked = string.Equals(iniDriver, AsioProxySetup.ProxyName, StringComparison.OrdinalIgnoreCase);
			var real = AsioProxySetup.ListRealDrivers();
			asioDriverRegistered = registered && !repairable;
			asioResolvedDriver = ResolveBridgeInterface(real, target, iniDriver);

			// Only relevant in ASIO mode; stays visible in cable mode while installed so it can be removed.
			asioDriverRow.Visible = asioDriverDivider.Visible = asioMode || registered;

			asioDriverButton.Text = repairable ? "Repair" : registered ? "Uninstall" : "Install";
			asioDriverButton.Kind = registered && !repairable ? StudioButtonKind.Ghost : StudioButtonKind.Primary;
			asioDriverButton.Enabled = !gameRunning && (registered || asioResolvedDriver != null);

			// One short line, only saying something the user can act on or needs to know.
			Color colour = OverlayLook.Muted;
			string status;
			if (broken && !repairable)
			{
				status = "The driver file is missing from the game folder, so Rocksmith would find no audio device. Uninstall removes the leftover entry; reinstall the mod to get the driver back.";
				colour = OverlayLook.Warn;
			}
			else if (broken)
			{
				string points = AsioProxySetup.RegisteredProxyPath();
				status = "Needs repair: the driver entry points at " + (string.IsNullOrEmpty(points) ? "a missing file" : Path.GetFileName(points))
					+ ", so Rocksmith would find no audio device.";
				colour = OverlayLook.Warn;
			}
			else if (!registered)
			{
				status = real.Count == 0 ? "No ASIO drivers found."
					: asioResolvedDriver == null ? "Several ASIO drivers are installed. Set Driver= in RS_ASIO.ini to your interface first, so the bridge knows which one to use."
					: "Not installed.";
				if (asioResolvedDriver == null) colour = OverlayLook.Warn;
			}
			else if (linked)
			{
				status = "Installed.";
				colour = OverlayLook.Good;
			}
			else
			{
				status = "Installed. To use it, set Driver=" + AsioProxySetup.ProxyName + " in RS_ASIO.ini.";
			}
			asioDriverStatus.Text = status;
			asioDriverStatus.ForeColor = colour;
		}

		private void InstallAsioDriver()
		{
			string driver = asioResolvedDriver;
			if (driver == null)
			{
				MessageBox.Show(this, "No ASIO drivers found.", "ASIO bridge driver", MessageBoxButtons.OK, MessageBoxIcon.Information);
				return;
			}
			if (new AudioInputModeConfiguration(GenUtil.GetRSDirectory()).IsGameRunning())
			{
				MessageBox.Show(this, "Close Rocksmith first. RS_ASIO loads its driver when the game starts.", "ASIO bridge driver", MessageBoxButtons.OK, MessageBoxIcon.Warning);
				return;
			}
			try
			{
				AsioProxySetup.Register(ProxyDllPath);
				AsioProxySetup.SelectTarget(driver);
				AsioProxySetup.SetPreferRealOutput(true);
			}
			catch (OperationCanceledException error) { MessageBox.Show(this, error.Message, "ASIO bridge driver", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
			catch (Exception error) { MessageBox.Show(this, "The driver could not be installed: " + error.Message, "ASIO bridge driver", MessageBoxButtons.OK, MessageBoxIcon.Error); }
			RefreshAudioBridgeSetup();
		}

		private void RemoveAsioDriver()
		{
			if (new AudioInputModeConfiguration(GenUtil.GetRSDirectory()).IsGameRunning())
			{
				MessageBox.Show(this, "Close Rocksmith first.", "ASIO bridge driver", MessageBoxButtons.OK, MessageBoxIcon.Warning);
				return;
			}
			string target = AsioProxySetup.ReadTarget();
			string restore = string.IsNullOrEmpty(target) ? "the ASIO driver you used before" : target;
			bool linked = AsioProxySetup.IsLinked(GenUtil.GetRSDirectory());
			if (MessageBox.Show(this, "Uninstall the bridge driver?" + (linked
				? "\n\nBefore starting Rocksmith again, change the Driver= lines in RS_ASIO.ini back to " + restore + ", or Rocksmith will find no audio device. The bridge never edits that file for you."
				: ""),
				"ASIO bridge driver", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK) return;
			try { AsioProxySetup.Unregister(ProxyDllPath); }
			catch (OperationCanceledException error) { MessageBox.Show(this, error.Message, "ASIO bridge driver", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
			catch (Exception error) { MessageBox.Show(this, "The driver could not be removed: " + error.Message, "ASIO bridge driver", MessageBoxButtons.OK, MessageBoxIcon.Error); }
			RefreshAudioBridgeSetup();
		}

		// ---- Recordings folder ------------------------------------------------------------------------------------
		// Where in-game takes (overlay Record button and the record hotkey) are saved: AudioRouting.ini [Audio]
		// RecordingDirectory, read by DLL/Audio/TakeRecorder.cpp. Picked here because a folder dialog cannot show
		// over fullscreen Rocksmith. It can be changed while the game runs; the next take uses it.

		private static string DefaultRecordingFolder =>
			Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "RSModsPlus");

		private static string ReadRecordingFolder()
		{
			var value = new StringBuilder(1024);
			GetPrivateProfileStringW("Audio", "RecordingDirectory", "", value, value.Capacity, AudioRoutingIniPath);
			return value.Length > 0 ? value.ToString() : DefaultRecordingFolder;
		}

		private void InitializeRecordingFolderSetup(FlowLayoutPanel page)
		{
			// Its own card: the path in a rounded field, then Change and Open, all one height in one line. The
			// overlay's Saving card sends users here to change the folder, so it stays on this game-closed page.
			var card = new SetupCard("Recordings", page.LogicalToDeviceUnits(SetupCardWidth));
			var line = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0) };
			var change = CreateSetupButton("Change", card);
			var open = CreateSetupButton("Open", card);
			change.Margin = new Padding(card.LogicalToDeviceUnits(8), 0, 0, 0);
			open.Margin = new Padding(card.LogicalToDeviceUnits(8), 0, 0, 0);
			int fieldWidth = card.InnerWidth - change.Width - open.Width - card.LogicalToDeviceUnits(16);
			recordingFolderLabel = new PathField { Text = ReadRecordingFolder(), Size = new Size(fieldWidth, change.Height), Margin = new Padding(0) };
			change.Click += ChooseRecordingFolder;
			open.Click += (sender, args) =>
			{
				try
				{
					string folder = ReadRecordingFolder();
					Directory.CreateDirectory(folder);
					System.Diagnostics.Process.Start("explorer.exe", "\"" + folder + "\"");
				}
				catch (Exception error) { MessageBox.Show(this, error.Message, "Recordings folder", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
			};
			line.Controls.Add(recordingFolderLabel);
			line.Controls.Add(change);
			line.Controls.Add(open);
			card.Controls.Add(line);
			card.Controls.Add(new Label { Text = "Where the overlay's Record button and the record hotkey save takes.", Font = StudioTheme.Small, ForeColor = OverlayLook.Muted, AutoSize = true, Margin = new Padding(0, card.LogicalToDeviceUnits(8), 0, 0) });
			page.Controls.Add(card);
		}

		private void ChooseRecordingFolder(object sender, EventArgs args)
		{
			using (var dialog = new FolderBrowserDialog { Description = "Where should takes be saved?" })
			{
				string current = ReadRecordingFolder();
				if (Directory.Exists(current)) dialog.SelectedPath = current;
				if (dialog.ShowDialog(this) != DialogResult.OK) return;
				if (!WritePrivateProfileStringW("Audio", "RecordingDirectory", dialog.SelectedPath, AudioRoutingIniPath))
				{
					MessageBox.Show(this, "The folder could not be saved.", "Recordings folder", MessageBoxButtons.OK, MessageBoxIcon.Warning);
					return;
				}
				recordingFolderLabel.Text = dialog.SelectedPath;
			}
		}

		private void SetAudioBridgePower(bool enabled)
		{
			var config = new AudioInputModeConfiguration(GenUtil.GetRSDirectory());
			if (config.IsGameRunning())
			{
				MessageBox.Show(this, "Close Rocksmith before turning the audio bridge on or off.", "Audio bridge", MessageBoxButtons.OK, MessageBoxIcon.Warning);
				RefreshAudioBridgeSetup();
				return;
			}
			string path = AudioRoutingIniPath;
			if (enabled)
			{
				WritePrivateProfileStringW("Audio", "MasterEnabled", "1", path);
			}
			else
			{
				WritePrivateProfileStringW("Audio", "Enabled", "0", path);
				WritePrivateProfileStringW("Audio", "MasterEnabled", "0", path);
			}
			RefreshAudioBridgeSetup();
		}

		private void SetAudioInputMode(bool asio)
		{
			try
			{
				new AudioInputModeConfiguration(GenUtil.GetRSDirectory()).SetAsioEnabled(asio);
			}
			catch (Exception error)
			{
				MessageBox.Show(this, error.Message, "Input mode", MessageBoxButtons.OK, MessageBoxIcon.Warning);
			}
			RefreshAudioBridgeSetup();
		}
		// A read-only rounded field (overlay Track colour) showing one line of text: vertically centred, ellipsised at
		// the end, full text in a tooltip. Sized by its owner to match the buttons beside it.
		private sealed class PathField : Control
		{
			private readonly ToolTip tip = new ToolTip();

			public PathField()
			{
				SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
				ForeColor = OverlayLook.Text;
				Font = StudioTheme.Body;
			}

			protected override void OnTextChanged(EventArgs e)
			{
				base.OnTextChanged(e);
				tip.SetToolTip(this, Text);
				Invalidate();
			}

			protected override void OnPaint(PaintEventArgs e)
			{
				e.Graphics.Clear(Parent?.BackColor ?? OverlayLook.Card);
				e.Graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
				using (var path = StudioTheme.RoundedRectangle(new Rectangle(0, 0, Width - 1, Height - 1), LogicalToDeviceUnits(6)))
				using (var fill = new SolidBrush(OverlayLook.Track))
					e.Graphics.FillPath(fill, path);
				int inset = LogicalToDeviceUnits(12);
				var bounds = new Rectangle(inset, 0, Math.Max(0, Width - inset * 2), Height);
				TextRenderer.DrawText(e.Graphics, Text, Font, bounds, ForeColor,
					TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.PathEllipsis | TextFormatFlags.NoPrefix);
			}

			protected override void Dispose(bool disposing)
			{
				if (disposing) tip.Dispose();
				base.Dispose(disposing);
			}
		}
	}
}
