using System;
using System.IO;

namespace RSMods.Audio
{
	internal static class StudioFormat
	{
		public const long AudioBytesPerSecond = 192000;

		public static string Bytes(long value)
		{
			if (value >= 1L << 30)
				return (value / (double)(1L << 30)).ToString("0.0") + " GB";
			if (value >= 1L << 20)
				return (value / (double)(1L << 20)).ToString("0") + " MB";
			return Math.Max(1, value / 1024) + " KB";
		}

		public static string Length(TimeSpan value)
		{
			if (value.TotalHours >= 1)
				return (int)value.TotalHours + " h " + value.Minutes + " m";
			if (value.TotalMinutes >= 1)
				return value.Minutes + " m " + value.Seconds.ToString("00") + " s";
			return value.Seconds + "." + (value.Milliseconds / 100) + " s";
		}

		public static string Timecode(TimeSpan value)
		{
			return value.TotalHours >= 1 ? value.ToString(@"h\:mm\:ss\.f") : value.ToString(@"mm\:ss\.f");
		}

		public static TimeSpan? WaveLength(string path)
		{
			try
			{
				using (var file = File.OpenRead(path))
				{
					var header = new byte[44];
					if (file.Read(header, 0, header.Length) != header.Length)
						return null;
					uint bytesPerSecond = BitConverter.ToUInt32(header, 28);
					uint dataBytes = BitConverter.ToUInt32(header, 40);
					if (bytesPerSecond == 0)
						return null;
					if (dataBytes == 0 || dataBytes > file.Length - 44)
						dataBytes = (uint)Math.Max(0, file.Length - 44);
					return TimeSpan.FromSeconds(dataBytes / (double)bytesPerSecond);
				}
			}
			catch (Exception error) when (error is IOException || error is UnauthorizedAccessException)
			{
				return null;
			}
		}

		public static long FreeSpace(string directory)
		{
			try
			{
				string root = Path.GetPathRoot(Path.GetFullPath(directory));
				return string.IsNullOrEmpty(root) ? -1 : new DriveInfo(root).AvailableFreeSpace;
			}
			catch (Exception error) when (error is IOException || error is ArgumentException || error is UnauthorizedAccessException)
			{
				return -1;
			}
		}
	}
}
