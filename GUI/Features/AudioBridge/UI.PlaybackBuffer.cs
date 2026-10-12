using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
using RSMods.Audio;

namespace RSMods
{
	// Playback buffer: the size of the Windows playback stream the bridge plays the game through. That stream is used
	// whenever the game's sound goes to a Windows device: always with the Real Tone Cable, and with ASIO when the
	// overlay switches output to a Windows device. It is not the ASIO buffer (set in the interface's own panel).
	//
	// One setting for every device, RSMods.ini [Audio Bridge] OutputPeriodFrames (absent = Automatic, about 3 ms; the
	// DLL fits any value to the device, OutputBackend::FitPeriod). Saving it removes the old per-device
	// [Audio Bridge Output Buffer <id>] sections, which would otherwise win (AudioBridgeEngine ReadSavedPeriod).
	// With Rocksmith running it is applied live (engine op 6): the game keeps its stream, only the Windows side
	// reopens, so there is a short gap and nothing for the game to notice.
	//
	// Brought back 2026-10-11: a cable player heard a delay after every note that the game's LatencyBuffer could not
	// change; 144 frames instead of the Windows default 480 fixed it. Too small a buffer only crackles, it cannot
	// crash the game, which always sees the bridge's fixed 10 ms stream.
	public partial class MainForm
	{
		[DllImport("kernel32", CharSet = CharSet.Unicode)]
		private static extern int GetPrivateProfileSectionNamesW(char[] buffer, int size, string path);

		private ComboBox playbackBufferPicker;
		private Label playbackBufferStatus;
		private bool applyingPlaybackBuffer;

		private sealed class BufferChoice
		{
			public uint Frames;
			public string Name;
			public override string ToString() => Name;
		}

		private static readonly BufferChoice[] BufferChoices =
		{
			new BufferChoice { Frames = 0, Name = "Automatic (about 3 ms)" },
			new BufferChoice { Frames = 96, Name = "2 ms (lowest, may crackle)" },
			new BufferChoice { Frames = 240, Name = "5 ms" },
			new BufferChoice { Frames = 480, Name = "10 ms (Windows default)" },
			new BufferChoice { Frames = 960, Name = "20 ms (busy PC)" },
		};

		private void InitializePlaybackBufferSetup(SetupCard card)
		{
			AddSetupDivider(card);
			int width = card.LogicalToDeviceUnits(SetupCardWidth - SetupLabelWidth - 40);
			var column = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, Margin = new Padding(0) };
			playbackBufferPicker = new ComboBox { Width = width, AccessibleName = "Playback buffer" };
			StudioTheme.StyleSelector(playbackBufferPicker);
			playbackBufferPicker.SelectedIndexChanged += async (sender, args) =>
			{
				if (syncingAudioSetup || applyingPlaybackBuffer || !(playbackBufferPicker.SelectedItem is BufferChoice choice)) return;
				await ApplyPlaybackBuffer(choice.Frames);
			};
			column.Controls.Add(playbackBufferPicker);
			playbackBufferStatus = new Label { Text = "", Font = StudioTheme.Small, ForeColor = OverlayLook.Muted, AutoSize = true, MaximumSize = new Size(width, 0), Margin = new Padding(0, card.LogicalToDeviceUnits(4), 0, 0) };
			column.Controls.Add(playbackBufferStatus);
			AddSetupRow(card, "Playback buffer", "Smaller is less delay after each note. Used when the game plays through a Windows device: always with the Real Tone Cable. Changes at once while Rocksmith runs.", column);
		}

		private static uint ReadPlaybackBuffer()
		{
			var value = new StringBuilder(16);
			GetPrivateProfileStringW("Audio Bridge", "OutputPeriodFrames", "", value, value.Capacity, BridgeSettingsPath);
			return uint.TryParse(value.ToString(), NumberStyles.None, CultureInfo.InvariantCulture, out uint frames) && frames <= 48000 ? frames : 0;
		}

		private void RefreshPlaybackBufferSetup()
		{
			if (playbackBufferPicker == null || applyingPlaybackBuffer) return;
			uint saved = ReadPlaybackBuffer();
			playbackBufferPicker.BeginUpdate();
			playbackBufferPicker.Items.Clear();
			foreach (var choice in BufferChoices) playbackBufferPicker.Items.Add(choice);
			int index = Array.FindIndex(BufferChoices, c => c.Frames == saved);
			if (index < 0)
			{
				// A hand-edited size keeps its own entry, so opening the tab never changes it.
				playbackBufferPicker.Items.Add(new BufferChoice { Frames = saved, Name = DescribeFrames(saved) + " (custom)" });
				index = playbackBufferPicker.Items.Count - 1;
			}
			playbackBufferPicker.SelectedIndex = index;
			playbackBufferPicker.EndUpdate();
			if (playbackBufferStatus.Text.Length == 0)
				playbackBufferStatus.Text = "If you hear crackles, pick a larger buffer.";
		}

		private static string DescribeFrames(uint frames) =>
			(frames / 48.0).ToString("0.#", CultureInfo.InvariantCulture) + " ms (" + frames + " frames)";

		private async System.Threading.Tasks.Task ApplyPlaybackBuffer(uint frames)
		{
			applyingPlaybackBuffer = true;
			playbackBufferPicker.Enabled = false;
			try
			{
				if (!SavePlaybackBuffer(frames))
				{
					ShowPlaybackBufferStatus("The buffer could not be saved to RSMods.ini.", OverlayLook.Warn);
					return;
				}
				AudioControlClient client = FindGameAudioClient();
				if (client == null)
				{
					ShowPlaybackBufferStatus("Saved. Used next time Rocksmith starts. If you hear crackles, pick a larger buffer.", OverlayLook.Good);
					return;
				}
				await ApplyPlaybackBufferLive(client, frames);
			}
			finally
			{
				applyingPlaybackBuffer = false;
				playbackBufferPicker.Enabled = true;
			}
		}

		// Ask the running game for the device's limits (cable output only; on ASIO op 5 has none), fit the size to them,
		// apply it with op 6 and read back what the device runs.
		private async System.Threading.Tasks.Task ApplyPlaybackBufferLive(AudioControlClient client, uint frames)
		{
			AudioBufferSnapshot snapshot = null;
			string endpoint = "";
			try
			{
				var status = await client.SendAsync(5);
				endpoint = status.EndpointId ?? "";
				snapshot = AudioBufferSnapshot.Parse(status);
			}
			catch { snapshot = null; }

			uint request = frames;
			if (snapshot != null && frames != 0)
			{
				request = Math.Max(snapshot.Minimum, (frames + snapshot.Fundamental - 1) / snapshot.Fundamental * snapshot.Fundamental);
				if (request > snapshot.Maximum) request = snapshot.Maximum / snapshot.Fundamental * snapshot.Fundamental;
			}
			try
			{
				await client.SendAsync(6, request.ToString(CultureInfo.InvariantCulture) + "\n" + endpoint);
			}
			catch (Exception error)
			{
				// 0x800700AA = ERROR_BUSY: the recorder shares this stream, so the engine refuses while a take runs.
				string why = error.Message.IndexOf("800700AA", StringComparison.OrdinalIgnoreCase) >= 0
					? "Stop recording first, or it applies next launch."
					: "It applies next time the game plays through a Windows device.";
				ShowPlaybackBufferStatus("Saved. Not changed now: " + why, OverlayLook.Muted);
				return;
			}

			// The Windows stream reopens in the background; confirm what it settled on.
			for (int attempt = 0; attempt < 20; attempt++)
			{
				await System.Threading.Tasks.Task.Delay(100);
				try
				{
					var now = AudioBufferSnapshot.Parse(await client.SendAsync(5));
					ShowPlaybackBufferStatus("Now playing with " + DescribeFrames(now.Period) + ". Saved. If you hear crackles, pick a larger buffer.", OverlayLook.Good);
					return;
				}
				catch { /* still reopening, or not the cable output */ }
			}
			ShowPlaybackBufferStatus("Saved and sent to Rocksmith. If you hear crackles, pick a larger buffer.", OverlayLook.Good);
		}

		private void ShowPlaybackBufferStatus(string text, Color colour)
		{
			playbackBufferStatus.Text = text;
			playbackBufferStatus.ForeColor = colour;
		}

		// Write the one setting (Automatic removes the key) and drop every per-device section so it is the value used.
		private static bool SavePlaybackBuffer(uint frames)
		{
			string path = BridgeSettingsPath;
			if (!WritePrivateProfileStringW("Audio Bridge", "OutputPeriodFrames", frames == 0 ? null : frames.ToString(CultureInfo.InvariantCulture), path)
				&& frames != 0)
				return false;
			foreach (string section in ReadSectionNames(path))
				if (section.StartsWith("Audio Bridge Output Buffer ", StringComparison.OrdinalIgnoreCase))
					WritePrivateProfileStringW(section, null, null, path);
			return true;
		}

		private static List<string> ReadSectionNames(string path)
		{
			var names = new List<string>();
			var buffer = new char[32768];
			int length = GetPrivateProfileSectionNamesW(buffer, buffer.Length, path);
			int start = 0;
			for (int i = 0; i < length; i++)
			{
				if (buffer[i] != '\0') continue;
				if (i > start) names.Add(new string(buffer, start, i - start));
				start = i + 1;
			}
			return names;
		}
	}
}
