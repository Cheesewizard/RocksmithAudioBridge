#nullable enable
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Threading.Tasks;

namespace RocksmithAudioBridge.Core
{
	/// <summary>
	/// The running game's audio engine control pipe (\\.\pipe\RSModsPlus.Audio.&lt;pid&gt;), ported from
	/// GUI/Audio/AudioControlClient.cs. Only what this app needs: send one op and get its result. Protocol version 6,
	/// 2056-byte request, 3176-byte response; ops used here: 25 (switch the bridge interface) and 30 (input interface).
	/// </summary>
	public sealed class AudioControlClient
	{
		public const uint OpSwitchInterface = 25;
		public const uint OpSwitchInputInterface = 30;

		public int ProcessId { get; }

		public AudioControlClient(int processId)
		{
			if (processId <= 0) throw new ArgumentOutOfRangeException(nameof(processId));
			ProcessId = processId;
		}

		/// <summary>The running game's engine (one pipe per Rocksmith process), or null when none answers.</summary>
		public static AudioControlClient? FindGame()
		{
			foreach (var process in Process.GetProcessesByName("Rocksmith2014"))
				using (process)
					if (File.Exists(@"\\.\pipe\RSModsPlus.Audio." + process.Id)) return new AudioControlClient(process.Id);
			return null;
		}

		/// <summary>Sends the op; throws when the engine rejects it (negative result), times out or the versions differ.</summary>
		public async Task SendAsync(uint operation, string value = "")
		{
			if (value == null || value.Length >= 1024 || value.IndexOf('\0') >= 0)
				throw new ArgumentException("Invalid audio command value.", nameof(value));
			using var pipe = new NamedPipeClientStream(".", "RSModsPlus.Audio." + ProcessId, PipeDirection.InOut, PipeOptions.Asynchronous);
			await pipe.ConnectAsync(350);
			var request = new byte[2056];
			Buffer.BlockCopy(BitConverter.GetBytes(6u), 0, request, 0, 4);
			Buffer.BlockCopy(BitConverter.GetBytes(operation), 0, request, 4, 4);
			Encoding.Unicode.GetBytes(value, 0, value.Length, request, 8);
			var exchange = ExchangeAsync(pipe, request);
			if (await Task.WhenAny(exchange, Task.Delay(5000)) != exchange)
			{
				pipe.Dispose();
				try { await exchange; }
				catch (IOException) { }
				catch (ObjectDisposedException) { }
				throw new TimeoutException("The audio bridge did not acknowledge the command. Check its live status before retrying.");
			}
			await exchange;
		}

		private static async Task ExchangeAsync(Stream pipe, byte[] request)
		{
			await pipe.WriteAsync(request);
			var response = new byte[3176];
			int received = 0;
			while (received < response.Length)
			{
				int count = await pipe.ReadAsync(response.AsMemory(received, response.Length - received));
				if (count == 0) throw new IOException("The game closed the audio control connection.");
				received += count;
				if (received >= 4 && BitConverter.ToUInt32(response, 0) != 6)
					throw new NotSupportedException("Audio bridge version mismatch. Use matching settings and game DLL builds, then restart Rocksmith.");
			}
			int result = BitConverter.ToInt32(response, 4);
			if (result < 0) throw new InvalidOperationException("Audio command failed (0x" + result.ToString("X8") + ").");
		}
	}
}
