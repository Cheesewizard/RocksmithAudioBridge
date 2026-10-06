using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace RSMods.Audio
{
	internal sealed class WindowCaptureRecorder : IDisposable
	{
		private const uint FRAMES_PER_SECOND = 30;
		private const uint BITS_PER_SECOND = 12000000;
		private const long MAXIMUM_DRIFT_TICKS = 30 * TimeSpan.TicksPerSecond;
		private const int ERROR_EMPTY = unchecked((int)0x800700FE);
		private const int ERROR_NOT_SUPPORTED = unchecked((int)0x80070032);

		public string RetainedVideoPath => videoPath;

		public static bool Supported
		{
			get
			{
				try { return RsCaptureSupported() != 0; }
				catch (DllNotFoundException) { return false; }
				catch (EntryPointNotFoundException) { return false; }
			}
		}

		public static string Requirement
		{
			get { return "Video capture needs Windows 10 1903 or newer with the recorder library installed."; }
		}

		public bool IsCapturing
		{
			get { return capturing; }
		}

		private string videoPath;
		private bool capturing;

		public Task StartAsync(int gameProcessId, string directory)
		{
			using (var game = Process.GetProcessById(gameProcessId))
			{
				if (game.MainWindowHandle == IntPtr.Zero)
					throw new InvalidOperationException("Rocksmith has no capture window.");
				Start(game.MainWindowHandle, directory);
			}
			return Task.FromResult(true);
		}

		/// <summary>Starts capturing a known window. The headless video take (VideoTakeHost) passes the game's
		/// window handle from the DLL, since a hidden helper has no reason to look it up by process.</summary>
		public void Start(IntPtr window, string directory)
		{
			if (capturing)
				throw new InvalidOperationException("A video take is already open.");
			if (window == IntPtr.Zero)
				throw new InvalidOperationException("Rocksmith has no capture window.");
			Directory.CreateDirectory(directory);
			videoPath = Path.Combine(directory, "Rocksmith-video-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N") + ".mp4");
			Check(RsCaptureStart(window, videoPath, BITS_PER_SECOND, FRAMES_PER_SECOND), "Video capture could not start");
			capturing = true;
		}

		public Task<string> StopAsync(AudioControlStatus audio)
		{
			return audio == null ? StopAsync(null, 0, 0) : StopAsync(audio.FilePath, audio.RecordingStarted, audio.Frames);
		}

		/// <summary>Stops the capture and muxes it with the finished game-audio take. The audio values are the ones
		/// the engine reports when a take stops (op 3): the WAV path, its start FILETIME and frame count.</summary>
		public async Task<string> StopAsync(string audioPath, ulong audioStarted, ulong audioFrames)
		{
			if (!capturing)
				throw new InvalidOperationException("No video take is open.");
			capturing = false;
			ulong startFileTime;
			ulong frames;
			int stopped = RsCaptureStop(out startFileTime, out frames);
			if (stopped == ERROR_EMPTY)
				throw new IOException("No frames were captured. Play in a visible window and try the take again.");
			Check(stopped, "Video capture failed");
			if (audioStarted == 0 || audioFrames == 0 || string.IsNullOrEmpty(audioPath) || !File.Exists(audioPath))
				throw new IOException("No game audio was captured. The video file was retained.");
			long offset = (long)audioStarted - (long)startFileTime;
			if (Math.Abs(offset) > MAXIMUM_DRIFT_TICKS)
				throw new IOException("Video and audio clocks did not agree. Separate take files were retained.");
			string output = Path.ChangeExtension(audioPath, ".mp4");
			string video = videoPath;
			await Task.Run(() => Combine(video, audioPath, output, offset));
			Delete(video);
			videoPath = null;
			// A video take is dry WAV + MP4, never a third file: the MP4 carries the wet audio, so the wet WAV
			// is redundant. It is only removed after a successful mux; on any failure above it stays as the take.
			Delete(audioPath);
			return output;
		}

		public static string Combine(string videoFile, string audioFile, string outputFile, long audioOffset)
		{
			Check(RsCaptureMux(videoFile, audioFile, outputFile, audioOffset), "Video and audio could not be combined");
			return outputFile;
		}

		public void Dispose()
		{
			if (capturing)
			{
				capturing = false;
				ulong startFileTime;
				ulong frames;
				int result = RsCaptureStop(out startFileTime, out frames);
				if (result < 0)
				{
					Trace.TraceError("Video finalization failed (0x" + result.ToString("X8", CultureInfo.InvariantCulture) + "). Source retained at " + videoPath);
				}
			}
			if (!string.IsNullOrEmpty(videoPath))
			{
				Trace.TraceInformation("Video retained at " + videoPath);
			}
		}

		private static void Delete(string path)
		{
			if (string.IsNullOrEmpty(path))
				return;
			try { if (File.Exists(path)) File.Delete(path); }
			catch (IOException error) { Trace.TraceError("Capture cleanup failed: " + error); }
			catch (UnauthorizedAccessException error) { Trace.TraceError("Capture cleanup failed: " + error); }
		}

		private static void Check(int result, string message)
		{
			if (result >= 0)
				return;
			if (result == ERROR_NOT_SUPPORTED)
				throw new NotSupportedException(Requirement);
			throw new IOException(message + " (0x" + result.ToString("X8", CultureInfo.InvariantCulture) + ").");
		}

		[DllImport("rswindowcapture.dll")]
		private static extern int RsCaptureSupported();

		[DllImport("rswindowcapture.dll", CharSet = CharSet.Unicode)]
		private static extern int RsCaptureStart(IntPtr window, string path, uint bitsPerSecond, uint framesPerSecond);

		[DllImport("rswindowcapture.dll")]
		private static extern int RsCaptureStop(out ulong startFileTime, out ulong frames);

		[DllImport("rswindowcapture.dll", CharSet = CharSet.Unicode)]
		private static extern int RsCaptureMux(string videoPath, string audioPath, string outputPath, long audioOffset);
	}
}
