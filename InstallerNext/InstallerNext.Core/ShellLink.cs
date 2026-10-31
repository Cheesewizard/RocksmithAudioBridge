#nullable enable
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;

namespace RocksmithAudioBridge.Setup
{
	/// <summary>Writes a Windows .lnk shortcut through the shell's own IShellLink (no WScript, no extra package).</summary>
	public static class ShellLink
	{
		public static void Create(string shortcutPath, string target, string workingDirectory, string description)
		{
			var link = (IShellLinkW)new CShellLink();
			try
			{
				link.SetPath(target);
				link.SetWorkingDirectory(workingDirectory);
				link.SetDescription(description);
				link.SetIconLocation(target, 0);
				((IPersistFile)link).Save(shortcutPath, true);
			}
			finally
			{
				Marshal.FinalReleaseComObject(link);
			}
		}

		/// <summary>The target a .lnk points at, or null when it cannot be read.</summary>
		public static string? ReadTarget(string shortcutPath)
		{
			if (!File.Exists(shortcutPath)) return null;
			var link = (IShellLinkW)new CShellLink();
			try
			{
				((IPersistFile)link).Load(shortcutPath, 0);
				var buffer = new StringBuilder(1024);
				link.GetPath(buffer, buffer.Capacity, IntPtr.Zero, 0);
				return buffer.ToString();
			}
			catch { return null; }
			finally { Marshal.FinalReleaseComObject(link); }
		}

		[ComImport, Guid("00021401-0000-0000-C000-000000000046")]
		private class CShellLink { }

		[ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("000214F9-0000-0000-C000-000000000046")]
		private interface IShellLinkW
		{
			void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszFile, int cch, IntPtr pfd, uint fFlags);
			void GetIDList(out IntPtr ppidl);
			void SetIDList(IntPtr pidl);
			void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszName, int cch);
			void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string pszName);
			void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszDir, int cch);
			void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string pszDir);
			void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszArgs, int cch);
			void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string pszArgs);
			void GetHotkey(out short pwHotkey);
			void SetHotkey(short wHotkey);
			void GetShowCmd(out int piShowCmd);
			void SetShowCmd(int iShowCmd);
			void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszIconPath, int cch, out int piIcon);
			void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string pszIconPath, int iIcon);
			void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string pszPathRel, uint dwReserved);
			void Resolve(IntPtr hwnd, uint fFlags);
			void SetPath([MarshalAs(UnmanagedType.LPWStr)] string pszFile);
		}
	}
}
