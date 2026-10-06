using System;
using System.Diagnostics;
using System.IO;
using System.Threading;

namespace RSMods.Audio
{
	/// <summary>
	/// Headless video take: RSMods.exe started by the game DLL with no window, alive only while a video take is
	/// open. The DLL records the game audio itself (in process); this helper only runs the Windows Graphics
	/// Capture recorder (rswindowcapture.dll, same code the desktop bridge uses) and muxes the result. It reuses
	/// RSMods.exe instead of shipping a new executable so antivirus sees an already-trusted binary.
	///
	/// Contract with DLL/Audio/TakeRecorder.cpp (change both together):
	///   RSMods.exe --video-take &lt;hwnd&gt; &lt;game pid&gt; "&lt;directory&gt;" &lt;token&gt;
	///   Events (created by the DLL): Local\RSModsVideoTake-&lt;token&gt;-ready (set here once capturing) and
	///   Local\RSModsVideoTake-&lt;token&gt;-stop (set by the DLL after it stops the audio take).
	///   %TEMP%\RSModsVideoTake-&lt;token&gt;.audio (written by the DLL before stop): WAV path, start FILETIME, frames.
	///   %TEMP%\RSModsVideoTake-&lt;token&gt;.result (written here): "ok", "video", or "error", then a path or message.
	/// </summary>
	internal static class VideoTakeHost
	{
		public static bool TryRun(string[] arguments, out int exitCode)
		{
			exitCode = 0;
			if (arguments.Length == 0 || arguments[0] != "--video-take")
				return false;
			exitCode = Run(arguments);
			return true;
		}

		private static int Run(string[] arguments)
		{
			if (arguments.Length != 5 || !long.TryParse(arguments[1], out long window) || !int.TryParse(arguments[2], out int gameId)
				|| !Path.IsPathRooted(arguments[3]) || arguments[4].Length == 0 || arguments[4].IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
				return 2;
			string directory = arguments[3];
			string token = arguments[4];
			string temp = Path.GetTempPath();
			string resultPath = Path.Combine(temp, "RSModsVideoTake-" + token + ".result");
			string audioPath = Path.Combine(temp, "RSModsVideoTake-" + token + ".audio");

			var recorder = new WindowCaptureRecorder();
			try
			{
				using (var ready = EventWaitHandle.OpenExisting(@"Local\RSModsVideoTake-" + token + "-ready"))
				using (var stop = EventWaitHandle.OpenExisting(@"Local\RSModsVideoTake-" + token + "-stop"))
				using (var game = Process.GetProcessById(gameId))
				{
					recorder.Start(new IntPtr(window), directory);
					ready.Set();
					// Stop when the DLL says so, or when the game goes away (keeps whatever video was captured).
					using (var gameExited = new ManualResetEvent(false) { SafeWaitHandle = new Microsoft.Win32.SafeHandles.SafeWaitHandle(game.Handle, false) })
						WaitHandle.WaitAny(new WaitHandle[] { stop, gameExited });
				}

				string audioFile = null;
				ulong started = 0, frames = 0;
				if (File.Exists(audioPath))
				{
					string[] lines = File.ReadAllLines(audioPath);
					if (lines.Length >= 3)
					{
						audioFile = lines[0];
						ulong.TryParse(lines[1], out started);
						ulong.TryParse(lines[2], out frames);
					}
					TryDelete(audioPath);
				}
				try
				{
					string output = recorder.StopAsync(audioFile, started, frames).GetAwaiter().GetResult();
					WriteResult(resultPath, "ok", output);
				}
				catch (IOException error) when (!string.IsNullOrEmpty(recorder.RetainedVideoPath) && File.Exists(recorder.RetainedVideoPath))
				{
					// Capture worked but it could not be combined with the audio: keep the video on its own.
					WriteResult(resultPath, "video", recorder.RetainedVideoPath + "\n" + error.Message);
				}
				return 0;
			}
			catch (Exception error)
			{
				WriteResult(resultPath, "error", error is NotSupportedException ? WindowCaptureRecorder.Requirement : error.Message);
				return 1;
			}
			finally
			{
				recorder.Dispose();
			}
		}

		private static void WriteResult(string path, string kind, string detail)
		{
			try { File.WriteAllText(path, kind + "\n" + (detail ?? "").Replace("\r", "")); }
			catch (IOException) { }
			catch (UnauthorizedAccessException) { }
		}

		private static void TryDelete(string path)
		{
			try { File.Delete(path); }
			catch (IOException) { }
			catch (UnauthorizedAccessException) { }
		}
	}
}
