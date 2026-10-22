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
		/// <summary>
		/// Player 1 noise gate meter, from the uint32 at byte 3172 (formerly struct padding, so the response size and
		/// version are unchanged). 0 = no data / gate not running. Otherwise bit 30 = gate running, bit 31 = gate open,
		/// bits 0..15 = detector level as (dB * 10 + 2000), so -70.0 dB = 1300.
		/// </summary>
		public uint GateMeter { get; set; }
		public bool GateRunning => (GateMeter & 0x40000000u) != 0;
		public bool GateOpen => GateRunning && (GateMeter & 0x80000000u) != 0;
		/// <summary>The gate detector level in dB, or null when the gate is not running.</summary>
		public double? GateLevelDb => GateRunning ? ((int)(GateMeter & 0xFFFFu) - 2000) / 10.0 : (double?)null;

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

	/// <summary>
	/// Where noise gate edits (ops 19 and 31) land right now, from op 32. While a saved Tone designer tone plays the
	/// game DLL targets that tone; otherwise (game amp, unsaved chain) the global gate in RSMods.ini. The DLL answers
	/// op 32 in the response's file text as "key=value" lines separated by "\n".
	/// </summary>
	internal sealed class GateTarget
	{
		/// <summary>The tone the gate edits go to; empty = all tones (the global gate).</summary>
		public string Tone { get; set; } = "";
		public bool On { get; set; }
		/// <summary>Signed tenths of a dB, always negative; kept while the gate is off.</summary>
		public int ThresholdTenths { get; set; }
		public int DecayMs { get; set; }
		public int ReductionDb { get; set; }
		/// <summary>Tenths of a ms.</summary>
		public int AttackTenths { get; set; }
		/// <summary>The tone has gate settings of its own (op 33 can remove them).</summary>
		public bool Own { get; set; }
		/// <summary>Changes whenever the target or any of its values change.</summary>
		public uint Revision { get; set; }

		public bool IsGlobal => string.IsNullOrEmpty(Tone);

		/// <summary>Parses the op 32 reply text. Null when it is not a gate target reply (no revision line).</summary>
		public static GateTarget Parse(string text)
		{
			if (string.IsNullOrEmpty(text)) return null;
			var target = new GateTarget();
			bool hasRevision = false;
			var invariant = System.Globalization.CultureInfo.InvariantCulture;
			foreach (string rawLine in text.Split('\n'))
			{
				string line = rawLine.TrimEnd('\r');
				int equals = line.IndexOf('=');
				if (equals <= 0) continue;
				string key = line.Substring(0, equals).Trim();
				string value = line.Substring(equals + 1);
				int number;
				switch (key)
				{
					case "tone": target.Tone = value.Trim(); break;
					case "on": target.On = value.Trim() == "1"; break;
					case "threshold": if (int.TryParse(value.Trim(), System.Globalization.NumberStyles.Integer, invariant, out number)) target.ThresholdTenths = number; break;
					case "decay": if (int.TryParse(value.Trim(), System.Globalization.NumberStyles.Integer, invariant, out number)) target.DecayMs = number; break;
					case "reduction": if (int.TryParse(value.Trim(), System.Globalization.NumberStyles.Integer, invariant, out number)) target.ReductionDb = number; break;
					case "attack": if (int.TryParse(value.Trim(), System.Globalization.NumberStyles.Integer, invariant, out number)) target.AttackTenths = number; break;
					case "own": target.Own = value.Trim() == "1"; break;
					case "revision":
						if (uint.TryParse(value.Trim(), System.Globalization.NumberStyles.Integer, invariant, out uint revision))
						{
							target.Revision = revision;
							hasRevision = true;
						}
						break;
				}
			}
			return hasRevision ? target : null;
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

		/// <summary>
		/// Op 32: the current noise gate target. The reply text arrives in the file field (FilePath). Returns null when
		/// the reply carries no gate target (an older game DLL that answers op 32 with something else). A DLL that
		/// rejects op 32 makes SendAsync throw InvalidOperationException; pipe errors propagate as usual.
		/// </summary>
		public async Task<GateTarget> GetGateTargetAsync()
		{
			var status = await SendAsync(32);
			return GateTarget.Parse(status.FilePath);
		}

		/// <summary>Op 33: the playing tone drops its own gate settings and follows the global gate again.
		/// Throws InvalidOperationException when no saved tone plays or it has no settings of its own.</summary>
		public Task<AudioControlStatus> UseGlobalGateAsync() => SendAsync(33);

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
				ProxyInputMode = (int)BitConverter.ToUInt32(response, 3168),
				GateMeter = BitConverter.ToUInt32(response, 3172)
			};
		}
	}
}
