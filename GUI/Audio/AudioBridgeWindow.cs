using System;
using System.Runtime.InteropServices;

namespace RSMods.Audio
{
	// The one place that knows how to find a running audio bridge window and bring it forward.
	// Both the main window's "Open audio bridge" button and a second `--audio-bridge` launch use
	// this, so the title lookup and the user32 calls are not repeated per caller.
	internal static class AudioBridgeWindow
	{
		public const string Title = ProductInfo.AUDIO_BRIDGE_WINDOW_TITLE;

		internal enum Presence
		{
			NotFound,          // no bridge window exists
			Shown,             // found and now in the foreground
			FoundButNotShown   // found, but Windows refused to focus it from this process (typically a
			                   // bridge launched by an elevated Rocksmith while this process is not elevated)
		}

		private const int SW_RESTORE = 9;

		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool IsIconic(IntPtr hWnd);

		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool SetForegroundWindow(IntPtr hWnd);

		[DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
		private static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

		// Restore (if minimised) and activate the existing bridge window. It remains a normal desktop
		// window rather than permanently occupying the topmost band.
		public static Presence TryBringToFront(bool forceRestore = false)
		{
			IntPtr handle = FindWindow(null, Title);
			if (handle == IntPtr.Zero) return Presence.NotFound;

			if (forceRestore || IsIconic(handle))
				ShowWindow(handle, SW_RESTORE);

			bool shown = SetForegroundWindow(handle);
			return shown && !IsIconic(handle) ? Presence.Shown : Presence.FoundButNotShown;
		}
	}
}
