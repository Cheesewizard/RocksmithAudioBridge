// Ported from GUI/Features/BugReport/BugReport.cs (4.x WinForms GUI); behaviour, registry, ini and log names unchanged.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Text;
using System.Text.RegularExpressions;

namespace RocksmithAudioBridge.Core
{
	// "Report a problem": zips the logs and settings that explain a bad launch and opens a pre-filled email to the
	// dev address, so the user only has to attach the zip and press send. The GitHub issue is filed from that email.
	//
	// Why email and not something more automatic: the app is public, so it cannot hold SMTP or upload credentials
	// without handing them to anyone who opens the exe, and a pre-filled GitHub issue would let anyone spam the
	// tracker. mailto cannot attach files, so the zip is shown selected in Explorer for the user to drag in.
	//
	// Kept emailable by sending only the last two launches: the mod rotates RSMods_debug.txt to
	// RSMods_debug.previous.txt on every launch (DLL/dllmain.cpp SetupLogging), and the game does the same for
	// audiodump.txt. Both are sent because users often relaunch to check before reporting, which makes the current
	// log the clean run and the previous one the launch that went wrong. A file over FileCap keeps its start (boot,
	// driver bind) and its end; a normal launch never gets near it.
	//
	// The game must be closed first: the mod writes RSMods_debug.txt through a buffered stderr stream, so the last
	// lines of a running session (often the ones that show the problem) may not be on disk yet. The report never
	// closes the game itself.
	public static class BugReport
	{
		public const string DevEmail = "cheesewizard.dev@outlook.com";

		private const int FileCap = 4 * 1024 * 1024;   // bytes per file before head + tail trimming
		private const int HeadKeep = 1 * 1024 * 1024;
		private const int TailKeep = 3 * 1024 * 1024;

		// Relative to the game folder. Missing files are skipped and listed as missing in summary.txt.
		private static readonly string[] GameFiles =
		{
			"RSMods_debug.txt",
			"RSMods_debug.previous.txt",
			"audiodump.txt",
			"audiodump.previous.txt",
			"RS_ASIO-log.txt",
			"RS_ASIO.ini",
			"Rocksmith.ini",
			"RSMods.ini",
			"RocksmithAudioBridge-amp-link-log.txt",
			"RocksmithAudioBridge-vst-host-log.txt",
			"RocksmithAudioBridge-log.txt",
			@"RSMods\AudioBridgeDriverInstall.log",
			@"RSMods\NoteByNoteMenuInstall.log",
		};

		public sealed class Result
		{
			public string ZipPath;
			public string Summary;           // short, goes in the email body
			public bool LoggingWasOff;       // RSMods_debug.txt did not exist, so the mod was not logging; created now
		}

		public static string ReportFolder =>
			Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), ProductInfo.PRODUCT_NAME, "Bug reports");

		public static Result Build(string gameDirectory)
		{
			var result = new Result();

			// The mod only logs when RSMods_debug.txt already exists (upstream's opt-in). Without it there is nothing
			// to send, so turn logging on for next time and say so.
			string debugLog = Path.Combine(gameDirectory, "RSMods_debug.txt");
			if (!File.Exists(debugLog))
			{
				File.WriteAllText(debugLog, "");
				result.LoggingWasOff = true;
			}

			string summary = BuildSummary(gameDirectory);
			result.Summary = summary;

			Directory.CreateDirectory(ReportFolder);
			string zipPath = Path.Combine(ReportFolder, "Bug report " + DateTime.Now.ToString("yyyy-MM-dd HHmmss") + ".zip");
			var fileList = new StringBuilder();
			using (var stream = new FileStream(zipPath, FileMode.CreateNew, FileAccess.Write))
			using (var zip = new ZipArchive(stream, ZipArchiveMode.Create))
			{
				foreach (string relative in GameFiles)
				{
					string path = Path.Combine(gameDirectory, relative);
					if (!File.Exists(path))
					{
						fileList.AppendLine("  missing   " + relative);
						continue;
					}
					var info = new FileInfo(path);
					bool trimmed;
					string text = Scrub(ReadCapped(path, out trimmed));
					var entry = zip.CreateEntry(relative.Replace('\\', '/'), CompressionLevel.Optimal);
					entry.LastWriteTime = info.LastWriteTime;
					using (var writer = new StreamWriter(entry.Open(), new UTF8Encoding(false)))
						writer.Write(text);
					fileList.AppendLine("  " + info.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss") + "  " + info.Length.ToString().PadLeft(9) + " bytes  " + relative + (trimmed ? "  (middle cut, file was over 4 MB)" : ""));
				}

				var summaryEntry = zip.CreateEntry("summary.txt");
				using (var writer = new StreamWriter(summaryEntry.Open(), new UTF8Encoding(false)))
				{
					writer.WriteLine(ProductInfo.DISPLAY_NAME + " bug report, made " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
					writer.WriteLine();
					writer.Write(summary);
					writer.WriteLine();
					writer.WriteLine("Files (last written, size on disk):");
					writer.Write(fileList.ToString());
					if (result.LoggingWasOff)
						writer.WriteLine("RSMods_debug.txt did not exist, so the mod was not logging. It was created now; logs start next launch.");
				}
			}
			result.ZipPath = zipPath;
			return result;
		}

		private static string BuildSummary(string gameDirectory)
		{
			var s = new StringBuilder();
			s.AppendLine("Version: " + ProductInfo.DISPLAY_NAME);
			s.AppendLine("Windows: " + Environment.OSVersion.VersionString + (Environment.Is64BitOperatingSystem ? " 64-bit" : " 32-bit"));
			s.AppendLine("Game exe: " + DescribeGameExe(gameDirectory));
			string mode;
			try
			{
				switch (new AudioInputModeConfiguration(gameDirectory).ReadMode())
				{
					case AudioInputMode.Asio: mode = "ASIO interface"; break;
					case AudioInputMode.Cable: mode = "Real Tone Cable"; break;
					default: mode = "Real Tone Cable (RS_ASIO not installed)"; break;
				}
			}
			catch (Exception error) { mode = "unknown (" + error.Message + ")"; }
			s.AppendLine("Guitar input: " + mode);
			s.AppendLine("Bridge power: " + (ReadIni(Path.Combine(gameDirectory, "RSMods.ini"), "Audio Bridge", "MasterEnabled") == "0" ? "off" : "on"));
			string driver = AsioProxySetup.ReadOutputDriver(gameDirectory);
			if (driver.Length > 0) s.AppendLine("RS_ASIO Driver=: " + driver);
			string target = AsioProxySetup.ReadTarget();
			if (target.Length > 0) s.AppendLine("Bridge driver wraps: " + target);
			if (AsioProxySetup.ReadLastBindError(out string failedDriver, out string reason))
				s.AppendLine("Last launch bind error: " + (failedDriver.Length > 0 ? failedDriver + ": " : "") + reason);
			return Scrub(s.ToString());
		}

		// The mod picks its addresses by the exe's PE header CheckSum (DLL/VersioningStruct.cpp ourVersionChecksums) and
		// falls back to the Sept 2022 addresses on anything else, which crashes early with empty logs. Keep this table
		// in step with that one.
		private static readonly Dictionary<uint, string> KnownGameBuilds = new Dictionary<uint, string>
		{
			{ 0x00B13D7C, "Remastered September 2022" },
			{ 0x0176EC34, "Learn & Play December 2024" },
		};

		private static string DescribeGameExe(string gameDirectory)
		{
			string exe = Path.Combine(gameDirectory, "Rocksmith2014.exe");
			try
			{
				if (!File.Exists(exe)) return "missing";
				using (var reader = new BinaryReader(new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)))
				{
					long size = reader.BaseStream.Length;
					reader.BaseStream.Seek(0x3C, SeekOrigin.Begin);   // e_lfanew
					int peHeader = reader.ReadInt32();
					reader.BaseStream.Seek(peHeader + 0x58, SeekOrigin.Begin);   // "PE\0\0" + file header (0x18) + OptionalHeader.CheckSum (0x40)
					uint checksum = reader.ReadUInt32();
					string edition = KnownGameBuilds.TryGetValue(checksum, out string name) ? name : "UNKNOWN build, the mod does not support it";
					return edition + " (PE checksum 0x" + checksum.ToString("X8") + ", " + size + " bytes)";
				}
			}
			catch (Exception error) { return "unreadable (" + error.Message + ")"; }
		}

		// Whole file when it is small enough; otherwise the first HeadKeep and last TailKeep bytes. Shared read and
		// delete: the desktop mixer or VST host may still hold their logs open.
		private static string ReadCapped(string path, out bool trimmed)
		{
			using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
			{
				trimmed = stream.Length > FileCap;
				if (!trimmed)
				{
					using (var reader = new StreamReader(stream)) return reader.ReadToEnd();
				}
				var head = new byte[HeadKeep];
				ReadFully(stream, head);
				var tail = new byte[TailKeep];
				stream.Seek(-TailKeep, SeekOrigin.End);
				ReadFully(stream, tail);
				return Encoding.UTF8.GetString(head)
					+ "\r\n\r\n----- " + (stream.Length - HeadKeep - TailKeep) + " bytes cut here to keep the report emailable -----\r\n\r\n"
					+ Encoding.UTF8.GetString(tail);
			}
		}

		private static void ReadFully(Stream stream, byte[] buffer)
		{
			int offset = 0;
			while (offset < buffer.Length)
			{
				int read = stream.Read(buffer, offset, buffer.Length - offset);
				if (read <= 0) break;
				offset += read;
			}
		}

		// The Windows user name shows up in every path under C:\Users. Replaced only right after "Users\" so a short
		// name cannot eat unrelated text.
		private static string Scrub(string text)
		{
			string name = Environment.UserName;
			if (string.IsNullOrEmpty(name)) return text;
			return Regex.Replace(text, @"(?<=[\\/]Users[\\/])" + Regex.Escape(name) + @"(?=[\\/""'\s]|$)", "<user>", RegexOptions.IgnoreCase);
		}

		private static string ReadIni(string path, string section, string key)
		{
			if (!File.Exists(path)) return "";
			bool inSection = false;
			foreach (string raw in File.ReadAllLines(path))
			{
				string line = raw.Trim();
				if (line.StartsWith("[")) { inSection = string.Equals(line, "[" + section + "]", StringComparison.OrdinalIgnoreCase); continue; }
				if (inSection && line.StartsWith(key + "=", StringComparison.OrdinalIgnoreCase)) return line.Substring(key.Length + 1).Trim();
			}
			return "";
		}

		/// <summary>Opens the user's mail app with the dev address, a subject and the summary filled in. False when
		/// no mail app handles mailto: links; the caller then shows the address instead.</summary>
		public static bool OpenEmail(Result report)
		{
			string body =
				"Please attach the zip that is selected in the Explorer window: " + Path.GetFileName(report.ZipPath) + "\r\n\r\n"
				+ "What happened, and what were you doing just before?\r\n\r\n\r\n\r\n"
				+ "----\r\n" + report.Summary;
			string mailto = "mailto:" + DevEmail
				+ "?subject=" + Uri.EscapeDataString(ProductInfo.DISPLAY_NAME + " bug report")
				+ "&body=" + Uri.EscapeDataString(body);
			try
			{
				Process.Start(new ProcessStartInfo(mailto) { UseShellExecute = true });
				return true;
			}
			catch { return false; }
		}

		public static void ShowInExplorer(string path)
		{
			try { Process.Start(new ProcessStartInfo("explorer.exe", "/select,\"" + path + "\"") { UseShellExecute = true }); } catch { }
		}
	}
}
