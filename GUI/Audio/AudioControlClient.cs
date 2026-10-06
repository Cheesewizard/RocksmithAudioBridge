using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Threading.Tasks;

namespace RSMods.Audio
{
	internal sealed class AudioControlStatus
	{
		public int MixerError { get; set; }
		public bool IsDryInputReady { get; set; }
		public bool IsDryRecording { get; set; }
		public float[] Volumes { get; set; } = new float[7];
		public float[] OutputPeak { get; set; } = new float[2];
		public float[] OutputRms { get; set; } = new float[2];
		public int ProxyInputMode { get; set; }

		public int OutputError
		{
			get; set;
		}
		public int RecordingError
		{
			get; set;
		}
		public bool IsRecording
		{
			get; set;
		}
		public int Peak
		{
			get; set;
		}
		public ulong Frames
		{
			get; set;
		}
		public string FilePath
		{
			get; set;
		}
		public string EndpointId
		{
			get; set;
		}
		public ulong RecordingStarted
		{
			get; set;
		}
	}

	internal sealed class AudioControlClient
	{
		public int ProcessId
		{
			get;
		}

		public AudioControlClient(int processId)
		{
			if (processId <= 0)
				throw new ArgumentOutOfRangeException(nameof(processId));
			ProcessId = processId;
		}

		public async Task<AudioControlStatus> SendAsync(uint operation, string value = "")
		{
			if (value == null || value.Length >= 1024 || value.IndexOf('\0') >= 0)
				throw new ArgumentException("Invalid audio command value.", nameof(value));
			using (var pipe = new NamedPipeClientStream(".", "RSModsPlus.Audio." + ProcessId, PipeDirection.InOut, PipeOptions.Asynchronous))
			{
				await pipe.ConnectAsync(350);
				var request = new byte[2056];
				Buffer.BlockCopy(BitConverter.GetBytes(6u), 0, request, 0, 4);
				Buffer.BlockCopy(BitConverter.GetBytes(operation), 0, request, 4, 4);
				Encoding.Unicode.GetBytes(value, 0, value.Length, request, 8);
				var exchange = ExchangeAsync(pipe, request);
				if (await Task.WhenAny(exchange, Task.Delay(5000)) != exchange)
				{
					pipe.Dispose();
					try
					{
						await exchange;
					}
					catch (IOException) { }
					catch (ObjectDisposedException) { }
					throw new TimeoutException("The audio bridge did not acknowledge the command. Check its live status before retrying.");
				}
				return await exchange;
			}
		}

		private static async Task<AudioControlStatus> ExchangeAsync(Stream pipe, byte[] request)
		{
			await pipe.WriteAsync(request, 0, request.Length);
			var response = new byte[3176];
			int received = 0;
			while (received < response.Length)
			{
				int count = await pipe.ReadAsync(response, received, response.Length - received);
				if (count == 0)
					throw new IOException("The game closed the audio control connection.");
				received += count;
				if (received >= 4 && BitConverter.ToUInt32(response, 0) != 6)
					throw new NotSupportedException("Audio bridge version mismatch. Use matching settings and game DLL builds, then restart Rocksmith.");
			}
			int result = BitConverter.ToInt32(response, 4);
			if (result < 0)
				throw new InvalidOperationException("Audio command failed (0x" + result.ToString("X8") + ").");
			return new AudioControlStatus
			{
				OutputError = BitConverter.ToInt32(response, 8),
				RecordingError = BitConverter.ToInt32(response, 12),
				IsRecording = BitConverter.ToUInt32(response, 16) != 0,
				Peak = Math.Min(1000, (int)BitConverter.ToUInt32(response, 20)),
				Frames = BitConverter.ToUInt64(response, 24),
				// Cut C-strings at the first null. TrimEnd('\0') is not enough: a debug game build fills
				// the unused tail of these fixed buffers with 0xFE (wcsncpy_s' secure fill), so bytes after
				// the terminator are not zero and would otherwise leave garbage on the string.
				FilePath = Encoding.Unicode.GetString(response, 32, 2048).Split('\0')[0],
				EndpointId = Encoding.Unicode.GetString(response, 2080, 1024).Split('\0')[0],
				RecordingStarted = BitConverter.ToUInt64(response, 3104),
				MixerError = BitConverter.ToInt32(response, 3112),
				IsDryInputReady = BitConverter.ToUInt32(response, 3144) != 0,
				IsDryRecording = BitConverter.ToUInt32(response, 3148) != 0,
				Volumes = new[] { BitConverter.ToSingle(response, 3116), BitConverter.ToSingle(response, 3120), BitConverter.ToSingle(response, 3124), BitConverter.ToSingle(response, 3128), BitConverter.ToSingle(response, 3132), BitConverter.ToSingle(response, 3136), BitConverter.ToSingle(response, 3140) },
				OutputPeak = new[] { BitConverter.ToSingle(response, 3152), BitConverter.ToSingle(response, 3156) },
				OutputRms = new[] { BitConverter.ToSingle(response, 3160), BitConverter.ToSingle(response, 3164) },
				ProxyInputMode = (int)BitConverter.ToUInt32(response, 3168)
			};
		}
	}
}
