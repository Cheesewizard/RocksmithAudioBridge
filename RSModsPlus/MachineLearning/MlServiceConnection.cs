using System;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Threading;

namespace RSModsPlus.MachineLearning
{
	public sealed class MlServiceConnection
	{
		public string Message { get; }
		public bool CanRestart { get; }
		public bool IsConnected { get; }

		private MlServiceConnection(string message, bool canRestart = false, bool isConnected = false)
		{
			Message = message;
			CanRestart = canRestart;
			IsConnected = isConnected;
		}

		[DllImport("kernel32.dll")]
		private static extern ulong GetTickCount64();

		public static MlServiceConnection Read(int gameProcessId, string channelSuffix = "")
		{
			if (gameProcessId <= 0) throw new ArgumentOutOfRangeException(nameof(gameProcessId));
			if (channelSuffix == null || (channelSuffix.Length != 0 && !channelSuffix.StartsWith(".Test", StringComparison.Ordinal))) throw new ArgumentException("Only isolated test channel suffixes are allowed.", nameof(channelSuffix));
			try
			{
				using (var mapping = MemoryMappedFile.OpenExisting("Local\\RSModsPlus.MlControl.v1." + gameProcessId, MemoryMappedFileRights.Read))
				using (var view = mapping.CreateViewAccessor(0, 32, MemoryMappedFileAccess.Read))
				{
					for (var attempt = 0; attempt < 4; attempt++)
					{
						var sequence = view.ReadUInt32(4);
						if ((sequence & 1) != 0) continue;
						Thread.MemoryBarrier();
						var version = view.ReadUInt32(0);
						var tick = view.ReadUInt64(8);
						var processId = view.ReadUInt32(16);
						var state = view.ReadUInt32(20);
						var error = view.ReadUInt32(24);
						Thread.MemoryBarrier();
						if (view.ReadUInt32(4) != sequence) continue;
						if (version != 1 || processId != gameProcessId) return new MlServiceConnection("ML controls do not match this game. Install the matching build.");
						var now = GetTickCount64();
						if (tick == 0 || now < tick || now - tick > 2000) return new MlServiceConnection("Game connection lost — status updates have stopped.");
						switch (state)
						{
							case 1: return new MlServiceConnection("Starting ML service…");
							case 3: return ReadResults(channelSuffix);
							case 4: return new MlServiceConnection("ML service stopped (exit code " + error + ").", true);
							case 5: return new MlServiceConnection(error == 2 ? "ML files missing — install the complete matching package." : "ML service could not start (Windows error " + error + ").", true);
							case 6: return new MlServiceConnection("Restarting ML service…");
							default: return new MlServiceConnection("Unknown ML status. Install the matching build.");
						}
					}
					return new MlServiceConnection("Waiting for a consistent game status…");
				}
			}
			catch (FileNotFoundException)
			{
				return new MlServiceConnection("ML controls unavailable — the game may be starting or needs the updated build.");
			}
		}

		private static MlServiceConnection ReadResults(string channelSuffix)
		{
			try
			{
				using (var mapping = MemoryMappedFile.OpenExisting("Local\\RSModsPlus.MlStringFret.v2" + channelSuffix, MemoryMappedFileRights.Read))
				using (var result = mapping.CreateViewAccessor(0, 112, MemoryMappedFileAccess.Read))
				using (var audioMapping = MemoryMappedFile.OpenExisting("Local\\RSModsPlus.MlAudio.v2" + channelSuffix, MemoryMappedFileRights.Read))
				using (var audio = audioMapping.CreateViewAccessor(0, 64, MemoryMappedFileAccess.Read))
				{
					if (result.ReadUInt32(0) != 0x46535352 || result.ReadUInt32(4) != 2 || audio.ReadUInt32(0) != 0x4C4D5352 || audio.ReadUInt32(4) != 2)
						return new MlServiceConnection("ML data version mismatch — install the matching package.", true);
					for (var attempt = 0; attempt < 4; attempt++)
					{
						var sequence = result.ReadUInt32(8);
						if ((sequence & 1) != 0) continue;
						Thread.MemoryBarrier();
						var tick = result.ReadUInt64(88);
						var sample = result.ReadUInt64(96);
						var rate = result.ReadUInt32(104);
						Thread.MemoryBarrier();
						if (result.ReadUInt32(8) != sequence) continue;
						var currentRate = audio.ReadUInt32(8);
						var currentSample = audio.ReadUInt64(16);
						var now = GetTickCount64();
						if (tick != 0 && now >= tick && now - tick <= 500 && rate != 0 && rate == currentRate && currentSample >= sample && currentSample - sample <= rate / 2)
							return new MlServiceConnection("Connected — receiving fresh ML results.", true, true);
						return new MlServiceConnection("ML service running — waiting for fresh audio/results.", true);
					}
					return new MlServiceConnection("ML service running — waiting for a consistent result.", true);
				}
			}
			catch (FileNotFoundException)
			{
				return new MlServiceConnection("ML service running — initializing or waiting for game audio.", true);
			}
		}

		public static void RequestRestart(int gameProcessId)
		{
			var connection = Read(gameProcessId);
			if (!connection.CanRestart) throw new InvalidOperationException(connection.Message);
			using (var restart = EventWaitHandle.OpenExisting("Local\\RSModsPlus.MlRestart.v1." + gameProcessId, System.Security.AccessControl.EventWaitHandleRights.Modify))
			{
				if (!restart.Set()) throw new IOException("The game did not accept the ML restart request.");
			}
		}
	}
}
